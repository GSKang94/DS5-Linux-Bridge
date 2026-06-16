//
// usb_net.h -- onboard config web UI over a USB CDC-NCM network interface.
//
// Enabled only in non-serial builds (ENABLE_WEBCONFIG), where the CDC-serial
// endpoints are free for the NCM bulk/notification endpoints.
//

#ifndef DS5_BRIDGE_USB_NET_H
#define DS5_BRIDGE_USB_NET_H

#ifdef ENABLE_WEBCONFIG
void usb_net_init();   // call once after tusb_init()
void usb_net_task();   // call every main-loop iteration (services lwIP timers)
#else
static inline void usb_net_init() {}
static inline void usb_net_task() {}
#endif

#endif // DS5_BRIDGE_USB_NET_H
