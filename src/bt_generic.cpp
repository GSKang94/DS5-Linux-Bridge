//
// bt_generic.cpp -- Generic BT HID host for 8BitDo Pro 2 (D-input mode).
//
// Stripped version of bt.cpp: no DS5 feature probing, no audio, no haptics,
// no DSE detection. Just pairs with a BT HID gamepad and forwards reports.
//

#include "bt_generic.h"

#include <cstdio>
#include <cstring>
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "btstack.h"
#include "btstack_run_loop.h"
#include "classic/sdp_server.h"

#include "config.h"
#include "slots.h"

// ─── Slot state ──────────────────────────────────────────────────────────────

struct bt_slot {
    hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
    bd_addr_t addr{};
    uint16_t control_cid = 0;
    uint16_t interrupt_cid = 0;
    bool new_pair = false;
    absolute_time_t inactive_time = 0;
    // Outbound send queue
    queue_t send_fifo{};
    volatile bool send_chain_active = false;
};

static bt_slot slots[1]; // single controller
static bt_data_callback_t bt_data_callback = nullptr;

// ─── Forward declarations ────────────────────────────────────────────────────

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size);
static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);

// ─── Connection state ────────────────────────────────────────────────────────

static bool scanning = false;
static bool connected = false;
static absolute_time_t connect_attempt_started = 0;
#define CONNECT_WATCHDOG_TIMEOUT_US (15 * 1000 * 1000)

// ─── Send infrastructure ─────────────────────────────────────────────────────

struct send_element {
    uint8_t data[128];
    uint16_t len;
};

static void slot_clear(bt_slot *s) {
    s->acl_handle = HCI_CON_HANDLE_INVALID;
    s->control_cid = 0;
    s->interrupt_cid = 0;
    s->new_pair = false;
    while (queue_try_remove(&s->send_fifo, NULL)) {}
    s->send_chain_active = false;
}

// ─── Inquiry & pairing ───────────────────────────────────────────────────────

static void bt_start_inquiry(void) {
    if (scanning) return;
    printf("[BT] Starting inquiry...\n");
    gap_inquiry_start(30); // 30 * 1.28s = ~38s scan window
    scanning = true;
}

static void bt_stop_inquiry(void) {
    if (!scanning) return;
    gap_inquiry_stop();
    scanning = false;
}

// ─── HCI packet handler ─────────────────────────────────────────────────────

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    bd_addr_t addr;

    switch (event) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            printf("[BT] Stack ready\n");
            if (!connected) bt_start_inquiry();
        }
        break;

    case GAP_EVENT_INQUIRY_RESULT: {
        uint32_t cod;
        switch (hci_event_inquiry_result_get_page_scan_repetition_mode(packet)) {
        default:
            cod = hci_event_inquiry_result_get_class_of_device(packet);
            break;
        }
        // Accept any Peripheral class device (gamepads, etc.)
        if ((cod & 0x000F00) == 0x000500) {
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            printf("[BT] Gamepad found: %s (CoD: 0x%06x)\n",
                   bd_addr_to_str(addr), (unsigned)cod);
            bt_stop_inquiry();
            // Connect
            connect_attempt_started = get_absolute_time();
            gap_connect(addr, BD_ADDR_TYPE_LE_PUBLIC);
            l2cap_create_channel(l2cap_packet_handler, addr,
                                 BLUETOOTH_PSM_HID_CONTROL, 672, NULL);
        }
        break;
    }

    case GAP_EVENT_INQUIRY_COMPLETE:
        scanning = false;
        if (!connected) {
            printf("[BT] Inquiry complete, no device found. Retrying...\n");
            bt_start_inquiry();
        }
        break;

    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, addr);
        printf("[BT] Incoming connection from %s\n", bd_addr_to_str(addr));
        gap_accept_connection_request(addr, 1);
        connect_attempt_started = get_absolute_time();
        break;

    case HCI_EVENT_CONNECTION_COMPLETE: {
        uint8_t status = hci_event_connection_complete_get_status(packet);
        if (status != 0) {
            printf("[BT] Connection failed: 0x%02x\n", status);
            connect_attempt_started = 0;
            if (!connected) bt_start_inquiry();
            break;
        }
        hci_event_connection_complete_get_bd_addr(packet, addr);
        hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
        printf("[BT] ACL connected: %s handle=0x%04x\n", bd_addr_to_str(addr), handle);
        slots[0].acl_handle = handle;
        memcpy(slots[0].addr, addr, 6);
        // Open HID channels
        l2cap_create_channel(l2cap_packet_handler, addr,
                             BLUETOOTH_PSM_HID_CONTROL, 672, NULL);
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        printf("[BT] Disconnected\n");
        connected = false;
        slot_clear(&slots[0]);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        connect_attempt_started = 0;
        bt_start_inquiry();
        break;
    }

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        gap_pin_code_response(addr, "0000");
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        gap_ssp_confirmation_response(addr);
        break;

    default:
        break;
    }
}

// ─── L2CAP packet handler ────────────────────────────────────────────────────

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    if (packet_type == HCI_EVENT_PACKET) {
        uint8_t event = hci_event_packet_get_type(packet);
        if (event == L2CAP_EVENT_CHANNEL_OPENED) {
            uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);
            uint8_t status = l2cap_event_channel_opened_get_status(packet);

            if (status != 0) {
                printf("[L2CAP] Channel open failed psm=0x%04x status=0x%02x\n", psm, status);
                return;
            }

            if (psm == BLUETOOTH_PSM_HID_CONTROL) {
                printf("[L2CAP] HID Control opened (cid=0x%04x)\n", cid);
                slots[0].control_cid = cid;
                // Now open interrupt channel
                bd_addr_t addr;
                l2cap_event_channel_opened_get_address(packet, addr);
                l2cap_create_channel(l2cap_packet_handler, addr,
                                     BLUETOOTH_PSM_HID_INTERRUPT, 672, NULL);
            } else if (psm == BLUETOOTH_PSM_HID_INTERRUPT) {
                printf("[L2CAP] HID Interrupt opened (cid=0x%04x)\n", cid);
                slots[0].interrupt_cid = cid;
                connected = true;
                connect_attempt_started = 0;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
                printf("[BT] 8BitDo connected and ready\n");

                // Trigger wake if host is suspended
                extern void wake_on_bt_connect(void);
                wake_on_bt_connect();
            }
        } else if (event == L2CAP_EVENT_CHANNEL_CLOSED) {
            uint16_t cid = l2cap_event_channel_closed_get_local_cid(packet);
            if (cid == slots[0].control_cid) slots[0].control_cid = 0;
            if (cid == slots[0].interrupt_cid) slots[0].interrupt_cid = 0;
            if (slots[0].control_cid == 0 && slots[0].interrupt_cid == 0) {
                connected = false;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
                bt_start_inquiry();
            }
        }
        return;
    }

    if (packet_type != L2CAP_DATA_PACKET) return;

    // Forward HID interrupt data to the callback
    if (channel == slots[0].interrupt_cid && bt_data_callback) {
        bt_data_callback(0, INTERRUPT, packet, size);
    } else if (channel == slots[0].control_cid && bt_data_callback) {
        bt_data_callback(0, CONTROL, packet, size);
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

int bt_init(void) {
    queue_init(&slots[0].send_fifo, sizeof(send_element), 8);

    // Register HID host service (allows incoming connections from bonded devices)
    l2cap_register_service(l2cap_packet_handler, BLUETOOTH_PSM_HID_CONTROL, 672,
                           LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, BLUETOOTH_PSM_HID_INTERRUPT, 672,
                           LEVEL_2);

    // SSP: just-works (no display)
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE |
                                         LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_bondable_mode(1);
    gap_set_local_name("8BitDo Bridge");
    gap_discoverable_control(0);
    gap_connectable_control(1);
    gap_set_page_scan_activity(0x0050, 0x0030);

    // Class of Device: set as peripheral so controllers reconnect to us
    gap_set_class_of_device(0x002508); // Gamepad

    sdp_init();

    hci_event_callback_registration_t hci_cb;
    hci_cb.callback = hci_packet_handler;
    hci_add_event_handler(&hci_cb);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

void bt_register_data_callback(bt_data_callback_t callback) {
    bt_data_callback = callback;
}

void bt_write(uint8_t slot, const uint8_t *data, uint16_t len) {
    (void)slot;
    if (!connected || slots[0].interrupt_cid == 0) return;
    l2cap_send(slots[0].interrupt_cid, (uint8_t *)data, len);
}

bool bt_is_connected(void) {
    return connected;
}

void bt_connection_watchdog_tick(void) {
    if (connect_attempt_started == 0) return;
    if (absolute_time_diff_us(connect_attempt_started, get_absolute_time())
        >= CONNECT_WATCHDOG_TIMEOUT_US) {
        printf("[BT] Connection watchdog fired\n");
        connect_attempt_started = 0;
        if (!connected) bt_start_inquiry();
    }
}

void bt_pump(void) {
    // No complex send chain needed for generic HID
}

bool bt_send_pending(void) {
    return false;
}

int bt_connected_count(void) {
    return connected ? 1 : 0;
}

void bt_dualsense_power_off(void) {
    // Not applicable for 8BitDo - it auto-sleeps on its own
}

void bt_get_status(uint8_t slot, BtStatus *out) {
    (void)slot;
    memset(out, 0, sizeof(*out));
    out->connected = connected;
    if (connected) memcpy(out->addr, slots[0].addr, 6);
}
