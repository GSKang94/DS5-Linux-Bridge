//
// tv_control.h -- ADB-over-WiFi TV control (ENABLE_WIFI_WOL builds).
//
// Sends shell commands to an Android TV via the ADB TCP protocol (port 5555)
// to sleep/wake the TV and switch HDMI inputs. Triggered by USB suspend/resume
// events and controller connect/disconnect. Authentication uses an RSA-2048
// keypair stored in flash config (uploaded via the web UI from the user's
// ~/.android/adbkey).
//

#ifndef DS5_BRIDGE_TV_CONTROL_H
#define DS5_BRIDGE_TV_CONTROL_H

#include <cstdint>

#ifdef ENABLE_WIFI_WOL

// Initialise the TV control subsystem. Call after config_load() and
// wifi_net_init(). No-op if tv_adb_enabled is false in config.
void tv_control_init(void);

// Main-loop poll. Drives the ADB connection state machine (non-blocking).
void tv_control_task(void);

// --- Trigger hooks (called from other subsystems) ---

// USB host entered suspend. If tv_sleep_on_suspend is enabled, queues an ADB
// "input keyevent 223" (KEYCODE_SLEEP) to put the TV to sleep.
void tv_on_host_suspend(void);

// USB host resumed / controller reconnected. If tv_input_on_wake is enabled,
// queues an ADB "am start ..." to switch the TV to the configured HDMI input.
// WoL to the TV MAC is handled separately by the existing wake path.
void tv_on_host_wake(void);

// Send a test command from the web UI. `cmd` is one of "sleep", "wake",
// "input". Returns true if the command was queued (the ADB state machine
// will execute it asynchronously).
bool tv_test_command(const char *cmd);

// Return true if the ADB connection is currently established and authenticated.
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
