//
// web_api_8bitdo.cpp -- Stripped web API for 8BitDo standalone firmware.
//
// Only serves: /api/config (GET/POST), /api/status, /api/wol, the web page.
// No bonds management, no OTA status polling, no DS5-specific features.
//

#ifdef ENABLE_WEBUI

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "lwip/pbuf.h"

#include "hardware/watchdog.h"

#include "bt_generic.h"
#include "config.h"
#include "tv_control.h"
#include "web_page_8bitdo.h"
#include "weblog.h"
#include "wifi_net.h"

#ifndef PICO_PROGRAM_VERSION_STRING
#define PICO_PROGRAM_VERSION_STRING "dev"
#endif

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool last_save_ok = true;

static void mac_to_hex(const uint8_t *mac, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = hex[mac[i] >> 4];
        out[i * 2 + 1] = hex[mac[i] & 0x0F];
    }
    out[12] = '\0';
}

static bool hex_to_addr(const char *s, uint8_t a[6]) {
    if (!s || strlen(s) < 12) return false;
    for (int i = 0; i < 6; i++) {
        auto nib = [](char x) -> int {
            if (x >= '0' && x <= '9') return x - '0';
            if (x >= 'a' && x <= 'f') return x - 'a' + 10;
            if (x >= 'A' && x <= 'F') return x - 'A' + 10;
            return -1;
        };
        int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        a[i] = (hi << 4) | lo;
    }
    return true;
}

static void url_decode(char *s) {
    char *w = s;
    auto nib = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
    };
    for (char *r = s; *r; r++) {
        if (*r == '+') { *w++ = ' '; }
        else if (*r == '%' && r[1] && r[2]) {
            int hi = nib(r[1]), lo = nib(r[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char)((hi << 4) | lo); r += 2; }
            else *w++ = *r;
        } else { *w++ = *r; }
    }
    *w = '\0';
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ─── JSON builders ───────────────────────────────────────────────────────────

static int json_config(char *out, size_t cap) {
    const Config_body &c = get_config();
    char wol_hex[13], wol_hex2[13];
    mac_to_hex(c.wol_target_mac, wol_hex);
    mac_to_hex(c.wol_target_mac2, wol_hex2);
    return snprintf(out, cap,
        "{\"version\":\"%s\","
        "\"hostname\":\"%s\","
        "\"wol_target_mac\":\"%s\","
        "\"wol_target_mac2\":\"%s\","
        "\"tv_adb_enabled\":%u,"
        "\"tv_server_ip\":\"%u.%u.%u.%u\","
        "\"tv_sleep_on_suspend\":%u,"
        "\"tv_input_on_wake\":%u,"
        "\"tv_connected\":%s}",
        PICO_PROGRAM_VERSION_STRING,
        c.hostname,
        wol_hex, wol_hex2,
        (unsigned)c.tv_adb_enabled,
        c.tv_server_ip[0], c.tv_server_ip[1], c.tv_server_ip[2], c.tv_server_ip[3],
        (unsigned)c.tv_sleep_on_suspend,
        (unsigned)c.tv_input_on_wake,
        tv_is_connected() ? "true" : "false");
}

static int json_status(char *out, size_t cap) {
    BtStatus st;
    bt_get_status(0, &st);
    return snprintf(out, cap, "{\"connected\":%s}",
                    st.connected ? "true" : "false");
}

// ─── httpd custom file system ────────────────────────────────────────────────

static struct fs_file dynamic_file;
static char body[512];

static int make_file(struct fs_file *file, const char *status,
                     const char *content_type, const char *data, int len) {
    static char hdr[128];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nContent-Length: %d\r\n\r\n",
        status, content_type, len);
    // Point file at header + body
    static char resp[640];
    memcpy(resp, hdr, hdr_len);
    memcpy(resp + hdr_len, data, len);
    file->data = resp;
    file->len = hdr_len + len;
    file->index = 0;
    file->flags = 0;
    return 1;
}

int fs_open_custom(struct fs_file *file, const char *name) {
    if (strcmp(name, "/api/config") == 0) {
        int len = json_config(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/api/status") == 0) {
        int len = json_status(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    // Serve the web page for everything else
    file->data = WEB_PAGE;
    file->len = sizeof(WEB_PAGE) - 1;
    file->index = 0;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
    return 1;
}

void fs_close_custom(struct fs_file *file) { (void)file; }

int fs_read_custom(struct fs_file *file, char *buffer, int count) {
    int remaining = file->len - file->index;
    if (remaining <= 0) return FS_READ_EOF;
    int to_copy = remaining < count ? remaining : count;
    memcpy(buffer, file->data + file->index, to_copy);
    file->index += to_copy;
    return to_copy;
}

// ─── POST handling ───────────────────────────────────────────────────────────

#define POST_BUFSIZE 512
static char post_buf[POST_BUFSIZE];
static uint16_t post_pos = 0;
static char post_uri[32];

static void apply_config_post(char *buf) {
    Config_body c = get_config();

    for (char *tok = strtok(buf, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        const int val = atoi(eq);

        if (strcmp(tok, "tv_adb_enabled") == 0) {
            c.tv_adb_enabled = val ? 1 : 0;
        } else if (strcmp(tok, "tv_sleep_on_suspend") == 0) {
            c.tv_sleep_on_suspend = val ? 1 : 0;
        } else if (strcmp(tok, "tv_input_on_wake") == 0) {
            c.tv_input_on_wake = val ? 1 : 0;
        } else if (strcmp(tok, "tv_server_ip") == 0) {
            url_decode(eq);
            unsigned a, b, d, e;
            if (sscanf(eq, "%u.%u.%u.%u", &a, &b, &d, &e) == 4 &&
                a <= 255 && b <= 255 && d <= 255 && e <= 255) {
                c.tv_server_ip[0] = a; c.tv_server_ip[1] = b;
                c.tv_server_ip[2] = d; c.tv_server_ip[3] = e;
            }
        } else if (strcmp(tok, "wol_target_mac") == 0) {
            uint8_t mac[6];
            if (hex_to_addr(eq, mac)) memcpy(c.wol_target_mac, mac, 6);
        } else if (strcmp(tok, "wol_target_mac2") == 0) {
            uint8_t mac[6];
            if (hex_to_addr(eq, mac)) memcpy(c.wol_target_mac2, mac, 6);
        } else if (strcmp(tok, "hostname") == 0) {
            url_decode(eq);
            strncpy(c.hostname, eq, CONFIG_HOSTNAME_LEN - 1);
            c.hostname[CONFIG_HOSTNAME_LEN - 1] = '\0';
        } else if (strcmp(tok, "tv_test") == 0) {
            url_decode(eq);
            tv_test_command(eq);
        }
    }

    set_config(c);
    watchdog_update();
    last_save_ok = config_save();
}

static void apply_wol_post(char *buf) {
    if (strstr(buf, "action=wake")) {
        // Fire WoL to all targets
        const Config_body &c = get_config();
        extern bool wifi_wol_send(const uint8_t mac[6]);
        bool any = false;
        static const uint8_t zero[6] = {};
        if (memcmp(c.wol_target_mac, zero, 6) != 0)
            any |= wifi_wol_send(c.wol_target_mac);
        if (memcmp(c.wol_target_mac2, zero, 6) != 0)
            any |= wifi_wol_send(c.wol_target_mac2);
    }
}

err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                       u16_t http_request_len, int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void)connection; (void)http_request; (void)http_request_len; (void)post_auto_wnd;
    if (content_len >= POST_BUFSIZE) return ERR_VAL;
    strncpy(post_uri, uri, sizeof(post_uri) - 1);
    post_pos = 0;
    post_buf[0] = 0;
    snprintf(response_uri, response_uri_len, "/api/config");
    return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    (void)connection;
    uint16_t space = POST_BUFSIZE - 1 - post_pos;
    uint16_t take = p->tot_len < space ? p->tot_len : space;
    post_pos += pbuf_copy_partial(p, post_buf + post_pos, take, 0);
    post_buf[post_pos] = 0;
    return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    (void)connection;
    if (strncmp(post_uri, "/api/config", 11) == 0) {
        apply_config_post(post_buf);
    } else if (strncmp(post_uri, "/api/wol", 8) == 0) {
        apply_wol_post(post_buf);
    } else if (strncmp(post_uri, "/api/wifi_reset", 15) == 0) {
        wifi_reset_provisioning_apply();
    }
    snprintf(response_uri, response_uri_len, "/api/config");
}

// ─── Init ────────────────────────────────────────────────────────────────────

void web_api_init(void) {
    httpd_init();
}

// Weak WoL hooks (provided by wifi_net.cpp)
extern "C" __attribute__((weak)) bool web_api_wol_send(const uint8_t *mac) {
    (void)mac; return false;
}
extern "C" __attribute__((weak)) bool web_api_wol_send_all(void) { return false; }
extern "C" __attribute__((weak)) void web_api_resolve_mac_start(const uint8_t *ip) { (void)ip; }
extern "C" __attribute__((weak)) int web_api_resolve_mac_poll(uint8_t *mac) { (void)mac; return -1; }

#endif // ENABLE_WEBUI
