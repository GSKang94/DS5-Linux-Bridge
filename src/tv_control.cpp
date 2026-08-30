//
// tv_control.cpp -- TV control via HTTP requests to a local server.
//
// Instead of implementing the full ADB protocol on the Pico, we send simple
// HTTP GET requests to a lightweight Python server running on the user's
// always-on machine. That server handles the actual ADB commands.
//
// Endpoints: GET /tv/sleep, GET /tv/input, GET /tv/wake
//

#include "tv_control.h"

#ifdef ENABLE_WIFI_WOL

#include <cstdio>
#include <cstring>
#include "pico/time.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "config.h"
#include "wifi_net.h"

#define TV_SERVER_PORT 7777

// ─── State ───────────────────────────────────────────────────────────────────

typedef enum {
    TV_IDLE,
    TV_CONNECTING,
    TV_SENT_REQUEST,
    TV_DONE,
    TV_COOLDOWN,
} tv_state_t;

typedef enum {
    TV_CMD_NONE,
    TV_CMD_SLEEP,
    TV_CMD_INPUT,
    TV_CMD_WAKE,
} tv_cmd_t;

static tv_state_t state = TV_IDLE;
static struct tcp_pcb *tv_pcb = nullptr;
static uint64_t state_entered_us = 0;
static volatile tv_cmd_t pending_cmd = TV_CMD_NONE;
static tv_cmd_t active_cmd = TV_CMD_NONE;
static char response_line[32];
static uint8_t response_line_len = 0;
static int response_status = 0;

#define TV_TIMEOUT_US 5000000ULL  // 5s
#define TV_COOLDOWN_US 1000000ULL // 1s

static void enter_state(tv_state_t s) {
    state = s;
    state_entered_us = time_us_64();
}

static uint64_t in_state_us(void) {
    return time_us_64() - state_entered_us;
}

static bool tv_configured(void) {
    const Config_body &cfg = get_config();
    return cfg.tv_adb_enabled && cfg.tv_server_ip[0] != 0;
}

// USB resume is normally followed by mount. Treat that pair as one wake action
// until the request has completed, so both callbacks cannot switch input twice.
static bool wake_action_in_progress(void) {
    return pending_cmd == TV_CMD_INPUT || pending_cmd == TV_CMD_WAKE ||
           active_cmd == TV_CMD_INPUT || active_cmd == TV_CMD_WAKE;
}

static void record_response(const struct pbuf *p) {
    if (response_status || response_line_len == sizeof(response_line) - 1) return;

    for (uint16_t offset = 0; offset < p->tot_len &&
                              response_line_len < sizeof(response_line) - 1; offset++) {
        char c;
        pbuf_copy_partial(p, &c, 1, offset);
        if (c == '\r') continue;
        if (c == '\n') {
            response_line[response_line_len] = '\0';
            if (sscanf(response_line, "HTTP/%*u.%*u %d", &response_status) != 1) {
                response_status = -1;
            }
            return;
        }
        response_line[response_line_len++] = c;
    }
}

// ─── lwIP callbacks ──────────────────────────────────────────────────────────

static void tv_close(void) {
    if (tv_pcb) {
        tcp_arg(tv_pcb, nullptr);
        tcp_recv(tv_pcb, nullptr);
        tcp_err(tv_pcb, nullptr);
        tcp_close(tv_pcb);
        tv_pcb = nullptr;
    }
}

static const char *cmd_path(tv_cmd_t cmd) {
    switch (cmd) {
    case TV_CMD_SLEEP: return "/tv/sleep";
    case TV_CMD_INPUT: return "/tv/input";
    case TV_CMD_WAKE:  return "/tv/wake";
    default: return "/";
    }
}

static err_t tv_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        tv_close();
        enter_state(TV_COOLDOWN);
        return ERR_OK;
    }

    // Send HTTP GET
    char req[128];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: tv\r\n\r\n", cmd_path(active_cmd));

    if (len <= 0 || len >= (int)sizeof(req) ||
        tcp_write(tpcb, req, len, TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_output(tpcb) != ERR_OK) {
        printf("[tv] request write failed\n");
        tv_close();
        enter_state(TV_COOLDOWN);
    } else {
        enter_state(TV_SENT_REQUEST);
    }
    return ERR_OK;
}

static err_t tv_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    (void)err;
    if (!p) {
        tv_close();
        if (response_status == 200) {
            printf("[tv] command done\n");
        } else {
            printf("[tv] server response failed (%d)\n", response_status);
        }
        enter_state(TV_COOLDOWN);
        return ERR_OK;
    }
    record_response(p);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tv_err_cb(void *arg, err_t err) {
    (void)arg;
    (void)err;
    tv_pcb = nullptr;
    printf("[tv] connection error (%d)\n", err);
    enter_state(TV_COOLDOWN);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void tv_control_init(void) {
    // Just log at boot. Actual readiness is checked live in tv_control_task().
    const Config_body &cfg = get_config();
    if (cfg.tv_adb_enabled && cfg.tv_server_ip[0] != 0) {
        printf("[tv] TV control ready (server %d.%d.%d.%d:%d)\n",
               cfg.tv_server_ip[0], cfg.tv_server_ip[1],
               cfg.tv_server_ip[2], cfg.tv_server_ip[3], TV_SERVER_PORT);
    } else {
        printf("[tv] TV control not configured yet\n");
    }
}

void tv_control_task(void) {
    const Config_body &cfg = get_config();
    if (!tv_configured()) return;

    switch (state) {
    case TV_IDLE:
        if (pending_cmd != TV_CMD_NONE) {
            const Config_body &cfg = get_config();
            active_cmd = pending_cmd;
            pending_cmd = TV_CMD_NONE;
            response_line_len = 0;
            response_status = 0;
            tv_pcb = tcp_new();
            if (!tv_pcb) { printf("[tv] no TCP PCB\n"); enter_state(TV_COOLDOWN); return; }

            tcp_arg(tv_pcb, nullptr);
            tcp_recv(tv_pcb, tv_recv_cb);
            tcp_err(tv_pcb, tv_err_cb);

            ip_addr_t addr;
            IP4_ADDR(&addr, cfg.tv_server_ip[0], cfg.tv_server_ip[1],
                     cfg.tv_server_ip[2], cfg.tv_server_ip[3]);

            err_t err = tcp_connect(tv_pcb, &addr, TV_SERVER_PORT, tv_connected_cb);
            if (err != ERR_OK) {
                tcp_close(tv_pcb);
                tv_pcb = nullptr;
                enter_state(TV_COOLDOWN);
            } else {
                enter_state(TV_CONNECTING);
            }
        }
        break;

    case TV_CONNECTING:
    case TV_SENT_REQUEST:
        if (in_state_us() > TV_TIMEOUT_US) {
            printf("[tv] timeout (%s)\n", cmd_path(active_cmd));
            tv_close();
            enter_state(TV_COOLDOWN);
        }
        break;

    case TV_DONE:
        enter_state(TV_IDLE);
        break;

    case TV_COOLDOWN:
        if (in_state_us() > TV_COOLDOWN_US) {
            active_cmd = TV_CMD_NONE;
            enter_state(TV_IDLE);
        }
        break;
    }
}

void tv_on_host_suspend(void) {
    const Config_body &cfg = get_config();
    if (!tv_configured() || !cfg.tv_sleep_on_suspend) return;
    printf("[tv] host suspend -> TV sleep\n");
    // A confirmed suspend supersedes an input request that has not been sent.
    if (state == TV_IDLE) pending_cmd = TV_CMD_NONE;
    pending_cmd = TV_CMD_SLEEP;
}

void tv_on_host_wake(void) {
    const Config_body &cfg = get_config();
    if (!tv_configured() || !cfg.tv_input_on_wake) return;
    if (wake_action_in_progress()) {
        printf("[tv] host wake already queued\n");
        return;
    }

    // Wake TV instantly via direct WoL (no server round-trip).
    // wol_target_mac2 holds the TV MAC if configured.
    if (cfg.wol_target_mac2[0] | cfg.wol_target_mac2[1] | cfg.wol_target_mac2[2] |
        cfg.wol_target_mac2[3] | cfg.wol_target_mac2[4] | cfg.wol_target_mac2[5]) {
        wifi_wol_send(cfg.wol_target_mac2);
        printf("[tv] WoL sent directly to TV\n");
    }

    // Queue HDMI input switch via server (TV will be awake by the time
    // the server processes it).
    printf("[tv] host wake -> TV input\n");
    pending_cmd = TV_CMD_INPUT;
}

bool tv_test_command(const char *cmd) {
    const Config_body &cfg = get_config();
    if (!tv_configured()) return false;
    if (strcmp(cmd, "sleep") == 0) { pending_cmd = TV_CMD_SLEEP; return true; }
    if (strcmp(cmd, "input") == 0) { pending_cmd = TV_CMD_INPUT; return true; }
    if (strcmp(cmd, "wake") == 0)  { pending_cmd = TV_CMD_WAKE;  return true; }
    return false;
}

bool tv_is_connected(void) {
    const Config_body &cfg = get_config();
    return cfg.tv_adb_enabled && cfg.tv_server_ip[0] != 0;
}

#endif // ENABLE_WIFI_WOL
