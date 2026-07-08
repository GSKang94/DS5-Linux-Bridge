# Code Review 2 — post-cleanup findings

> **STATUS 2026-07-08 — EXECUTED.** All findings landed and HW-verified except W1
> (deliberately deferred). Commits:
> `a5f140a` (N4/N6/W2/W4/D2), `26602a6` (N6 regression fix — the `size<2` guard
> skipped DS5 detection), `69393fa` (N1/N2/N5), `2164c39` (N3), `49051de`
> (N7/C1), `ba739a3` (W3/D1). Plus `89b1ae4` — throttled a pre-existing
> `tud_hid_report` error flood surfaced by the D1 wake HW test (not a review item).
> HW passes: fresh pair (both directions), pairing-window expiry with a bond,
> forget/forget-all/inactivity/out-of-range disconnect matrix, mic re-connect
> cycles, AP-mode API allowlist (curl), S3 remote-wake (both cycles).

Reviewed: branch `feat/wifi-wol-bt-latency` @ `c21d466`, 2026-07-08.
Scope: everything under `src/`, `CMakeLists.txt`, `.github/workflows/build.yml`.
Audience: an LLM (or human) executing the fixes. Each finding has an ID,
severity, exact locations, the recommended change, and how to verify it.

Supersedes `CODE_REVIEW.md` (review 1): **every item in it was verified
executed** (commits `ae1955b`, `eeafb17`, `0a32e34` — symbol-level grep confirms
`print_hex`, `bt_send_packet`, `bt_send_control`, `bt_get_signal_strength`,
`bt_rssi`, the RSSI event cases, `addr_to_hex`, the main.cpp `packetCounter`,
`wifi_associated`, and the `<iostream>` includes are all gone; `utils.h` has an
include guard; comment/doc fixes landed). Additionally, both issues review 1
listed as "known-deferred" have since been FIXED on this branch:

* **Issue #2 (empty bond list / bonds don't persist)** — three-part fix, all
  committed: `ea0b559` (app-wide `get_flash_safety_helper()` override so the
  BTstack TLV bank formats on first boot), `df72338` (store the SSP link key
  ourselves in `HCI_EVENT_LINK_KEY_NOTIFICATION`), `ba4fe1f` (drop the
  per-connection link-policy send that swallowed `hci_authentication_requested`
  and broke every fresh pair).
* **forget-all-after-full-session pairing gap** — fixed by `c21d466`:
  race-free `gap_register_classic_connection_filter()` gate (blacklist +
  pairing window evaluated BEFORE BTstack auto-accepts) plus the
  intent-flag inquiry re-arm model (`bt_inquiry_wanted`).

`CODE_REVIEW.md` is fully executed and can be archived or deleted.

---

## 0. Read this first — guardrails (deliberate, hardware-verified; do NOT "fix")

Everything in review 1's guardrail table still applies (append-only
`Config_body` + `offsetof` asserts, Opus RAM relocation + the `analysis.c`
flash exception, frozen USB interface ordering + the dummy-HID-instance-0
trick, the 47-byte-minimum `state_update` check, `CFG_TUD_HID 2`
unconditionally, forced Release build, `PICO_FLASH_ASSUME_CORE1_SAFE=0`,
`pico_fsdata.inc`, lwipopts pool sizes annotated with their OOM incidents).
NEW guardrails added by the commits since `ba97cd8`:

| Item | Why it must stay |
|---|---|
| The strong `get_flash_safety_helper()` override in [flash_safety.cpp](src/flash_safety.cpp) and the `flash_safety_note_core1_launch()` call in [audio.cpp:226](src/audio.cpp#L226) | THE issue-#2 fix. The SDK default helper refuses pre-core1 flash writes, silently dropping the BTstack TLV bank format inside `cyw43_arch_init()`. Never revert to the default, never remove the note-launch call (it closes the launch→registration race). |
| Manual link-key store in `HCI_EVENT_LINK_KEY_NOTIFICATION` ([bt.cpp:666-696](src/bt.cpp#L666-L696)) | BTstack's internal handler does NOT persist the key on a genuine fresh pair (HW-confirmed). Do NOT remove; do NOT wrap in `flash_safe_execute()` — the TLV bank write is already parked via the helper, wrapping would nest the lockout and deadlock (the c4bda6e-vs-dcd4366 lesson). |
| NO per-connection `hci_write_link_policy_settings` at `HCI_EVENT_CONNECTION_COMPLETE`; `hci_authentication_requested` must be the ONLY command sent from that event ([bt.cpp:107-114](src/bt.cpp#L107-L114), [637-641](src/bt.cpp#L637-L641)) | BTstack holds ONE outstanding HCI command; a second raw send from this event swallowed the auth request and broke every from-scratch pairing (`ba4fe1f`). |
| `gap_register_classic_connection_filter()` as the incoming-connection gate ([bt.cpp:292-324](src/bt.cpp#L292-L324), [507-512](src/bt.cpp#L507-L512)) | Race-free: evaluated inside BTstack BEFORE its auto-accept. Never go back to raw `hci_reject_connection_request` in the event handler (loses the race) or the after-the-fact disconnect. |
| The `bt_inquiry_wanted` intent-flag model; never call `gap_inquiry_stop()` then `gap_inquiry_start()` back-to-back ([bt.cpp:141-193](src/bt.cpp#L141-L193)) | `gap_inquiry_start()` is rejected unless BTstack's inquiry state is exactly IDLE, and stop→IDLE is asynchronous. Stop-then-start silently dropped the start (dongle dormant after forget-all, HW-observed). Re-arm ONLY from the two INQUIRY_COMPLETE labels (both must stay handled). |
| `TINYUSB_REF` pin (post-0.20.0 commit) in [build.yml:17](.github/workflows/build.yml#L17) | The 0.20.0 tag predates control-transfer wLength fixes the MS OS 2.0 fetch needs; building against it makes MINIMAL fail Windows enumeration (Code 10). |
| `PICO_CYW43_ARCH_DEFAULT_COUNTRY_CODE=('U','S',0)` ([CMakeLists.txt:177](CMakeLists.txt#L177)) | With the 'XX' worldwide default the onboarding SoftAP beacons but never admits a station (HW-observed: stas=0, DHCP loop forever). |
| `pkt[4] = 0b11111111` (mic-enable bit) and the 200-byte speaker payload / 160 kbps Opus settings in [audio.cpp](src/audio.cpp) | Lower speaker bitrates break the DS5's decoder; mic upload rides bit 0 of that byte. HW-verified quality floor. |
| Pairing model: ONE controller, bond-gated inquiry, blacklist cleared only by an inquiry-path re-pair | Whole design; see the comments at [bt.cpp:52-79](src/bt.cpp#L52-L79) and [126-193](src/bt.cpp#L126-L193). |

General build verification for any change below:

```sh
cmake -B build-review -DCMAKE_BUILD_TYPE=Release && cmake --build build-review -j
# Variant matrix (mirrors CI):
#   -DENABLE_WAKE_HID=OFF
#   -DENABLE_WIFI_WOL=OFF
#   -DENABLE_BATT_LED=OFF
#   -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON
#   -DPICO_W_BUILD=ON
#   (-DENABLE_WAKE_LINK=ON -- currently NOT in CI, see C1)
```

Anything touching BT pairing/reconnect, USB descriptors, audio, or wake also
needs a hardware pass (mark "needs HW verification" in the commit message if
you can't run one).

---

## 1. BT correctness findings (the new gating code)

### N1 — inquiry re-arms in the middle of an incoming connection attempt
**Severity: medium. Effort: small. Needs HW pass (fresh pairing).**

The allowed branch of `HCI_EVENT_CONNECTION_REQUEST`
([bt.cpp:784-789](src/bt.cpp#L784-L789)) calls `gap_inquiry_stop()` and arms
the connect watchdog, but does not touch `bt_inquiry_wanted`. The stop is a
cancel, whose completion arrives as `GAP_EVENT_INQUIRY_COMPLETE` — where the
re-arm logic ([bt.cpp:599-602](src/bt.cpp#L599-L602)) sees `device_found ==
false` (the incoming path never sets it) and `bt_inquiry_wanted == true`, and
restarts a fresh inquiry **while the incoming ACL/auth/encryption setup is in
flight**. Inquiry contends with page response and auth on the shared radio —
exactly the fresh-pair instability class this branch just fought. This bites
whenever a DualSense in pairing mode pages the dongle itself (which is why the
incoming path arms the watchdog at all) during the 0-bond open-to-pair state.

**Fix:** gate the re-arm on no connect being in flight:
`if (bt_inquiry_wanted && connect_attempt_started == 0)` at
[bt.cpp:599](src/bt.cpp#L599). The failure paths already reopen inquiry
(`bt_restart_inquiry` from CONNECTION_COMPLETE-failure / watchdog), and the
success path closes it at HID-open, so no state is stranded.

**Verify:** fresh pair from forget-all (both directions: dongle-initiated via
inquiry find, and controller-initiated page). UART should show no
`[HCI] Re-arm inquiry` between `Incoming ACL request ... (allowed by filter)`
and `HID Interrupt opened`.

### N2 — pairing-window expiry leaves the dongle inquiring forever (bond-gate bypass)
**Severity: medium. Effort: small. Needs HW pass.**

When the web-UI pairing window expires with nothing found
([bt.cpp:591-596](src/bt.cpp#L591-L596)), `pairing_window` is cleared — but
`bt_inquiry_wanted` (set by `bt_start_pairing()` → `bt_inquiry_open()`) is
not. The very next lines re-arm inquiry, and every subsequent
INQUIRY_COMPLETE re-arms it again. Net effect: after a user opens "pair new
controller" and walks away, a dongle **with a bond stored** loops inquiry
indefinitely — it can grab any nearby DualSense in pairing mode (the exact
thing the "bonded → page scan, no inquiry" policy exists to prevent) and the
continuous inquiry also degrades the bonded pad's page-scan reconnect. It
only stops when some controller completes HID-open (`bt_inquiry_close()` at
[bt.cpp:955](src/bt.cpp#L955)).

**Fix:** in the expiry branch, mirror `bt_restart_inquiry()`'s gate — after
`pairing_window = false;` add:
`if (bt_has_stored_link_key()) { bt_inquiry_close(); break-equivalent }` so
re-arm only continues for the 0-bond open-to-pair loop.

**Verify:** with one bonded pad connected, POST `/api/bonds action=pair`, let
the window expire (~38 s) with no new pad. UART must show
`Pairing window closed` followed by `Stored controller -> page scan`-style
behavior, NOT `Re-arm inquiry` loops; the original pad must reconnect on PS
press.

### N3 — raw `hci_send_cmd(&hci_disconnect, …)` from non-event contexts risks the single-command-slot swallow
**Severity: medium (systemic; same failure class as the `ba4fe1f` bug). Effort: small. Needs HW pass.**

`ba4fe1f` established that a raw `hci_send_cmd()` occupies BTstack's single
outstanding command slot and can silently swallow a queued stack command. The
tree still issues raw disconnects from contexts where BTstack may have a
command in flight:

* `bt_disconnect()` ([bt.cpp:122](src/bt.cpp#L122)) — called from the
  **connect watchdog tick** ([bt.cpp:232](src/bt.cpp#L232), main-loop timing,
  completely unsynchronized with the stack), the inactivity watchdog, and the
  auth-failure path.
* `bt_bond_forget()` ([bt.cpp:402](src/bt.cpp#L402)) and
  `bt_bond_forget_all()` ([bt.cpp:422](src/bt.cpp#L422)) — called from httpd
  POST context.

If the send is dropped at the wrong moment the link stays up, no
DISCONNECTION_COMPLETE ever arrives, and the recovery paths (which are all
driven by that event) never run. BTstack 2.2.0 provides `gap_disconnect(handle)`
(gap.h:349) which routes through the GAP state machine and sends when the slot
is free.

**Fix:** replace all three `hci_send_cmd(&hci_disconnect, acl_handle, 0x13)`
sites with `gap_disconnect(acl_handle)` (reason is fixed at 0x13 by BTstack).
The replies sent from inside HCI event handlers (link-key reply, auth
request, encryption request, user-confirmation reply) can stay raw — the slot
was just freed by the event being processed, and they are upstream-proven.

**Verify:** full session cycle: pair, play, forget-while-connected,
forget-all-while-connected, watchdog-triggered teardown (unplug pad battery /
walk out of range), inactivity disconnect. Each must reach
`Disconnected reason=0x..` on UART.

### N4 — `tud_hid_get_report_cb` copies a BT-fed feature report into a 64-byte TinyUSB buffer without clamping
**Severity: low-medium (needs a hostile/buggy controller; memory-safety). Effort: trivial.**

[main.cpp:287-292](src/main.cpp#L287-L292): `feature_data` entries are cached
verbatim from L2CAP control-channel `0xA3` packets
([bt.cpp:915-917](src/bt.cpp#L915-L917)) whose size can be up to the 672-byte
MTU. The callback then does `memcpy(buffer, feature_data.data() + 1,
feature_data.size() - 1)` where `buffer` is TinyUSB's control buffer of
`CFG_TUD_HID_EP_BUFSIZE` = 64 bytes (hid_device.c allocates `ctrl` at that
size and caps `reqlen` to it). A controller (or spoofed pad) answering a
feature GET with >65 bytes overflows the buffer.

**Fix:**
```cpp
if (!feature_data.empty()) {
    uint16_t n = (uint16_t)(feature_data.size() - 1);
    if (n > reqlen) n = reqlen;
    memcpy(buffer, feature_data.data() + 1, n);
    return n;
}
return 0;
```

**Verify:** build + normal Steam/PS-app feature-report flows (DSE profile
read) unchanged; genuine reports are all ≤ 64 B so behavior is identical.

### N5 — L2CAP send-chain state (`retry_pending`/`retry_packet`/`send_chain_active`) survives disconnect
**Severity: low. Effort: small.**

`L2CAP_EVENT_CAN_SEND_NOW`'s retry state lives in function-statics
([bt.cpp:1028-1029](src/bt.cpp#L1028-L1029)) and `send_chain_active`
([bt.cpp:1111](src/bt.cpp#L1111)) is cleared only by the handler itself.
`HCI_EVENT_DISCONNECTION_COMPLETE` drains `send_fifo`
([bt.cpp:817](src/bt.cpp#L817)) but resets neither. Consequences:

1. If an `l2cap_send` failed just before a disconnect, `retry_pending=true`
   persists; the first CAN_SEND_NOW of the **next** session transmits the
   stale packet from the previous session (valid CRC, stale seq/state — e.g.
   a lightbar/rumble frame meant for the old connection).
2. If a can-send-now request was in flight when the channel died, the event
   never fires and `send_chain_active` sticks `true`, wedging `bt_pump()`
   (the audio path's only kick) until the first `kick=true` `bt_write()` of
   the next session resets the chain. Recovery does happen (HID-open sends
   report 0x32 with kick=true), so this is a wart, not a hang — but it's an
   ordering/state leak across sessions.

**Fix:** hoist the two statics to file scope next to `send_chain_active` and
clear all three in the DISCONNECTION_COMPLETE teardown block (alongside the
FIFO drain).

**Verify:** power the pad off mid-audio repeatedly; reconnect; rumble/lightbar
and audio resume cleanly (no one-frame stale flash on connect).

### N6 — control-channel L2CAP data branch reads `packet[0..1]` with no size check
**Severity: low (same class as the already-fixed interrupt-branch guard). Effort: trivial.**

[bt.cpp:880-927](src/bt.cpp#L880-L927): the `channel == hid_control_cid`
branch dereferences `packet[0]` (check_dse compare, `0xA3` cache check) and
`packet[1]` (report id) before any length validation; `dse_on_control_packet`
guards itself, but a 0- or 1-byte runt frame from a misbehaving peer reads out
of bounds. Add `if (size < 2) return;` at the top of the branch (a 1-byte
HANDSHAKE is currently possible — `dse_on_control_packet` handles `size == 1`
— so route: `if (size >= 1) dse_on_control_packet(packet, size);` first, then
`if (size < 2) return;` before the rest, or simply guard each read).

**Verify:** normal DSE handshake logging (`[DSE] HID HANDSHAKE`) still appears
on a DualSense Edge connect.

### N7 — `DISABLE_SPEAKER_PROC` builds (Pico W) operate on never-initialized queues
**Severity: low-medium for the PicoW artifact (latent UB that currently works by accident). Effort: small.**

With `DISABLE_SPEAKER_PROC=1` (forced for `PICO_W_BUILD`,
[CMakeLists.txt:100](CMakeLists.txt#L100)), the whole body of `audio_init()`
is compiled out ([audio.cpp:217-231](src/audio.cpp#L217-L231)) — none of
`audio_fifo`/`mic_fifo`/`mic_decode_fifo`/`opus_cs` are initialized and core1
never launches. But:

* `audio_loop()`'s mic-drain runs unconditionally every main-loop iteration
  ([audio.cpp:67-81](src/audio.cpp#L67-L81)) →
  `queue_try_remove(&mic_decode_fifo, …)` on a zeroed `queue_t` →
  `spin_lock_blocking(NULL)`.
* `mic_add_queue()` ([audio.cpp:334-346](src/audio.cpp#L334-L346)) does the
  same on `mic_fifo` whenever the host opens the mic interface (the PicoW
  build still enumerates the audio interfaces).

This only "works" because a NULL spinlock pointer dereferences ROM at address
0 (reads nonzero → lock treated as instantly acquired; unlock write ignored).
That's UB riding a silicon quirk.

**Fix:** early-return under the flag:
`#if DISABLE_SPEAKER_PROC` → `audio_loop(){ return; }`-style guards for the
mic-drain block and `mic_add_queue` (or wrap both bodies in
`#if !DISABLE_SPEAKER_PROC`). Note `mic_add_queue` is called from
`on_bt_data`; the cheap guard there is fine.

**Verify:** `-DPICO_W_BUILD=ON` compiles; standard build's duplex audio
unchanged (guards compile away).

---

## 2. Web / WiFi findings

### W1 — first-boot STA verification budget can wipe VALID credentials (KNOWN, deliberately deferred — harden when touched)
**Severity: medium (UX trap), known design trade-off. Effort: medium.**

[wifi_net.cpp:493-563](src/wifi_net.cpp#L493-L563): until the first successful
connect of a boot, a 45 s budget (or 2× BADAUTH) clears the stored WiFi creds
and reboots to AP onboarding. Problem: on a **first post-fix boot with 0 BT
bonds**, BT inquiry loops on the shared radio and contends with the STA join —
`CYW43_LINK_NONET`/`FAIL` are then false positives, and a perfectly valid
network can get wiped (observed; hits exactly the issue-#2 recovery cohort:
fresh unit → 0 bonds → inquiry storm → join starves → creds gone). Hardening
options, in preference order:

1. Wipe on BADAUTH only; on timeout, keep creds and keep retrying (a
   persistent AP-fallback escape already exists via `wifi_reset` +, if ever
   fixed, BOOTSEL).
2. Persist a consecutive-failed-boots counter in config (append-only field)
   and only re-onboard after N ≥ 2-3 boots that never joined.
3. Pause/duty-cycle inquiry (`bt_inquiry_close()` + re-open) until the first
   join resolves — reduces the false-positive source directly but touches the
   pairing state machine; coordinate with N1/N2.

**Verify (HW):** fresh-flash a unit with 0 bonds + valid creds; boot 10×; the
device must never fall back to AP.

### W2 — `/api/wifi_scan` JSON silently truncates → malformed JSON in dense RF
**Severity: low-medium (breaks onboarding UX exactly when it's needed). Effort: small.**

`wifi_scan_json()` ([wifi_net.cpp:364-391](src/wifi_net.cpp#L364-L391)) is
serialized into the shared 512-byte `body`
([web_api.cpp:316](src/web_api.cpp#L316), [318-327](src/web_api.cpp#L318-L327)).
Worst case is `SCAN_MAX` (16) networks × (~40 B overhead + up to 32-char
SSID) ≈ 1.2 KB. In an apartment block the buffer runs out mid-array; the
guards prevent overflow but the result is **invalid JSON** (unterminated
string/array), so the portal's `r.json()` throws and the dropdown breaks.

**Fix (pick one):** give the scan route its own `static char scan_body[1280]`;
or cap serialization at the strongest N networks that provably fit (stop
before a full entry would overflow and close the array cleanly — the sort
already puts strongest first, which is what the user needs).

**Verify:** host-side unit-ish check: feed 16 max-length SSIDs through
`wifi_scan_json` into 512 B and `JSON.parse` the result; then a real scan in a
dense environment.

### W3 — AP-onboarding mode exposes the full config/bond API on an OPEN network with BT never initialized
**Severity: low (transient mode, needs a nearby actor), defense-in-depth. Effort: small.**

In AP mode, `fs_open_custom` forwards every `/api/*` GET to the shared
handlers ([web_api.cpp:292-307](src/web_api.cpp#L292-L307)) and
`httpd_post_begin` accepts every POST route
([web_api.cpp:697-706](src/web_api.cpp#L697-L706)). The portal page itself
only uses `wifi_scan`/`wifi_provision`, but anyone who joins the open
`DS5-Setup-XXXX` AP can: POST `/api/config` (rewrites + persists settings),
POST `/api/bonds action=forgetall` (`gap_delete_all_link_keys()` — the TLV
exists even though `bt_init()` was skipped, so stored bonds can really be
erased), or poke `bt_start_pairing()` on a never-powered stack (mostly
no-ops, but unverified territory).

**Fix:** in AP mode, allowlist exactly `/api/wifi_scan`,
`/api/wifi_provision`, `/api/wifi_provision_result` (GET + POST); 404/reject
the rest. Two small gates: one in `fs_open_custom`'s `is_portal_api` check,
one in `httpd_post_begin`.

**Verify:** onboarding flow end-to-end still works; `curl` of `/api/bonds`
against the AP returns 404.

### W4 — an aborted POST applies a partial body (and burns a flash save)
**Severity: low (robustness). Effort: small.**

lwIP httpd calls `httpd_post_finished()` when a POST connection dies
mid-body. Our handler ([web_api.cpp:726-763](src/web_api.cpp#L726-L763)) can't
tell "complete" from "aborted": it parses whatever partial bytes arrived —
a truncated `inactive_time=60` can apply as `6`... wait, `clampi(6,5,60)` = 6,
i.e. a silently wrong value — and runs `config_save()` (flash erase/program)
for a request the client never finished. Fix: record `content_len` in
`httpd_post_begin` and skip the apply in `httpd_post_finished` when
`post_pos != content_len` (respond with the failure route).

**Verify:** normal saves unaffected; `curl` with `Content-Length` larger than
the sent body must NOT change config.

---

## 3. Dead code / cleanup

### D1 — `wake_on_bt_disconnect()` has zero callers
**Severity: low. Effort: trivial — but decide intent first.**

[wake.cpp:340-346](src/wake.cpp#L340-L346), declared at
[wake.h:14](src/wake.h#L14). Nothing calls it (grep confirms; the BT
disconnect handler only does the variant swap + battery-LED reset). It was
presumably meant to run from `HCI_EVENT_DISCONNECTION_COMPLETE` to reset the
wake FSM + button-diff state when the controller goes away. Today the FSM
re-arms unconditionally in `tud_suspend_cb`, and `prev_b7/8/9` staleness
across a reconnect merely causes one extra (harmless, armable-gated) wake
attempt. Either wire it into the disconnect handler (safer bookkeeping) or
delete it and its declarations. Recommendation: wire it in — it's what the
stale-button-bytes reset exists for — and note "needs HW verification" for
the suspend/wake matrix.

### D2 — `set_config(const uint8_t*, uint16_t)` overload is WebHID-era dead code
**Severity: trivial.**

[config.cpp:353-362](src/config.cpp#L353-L362), decl
[config.h:124](src/config.h#L124). No callers (the web UI uses the
`Config_body&` overload). Delete both. (Grep `set_config(` after removal:
only the reference overload and its call sites remain.)

---

## 4. Build / CI

### C1 — `ENABLE_WAKE_LINK=ON` is never compiled anywhere
**Severity: low (rot guard). Effort: trivial.**

`wake_link.cpp` only compiles under `-DENABLE_WAKE_LINK=ON`
([CMakeLists.txt:320-324](CMakeLists.txt#L320-L324)), and no CI job builds it
([build.yml](.github/workflows/build.yml) matrix: standard, debug, waveshare,
no-batt-led, no-wake, no-wifi, picow). Any refactor of `wake.cpp`/`wake_link.h`
can silently break it. Cheapest fix: add `-DENABLE_WAKE_LINK=ON` to one
existing compile-check job (e.g. the no-batt-led build) rather than adding a
whole new job.

---

## 5. Non-findings (checked, deliberately NOT flagged — don't re-litigate)

* **`queue_try_remove(&q, NULL)` drops** (bt.cpp:817, audio.cpp:117/257/342):
  the pico-SDK guards `if (data)` before the memcpy — legal and intended.
  (The N7 issue is *uninitialized queues*, not the NULL data pointer.)
* **Concurrent page streams sharing `cur_page_body`/`PAGE_HDR`**
  (web_api.cpp:109-128): up to 3 parallel httpd connections can stream, but
  within a mode there is only ever ONE page (portal in AP, config page in
  STA), so the module-static is always re-set to identical values. Safe.
* **Interrupt-channel creation on fresh pair**: `HCI_EVENT_ENCRYPTION_CHANGE`
  only creates the control channel; the interrupt channel arrives as an
  incoming L2CAP connection from the controller (accepted at bt.cpp:996-1002).
  Looks asymmetric, is correct, HW-verified.
* **Raw `hci_send_cmd` replies inside HCI event handlers** (link-key reply,
  auth request, encryption request, user-confirmation, pin): event context =
  slot just freed; upstream-proven. Only the non-event-context disconnects
  (N3) need migration.
* **Immediate (non-deferred) blacklist persist in `bt_blacklist_add`**:
  deliberate — forget is a user action where a flash blip is acceptable;
  only the re-pair removal defers (comment at bt.cpp:326-328). The
  4-writes-in-a-row on forget-all is ugly but rare and user-triggered.
* **Blacklist capacity = `NVM_NUM_LINK_KEYS` (4) with silent drop when full**
  (bt.cpp:333): reachable only by forgetting ≥5 distinct controllers without
  ever re-pairing one; the un-blacklisted 5th can then auto-reconnect after a
  forget. Accepted as an edge; if it ever bites, evict-oldest is the fix.
* **`gap_inquiry_start(30)`** is 30 × 1.28 s = 38.4 s, not "30 s" as comments
  say. Cosmetic; don't change the value.
* **`bt_get_status()` reading `interrupt_in_data[52]` unlocked**: everything
  (BT run loop, httpd, main loop) is core0; core1 is Opus-only. Fine.
* **`iSerialNumber = 0`** in the device descriptor with an unused
  `STRID_SERIAL` case: matches a real DualSense (no serial string); the dead
  switch case is harmless.
* **`check_dse`/`is_dse` staleness across a disconnect**: re-initialized by
  `init_feature()` on the next HID-open before any USB connect. Fine.
* **AP-mode `config_save()` direct-write path in config.cpp:316-320**:
  redundant with the flash_safety helper but kept deliberately (comment says
  so). Leave it.

---

## 6. Suggested execution order for an acting LLM

1. **Commit 1 (pure hardening, no behavior change intended):** N4, N6, W2,
   W4, D2. Build all variants; grep removed symbols.
2. **Commit 2 (BT state-machine fixes):** N1, N2, N5 — one commit, they all
   touch the same handlers. **Needs HW pass**: fresh pair (both directions),
   pairing-window expiry with a bond, forget-all, reconnect-after-poweroff
   with audio streaming.
3. **Commit 3:** N3 (`gap_disconnect` migration). Separate commit so a
   regression bisects cleanly. **Needs HW pass** (the full
   disconnect-path matrix listed in N3).
4. **Commit 4:** N7 + C1 together (PicoW guard + the wake-link compile guard).
5. **Commit 5:** W3 (AP-mode API allowlist) + D1 (wire or delete
   `wake_on_bt_disconnect`). HW: onboarding flow + suspend/wake matrix.
6. **W1 stays deferred** unless this branch is being hardened for the issue-#2
   reporter rollout — in that case do option 1 (BADAUTH-only wipe) as the
   minimal change.

Do NOT fold feature work into these commits. Anything touching bt.cpp's
pairing flow must keep the guardrail comments (section 0) intact — several
document HW-observed failure modes that will otherwise be re-introduced by a
well-meaning cleanup.
