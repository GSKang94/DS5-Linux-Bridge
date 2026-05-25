# DualSense USB-to-Bluetooth Adapter (Pico2W)

An advanced wireless adapter firmware for the Raspberry Pi Pico 2 W that turns it into a latency-optimized Bluetooth bridge for the Sony DualSense (DS5) controller, duplicating the wired controller experience (including speaker, mic, and native HD haptic feedback) over Bluetooth.

This project is a heavily optimized fork based on [DS5Dongle by awalol](https://github.com/awalol/DS5Dongle), licensed under the MIT License.

## Core Features

- 🎮 **Full Wireless Controller Emulation:** Converts DualSense Bluetooth reports into standard USB HID gamepad inputs at 1000Hz. Supports both standard DualSense (DS5) and DualSense Edge (DSE) profiling.
- 📳 **Wireless HD Haptics:** Natively recreates the cabled advanced audio-based haptic feedback (HD haptics) over Bluetooth. It captures the dedicated haptic waveforms sent by games with native Dualsense support to the controller's haptic channels, streaming them wirelessly to the voice-coil actuators at matching intensity. Note that the compatibility of HD haptics is similar to wired experience: for example, Death Stranding Director's Cut supports HD Haptics on Windows OOTB, it does not work on Linux (the same happens for wired Dualsense).
- 🔊 **Wireless Audio Stream:** Supports high-quality speaker and headphone audio playback directly through the controller's audio jack and speaker.
- 🎙️ **Wireless Microphone Upload:** Decodes and streams the controller's microphone audio back to the host system via standard USB Audio Class interfaces. Full quality microphone, no headset profile (HSP) downgrade typical for Bluetooth headsets.
- 🔇 **Hybrid Hardware Microphone Mute:** 
  - A driverless local hardware mute toggle using the controller's physical Mute button.
  - Automatically synchronizes with the host OS sound control panel's mute state.
  - Dynamically yields control to active host-level drivers (like Linux's `hid-playstation`) to avoid state conflicts.
- 🔌 **Wake from S3 Sleep and Dynamic USB Descriptors:**
  - Swaps USB configurations dynamically to hide audio/gamepad interfaces when the controller is disconnected, preventing "ghost" devices in the OS.
  - After turning on controller you can wake the host PC from S3 sleep by pressing any button.
  - Wake from S5 is available on compatible(!) motherboards. Before purchasing this adapter, check if your motherboard can be woken up from S5 by keyboard (not mouse).
  - Automatically powers off the DualSense controller after 10 seconds of inactivity when the host PC enters sleep mode or is turned off.
- 📡 **USB 3.0 RF Noise Watchdog:** Auto-retries Bluetooth connections when stalled due to 2.4GHz RF interference from USB 3.0 ports. Generally, USB 2.0 ports on the motherboard are recommended.
- 🚨 **Visual Notifications:**
  - Power-On Self Test (POST) LED pattern (triple fast blinks) to indicate successful boot.
  - Solid LED light shows an established connection between dongle and Dualsense.
  - Low-battery alert (once per second onboard LED blink when controller battery drops to <= 10%).
- ⚡ **Low-Latency & Performance Optimizations:**
  - CPU overclocked to 320 MHz @ 1.20V.
  - Highly optimized thread scheduling and data pipelines designed to minimize Bluetooth latency and eliminate audio stuttering.


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
*   **Audio & Mute Sync:** Runs driverless. The physical Mute button on the controller operates at the hardware level, muting the microphone stream in the firmware and lighting up the controller's orange LED. Muting/unmuting the microphone via the Windows Sound control panel also synchronizes with the controller's LED. (Note: Because it is driverless, toggling the physical button will not change the Windows Sound Panel checkmark state; the mic stream is muted directly on the adapter).

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
