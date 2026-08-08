//
// tusb_config_8bitdo.h -- TinyUSB config for 8BitDo standalone firmware.
// Single HID gamepad, no audio, no CDC.
//

#ifndef TUSB_CONFIG_8BITDO_H
#define TUSB_CONFIG_8BITDO_H

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE  64

// Only HID
#define CFG_TUD_HID     1
#define CFG_TUD_AUDIO   0
#define CFG_TUD_CDC     0
#define CFG_TUD_MSC     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VENDOR  0

// HID buffer sizes
#define CFG_TUD_HID_EP_BUFSIZE  64

#endif
