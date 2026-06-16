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

## What's different in this fork

These are the deliberate, code-level departures from upstream that define this
fork's character. (Upstream is actively developed and excellent; this list is
about *direction*, not "things upstream gets wrong.")

- 🎚️ **Boxcar / linear resampler instead of WDL.** The haptic decimation and the
  512→480 speaker resample use a lightweight boxcar + linear interpolator
  rather than the WDL resampler — far cheaper on CPU and tuned to better match cabled intensity.
- 🔈 **Volume fully yielded to the host.** Speaker/headset volume is owned by the
  host OS mixer and held in RAM only — no volume state is written to flash.
- 🐧 **Linux-first integration & documentation.** Development and testing target
  Linux / SteamOS (Bazzite, CachyOS) first. This includes `hid-playstation`
  jack-detection wiring and documented, kernel-version-aware notes for getting
  stereo audio routing working (see
  [OS & driver behavior](#operating-system--driver-behavior)).

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
  - Wake the host(S3) by turning on the controller.
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

### Linux / SteamOS (Bazzite, CachyOS)
- **Native Driver Integration:** Compatible with the kernel `hid-playstation`
  driver. When the Linux driver is active, the firmware yields LED and button
  control to the OS driver to avoid conflicts.
- **Jack Detection:** The DS5's real `HP_DETECT` / `MIC_DETECT` jack bits are
  passed through to the host so `hid-playstation` (kernel ≥6.18) emits
  `SW_HEADPHONE_INSERT` / `SW_MICROPHONE_INSERT`. The ≥6.17 USB-audio mixer
  quirk wires these to the ALSA "Headphone Jack" / "Headset Mic Jack" controls
  that `alsa-ucm-conf` uses to switch between the mono Internal Speaker and the
  stereo Headphones profiles. The firmware also forces the HP_DETECT bit high in
  the report it presents to the host to bias toward the stereo Headphones
  profile for headphone output.

> **Known issue — one-earphone / mono audio on some setups.** Audio routing is
> ultimately decided host-side by PipeWire/ALSA via `alsa-ucm-conf`, and on some
> distros/kernels it lands on a mono profile (audio in one earphone only). This
> is kernel- and UCM-version dependent rather than a firmware fault — stereo
> generally needs a recent kernel (≥6.18) with the jack-detect mixer quirk.
> Note that bleeding-edge / rolling distros (e.g. CachyOS, Arch) can also *regress* here:
> a newer kernel or updated `alsa-ucm-conf` can change the routing behavior and
> break a setup that previously worked. Investigation is ongoing.

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

## Configuration

The adapter serves its own configuration web page — no app, no browser API, no
internet. It enumerates as a **USB network adapter** (CDC-NCM) alongside the
controller, and serves the page over a tiny onboard HTTP server.

1. With the controller connected, open **http://10.7.7.107/** in any browser.
   (`http://ds5config.local/` may also work, but mDNS resolution is unreliable —
   prefer the IP.)
2. Adjust settings — controller mode, polling rate, speaker volume, audio buffer
   length, inactivity timeout, auto-disconnect, onboard LED — and click **Save**.
   Settings are written to the adapter's flash.

This replaces the old WebHID approach, which didn't work in Firefox. The
embedded page works in any browser on any OS.

> **Notes:**
> - The config interface reuses the USB endpoints the debug serial would take,
>   so it is present in normal (release) builds and disabled when building with
>   `-DENABLE_SERIAL=ON`. It can also be turned off explicitly with
>   `-DENABLE_WEBCONFIG=OFF`.
> - The page is reachable while a controller is **connected** (the adapter
>   presents its full USB interface set then). With no controller connected the
>   adapter falls back to a minimal descriptor and the network interface is not
>   exposed.

---

## License & Acknowledgement

This project is licensed under the **GNU General Public License v3.0** — see
[LICENSE](LICENSE).

DS5-Linux-Bridge is a fork and continuation of the original
[DS5Dongle](https://github.com/awalol/DS5Dongle) project created by **awalol**,
which is MIT-licensed. Portions of this source originate from that MIT work;
the original MIT notice is preserved in [LICENSE-MIT](LICENSE-MIT) as that
license requires. The project as a whole is now distributed under GPLv3. This
fork continues to track upstream improvements (such as the RAM-relocation
infrastructure and DualSense Edge profile support).
