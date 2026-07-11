//
// ota.cpp -- GitHub-Releases OTA client (see ota.h for the design rationale).
//
// Everything here runs single-core with BT/audio NEVER INITIALISED (the OTA
// boot mode diverts out of main() before bt_init/audio_init), so:
//   - the heap is nearly empty -> mbedTLS's ~20 KB handshake fits easily;
//   - core1 never launched -> flash erase/program need only IRQs off, no
//     multicore lockout (the same pre-core1 case flash_safety.cpp handles);
//   - blocking pump loops are fine -- there is no audio to starve.
//
// The HTTP layer is lwIP's httpc (http_client.c) over altcp_tls (mbedTLS).
// httpc can't follow redirects itself, so we capture Location: from the
// headers_done callback and re-issue; GitHub uses redirects both for version
// discovery (/releases/latest -> /releases/tag/<tag>, which we do NOT follow
// -- the Location IS the answer) and for asset downloads (github.com ->
// objects.githubusercontent.com, which we do follow, re-handshaking TLS with
// the new host's SNI).
//

#include "ota.h"

#ifdef ENABLE_OTA

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/flash.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/apps/httpd.h"
#include "lwip/apps/mdns.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "mbedtls/debug.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"

#include <malloc.h> // mallinfo(): heap telemetry around the TLS bring-up

#include "config.h"
#include "ota_certs.h"

//--------------------------------------------------------------------+
// Flash layout constants
//--------------------------------------------------------------------+

// Staged image window: [2 MB, config sector). 2 MB clears the largest image
// this firmware can grow to (enforced at build time by ota_size_check.cmake);
// the top bound keeps clear of the config store at -4 sectors (see the layout
// map in config.cpp -- the expression below MUST mirror CONFIG_FLASH_OFFSET
// there, which is private to config.cpp).
static constexpr uint32_t OTA_STAGING_OFFSET = 2u * 1024u * 1024u;
static constexpr uint32_t OTA_STAGING_LIMIT =
    PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE; // == CONFIG_FLASH_OFFSET
static constexpr uint32_t OTA_STAGING_CAPACITY = OTA_STAGING_LIMIT - OTA_STAGING_OFFSET;
static_assert(OTA_STAGING_OFFSET % FLASH_SECTOR_SIZE == 0);
static_assert(OTA_STAGING_LIMIT > OTA_STAGING_OFFSET,
              "flash too small for OTA staging (Pico W is excluded in CMake)");
static_assert(OTA_STAGING_CAPACITY >= 1u * 1024u * 1024u,
              "staging window must hold a full WiFi image");
// A plausible image is at least this big; anything smaller is a truncated
// download or a mispackaged release and must not be flashed.
static constexpr uint32_t OTA_MIN_IMAGE = 512u * 1024u;

//--------------------------------------------------------------------+
// Watchdog scratch protocol
//--------------------------------------------------------------------+
// scratch[2]: OTA boot request. Magic in the top 24 bits, flags below.
//   Survives watchdog_reboot (how the request crosses the reboot) but not
//   power loss (so a wedged OTA can never boot-loop: any power cycle lands
//   back in normal firmware). Cleared on entry to OTA mode -- one attempt
//   per request, a crash mid-OTA falls back to normal boot.
// scratch[3]: last OTA result, same magic scheme; read+cleared by
//   ota_boot_capture_result() in the next normal boot.
// scratch[4..7] belong to the SDK's watchdog_reboot vector protocol -- never
// touched here except zeroing scratch[4] before the final reset so the
// bootrom takes the normal flash-boot path.
static constexpr uint32_t OTA_MAGIC_MASK = 0xFFFFFF00u;
static constexpr uint32_t OTA_BOOT_MAGIC = 0x07A5EB00u;
static constexpr uint32_t OTA_BOOT_FLAG_FORCE = 0x1u;
static constexpr uint32_t OTA_BOOT_FLAG_BETA = 0x2u;
static constexpr uint32_t OTA_RESULT_MAGIC = 0x07A5EC00u;

static uint32_t captured_result = OTA_RESULT_NONE;

bool ota_boot_pending() {
    return (watchdog_hw->scratch[2] & OTA_MAGIC_MASK) == OTA_BOOT_MAGIC;
}

void ota_boot_capture_result() {
    const uint32_t v = watchdog_hw->scratch[3];
    if ((v & OTA_MAGIC_MASK) == OTA_RESULT_MAGIC) {
        captured_result = v & ~OTA_MAGIC_MASK;
    }
    watchdog_hw->scratch[3] = 0;
}

uint32_t ota_last_result() { return captured_result; }

const char *ota_result_str(uint32_t r) {
    switch (r) {
        case OTA_RESULT_NONE:             return "none";
        case OTA_RESULT_OK:               return "updated";
        case OTA_RESULT_ALREADY_CURRENT:  return "already up to date";
        case OTA_RESULT_NO_WIFI_CREDS:    return "no WiFi credentials";
        case OTA_RESULT_WIFI_JOIN_FAILED: return "WiFi join failed";
        case OTA_RESULT_TLS_INIT_FAILED:  return "TLS init failed";
        case OTA_RESULT_CHECK_FAILED:     return "release check failed";
        case OTA_RESULT_SHA_FETCH_FAILED: return "checksum fetch failed";
        case OTA_RESULT_DOWNLOAD_FAILED:  return "download failed";
        case OTA_RESULT_TOO_BIG:          return "image too big";
        case OTA_RESULT_SHA_MISMATCH:     return "checksum mismatch";
        case OTA_RESULT_TIMEOUT:          return "timed out";
        case OTA_RESULT_OOM:              return "out of memory";
        default:                          return "unknown";
    }
}

static int64_t ota_reboot_alarm(alarm_id_t, void *) {
    watchdog_reboot(0, 0, 0);
    return 0;
}

void ota_request_and_reboot(bool force, bool beta) {
    watchdog_hw->scratch[2] = OTA_BOOT_MAGIC | (force ? OTA_BOOT_FLAG_FORCE : 0) |
                              (beta ? OTA_BOOT_FLAG_BETA : 0);
    // Deferred so the HTTP response flushes to the browser first (same delay
    // wifi_net.cpp uses for its provision/reset reboots).
    add_alarm_in_ms(1200, ota_reboot_alarm, nullptr, true);
    printf("[OTA] update requested (force=%d beta=%d), rebooting into OTA mode\n",
           force ? 1 : 0, beta ? 1 : 0);
}

//--------------------------------------------------------------------+
// Status (served by web_api.cpp as /api/ota/status)
//--------------------------------------------------------------------+

static bool mode_active = false;
static const char *volatile status_state = "idle";
static volatile uint32_t status_bytes = 0;
static volatile uint32_t status_total = 0;
static char status_tag[48] = "";

bool ota_mode_active() { return mode_active; }

void ota_get_status(OtaStatus *out) {
    out->state = status_state;
    out->bytes = status_bytes;
    out->total = status_total;
    strncpy(out->tag, status_tag, sizeof(out->tag) - 1);
    out->tag[sizeof(out->tag) - 1] = '\0';
}

//--------------------------------------------------------------------+
// OTA context -- heap-allocated at OTA-mode entry so NONE of these buffers
// cost bss/heap in the normal (gaming) runtime.
//--------------------------------------------------------------------+

// GitHub's signed CDN redirect URLs run ~600-1000 chars; cap generously.
static constexpr size_t OTA_HOST_MAX = 64;
static constexpr size_t OTA_PATH_MAX = 1536;

enum class FetchKind : uint8_t {
    CHECK, // /releases/latest: the 302 Location IS the answer; do not follow
    SMALL, // follow redirects to a 200, accumulate a small text body (.sha256)
    BIN,   // follow redirects to a 200, stream the body to staging flash
};

struct OtaCtx {
    struct altcp_tls_config *tls;
    altcp_allocator_t alloc;

    // Current connection
    char host[OTA_HOST_MAX];
    char path[OTA_PATH_MAX];
    FetchKind kind;
    httpc_connection_t settings;
    httpc_state_t *conn;

    // Response capture
    volatile bool done;
    httpc_result_t result;
    uint32_t srv_status;      // HTTP status code parsed from the status line
    bool redirect_seen;
    char redirect_url[OTA_PATH_MAX];
    bool oversize;            // BIN body would overflow the staging window

    // Redirect-parse scratch. In the ctx (heap), NOT ota_fetch() locals: the
    // core0 stack is small and the TLS handshake underneath needs its share.
    char nhost[OTA_HOST_MAX];
    char npath[OTA_PATH_MAX];

    // SMALL-body accumulator. Sized for the beta channel's releases.atom
    // scan: the first <entry> (= newest release, prereleases included) sits
    // within the first few KB of the feed. The .sha256 file (~80 B) uses a
    // sliver of it.
    char small_body[4096];
    uint32_t small_len;

    // BIN streaming: one flash sector accumulated in RAM, then erase+program.
    // Also reused as the bounce buffer by the apply stub.
    uint8_t secbuf[FLASH_SECTOR_SIZE];
    uint32_t sec_fill;
    uint32_t staged;          // bytes already written to staging flash

    // Release metadata
    char tag[48];
    uint8_t expect_sha[32];
    bool force;
};

static OtaCtx *ctx;

// http_client.c's "no Content-Length header" sentinel; the macro is private
// to that .c file, so mirror its value here.
static constexpr u32_t OTA_HTTPC_CONTENT_LEN_INVALID = 0xFFFFFFFFu;

//--------------------------------------------------------------------+
// Flash helpers (single-core, core1 never launched: IRQs-off is sufficient,
// exactly the pre-core1 direct-write case in flash_safety.cpp -- used
// directly here to avoid 300 sectors of helper log spam)
//--------------------------------------------------------------------+

static void stage_flash_sector(uint32_t offset_in_staging, const uint8_t *data) {
    const uint32_t abs = OTA_STAGING_OFFSET + offset_in_staging;
    const uint32_t save = save_and_disable_interrupts();
    flash_range_erase(abs, FLASH_SECTOR_SIZE);
    flash_range_program(abs, data, FLASH_SECTOR_SIZE);
    restore_interrupts_from_disabled(save);
}

//--------------------------------------------------------------------+
// httpc callbacks
//--------------------------------------------------------------------+

extern "C" {
#if defined(MBEDTLS_DEBUG_C)
// mbedTLS handshake diagnostics straight to the UART. The altcp shim's own
// debug path needs LWIP_DEBUG, which breaks the lwIP 2.2 build (see
// lwipopts.h) -- so hook mbedTLS directly instead. Level 1 = errors,
// 2 = state changes; the threshold is set in ota_mode_main().
static void ota_mbedtls_dbg(void *, int level, const char *file, int line, const char *str) {
    // Basename only: mbedTLS passes full paths.
    const char *base = strrchr(file, '/');
    printf("[TLS%d] %s:%d %s", level, base ? base + 1 : file, line, str);
}
#endif

// TLS pcb allocator handed to httpc. httpc creates the pcb internally, which
// would normally leave no hook to set the SNI/verification hostname before
// the handshake -- so the allocator itself stamps it on the fresh TLS context
// (and, with diagnostics on, the debug callback: conf_dbg lives on the shared
// mbedtls_ssl_config the shim owns; MBEDTLS_ALLOW_PRIVATE_ACCESS is already
// required by the shim itself, so reaching ssl->conf is sanctioned here).
static struct altcp_pcb *ota_tls_alloc(void *arg, u8_t ip_type) {
    OtaCtx *c = (OtaCtx *) arg;
    struct altcp_pcb *pcb = altcp_tls_alloc(c->tls, ip_type);
    if (pcb) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *) altcp_tls_context(pcb);
        mbedtls_ssl_set_hostname(ssl, c->host);
#if defined(MBEDTLS_DEBUG_C)
        mbedtls_ssl_conf_dbg((mbedtls_ssl_config *) ssl->conf, ota_mbedtls_dbg, nullptr);
#endif
    }
    return pcb;
}

// Parse the status code and any Location: header out of the raw header pbuf.
static err_t ota_headers_done(httpc_state_t *, void *arg, struct pbuf *hdr,
                              u16_t hdr_len, u32_t content_len) {
    OtaCtx *c = (OtaCtx *) arg;

    // Status line: "HTTP/1.x NNN ..."
    char line[16] = {};
    pbuf_copy_partial(hdr, line, sizeof(line) - 1, 0);
    const char *sp = strchr(line, ' ');
    c->srv_status = sp ? (uint32_t) atoi(sp + 1) : 0;

    // Location header (GitHub emits "Location:"; check the lowercase form too).
    u16_t loc = pbuf_memfind(hdr, "\r\nLocation: ", 12, 0);
    if (loc == 0xFFFF) loc = pbuf_memfind(hdr, "\r\nlocation: ", 12, 0);
    if (loc != 0xFFFF && loc < hdr_len) {
        const u16_t val = loc + 12;
        u16_t end = pbuf_memfind(hdr, "\r\n", 2, val);
        if (end != 0xFFFF && end > val) {
            u16_t n = end - val;
            if (n >= sizeof(c->redirect_url)) {
                printf("[OTA] redirect URL too long (%u)\n", n);
                return ERR_VAL; // abort; treated as fetch failure
            }
            pbuf_copy_partial(hdr, c->redirect_url, n, val);
            c->redirect_url[n] = '\0';
            c->redirect_seen = true;
        }
    }

    // BIN transfers: bound the body BEFORE bytes start flowing. Requires a
    // definite Content-Length (GitHub serves release assets with one); a
    // chunked 200 is rejected rather than trusted blind.
    if (c->kind == FetchKind::BIN && c->srv_status == 200) {
        if (content_len == OTA_HTTPC_CONTENT_LEN_INVALID) {
            printf("[OTA] bin response has no Content-Length; rejecting\n");
            return ERR_VAL;
        }
        if (content_len > OTA_STAGING_CAPACITY || content_len < OTA_MIN_IMAGE) {
            printf("[OTA] bin size %lu outside [%lu, %lu]\n",
                   (unsigned long) content_len, (unsigned long) OTA_MIN_IMAGE,
                   (unsigned long) OTA_STAGING_CAPACITY);
            c->oversize = true;
            return ERR_VAL;
        }
        status_total = content_len;
    }
    return ERR_OK;
}

// Body sink. Ownership contract (http_client.c): we must altcp_recved() and
// pbuf_free() every delivered pbuf.
static err_t ota_body_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t) {
    OtaCtx *c = (OtaCtx *) arg;
    if (!p) return ERR_OK; // close is signalled to httpc separately

    if (c->srv_status == 200) {
        if (c->kind == FetchKind::SMALL) {
            const u16_t space = (u16_t) (sizeof(c->small_body) - 1 - c->small_len);
            const u16_t take = p->tot_len < space ? p->tot_len : space;
            c->small_len += pbuf_copy_partial(p, c->small_body + c->small_len, take, 0);
            c->small_body[c->small_len] = '\0';
        } else if (c->kind == FetchKind::BIN && !c->oversize) {
            // Append pbuf chain into the sector accumulator; flush each full
            // sector to staging flash. The 45 ms erase inside stage_flash_sector
            // simply delays our TCP ACKs -- the tiny receive window (TCP_WND
            // ~2 KB) means the sender pauses instead of overrunning us.
            u16_t off = 0;
            while (off < p->tot_len) {
                const u16_t space = (u16_t) (FLASH_SECTOR_SIZE - c->sec_fill);
                const u16_t remain = (u16_t) (p->tot_len - off);
                const u16_t take = remain < space ? remain : space;
                pbuf_copy_partial(p, c->secbuf + c->sec_fill, take, off);
                c->sec_fill += take;
                off += take;
                if (c->sec_fill == FLASH_SECTOR_SIZE) {
                    if (c->staged + FLASH_SECTOR_SIZE > OTA_STAGING_CAPACITY) {
                        c->oversize = true; // belt: headers_done already bounds this
                        break;
                    }
                    stage_flash_sector(c->staged, c->secbuf);
                    c->staged += FLASH_SECTOR_SIZE;
                    c->sec_fill = 0;
                    status_bytes = c->staged;
                }
            }
        }
    }
    // Bodies of non-200 responses (redirect HTML stubs) are drained unread.
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void ota_result(void *arg, httpc_result_t httpc_result, u32_t /*rx_len*/,
                       u32_t srv_res, err_t err) {
    OtaCtx *c = (OtaCtx *) arg;
    c->result = httpc_result;
    if (srv_res) c->srv_status = srv_res;
    c->conn = nullptr;
    c->done = true;
    if (httpc_result != HTTPC_RESULT_OK) {
        printf("[OTA] http result=%d srv=%lu err=%d\n",
               (int) httpc_result, (unsigned long) srv_res, (int) err);
    }
}
} // extern "C"

//--------------------------------------------------------------------+
// Fetch driver
//--------------------------------------------------------------------+

static void ota_pump() {
    cyw43_arch_poll();
    watchdog_update();
    sleep_us(200);
}

// Split an absolute https URL into ctx->host/path. Rejects http:// -- a
// plaintext redirect would be a downgrade attack against the firmware path.
static bool parse_https_url(const char *url, char *host, size_t host_cap,
                            char *path, size_t path_cap) {
    static const char scheme[] = "https://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0) return false;
    const char *h = url + sizeof(scheme) - 1;
    const char *slash = strchr(h, '/');
    const size_t hlen = slash ? (size_t) (slash - h) : strlen(h);
    if (hlen == 0 || hlen >= host_cap) return false;
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    const char *p = slash ? slash : "/";
    if (strlen(p) >= path_cap) return false;
    strcpy(path, p);
    return true;
}

// One GET, following up to `max_hops` https redirects (except CHECK, which
// stops at the first response by design). Returns true when the final
// response was consumed OK (status in ctx->srv_status, Location -- if any --
// in ctx->redirect_url).
static bool ota_fetch(const char *host, const char *path, FetchKind kind,
                      uint32_t timeout_ms, int max_hops) {
    snprintf(ctx->host, sizeof(ctx->host), "%s", host);
    snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    ctx->kind = kind;

    for (int hop = 0; ; hop++) {
        ctx->done = false;
        ctx->result = HTTPC_RESULT_OK;
        ctx->srv_status = 0;
        ctx->redirect_seen = false;
        ctx->oversize = false;
        ctx->small_len = 0;
        ctx->sec_fill = 0;
        ctx->staged = 0;
        status_bytes = 0;

        memset(&ctx->settings, 0, sizeof(ctx->settings));
        ctx->settings.altcp_allocator = &ctx->alloc;
        ctx->settings.result_fn = ota_result;
        ctx->settings.headers_done_fn = ota_headers_done;

        printf("[OTA] GET https://%s%.100s%s\n", ctx->host, ctx->path,
               strlen(ctx->path) > 100 ? "..." : "");
        const err_t rc = httpc_get_file_dns(ctx->host, 443, ctx->path, &ctx->settings,
                                            ota_body_recv, ctx, &ctx->conn);
        if (rc != ERR_OK) {
            printf("[OTA] httpc start failed: %d\n", (int) rc);
            return false;
        }

        const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
        while (!ctx->done) {
            ota_pump();
            if (time_reached(deadline)) {
                printf("[OTA] fetch timed out\n");
                // Aborting under httpc is messy; the whole mode has a hard
                // reset fallback (watchdog), so just report failure and let
                // ota_finish() reboot us out from under the stale connection.
                return false;
            }
        }

        const bool is_redirect = ctx->srv_status >= 301 && ctx->srv_status <= 308 &&
                                 ctx->redirect_seen;
        if (kind == FetchKind::CHECK) {
            // First response is the answer (302 expected); never follow.
            return ctx->srv_status != 0;
        }
        if (is_redirect) {
            if (hop >= max_hops) {
                printf("[OTA] too many redirects\n");
                return false;
            }
            if (!parse_https_url(ctx->redirect_url, ctx->nhost, sizeof(ctx->nhost),
                                 ctx->npath, sizeof(ctx->npath))) {
                printf("[OTA] unparseable redirect: %.120s\n", ctx->redirect_url);
                return false;
            }
            snprintf(ctx->host, sizeof(ctx->host), "%s", ctx->nhost);
            memcpy(ctx->path, ctx->npath, strlen(ctx->npath) + 1);
            continue;
        }
        return ctx->result == HTTPC_RESULT_OK && ctx->srv_status == 200 && !ctx->oversize;
    }
}

//--------------------------------------------------------------------+
// Apply: RAM-resident copy of staging -> [0, size), then hard reset.
//--------------------------------------------------------------------+

// EVERYTHING this touches must live in RAM or ROM: the loop overwrites the
// very flash the rest of the firmware executes from. flash_range_erase/
// flash_range_program are __no_inline_not_in_flash_func in the SDK; memcpy is
// the RAM-resident replacement from src/ram_mem.c; the final reset is a raw
// watchdog TRIGGER (the SDK's watchdog_reboot lives in flash -- can't call it
// once the image is gone). IRQs are disabled by the caller and never return.
static void __attribute__((noreturn)) __no_inline_not_in_flash_func(ota_apply_and_reset)(
        uint8_t *bounce, uint32_t size) {
    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        memcpy(bounce, (const void *) (XIP_BASE + OTA_STAGING_OFFSET + off),
               FLASH_SECTOR_SIZE);
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, bounce, FLASH_SECTOR_SIZE);
    }
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (true) { tight_loop_contents(); }
}

//--------------------------------------------------------------------+
// OTA boot mode
//--------------------------------------------------------------------+

[[noreturn]] static void ota_finish(uint32_t result) {
    printf("[OTA] finished: %s\n", ota_result_str(result));
    watchdog_hw->scratch[3] = OTA_RESULT_MAGIC | result;
    watchdog_hw->scratch[2] = 0;
    watchdog_reboot(0, 0, 10);
    while (true) { tight_loop_contents(); }
}

// Parse the leading 64 hex chars of a "sha256sum" line into out[32].
static bool parse_sha256_hex(const char *s, uint8_t out[32]) {
    auto nib = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; i++) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

[[noreturn]] void ota_mode_main() {
    mode_active = true;
    const bool force = (watchdog_hw->scratch[2] & OTA_BOOT_FLAG_FORCE) != 0;
    const bool beta = (watchdog_hw->scratch[2] & OTA_BOOT_FLAG_BETA) != 0;
    watchdog_hw->scratch[2] = 0; // one attempt per request (see scratch notes)

    printf("[OTA] === OTA boot mode (current %s, repo %s, asset %s, channel %s%s) ===\n",
           PICO_PROGRAM_VERSION_STRING, OTA_REPO, OTA_ASSET_NAME,
           beta ? "beta" : "stable", force ? ", FORCED" : "");

    // Hardware watchdog: hang anywhere (TLS stall, DNS blackhole, lwIP wedge)
    // -> reset; scratch[2] is already cleared, so that reset lands in NORMAL
    // firmware. Deliberately NOT set to the max: every pump refreshes it.
    watchdog_enable(8000, true);

    const Config_body &c = get_config();
    if (!c.wifi_provisioned || c.wifi_ssid[0] == '\0') {
        ota_finish(OTA_RESULT_NO_WIFI_CREDS);
    }

    // --- STA join (mirrors wifi_net.cpp's join SUPERVISION, minus its
    // cred-wiping AP fallback: a transient join failure here must NEVER cost
    // the user their provisioning -- we just report and reboot back).
    //
    // CRITICAL (HW-observed 2026-07-11): the CYW43 commonly fails the first
    // join or two with transient FAIL/NONET, and a failed join PARKS the
    // driver's join state machine -- it never self-recovers. The join must be
    // RE-ISSUED on failure states (wifi_net.cpp retries every ~5 s for the
    // same reason). The first cut of this loop issued connect_async once and
    // passively waited 45 s: it timed out "WiFi join failed" on hardware that
    // joined fine seconds later in normal mode.
    status_state = "wifi";
    cyw43_arch_enable_sta_mode();
    const uint32_t auth = (c.wifi_psk[0] == '\0') ? CYW43_AUTH_OPEN
                                                  : CYW43_AUTH_WPA2_MIXED_PSK;
    printf("[OTA] joining SSID \"%s\"...\n", c.wifi_ssid);
    {
        int rc = cyw43_arch_wifi_connect_async(
            c.wifi_ssid, c.wifi_psk[0] ? c.wifi_psk : NULL, auth);
        if (rc) printf("[OTA] connect kickoff failed (rc=%d); will retry\n", rc);
        const absolute_time_t deadline = make_timeout_time_ms(45000);
        absolute_time_t next_retry = make_timeout_time_ms(5000);
        int prev_link = -100; // impossible value: log the first state too
        while (true) {
            ota_pump();
            const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link != prev_link) {
                printf("[OTA] wifi link %s\n",
                       (link == CYW43_LINK_UP) ? "UP"
                       : (link == CYW43_LINK_JOIN) ? "JOINING"
                       : (link == CYW43_LINK_NOIP) ? "NO-IP"
                       : (link == CYW43_LINK_FAIL) ? "FAIL"
                       : (link == CYW43_LINK_NONET) ? "NO-NET"
                       : (link == CYW43_LINK_BADAUTH) ? "BADAUTH" : "DOWN");
                prev_link = link;
            }
            if (link == CYW43_LINK_UP && netif_default &&
                !ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
                break;
            }
            // Re-issue the join on any parked failure state, ~5 s apart (not
            // while JOIN/NOIP -- that's a join in progress).
            if ((link == CYW43_LINK_DOWN || link == CYW43_LINK_FAIL ||
                 link == CYW43_LINK_NONET || link == CYW43_LINK_BADAUTH) &&
                time_reached(next_retry)) {
                printf("[OTA] retrying join...\n");
                rc = cyw43_arch_wifi_connect_async(
                    c.wifi_ssid, c.wifi_psk[0] ? c.wifi_psk : NULL, auth);
                if (rc) printf("[OTA] connect kickoff failed (rc=%d)\n", rc);
                next_retry = make_timeout_time_ms(5000);
            }
            if (time_reached(deadline)) {
                ota_finish(OTA_RESULT_WIFI_JOIN_FAILED);
            }
        }
    }
    printf("[OTA] STA up, ip=%s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    // mDNS + the status httpd, so the config page that triggered the update
    // can keep polling /api/ota/status at the same name/IP for live progress
    // (fs_open_custom in web_api.cpp serves ONLY that route while
    // ota_mode_active()).
    netif_set_hostname(netif_default, get_config().hostname);
    mdns_resp_init();
    mdns_resp_add_netif(netif_default, get_config().hostname);
    httpd_init();

    ctx = (OtaCtx *) calloc(1, sizeof(OtaCtx));
    if (!ctx) ota_finish(OTA_RESULT_OOM);
    ctx->force = force;
    ctx->alloc.alloc = ota_tls_alloc;
    ctx->alloc.arg = ctx;
#if defined(MBEDTLS_DEBUG_C)
    // mbedTLS's debug callback is registered by the altcp shim, but the global
    // verbosity threshold defaults to 0 == silent even for FATAL handshake
    // errors. 2 = errors + state changes: enough to see where a handshake
    // stalls or why it aborts, without the level-3/4 hex-dump flood.
    mbedtls_debug_set_threshold(2);
#endif
    printf("[OTA] heap before TLS config: %d bytes used\n", mallinfo().uordblks);
    ctx->tls = altcp_tls_create_config_client(
        (const u8_t *) OTA_CA_ROOTS, sizeof(OTA_CA_ROOTS)); // len INCLUDES the NUL (PEM contract)
    if (!ctx->tls) ota_finish(OTA_RESULT_TLS_INIT_FAILED);
    printf("[OTA] heap after TLS config: %d bytes used\n", mallinfo().uordblks);

    // --- 1) Latest release tag.
    // Stable channel: the /releases/latest redirect -- GitHub defines it as
    // the newest NON-prerelease, so betas are invisible to it by design.
    // Beta channel: the releases.atom feed -- its first entry is the newest
    // release INCLUDING prereleases; scan the first buffer-full for the first
    // "/releases/tag/<tag>" link. (Caveat: lwIP's httpc does not de-chunk
    // HTTP/1.1 chunked bodies; a chunk boundary could in principle split the
    // token. The token sits within the first entry near the head of the feed,
    // so in practice one contiguous window covers it.)
    status_state = "check";
    char path[128];
    if (beta) {
        snprintf(path, sizeof(path), "/%s/releases.atom", OTA_REPO);
        if (!ota_fetch("github.com", path, FetchKind::SMALL, 60000, 2)) {
            ota_finish(OTA_RESULT_CHECK_FAILED);
        }
        const char *tagseg = strstr(ctx->small_body, "/releases/tag/");
        if (!tagseg) ota_finish(OTA_RESULT_CHECK_FAILED);
        tagseg += strlen("/releases/tag/");
        size_t n = 0;
        while (n < sizeof(ctx->tag) - 1 && tagseg[n] && tagseg[n] != '"' &&
               tagseg[n] != '<' && tagseg[n] != '&') {
            n++;
        }
        memcpy(ctx->tag, tagseg, n);
        ctx->tag[n] = '\0';
        if (n == 0) ota_finish(OTA_RESULT_CHECK_FAILED);
    } else {
        snprintf(path, sizeof(path), "/%s/releases/latest", OTA_REPO);
        if (!ota_fetch("github.com", path, FetchKind::CHECK, 60000, 0) ||
            !ctx->redirect_seen) {
            ota_finish(OTA_RESULT_CHECK_FAILED);
        }
        // ...releases/tag/<tag>. No /tag/ segment means no release exists yet.
        const char *tagseg = strstr(ctx->redirect_url, "/releases/tag/");
        if (!tagseg) ota_finish(OTA_RESULT_CHECK_FAILED);
        snprintf(ctx->tag, sizeof(ctx->tag), "%s", tagseg + strlen("/releases/tag/"));
    }
    snprintf(status_tag, sizeof(status_tag), "%s", ctx->tag);
    printf("[OTA] latest %s release: %s\n", beta ? "beta-channel" : "stable", ctx->tag);
    if (!ctx->force && strcmp(ctx->tag, PICO_PROGRAM_VERSION_STRING) == 0) {
        ota_finish(OTA_RESULT_ALREADY_CURRENT);
    }

    // --- 2) Release manifest hash (<asset>.sha256, "sha256sum" format).
    status_state = "sha";
    char dlpath[192];
    snprintf(dlpath, sizeof(dlpath), "/%s/releases/download/%s/%s.sha256",
             OTA_REPO, ctx->tag, OTA_ASSET_NAME);
    if (!ota_fetch("github.com", dlpath, FetchKind::SMALL, 60000, 3) ||
        !parse_sha256_hex(ctx->small_body, ctx->expect_sha)) {
        ota_finish(OTA_RESULT_SHA_FETCH_FAILED);
    }

    // --- 3) The image itself, streamed to staging flash.
    status_state = "download";
    snprintf(dlpath, sizeof(dlpath), "/%s/releases/download/%s/%s",
             OTA_REPO, ctx->tag, OTA_ASSET_NAME);
    if (!ota_fetch("github.com", dlpath, FetchKind::BIN, 300000, 3)) {
        ota_finish(ctx->oversize ? OTA_RESULT_TOO_BIG : OTA_RESULT_DOWNLOAD_FAILED);
    }
    // Flush the final partial sector, 0xFF-padded (matches erased flash, and
    // the padding is excluded from the verify below anyway).
    const uint32_t image_size = ctx->staged + ctx->sec_fill;
    if (ctx->sec_fill) {
        memset(ctx->secbuf + ctx->sec_fill, 0xFF, FLASH_SECTOR_SIZE - ctx->sec_fill);
        stage_flash_sector(ctx->staged, ctx->secbuf);
    }
    if (image_size < OTA_MIN_IMAGE) ota_finish(OTA_RESULT_DOWNLOAD_FAILED);
    printf("[OTA] staged %lu bytes, verifying\n", (unsigned long) image_size);

    // --- 4) Verify the STAGED FLASH (not the wire stream: this also catches
    // a bad program) against the release manifest.
    status_state = "verify";
    {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        const uint8_t *base = (const uint8_t *) (XIP_BASE + OTA_STAGING_OFFSET);
        for (uint32_t off = 0; off < image_size; off += 65536) {
            const uint32_t n = (image_size - off) < 65536 ? (image_size - off) : 65536;
            mbedtls_sha256_update(&sha, base + off, n);
            watchdog_update();
        }
        uint8_t got[32];
        mbedtls_sha256_finish(&sha, got);
        mbedtls_sha256_free(&sha);
        if (memcmp(got, ctx->expect_sha, 32) != 0) {
            ota_finish(OTA_RESULT_SHA_MISMATCH);
        }
    }
    printf("[OTA] verified; applying (DO NOT POWER OFF, ~20 s)\n");
    status_state = "apply";

    // --- 5) Point of no return. Stop the watchdog (the copy outlives its max
    // 8 s period and must not be interrupted), pre-arm the post-copy reset
    // while flash code still exists (full-chip PSM reset on watchdog TRIGGER,
    // scratch[4]=0 so the bootrom takes the normal flash boot into the NEW
    // image), record success, and hand off to the RAM stub. Power loss inside
    // the stub bricks until a USB BOOTSEL reflash -- documented tradeoff of
    // the no-A/B design (ota.h).
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    watchdog_hw->scratch[3] = OTA_RESULT_MAGIC | OTA_RESULT_OK;
    watchdog_hw->scratch[4] = 0;
    hw_set_bits(&psm_hw->wdsel,
                PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    save_and_disable_interrupts();
    const uint32_t copy_size =
        (image_size + FLASH_SECTOR_SIZE - 1) & ~(uint32_t) (FLASH_SECTOR_SIZE - 1);
    ota_apply_and_reset(ctx->secbuf, copy_size);
}

#endif // ENABLE_OTA
