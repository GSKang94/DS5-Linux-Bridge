# DS5-Linux-Bridge

A Linux-focused firmware for the Raspberry Pi Pico 2 W that turns it into a
latency-optimized USB-to-Bluetooth bridge for the Sony DualSense (DS5)
controller — reproducing the wired experience (speaker, microphone, and native
HD haptics) over Bluetooth. Windows works too, but the project's priority is
getting it right on Linux / SteamOS (Bazzite).

<p align="center">
  <a href="https://ko-fi.com/mkungaa">☕ <b>Support this project on Ko-fi</b></a>
  &nbsp;·&nbsp;
  <a href="docs/USER_GUIDE.md">📖 User guide</a>
  &nbsp;·&nbsp;
  <a href="https://github.com/kungaa/DS5-Linux-Decky">🎮 Steam Deck plugin</a>
</p>

> **Opinionated Linux fork of [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle).**
> A downstream fork that tracks awalol's excellent upstream firmware and adds
> Linux-specific fixes and tuning. **Not** an official release of that project.
> All upstream work is credited under the MIT License — see
> [License & Acknowledgement](#license--acknowledgement).

---

## Core Features

- 🎮 **Full wireless controller emulation** — DualSense Bluetooth reports → standard
  USB HID gamepad at up to 1000 Hz. Supports DualSense (DS5) and DualSense Edge
  (DSE), including DSE PS-app profiles.
- 📳 **Wireless HD haptics** — recreates the cabled audio-based haptic feedback over
  Bluetooth, streaming the dedicated haptic waveforms to the controller's
  voice-coil actuators. Compatibility mirrors the wired experience.
- 🔊 **Wireless audio (speaker + mic)** — speaker/headphone playback and microphone
  upload over standard USB Audio Class — full quality, no Bluetooth headset-profile
  (HSP) downgrade.
- 🔇 **Hybrid hardware mic mute** — driverless local mute via the physical Mute
  button, synced with the host sound panel, yielding to active host drivers
  (e.g. Linux `hid-playstation`) to avoid conflicts.
- 🌐 **On-device web config + bond management** — the adapter hosts its own
  configuration page (no app, no WebHID, any browser). Adjust settings and
  manage remembered controllers. Reachable whether or not a controller is
  connected.
- 🎮 **Steam Deck companion plugin** — a [Decky Loader plugin](https://github.com/kungaa/DS5-Linux-Decky)
  surfaces controller status and settings in the Quick Access Menu, talking to
  the same on-device API as the web page.
- 🔌 **Wake from sleep (S3 / S5)** — wake the host by turning on the controller
  (S5 needs a board that wakes from a USB **keyboard**). The DualSense auto-powers
  off after the host sleeps/shuts down to save its battery.
- ⚡ **Low-latency performance** — Bluetooth/USB/audio hot paths run from RAM
  (`.time_critical`) to avoid Flash XIP cache thrashing, with tuned scheduling
  and pipelines to minimize latency and eliminate audio stutter.
- 📡 **USB 3.0 RF-noise watchdog** — auto-retries Bluetooth connections stalled by
  2.4 GHz interference from USB 3.0 ports (USB 2.0 ports recommended).
- 🚨 **Visual notifications** — POST LED pattern, solid LED on a live link, and a
  once-per-second low-battery blink (≤10%).

📖 **See the [User Guide](docs/USER_GUIDE.md)** for flashing, pairing, the config
page, and OS-specific (Linux / Windows) behavior and troubleshooting.

---

## Quick Start

1. **Flash** — hold **BOOTSEL** on the Pico 2 W, plug it into USB, and drop the
   `.uf2` onto the mounted `RP2350` volume.
2. **Pair** — put the DualSense in pairing mode (hold **Share + PS** until the
   lightbar double-blinks). The adapter detects, pairs, and connects; the onboard
   LED goes solid.
3. **Configure** *(optional)* — browse to **http://10.55.55.105/** to change
   settings or manage paired controllers.

Full details, including how to add a second controller and per-OS audio notes,
are in the **[User Guide](docs/USER_GUIDE.md)**.

---

<details>
<summary><b>What's different in this fork</b> (vs. upstream awalol/DS5Dongle)</summary>

These are the deliberate departures from upstream that define this fork's
*direction* (upstream is actively developed and excellent — this isn't a list of
things it gets wrong):

- 🌐 **On-device web config instead of WebHID.** The adapter serves its own
  config page over a USB network interface — works in any browser on any OS, no
  WebHID (which never worked in Firefox). Includes bond management.
- 🎮 **Steam Deck Decky plugin** that drives the same on-device API.
- 🔌 **Config page reachable with no controller connected**, and a single stable
  network adapter that survives controller connect/disconnect (the network
  interface keeps the same identity in both of the adapter's USB modes).
- 👻 **No "ghost" devices when idle** — when no controller is connected the
  adapter hides its audio/gamepad interfaces, while staying awake for remote
  wake and never misrouting input to the keyboard.
- 🎚️ **Lightweight boxcar/linear resampler** for haptics and the 512→480 speaker
  resample instead of WDL — cheaper on CPU, tuned to match cabled intensity.
- 🔈 **Volume fully yielded to the host** — held in RAM only, never written to flash.
- 🐧 **Linux-first integration & docs** — `hid-playstation` jack-detection wiring
  and kernel-version-aware stereo-audio notes (see the [User Guide](docs/USER_GUIDE.md)).

</details>

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
   `-DENABLE_BATT_LED=OFF`. To turn off the web config UI, `-DENABLE_WEBCONFIG=OFF`.

### Board targets

The default build targets the **Raspberry Pi Pico 2 W** (RP2350). Two other
boards are supported via CMake options (mutually exclusive):

| Board | Configure with | Notes |
| --- | --- | --- |
| Raspberry Pi Pico 2 W (RP2350) | *(default)* | Full feature set. |
| Waveshare RP2350B-Plus-W | `-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON` | USB-C, 16 MB flash, RM2 wireless. Ships its own board header (the RM2 module is on different GPIOs than the Pico 2 W) and forces boot2 XIP setup for its Puya flash. A convenience script lives at `boards/build_waveshare_rp2350b_plus_w.sh`. |
| Raspberry Pi Pico W (RP2040) | `-DPICO_W_BUILD=ON` | Legacy RP2040; **no audio** (gamepad + haptics only). |

The release builds publish UF2s for the Pico 2 W (standard + verbose) and the
Waveshare board.

---

## Debugging

There is **no USB serial port**. USB-CDC serial was removed because it shares the
USB stack with the audio isochronous endpoints and perturbs the very timing you
need to measure.

Diagnostic output goes to **UART0** instead, which is independent of USB:

- **Pico GP0 (pin 1)** = UART TX → connect to your USB-serial adapter's **RX**
- **Pico GND (pin 3)** → adapter **GND**
- Settings: **115200 baud, 8N1**, no flow control (3.3 V logic — do not feed 5 V
  into GP0)

For louder logs, build the verbose variant with `-DENABLE_VERBOSE=ON` (the CI
also publishes a `ds5-bridge-debug.uf2` built this way).

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
