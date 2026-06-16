//
// usb_net.cpp -- CDC-NCM USB network interface + lwIP + config web UI.
//
// The dongle additionally enumerates as a USB network adapter. A small lwIP
// stack runs over it (NO_SYS, serviced from the main loop):
//   - dhserver (TinyUSB lib/networking) hands the host an address
//   - mDNS responder answers http://ds5config.local (best-effort)
//   - lwIP httpd serves the config page and a JSON API; all content is
//     generated in fs_open_custom / the POST hooks below (no static fsdata)
//
// The DHCP offer deliberately carries no gateway and no DNS server so the
// host never tries to route internet traffic or DNS lookups through us.
//
// Adapted from the GPLv3 PC-wake-dongle project (same author); the BLE
// scan/device-list config was replaced with the DS5 firmware settings.
//

#include "usb_net.h"

#ifdef ENABLE_WEBCONFIG

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tusb.h"

#include "dhserver.h"
#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "lwip/apps/mdns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "hardware/watchdog.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "config.h"
#include "web_page.h"

//--------------------------------------------------------------------+
// TinyUSB network glue (pattern from examples/device/net_lwip_webserver)
//--------------------------------------------------------------------+

// MAC the host's NIC uses; the device-side netif uses the same address with
// the last bit flipped (both locally administered, derived from the flash
// unique id in usb_net_init).
uint8_t tud_network_mac_address[6];

static struct netif netif_data;

#define INIT_IP4(a, b, c, d) {PP_HTONL(LWIP_MAKEU32(a, b, c, d))}

// 10.55.55.104/29: an obscure corner of RFC 1918 space (homes mostly use
// 192.168.x or 10.0.0.x). The dongle lives at .105; the host PC gets .106 by
// DHCP. The /29 (.104-.111) is small, so it shadows little even if a LAN
// happens to overlap. Distinct from the PC-wake-dongle (10.7.7.107) so both
// can be plugged in at once.
static const ip4_addr_t ipaddr  = INIT_IP4(10, 55, 55, 105);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 248);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t dhcp_entries[] = {
    {{0}, INIT_IP4(10, 55, 55, 106), 24 * 60 * 60},
    {{0}, INIT_IP4(10, 55, 55, 107), 24 * 60 * 60},
    {{0}, INIT_IP4(10, 55, 55, 108), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config = {
    INIT_IP4(0, 0, 0, 0),          // router: none -- link-local only
    67,                            // listen port
    INIT_IP4(0, 0, 0, 0),          // dns: none -- never hijack host lookups
    nullptr,                       // domain
    sizeof(dhcp_entries) / sizeof(dhcp_entries[0]),
    dhcp_entries,
};

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void) netif;
    // Bounded wait: spin only briefly for the NCM endpoint to drain, then drop
    // the frame. An unbounded loop here deadlocks the main loop if the host is
    // not draining -- e.g. an unsolicited mDNS/IGMP multicast sent before the
    // host has anything queued. Replies to host traffic always free up quickly;
    // a dropped multicast is harmless.
    const absolute_time_t deadline = make_timeout_time_ms(50);
    while (tud_ready()) {
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        if (time_reached(deadline)) return ERR_WOULDBLOCK;
        tud_task(); // service USB until the transmit path frees up
    }
    return ERR_USE;
}

static err_t netif_init_cb(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    netif->mtu = CFG_TUD_NET_MTU;
    // No NETIF_FLAG_IGMP: enabling it makes the mDNS responder join a multicast
    // group, whose membership-report transmit reliably faults this NCM setup
    // into a watchdog reboot loop. ds5config.local is therefore best-effort
    // only; the IP (10.7.7.107) is the documented, reliable way in.
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->linkoutput = linkoutput_fn;
    netif->output = etharp_output;
    return ERR_OK;
}

// Process the frame inline (pattern from the TinyUSB 0.20 example): the NCM
// driver delivers one datagram at a time and recv_renew re-arms delivery.
extern "C" bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (!p) return false;
        pbuf_take(p, src, size);
        if (netif_data.input(p, &netif_data) != ERR_OK) {
            pbuf_free(p);
        }
        tud_network_recv_renew();
    }
    return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *) ref;
    (void) arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

extern "C" void tud_network_init_cb(void) {
    // frames are processed inline in tud_network_recv_cb; nothing to reset
}

//--------------------------------------------------------------------+
// HTTP content: / (page), /api/config -- via fs_open_custom
//--------------------------------------------------------------------+

// Build a complete response (headers + body) into a malloc'd buffer owned by
// the fs_file (freed in fs_close_custom).
static int make_file(struct fs_file *file, const char *status, const char *content_type,
                     const char *body, int body_len) {
    const int hdr_max = 160;
    char *buf = (char *) malloc(hdr_max + body_len);
    if (!buf) return 0;
    int hdr_len = snprintf(buf, hdr_max,
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                           "Connection: close\r\nContent-Length: %d\r\n\r\n",
                           status, content_type, body_len);
    memcpy(buf + hdr_len, body, body_len);
    memset(file, 0, sizeof(*file));
    file->data = buf; // malloc'd; reclaimed in fs_close_custom via file->data
    file->len = (int) (hdr_len + body_len);
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
    return 1;
}

static int json_config(char *out, size_t cap) {
    const Config_body &c = get_config();
    return snprintf(out, cap,
                    "{\"version\":\"%s\","
                    "\"inactive_time\":%u,"
                    "\"disable_inactive_disconnect\":%u,"
                    "\"disable_pico_led\":%u,"
                    "\"polling_rate_mode\":%u,"
                    "\"audio_buffer_length\":%u,"
                    "\"controller_mode\":%u}",
                    PICO_PROGRAM_VERSION_STRING,
                    c.inactive_time,
                    c.disable_inactive_disconnect,
                    c.disable_pico_led,
                    c.polling_rate_mode,
                    c.audio_buffer_length,
                    c.controller_mode);
}

extern "C" int fs_open_custom(struct fs_file *file, const char *name) {
    if (strcmp(name, "/") == 0 || strcmp(name, "/index.html") == 0) {
        return make_file(file, "200 OK", "text/html; charset=utf-8",
                         WEB_PAGE, sizeof(WEB_PAGE) - 1);
    }
    if (strcmp(name, "/api/config") == 0) {
        static char body[512];
        const int len = json_config(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/404.html") == 0) {
        static const char nf[] = "not found";
        return make_file(file, "404 Not Found", "text/plain", nf, sizeof(nf) - 1);
    }
    return 0;
}

extern "C" void fs_close_custom(struct fs_file *file) {
    if (file && file->data) {
        free(const_cast<char *>(file->data));
        file->data = NULL;
    }
}

extern "C" int fs_read_custom(struct fs_file *file, char *buffer, int count) {
    (void) file;
    (void) buffer;
    (void) count;
    return FS_READ_EOF; // all content is provided up front in fs_open_custom
}

//--------------------------------------------------------------------+
// POST /api/config -- form fields:
//   speaker_volume, inactive_time, disable_inactive_disconnect,
//   disable_pico_led, polling_rate_mode, audio_buffer_length, controller_mode
// Each value clamped to the same ranges config_valid() enforces, so a bad
// POST can never persist an out-of-range setting.
//--------------------------------------------------------------------+

#define POST_BUFSIZE 512
static char post_buf[POST_BUFSIZE];
static u16_t post_pos;
static void *post_conn;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void apply_post(char *body) {
    Config_body c = get_config(); // start from current, overwrite parsed fields

    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        const int val = atoi(eq);
        if (strcmp(tok, "inactive_time") == 0) {
            c.inactive_time = (uint8_t) clampi(val, 5, 60);
        } else if (strcmp(tok, "disable_inactive_disconnect") == 0) {
            c.disable_inactive_disconnect = val ? 1 : 0;
        } else if (strcmp(tok, "disable_pico_led") == 0) {
            c.disable_pico_led = val ? 1 : 0;
        } else if (strcmp(tok, "polling_rate_mode") == 0) {
            c.polling_rate_mode = (uint8_t) clampi(val, 0, 2);
        } else if (strcmp(tok, "audio_buffer_length") == 0) {
            c.audio_buffer_length = (uint8_t) clampi(val, 16, 128);
        } else if (strcmp(tok, "controller_mode") == 0) {
            c.controller_mode = (uint8_t) clampi(val, 0, 2);
        }
    }

    set_config(c); // validates + stores in RAM
    // The sector erase blocks with interrupts off; feed the watchdog first.
    watchdog_update();
    config_save();
    printf("[NET] config saved via web UI\n");
}

extern "C" err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                                  u16_t http_request_len, int content_len, char *response_uri,
                                  u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void) http_request;
    (void) http_request_len;
    (void) response_uri;
    (void) response_uri_len;
    (void) post_auto_wnd;
    if (strcmp(uri, "/api/config") != 0 || content_len >= POST_BUFSIZE) return ERR_VAL;
    if (post_conn) return ERR_USE; // one POST at a time
    post_conn = connection;
    post_pos = 0;
    return ERR_OK;
}

extern "C" err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    if (connection == post_conn && p) {
        const u16_t space = POST_BUFSIZE - 1 - post_pos;
        const u16_t take = p->tot_len < space ? p->tot_len : space;
        post_pos += pbuf_copy_partial(p, post_buf + post_pos, take, 0);
        post_buf[post_pos] = 0;
    }
    if (p) pbuf_free(p);
    return ERR_OK;
}

extern "C" void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    if (connection != post_conn) return;
    post_conn = nullptr;
    apply_post(post_buf);
    snprintf(response_uri, response_uri_len, "/api/config");
}

//--------------------------------------------------------------------+
// Init / service
//--------------------------------------------------------------------+

void usb_net_init() {
    // Stable locally-administered MAC derived from the flash unique id.
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    tud_network_mac_address[0] = 0x02;
    memcpy(tud_network_mac_address + 1, board_id.id + 3, 5);

    lwip_init();

    netif_data.hwaddr_len = 6;
    memcpy(netif_data.hwaddr, tud_network_mac_address, 6);
    netif_data.hwaddr[5] ^= 0x01; // device side must differ from host side

    netif_add(&netif_data, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&netif_data, "ds5config");
#endif
    netif_set_default(&netif_data);
    netif_set_up(&netif_data);

    if (dhserv_init(&dhcp_config) != ERR_OK) {
        printf("[NET] dhcp server init failed\n");
    }
    mdns_resp_init();
    // Best-effort: without NETIF_FLAG_IGMP this can't join the multicast group,
    // so ds5config.local generally won't resolve -- but it also can't crash.
    mdns_resp_add_netif(&netif_data, "ds5config");
    httpd_init();

    printf("[NET] config UI at http://10.55.55.105/ (ds5config.local best-effort)\n");
}

void usb_net_task() {
    sys_check_timeouts();
}

#endif // ENABLE_WEBCONFIG
