//
// usb_descriptors_8bitdo.cpp -- USB descriptors for 8BitDo standalone firmware.
//
// Single HID gamepad interface with 8BitDo Pro 2 D-input report descriptor.
// VID/PID: 2DC8/6006 (real 8BitDo identity).
//

#include "tusb.h"
#include <cstring>

// ─── 8BitDo Pro 2 HID Report Descriptor (150 bytes) ─────────────────────────

const uint8_t desc_hid_report_8bitdo[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03, 0x05, 0x01,
    0x15, 0x00, 0x25, 0x07, 0x46, 0x3b, 0x01, 0x95, 0x01, 0x75,
    0x04, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42, 0x75, 0x01, 0x95,
    0x04, 0x81, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x95, 0x04, 0x75, 0x08,
    0x81, 0x02, 0x05, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09,
    0xc4, 0x09, 0xc5, 0x95, 0x02, 0x75, 0x08, 0x81, 0x02, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x10, 0x81, 0x02, 0x06, 0x00, 0xff, 0x85, 0x04,
    0x09, 0x23, 0x75, 0x08, 0x95, 0x1f, 0x81, 0x03, 0x06, 0x00,
    0xff, 0x85, 0x06, 0x09, 0x23, 0x95, 0x3f, 0xb1, 0x02, 0x05,
    0x06, 0x09, 0x20, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95,
    0x01, 0x81, 0x02, 0x05, 0x0f, 0x09, 0x70, 0x85, 0x05, 0x15,
    0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02, 0xc0,
    0x09, 0x02, 0x07, 0x35, 0x08, 0x35, 0x06, 0x09, 0x04, 0x00
};
const size_t desc_hid_report_8bitdo_len = sizeof(desc_hid_report_8bitdo);

// ─── Device Descriptor ───────────────────────────────────────────────────────

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2DC8,  // 8BitDo
    .idProduct          = 0x6006,  // Pro 2
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// ─── Configuration Descriptor ────────────────────────────────────────────────

#define CONFIG_TOTAL_LEN (9 + 9 + 9 + 7 + 7)

static uint8_t const desc_configuration[] = {
    // Config descriptor
    0x09, 0x02,
    CONFIG_TOTAL_LEN & 0xFF, (CONFIG_TOTAL_LEN >> 8) & 0xFF,
    0x01,       // bNumInterfaces
    0x01,       // bConfigurationValue
    0x00,       // iConfiguration
    0xA0,       // bmAttributes: bus powered, remote wakeup
    0xFA,       // bMaxPower: 500mA

    // Interface 0: HID Gamepad
    0x09, 0x04,
    0x00,       // bInterfaceNumber
    0x00,       // bAlternateSetting
    0x02,       // bNumEndpoints
    0x03,       // bInterfaceClass: HID
    0x00,       // bInterfaceSubClass
    0x00,       // bInterfaceProtocol
    0x00,       // iInterface

    // HID descriptor
    0x09, 0x21,
    0x11, 0x01, // bcdHID: 1.11
    0x00,       // bCountryCode
    0x01,       // bNumDescriptors
    0x22,       // bDescriptorType: Report
    sizeof(desc_hid_report_8bitdo) & 0xFF,
    (sizeof(desc_hid_report_8bitdo) >> 8) & 0xFF,

    // Endpoint IN: interrupt, 64 bytes
    0x07, 0x05,
    0x81,       // bEndpointAddress: EP1 IN
    0x03,       // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize: 64
    0x01,       // bInterval: 1ms

    // Endpoint OUT: interrupt, 64 bytes (for rumble)
    0x07, 0x05,
    0x01,       // bEndpointAddress: EP1 OUT
    0x03,       // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize: 64
    0x01,       // bInterval: 1ms
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ─── String Descriptors ──────────────────────────────────────────────────────

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: Language (English)
    "8BitDo",                    // 1: Manufacturer
    "Pro 2",                     // 2: Product
    "8BB001",                    // 3: Serial
};

static uint16_t _desc_str[32];

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
        chr_count = strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}
