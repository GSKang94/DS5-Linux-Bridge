# DS5-Linux-Bridge

A Linux-focused firmware for the Raspberry Pi Pico 2 W that turns it into a
latency-optimized USB-to-Bluetooth bridge for the Sony DualSense (DS5)
controller — reproducing the wired controller experience (speaker, microphone,
and native HD haptics) over Bluetooth. Windows works too, but the project's
priority is getting the experience right on Linux / SteamOS (Bazzite).

> **Opinionated Linux fork of [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle).**
> This is a downstream fork that tracks awalol's excellent upstream firmware and
> adds Linux-specific fixes and tuning. It is **not** an official release of that
> project. All upstream work is credited under the MIT License — see
> [License & Acknowledgement](#license--acknowledgement).

---

## Why this fork? (Linux differences vs upstream)

- 🎧 **`hid-playstation` jack-detection integration for correct stereo routing.**
  The DS5's real `HP_DETECT` / `MIC_DETECT` jack bits are passed through to the
  host so `hid-playstation` (kernel ≥6.18) emits `SW_HEADPHONE_INSERT` /
  `SW_MICROPHONE_INSERT`; the ≥6.17 USB-audio mixer quirk wires these to the
  ALSA "Headphone Jack" / "Headset Mic Jack" controls that `alsa-ucm-conf` uses
  to switch between the mono Internal Speaker and the **stereo Headphones**
  profiles. The firmware also forces the HP_DETECT bit high in the report it
  presents to the host so the stereo profile is selected for headphone output.
  (Note: on some distros/kernels PipeWire/ALSA may still land on a mono profile
  and play in one earphone only — this is host-side UCM/kernel behavior; see
  the kernel-version notes below.)
- 🎚️ **Boxcar / linear resampler instead of WDL.** The haptic decimation and the
  512→480 speaker resample use a lightweight boxcar + linear interpolator
  rather than the WDL resampler. This is far cheaper on CPU and tuned for a
  punchier, more "wired-DualSense-like" haptic feel.
- 🔈 **Volume fully yielded to the host.** Speaker/headset volume is owned by the
  host OS mixer and held in RAM only — no volume state is written to flash.
- 🎛️ **Haptic intensity boost** to better match cabled intensity.

---

## Core Features

- 🎮 **Full Wireless Controller Emulation:** Converts DualSense Bluetooth reports
  into standard USB HID gamepad input at 1000Hz. Supports both standard
  DualSense (DS5) and DualSense Edge (DSE), including DSE PS-app profiles.
- 📳 **Wireless HD Haptics:** Recreates the cabled advanced audio-based haptic
  feedback over Bluetooth by capturing the dedicated haptic waveforms games
  send to the controller's haptic channels and streaming them to the voice-coil
  actuators. Compatibility mirrors the wired experience (e.g. Death Stranding
  Director's Cut supports HD haptics on Windows OOTB; like the wired DualSense,
  it does not on Linux).
- 🔊 **Wireless Audio Stream:** High-quality speaker and headphone playback
  through the controller's audio jack and speaker.
- 🎙️ **Wireless Microphone Upload:** Decodes and streams the controller's mic
  back to the host via standard USB Audio Class — full quality, no Bluetooth
  headset-profile (HSP) downgrade.
- 🔇 **Hybrid Hardware Microphone Mute:**
  - Driverless local hardware mute toggle via the controller's physical Mute
    button.
  - Synchronizes with the host OS sound panel's mute state.
  - Dynamically yields control to active host drivers (e.g. Linux's
    `hid-playstation`) to avoid state conflicts.
- 🔌 **Wake from S3 Sleep and Dynamic USB Descriptors:**
  - Swaps USB configurations dynamically to hide audio/gamepad interfaces when
    the controller is disconnected, preventing "ghost" devices in the OS.
  - Wake the host from S3 sleep by pressing any button after turning on the
    controller.
  - Wake from S5 is available on compatible motherboards (check that yours can
    be woken from S5 by a USB **keyboard**, not just a mouse).
  - Automatically powers off the DualSense after 10s of inactivity when the host
    sleeps or powers off.
- 📡 **USB 3.0 RF Noise Watchdog:** Auto-retries Bluetooth connections stalled by
  2.4 GHz interference from USB 3.0 ports. Motherboard USB 2.0 ports are
  recommended.
- 🚨 **Visual Notifications:** Power-On Self Test LED pattern, solid LED on a
  live controller link, and a once-per-second low-battery blink (≤10%).
- ⚡ **Low-Latency Performance:** Critical Bluetooth/USB/audio hot paths are
  relocated into RAM (`.time_critical`) to eliminate Flash XIP cache thrashing,
  plus tuned thread scheduling and data pipelines to minimize Bluetooth latency
  and eliminate audio stutter.

---

## Getting Started

### Installation / Flashing
1. Hold the **BOOTSEL** button on your Raspberry Pi Pico 2 W.
2. Connect it to your PC via USB.
3. Drag and drop the compiled `.uf2` firmware onto the mounted `RP2350` volume.

### Pairing the Controller
1. Put the DualSense into Bluetooth pairing mode (hold **Share + PS** until the
   lightbar double-blinks).
2. The Pico 2 W detects, pairs, and connects. The onboard LED goes solid on a
   successful connection.
3. Once paired, the adapter enumerates the controller interfaces to the host.

---

## Operating System & Driver Behavior

### Linux / SteamOS (Bazzite)
- **Native Driver Integration:** Compatible with the kernel `hid-playstation`
  driver. When the Linux driver is active, the firmware yields LED and button
  control to the OS driver to avoid conflicts.
- **Jack Detection:** `HP_DETECT` and `MIC_DETECT` events are forwarded to the
  host for automatic profile switching in `alsa-ucm-conf`, PulseAudio, and
  PipeWire.
- **Stereo audio:** Plays correctly in both earphones (see
  [Why this fork?](#why-this-fork-linux-differences-vs-upstream)).

### Windows 10/11
- **Audio & Mute Sync:** Runs driverless. The physical Mute button operates at
  the hardware level, muting the mic stream in firmware and lighting the
  controller's orange LED. Muting/unmuting via the Windows Sound panel also
  syncs the controller LED. (Because it is driverless, toggling the physical
  button won't move the Windows checkmark; the mic stream is muted directly on
  the adapter.)

---

## Build Instructions

The build defaults to `Release`. Building `Debug` (`-O0`) causes audio
crackling — only do so when actually debugging.

1. Ensure the Pico SDK is configured and its `tinyusb` library is up to date.
2. Compile with the standard Pico CMake configuration:
   ```bash
   mkdir build
   cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   make
   ```
3. To disable the low-battery warning LED blink, configure with
   `-DENABLE_BATT_LED=OFF`.

---

## Configuration (planned)

A WebHID-based configuration tool (inactivity timeout, LED preferences, buffer
sizing) is planned. WebHID is not supported in Firefox, so this is treated as a
Linux-oriented convenience feature served from a local page rather than a
required step.

---

## License & Acknowledgement

This project is licensed under the **MIT License**.

DS5-Linux-Bridge is a fork and continuation of the original
[DS5Dongle](https://github.com/awalol/DS5Dongle) project created by **awalol**.
The original work is credited under the terms of the MIT license, and this fork
continues to track upstream improvements (such as the RAM-relocation
infrastructure and DualSense Edge profile support).

For the full license text, see the [LICENSE](LICENSE) file.
