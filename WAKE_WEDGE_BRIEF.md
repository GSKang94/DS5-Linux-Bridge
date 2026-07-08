# Bug brief: gamepad+audio wedge after "controller-off → S3 → controller-on-to-wake"

**Branch:** `feat/wifi-wol-bt-latency`
**Firmware to reproduce:** build with `-DENABLE_VERBOSE=ON -DWAKE_DEBUG=ON` (both flags
required — `WAKE_DEBUG` is a *separate* CMake option, default OFF, NOT implied by
`ENABLE_VERBOSE`; without it no `[wake]` lines print). Board: `pico2_w`.
**Status:** root-caused, NOT fixed. A log-flood symptom was already throttled in commit
`89b1ae4`; that is cosmetic and does NOT fix this wedge.

---

## 1. Symptom (hardware-observed)

Trigger sequence:
1. DualSense controller is **powered OFF**.
2. Host PC goes into **S3 sleep**.
3. User **powers the controller ON**; the intent is that the controller connecting
   wakes the PC (this dongle supports "turn-on-controller-to-wake").

The PC *does* wake. But then the gamepad is **dead**: the controller doesn't respond to
anything, the USB audio interfaces never enumerate, and pressing buttons shows up on the
dongle's UART (BT is fine) but the **host sees no HID input at all**. The only recovery is
to **power-cycle the controller** (force a fresh BT reconnect), which fixes it.

On UART during the wedge, `[USBHID] tud_hid_report error` is emitted every main-loop
iteration (now rate-limited to 1/sec with a "+N suppressed" count by commit `89b1ae4`).

Note: it does NOT happen every wake. A wake that resolves as a simple **resume** works
fine; the wedge is specifically when the wake resolves as a full **re-enumeration
(re-mount)** that collides with our own pending variant swap. See §4 for the log
signatures that distinguish the two.

---

## 2. Architecture you need (all in `src/`)

This firmware presents **two USB descriptor variants** and swaps between them by
re-enumerating (`ENABLE_WAKE_HID` builds):

- **MINIMAL**: no controller connected — a dummy HID at instance 0 (+ optional boot
  keyboard at instance 1). Keeps the dongle enumerated so USB remote-wakeup can fire.
- **FULL**: controller connected — audio + real gamepad at instance 0 (+ optional kbd
  at instance 1).

**Variant swap orchestrator** — `usb_descriptors.cpp`:
- Targets: `desired_target` and `active_target` (each `{variant, kbd}`), file-static
  volatile (`usb_descriptors.cpp:511-512`). Descriptor callbacks read ONLY `active_target`
  (latched at swap time), never live config.
- `usb_request_variant_full()` / `usb_request_variant_minimal()` set `desired_target.variant`
  (`usb_descriptors.cpp:565-566`).
- `usb_variant_task()` (`usb_descriptors.cpp:604`), pumped every main-loop iteration from
  `main.cpp:566`, drives the bounce:
  - **Guard (`usb_descriptors.cpp:605`):** if `host_suspended_flag` is true, it RETURNS
    EARLY — never re-enumerates during suspend (except it nudges a bus-resume when an
    UP-swap MINIMAL→FULL is pending, so the controller isn't stranded — issue #4).
  - `SWAP_IDLE` (`:622`): if `desc_target_differs()`, call `wake_reset_for_variant_swap()`
    + `tud_disconnect()`, go to `SWAP_DISCONNECTING`.
  - `SWAP_DISCONNECTING` (`:630`): after `SWAP_DISCONNECT_SETTLE_US` = **500 ms**, latch
    `active_target = desired_target`, call `tud_connect()`, go to `SWAP_CONNECTING`.
  - `SWAP_CONNECTING` (`:641`): after `SWAP_CONNECT_SETTLE_US` = **1500 ms**, back to IDLE.
- `usb_variant_swap_in_progress()` = `swap_state != SWAP_IDLE` (`:582`).

**Wake FSM + USB callbacks** — `wake.cpp`:
- `tud_suspend_cb()` (`wake.cpp:222`): sets `host_suspended=true`, arms a deferred
  DualSense power-off (10 s debounce), re-arms the F15 FSM to `PENDING_PRESS`.
- `tud_resume_cb()` (`wake.cpp:251`) and `tud_mount_cb()` (`wake.cpp:269`): clear
  `host_suspended` / `host_suspended_flag`, cancel the pending power-off. `tud_mount_cb`
  means a **full re-enumeration** (fresh SET_CONFIGURATION) happened, not just a resume.
- `wake_on_bt_connect()` (`wake.cpp:329`): when the controller connects while
  `host_suspended`, calls `request_host_wake("BT connect while suspended")`
  (`tud_remote_wakeup()` + arms the F15 FSM if the kbd variant is active).
- `usb_request_variant_full()` is called from `bt.cpp` when the controller's HID opens
  (the `Connected DS5 Controller` path).

**The failing report path** — `main.cpp`:
- `interrupt_loop()` (`main.cpp:152`) emits the gamepad input report via
  `tud_hid_report(0x01, ...)`. Guards: `usb_descriptor_variant_is_full()` +
  `tud_hid_ready()`. On failure it now calls the throttled `log_hid_report_error()`
  (`main.cpp:134`).
- `tud_hid_report` → TinyUSB `tud_hid_n_report` → `usbd_edpt_claim` + `usbd_edpt_xfer`.
  `usbd_edpt_xfer` (usbd.c:1552) calls `dcd_edpt_xfer`; on DCD failure it **clears
  BUSY|CLAIMED and returns false** (usbd.c:1577-1582). So a persistent `dcd_edpt_xfer`
  failure means `tud_hid_ready()` keeps reading "ready" (not busy) and we retry+fail
  every iteration forever.

---

## 3. Root cause (the race)

1. Controller powers off at/around suspend → a DOWN-swap to MINIMAL is requested
   (`desired=MINIMAL`). But `usb_variant_task` early-returns while `host_suspended_flag`
   (`usb_descriptors.cpp:605`), so **the swap never applies during suspend** — the device
   stays enumerated in whatever variant it had.
2. Controller powers ON while the host is still suspended → the BT connect path calls
   `usb_request_variant_full()` (`desired=FULL`) and `wake_on_bt_connect()` →
   `request_host_wake(...)` → `tud_remote_wakeup()`.
3. The host wakes. Depending on the host, this arrives as `tud_resume_cb` and/or a full
   `tud_mount_cb` (re-enumeration). `host_suspended_flag` clears.
4. Now, on the very next `usb_variant_task()` tick, `desc_target_differs()` is true
   (`active` != `desired=FULL`) → the orchestrator kicks off ITS OWN
   `tud_disconnect()`→(500 ms)→`tud_connect()` re-enumeration **at the same time the host
   is performing its wake re-enumeration**. The two USB reconfiguration cycles interleave,
   and the RP2350 DCD's gamepad IN endpoint comes back in a state where `dcd_edpt_xfer`
   permanently fails → gamepad + audio dead until a BT reconnect rebuilds everything.

**D1 is NOT the cause.** The recent commit `ba739a3` wired `wake_on_bt_disconnect()` into
the BT disconnect handler; it only resets the wake FSM + button-diff bytes and never
touches `desired_target`/`active_target`. `tud_suspend_cb` re-arms unconditionally
regardless. This wedge is pre-existing in the variant-swap-on-wake machinery.

---

## 4. Log signatures (from a WAKE_DEBUG=ON capture)

**GOOD wake cycle (resume, works):**
```
Connected DS5 Controller
[wake] BT connect while suspended -> REQUESTED, tud_remote_wakeup()=1
[wake] tud_resume_cb state=REQUESTED armed=0 swap=0
[wake] REQUESTED: sent keydown 0x68 -> 1        <- F15 actually sent
[wake] KEY_DOWN ... -> [wake] KEY_UP_SENT settle done -> DONE
```

**BAD wake cycle (re-mount collision, WEDGES):**
```
Connected DS5 Controller
[wake] BT connect while suspended -> REQUESTED, tud_remote_wakeup()=1
[wake] tud_resume_cb state=REQUESTED armed=1 swap=0
[wake] tud_mount_cb state=IDLE armed=0 swap=0    <- FULL re-enum; F15 never sent
[USBHID] tud_hid_report error                    <- floods (now throttled)
[USBHID] tud_hid_report error (+5059 suppressed in last window)
...continues until the pad is power-cycled...
```
A later `[wake] tud_mount_cb state=IDLE armed=0 swap=1` in the same stretch confirms our
own variant swap ran (interleaved with the host re-enum).

---

## 5. Candidate fixes (design + HW-test required — do NOT ship blind)

The fix must **serialize our variant swap behind the host's own wake re-enumeration**, or
**avoid the redundant swap entirely**. Options, roughly in increasing invasiveness:

1. **Skip-if-already-correct:** on wake, if the host's re-enumeration already brought us up
   in the desired variant (`active_target` already == `desired_target` after the mount, or
   the enumerated config matches), do NOT start a swap. Requires making the mount path
   reconcile `active_target` with what was actually enumerated. Lowest-risk conceptually
   but needs care: descriptor callbacks read `active_target`, so it must be latched to the
   truth the host enumerated.
2. **Post-mount settle gate:** after `tud_mount_cb`/`tud_resume_cb` clears
   `host_suspended_flag`, hold `usb_variant_task` in a "just resumed, wait" window (e.g.
   a few hundred ms to a second) before allowing `SWAP_IDLE → tud_disconnect`, so the
   host finishes its wake enumeration before we bounce. Simple, but a fixed timer is
   fragile across hosts.
3. **Coalesce the deferred DOWN/UP swaps:** the whole mess starts because a DOWN-swap
   (controller off) and an UP-swap (controller on) both got deferred through the suspend.
   If, at resume, `desired` already equals what a fresh enumeration would present, collapse
   to no-op.

### Constraints — the USB variant machinery is load-bearing and fragile
Do not reorder USB interfaces or change descriptor layout. Prior hard-won invariants
(see the project memory and commit history):
- Windows pins ascending `bInterfaceNumber`; the kbd must stay HID **instance 1** in BOTH
  variants (dummy HID at instance 0 in MINIMAL). This is what makes "rogue keyboard on
  wake" structurally impossible — don't disturb it.
- Real-DualSense canonical USB layout (audio at 0-2, gamepad at 3) must be preserved for
  the Windows driver; MINIMAL is *padded*, FULL matches the real device.
- The `host_suspended` early-return in `usb_variant_task` exists for a reason (never
  re-enumerate mid-suspend, or remote-wake can't fire), AND the MINIMAL→FULL bus-resume
  nudge exists for issue #4 (don't strand the controller in MINIMAL). Any fix must keep
  both.

### Verification (HW) — the reproducer is specific
Flash `-DENABLE_VERBOSE=ON -DWAKE_DEBUG=ON`, capture UART from a SECOND PC (so logging
survives the target's S3), and run this exact trigger several times:
1. Pair the controller; confirm normal input + audio while the host is awake.
2. **Power the controller OFF.**
3. Put the target PC into **S3**.
4. **Power the controller ON** and let it wake the PC.
5. PASS: gamepad responds and audio enumerates immediately after wake, on EVERY
   repetition — no `tud_hid_report error`, no need to power-cycle the pad. Watch for the
   GOOD-cycle log signature; the BAD signature (`tud_mount_cb ... -> tud_hid_report error`
   flood) must not appear.
Also regression-test the plain wake paths that already worked: controller-stays-connected
through S3 → PS-press wake; and the kbd-disabled (pure DualSense) variant.

---

## 6. Repro/build commands

```sh
# WAKE_DEBUG verbose build (pico2_w):
cmake -S . -B build/wakedbg -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPICO_SDK_PATH="<sdk>" \
  -DENABLE_VERBOSE=ON -DWAKE_DEBUG=ON
cmake --build build/wakedbg --target ds5-bridge
# -> build/wakedbg/ds5-bridge.uf2 ; UART GP0 TX, 115200 8N1
```
Build the full variant matrix before committing any fix (standard, no-wifi, no-wake,
no-batt-led+ENABLE_WAKE_LINK=ON, picow) — see `.github/workflows/build.yml`.
