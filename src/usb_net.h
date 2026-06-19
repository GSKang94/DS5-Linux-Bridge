//
// usb_net.h -- onboard config web UI over a USB CDC-NCM network interface.
//
// Enabled only in non-serial builds (ENABLE_WEBCONFIG), where the CDC-serial
// endpoints are free for the NCM bulk/notification endpoints.
//

#ifndef DS5_BRIDGE_USB_NET_H
#define DS5_BRIDGE_USB_NET_H

// Config-page address selector (Config_body.webconfig_subnet). Values 0..N-1
// index the vetted /29 preset table in usb_net.cpp; the extra value
// WEBCONFIG_SUBNET_CUSTOM means "use Config_body.webconfig_custom_ip instead".
// Shared here so config_valid() clamps to the same bound without duplicating a
// magic number. usb_net.cpp static_asserts its preset-table length against
// WEBCONFIG_SUBNET_COUNT; the web page <select> must list exactly these options.
#define WEBCONFIG_SUBNET_COUNT  3            // number of fixed presets (0..2)
#define WEBCONFIG_SUBNET_CUSTOM WEBCONFIG_SUBNET_COUNT // 3 == custom IP
// Highest valid selector value (presets + the custom sentinel).
#define WEBCONFIG_SUBNET_MAX    WEBCONFIG_SUBNET_CUSTOM

#ifdef ENABLE_WEBCONFIG
#include <cstdint>
void usb_net_init();   // call once after tusb_init()
void usb_net_task();   // call every main-loop iteration (services lwIP timers)
// True if ip[4] is a usable private (RFC-1918) host address for the config page
// (see usb_net.cpp). config_valid() uses this to gate the custom-IP option.
bool webconfig_ip_is_valid(const uint8_t ip[4]);
#else
static inline void usb_net_init() {}
static inline void usb_net_task() {}
static inline bool webconfig_ip_is_valid(const uint8_t *) { return false; }
#endif

#endif // DS5_BRIDGE_USB_NET_H
