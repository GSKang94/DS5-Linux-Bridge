//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

#ifdef ENABLE_WAKE_HID
// Dynamic config-descriptor variant. Switched when the DualSense connects
// or disconnects: minimal (kbd only — wake-from-S3 still works, no audio
// or gamepad ghost in OS) vs full (audio + gamepad + kbd, current
// behavior). Variant swap is a tud_disconnect()/tud_connect() bounce
// orchestrated by usb_apply_variant_swap().
void usb_set_descriptor_variant_full(void);
void usb_set_descriptor_variant_minimal(void);
bool usb_descriptor_variant_is_full(void);

#ifdef WAKE_VIA_USB_KBD
// TinyUSB HID instance index of the boot keyboard. STABLE at 1 in both
// variants: in full the gamepad is instance 0 (parsed first); in minimal a
// dummy placeholder HID holds instance 0 so the kbd stays instance 1. Kept as a
// function so callers stay decoupled from the constant. Absent in the WiFi-WOL
// build (no keyboard).
uint8_t usb_kbd_hid_instance(void);
#endif

// Request a variant swap: orchestrator notes the desired variant, then
// usb_variant_task() drives a tud_disconnect()/settle/swap/tud_connect()
// bounce on the main loop. Safe to call from any context. No-op if the
// desired variant is already active. The task internally refuses to act
// while the host is suspended — preserves wake-from-S3/S5 by avoiding
// USB re-enumeration mid-suspend.
void usb_request_variant_full(void);
void usb_request_variant_minimal(void);

// Drive variant-swap state machine. Call from main loop alongside
// wake_task() / btstack hci_run().
void usb_variant_task(void);

// True while a variant swap is in flight (between tud_disconnect() and
// the post-tud_connect() settle). wake.cpp uses this to ignore the
// tud_mount_cb / tud_resume_cb that fire as a consequence of our own
// re-enumeration — otherwise the wake FSM treats them as a host wake-up
// event and starts mashing F15 into the host (-> stray "fic" key spam).
bool usb_variant_swap_in_progress(void);

// Suspend-state plumbing. wake.cpp owns the authoritative suspended
// state; usb_variant_task queries this before starting/continuing a
// swap so we don't yank the bus during S3/S5.
void usb_set_host_suspended(bool suspended);

// True while the USB host has the bus suspended (S3/S4/S5) -- i.e. the PC the
// dongle is plugged into is asleep/off. The WiFi WOL transport reads this to
// gate the PS-button wake trigger: only fire WOL when the host is actually
// down, so a normal PS press during play (Steam menu) never sends a packet.
bool usb_host_suspended(void);
#else
// Without the wake subsystem there is no authoritative suspend tracking, so
// report "not suspended" -- the WiFi PS-button WOL trigger then stays inert
// (web-button WOL is unaffected). Keeps the WiFi build linkable with
// -DENABLE_WAKE_HID=OFF.
static inline bool usb_host_suspended(void) { return false; }
#endif

#endif //DS5_BRIDGE_USB_H