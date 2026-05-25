# DualSense USB-to-Bluetooth Adapter (Pico2W)

An advanced wireless adapter firmware for the Raspberry Pi Pico 2 W that turns it into a latency-optimized Bluetooth bridge for the Sony DualSense (DS5) controller, duplicating the wired controller experience (including speaker, mic, and native HD haptic feedback) over Bluetooth.

This project is a heavily optimized fork based on [DS5Dongle by awalol](https://github.com/awalolcn/DS5Dongle), licensed under the MIT License.

## Core Features

- 🎮 **Full Wireless Controller Emulation:** Converts DualSense Bluetooth reports into standard USB HID gamepad inputs. Supports both standard DualSense (DS5) and DualSense Edge (DSE) profiling.
- 🔊 **Wireless Audio Stream (Opus):** Supports high-quality speaker and headphone audio playback directly through the controller's audio jack and speaker.
- 🎙️ **Wireless Microphone Upload:** Decodes and streams the controller's microphone audio back to the host system via standard USB Audio Class interfaces.
- 📳 **Native HD Haptics Restoration:** Captures the 48kHz audio waveforms played by PC games to USB channels 3 & 4 (e.g., *Hogwarts Legacy*, *Returnal*, *Spider-Man*, *Deathloop*) and downsamples them using a lightweight 16:1 boxcar decimation filter to wirelessly drive the controller's voice-coil actuators. Includes a 33.3% gain boost to match wired rumble intensity.
- 🔇 **Hybrid Hardware Microphone Mute:** 
  - A driverless local hardware mute toggle using the controller's physical Mute button.
  - Automatically synchronizes with the host OS sound control panel's mute state.
  - Dynamically yields control to active host-level drivers (like Linux's `hid-playstation`) to avoid state conflicts.
- 🔌 **Dynamic USB Descriptors (Wake-on-PS / S3 Sleep):**
  - Swaps USB configurations dynamically to hide audio/gamepad interfaces when the controller is disconnected, preventing "ghost" devices in the OS.
  - Automatically registers a boot keyboard interface during host standby, enabling the controller's PS button to wake the host PC from S3 sleep (sends F15 keystroke).
- 📡 **USB 3.0 RF Noise Watchdog:** Auto-retries Bluetooth connections when stalled due to 2.4GHz RF interference from USB 3.0 ports instead of hanging on an amber lightbar.
- ⚡ **Low-Latency & Performance Optimizations:**
  - CPU overclocked to 360 MHz @ 1.30V.
  - Core 0 main loop throttled to 8kHz (125us sleep) to align with Bluetooth polling intervals and minimize bus contention.
  - High-performance memory copies and direct-to-queue Opus audio encoding/decoding pipeline.
- 🚨 **Visual Notifications:**
  - Power-On Self Test (POST) LED pattern indicating successful boot.
  - Low-battery alert (1Hz onboard LED blink when controller battery drops to <= 10%).

---

## Getting Started

### Installation / Flashing
1. Hold the **BOOTSEL** button on your Raspberry Pi Pico 2 W.
2. Connect it to your PC via a USB cable.
3. Drag and drop the compiled `.uf2` firmware file onto the mounted `RP2350` USB storage volume.

### Pairing the Controller
1. Place your DualSense controller into Bluetooth pairing mode (hold the Share + PS buttons until the lightbar double-blinks).
2. The Pico 2 W will detect, pair, and connect to the controller. The onboard LED will turn solid to indicate a successful connection.
3. Once paired, the adapter will dynamically enumerate the controller interfaces to the host PC.

---

## Configuration

You can customize the adapter's options (such as inactive timeout, LED preferences, and buffer sizing) using the web configuration tool:
- **Official Release:** [ds5.awalol.eu.org](https://ds5.awalol.eu.org)
- **Development Version:** [ds5-dev.awalol.eu.org](https://ds5-dev.awalol.eu.org)

---

## Operating System & Driver Behavior

### Windows 10/11
*   **Audio & Mute Sync:** Runs driverless. Muting the mic in the Windows Sound control panel syncs to the controller's physical orange LED.
*   **OS Sync Caveat:** Since the adapter is driverless and doesn't run a custom client or support a UAC status interrupt endpoint, pressing the physical mute button on the controller cannot force the Windows OS-level sound panel mixer to toggle. The controller mutes the stream locally in the firmware and turns on its LED. To resolve any visual desyncs, ensure the Windows Sound panel mixer is unmuted when using the physical controller button.

### Linux / SteamOS (Bazzite)
*   **Native Driver Integration:** Fully compatible with the official kernel `hid-playstation` driver. When the Linux driver is active, the firmware automatically yields LED and button control to the OS driver to avoid conflicts.
*   **Jack Detection:** Verbatim `HP_DETECT` and `MIC_DETECT` events are forwarded to the host, supporting automatic profile switching in `alsa-ucm-conf`, PulseAudio, and PipeWire.

---

## Build Instructions

To compile the project from source:
1. Ensure the Pico SDK is configured and its `tinyusb` library is updated to the latest release.
2. Compile using the standard Pico CMake configuration:
   ```bash
   mkdir build
   cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   make
   ```
3. To disable the low-battery warning LED blink at build time, configure with `-DENABLE_BATT_LED=OFF`.

---

## License & Acknowledgement

This project is licensed under the **MIT License**.

This firmware is a fork and continuation of the original [DS5Dongle](https://github.com/awalolcn/DS5Dongle) project created by **awalol**. We acknowledge and credit the original work under the terms of the MIT license.

For a full copy of the license, see the [LICENSE](file:///c:/Users/mkung/Documents/GitHub/DS5Dongle/LICENSE) file.
