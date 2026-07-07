# HANDOFF — feat/wifi-wol-bt-latency

Context file for the next working session. State as of 2026-07-03.

## Where things stand

Branch `feat/wifi-wol-bt-latency`, based on `ds5-linux-bridge` (the public mainline).
It is the selective port of `experiment/bt-input-latency` — W5500 Ethernet dropped
entirely, and (deliberate design decision, made mid-port) **USB-NCM dropped too**.

Commits on top of `ds5-linux-bridge`:

| commit | what |
|---|---|
| `81f02f7` | WiFi-only config + WOL + captive portal, pure-DualSense USB, wake-action options |
| `e87aa28` | Optimize BT HID input latency path (realtime HID queue) |
| `a62a5d2` | Overclock RP2350 to 200 MHz (no vreg bump) + guard double interrupt_loop |
| `a4251ae` | Emit HID report before audio_loop in main loop |
| `e989b56` | Relocate app BT hot-path glue to RAM (`__not_in_flash_func`) |
| `ba97cd8` | Make USB wake keyboard a runtime web-UI toggle (`WAKE_VIA_USB_KBD` CMake option retired → `Config_body.wake_kbd_enabled`) |

> For the authoritative per-commit history from here on, see `git log` /
> `CODE_REVIEW.md` — this table is not kept exhaustively up to date.

### HW-VERIFIED on real hardware (user, 2026-07-03)

- WiFi onboarding (AP `DS5-Setup-XXXX` → captive portal → STA join) ✅
- Audio under WiFi (the old "Gate 2" duplex-stutter question) ✅
- WOL (PS-button + web-button wake) ✅
- Changing settings via the WiFi web page ✅
- Settings survive reflashing ✅
- Latency: vibe check passed ✅

## Architecture after this branch

- **Config transport = WiFi only** (`ENABLE_WIFI_WOL`, default ON). `usb_net.cpp/h`
  deleted; `web_api.cpp` is the transport-agnostic httpd core (page streamed from
  flash, JSON API, POST handlers, factory reset, save-failed reporting).
  Page at `http://<hostname>.local/` (default `ds5wol.local`).
- **USB face is a pure DualSense by default** (anticheat goal): audio(0-2)+gamepad(3)
  in FULL, a single inert dummy HID in MINIMAL. Nothing else on Sony's VID:PID.
- Wake actions (decision machinery `ENABLE_WAKE_HID` unchanged — dynamic
  MINIMAL/FULL descriptor + suspend tracking):
  - WOL over WiFi (covers S4/S5) + USB bus resume — always.
  - `WAKE_VIA_USB_KBD=ON` (CMake, default OFF) — on-board F15 boot keyboard.
  - `ENABLE_WAKE_LINK=ON` (CMake, default OFF) — **scaffolding** for a future
    companion Pico: GP2 pulsed HIGH 100 ms, suspend-gated, once per suspend spell
    (own latch, separate from WOL's). Wire protocol in `src/wake_link.h`.
    Companion firmware does NOT exist yet.
- **Config store**: `CONFIG_VERSION = 9`, byte-identical to the experiment branch's
  v9 layout. APPEND-ONLY (offsetof pins in config.cpp). Reserved-but-kept fields:
  `webconfig_subnet`/`webconfig_custom_ip` (NCM-era), `wol_use_static_ip`/
  `wol_static_ip`/`wol_static_netmask` (W5500-era). Never remove/repurpose.
  `config_save()` writes directly (no `flash_safe_execute`) when core1 was never
  launched (AP onboarding mode) — core1 isn't a lockout victim there.

### Build matrix (all verified compiling, Release)

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w \
      -DPICO_SDK_PATH=C:/Users/mkung/.pico-sdk/sdk/2.2.0
```
Variants: default | `-DWAKE_VIA_USB_KBD=ON` | `-DENABLE_WAKE_LINK=ON` |
`-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON` | `-DENABLE_WIFI_WOL=OFF` (no web UI at all).
Local build dirs `build-port-*` exist and are gitignored.

## Wake-kbd runtime toggle — IMPLEMENTED (2026-07-03), initial HW pass OK

The planned task is done, exactly per the design-reviewed plan:

1. **Config**: `uint8_t wake_kbd_enabled` appended after `wifi_psk` (offset 228,
   offsetof pin added). Default 0, clamped in `config_valid()`.
   `CONFIG_VERSION` stays 9 (append-only; size migration handles it).
2. **Descriptors**: 2×2 macro-composed static arrays in usb_descriptors.cpp —
   `DS5_CFG_HDR_DESC` + `DS5_FULL_ITFS` (the frozen 218-byte canonical run) +
   `DS5_KBD_ITF_DESC` / `DS5_DUMMY_HID_ITF_DESC`. Arrays:
   `descriptor_configuration` (FULL no-kbd, also the non-wake build's only one),
   `descriptor_configuration_full_kbd` (kbd = itf 4 / HID instance 1),
   `descriptor_configuration_minimal`, `descriptor_configuration_minimal_kbd`.
   All four static_assert-length-locked. bInterval/report-len patch offsets
   unchanged (kbd is appended strictly after the gamepad).
3. **Orchestrator**: one atomic `usb_desc_target {variant, kbd}` for
   desired/active; `desc_target_differs()` drives the unchanged bounce FSM;
   `active` latched in SWAP_DISCONNECTING (bus down). Callbacks read only
   `active`. `usb_set_descriptor_variant_full/minimal` (unused) deleted.
4. **TinyUSB**: `CFG_TUD_HID = 2` unconditionally.
5. **Runtime gates**: wake.cpp F15 FSM compiles always, arms/pumps only when
   `usb_wake_kbd_active()` (the *enumerated* state). main.cpp HID get/set
   callbacks route itf 1 → kbd only when active. `WAKE_VIA_USB_KBD` CMake
   option DELETED (`ENABLE_WAKE_LINK` still compile-time).
   `usb_descriptor_init_from_config()` seeds desired+active before the first
   `tud_connect()` (no boot-time bounce).
6. **Web UI**: "Wake" section between Paired controllers and Network
   (checkbox + anticheat/re-plug hint, own Save via `wake_kbd_enabled=0/1`
   POST). JSON: `wake_kbd_enabled` + `wake_kbd_capable` (false when
   `ENABLE_WAKE_HID=OFF` → section hidden). Live apply:
   `usb_request_wake_kbd()` after `config_save()` in `apply_post()` (and after
   factory reset, which zeroes the toggle).

All FIVE build configs green (Release): default | ENABLE_WAKE_LINK=ON |
WAVESHARE | ENABLE_WIFI_WOL=OFF | ENABLE_WAKE_HID=OFF (build-port-nowake).
`build-port-kbd/` is now an OBSOLETE variant dir (option deleted) — delete it,
don't flash from it.

**HW status: user flashed and reports "seems legit so far" (2026-07-03).**
Remaining targeted HW checks (from the plan):
- [ ] kbd ON: keyboard appears as HID instance 1 in BOTH variants across
      connect/disconnect swaps — no rogue keyboard, no gamepad reports on it
- [ ] toggle while controller connected / while idle / while host suspended
      (suspended: apply must defer and survive resume)
- [ ] kbd ON: F15 actually wakes the PC from S3
- [ ] toggle survives reboot + reflash (config persistence)
- [ ] anticheat-relevant: with toggle OFF (default), enumeration is unchanged
      pure DualSense (compare USB tree against pre-change firmware)

## Other open items

- **README** still documents the NCM config page (10.55.55.105 etc.) — needs a
  rewrite for WiFi onboarding. User may want to word it themselves; ask first.
- Companion wake-keyboard Pico firmware (consumer of `wake_link.h` protocol) —
  future work, protocol v0 is frozen in that header.
- Merging this branch into `ds5-linux-bridge` — user's call, probably after the
  wake-kbd toggle lands.

## Standing gotchas (memory has more)

- Always build **Release** (`-O0` = audio crackle). Opus-in-RAM relocation is
  load-bearing — never remove to free heap. Heap budget is the fixed constraint.
- FULL descriptor layout is canonical/frozen (matches real DualSense; Windows
  rejects non-ascending interface numbers; audio-at-0/gamepad-at-3).
- Diagnostics: UART0 GP0 TX, 115200 8N1. No USB serial, ever.
- `lib/portal/` is vendored from pico-examples (AP-mode DHCP+DNS), WiFi build only.
