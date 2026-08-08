/*
 * usb_descriptors.c -- USB descriptors for 8BitDo Pro 2 bridge build.
 *
 * Simple single-HID-interface device that mirrors the 8BitDo Pro 2's
 * D-input mode HID descriptor (150 bytes). VID 0x2DC8, PID 0x6006.
 */

#include "tusb.h"

//--------------------------------------------------------------------+
// HID Report Descriptor (150 bytes, captured from 8BitDo Pro 2 D-input)
//--------------------------------------------------------------------+
static const uint8_t desc_hid_report[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
    0x05, 0x01, 0x15, 0x00, 0x25, 0x07, 0x46, 0x3b,
    0x01, 0x95, 0x01, 0x75, 0x04, 0x65, 0x14, 0x09,
    0x39, 0x81, 0x42, 0x75, 0x01, 0x95, 0x04, 0x81,
    0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x95, 0x04,
    0x75, 0x08, 0x81, 0x02, 0x05, 0x02, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x09, 0xc4, 0x09, 0xc5, 0x95,
    0x02, 0x75, 0x08, 0x81, 0x02, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x10, 0x81, 0x02, 0x06, 0x00, 0xff,
    0x85, 0x04, 0x09, 0x23, 0x75, 0x08, 0x95, 0x1f,
    0x81, 0x03, 0x06, 0x00, 0xff, 0x85, 0x06, 0x09,
    0x23, 0x95, 0x3f, 0xb1, 0x02, 0x05, 0x06, 0x09,
    0x20, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95,
    0x01, 0x81, 0x02, 0x05, 0x0f, 0x09, 0x70, 0x85,
    0x05, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95,
    0x04, 0x91, 0x02, 0xc0, 0x09, 0x02, 0x07, 0x35,
    0x08, 0x35, 0x06, 0x09, 0x04, 0x00
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2DC8,
    .idProduct          = 0x6006,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// HID Report Descriptor callback
//--------------------------------------------------------------------+
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define EPNUM_HID 0x81

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t desc_configuration[] = {
    // Config descriptor
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    // HID Interface: IN EP 0x81, poll 1ms, report desc 150 bytes
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 1)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English
    "8BitDo",                    // 1: Manufacturer
    "Pro 2",                     // 2: Product
    "000000000001"               // 3: Serial
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;

        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // First byte is length (including header), second byte is descriptor type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
