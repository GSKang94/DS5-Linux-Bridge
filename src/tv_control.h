//
// tv_control.h -- HTTP-helper TV control (ENABLE_WIFI_WOL builds).
//
// Sends requests to a local helper server, which runs the Android TV ADB
// commands. Triggered by USB suspend/resume events.
//

#ifndef DS5_BRIDGE_TV_CONTROL_H
#define DS5_BRIDGE_TV_CONTROL_H

#include <cstdint>

#ifdef ENABLE_WIFI_WOL

// Initialise the TV control subsystem. Call after config_load() and
// wifi_net_init(). No-op if tv_adb_enabled is false in config.
void tv_control_init(void);

// Main-loop poll. Drives the HTTP connection state machine (non-blocking).
void tv_control_task(void);

// --- Trigger hooks (called from other subsystems) ---

// USB host entered suspend. If tv_sleep_on_suspend is enabled, queues a helper
// request to send KEYCODE_SLEEP to the TV.
void tv_on_host_suspend(void);

// USB host resumed / controller reconnected. If tv_input_on_wake is enabled,
// queues a helper request to switch the TV to the configured HDMI input.
// WoL to the TV MAC is handled separately by the existing wake path.
void tv_on_host_wake(void);

// Send a test command from the web UI. `cmd` is one of "sleep", "wake",
// "input". Returns true if the command was queued (the ADB state machine
// will execute it asynchronously). A duplicate automated wake is coalesced.
bool tv_test_command(const char *cmd);

// Return true if TV control is enabled and the helper server has an address.
bool tv_is_connected(void);

#else
static inline void tv_control_init(void) {}
static inline void tv_control_task(void) {}
static inline void tv_on_host_suspend(void) {}
static inline void tv_on_host_wake(void) {}
static inline bool tv_test_command(const char *) { return false; }
static inline bool tv_is_connected(void) { return false; }
#endif // ENABLE_WIFI_WOL

#endif // DS5_BRIDGE_TV_CONTROL_H
