//
// Created by awalol on 2026/5/15.
//

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "state_mgr.h"
#include "tier.h"
#include "utils.h"

namespace {
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kPlayerIndicatorsOffset = offsetof(SetStateData, LedRed) - sizeof(uint8_t);
}

static constexpr uint8_t state_init_data[63] = {
    0xfd, 0xf7, 0x0, 0x0,
    0x7f, 0x64, // Headphones, Speaker
    0xff, 0x9, 0x0, 0x0F, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
    0x7, 0x0, 0x0, 0x2, 0x1,
    0x00,
    0xff, 0xd7, 0x00 // RGB LED: R, G, B (Nijika Color!)✨
};

#if BT_MAX_SLOTS > 1
// PS5-style player-indicator connect-time default per seat (5-LED bar:
// center / 2 / 3 / 4 dots) so each player can tell which pad is theirs.
// Games override it via AllowPlayerIndicators as usual; single-slot builds
// keep the stock all-off default so behavior stays regression-identical.
static constexpr uint8_t slot_player_leds[] = {
    0x04, // player 1: -- -- ## -- --
    0x0A, // player 2: -- ## -- ## --
    0x15, // player 3: ## -- ## -- ##
    0x1B, // player 4: ## ## -- ## ##
};
static_assert(sizeof(slot_player_leds) >= BT_MAX_SLOTS,
              "slot_player_leds needs one pattern per slot");
#endif

volatile bool g_firmware_mic_muted = false;
volatile bool g_host_hid_manages_mute = false;
volatile uint8_t g_last_uac_mute = 0xFF;

static uint8_t state[BT_MAX_SLOTS][63]{};

void state_reset_mute() {
    g_firmware_mic_muted = false;
    g_host_hid_manages_mute = false;
    g_last_uac_mute = 0xFF;
    state[tier_audio_slot()][8] = 0; // MuteLight::Off
    state[tier_audio_slot()][9] &= ~(1 << 4); // Clear MicMute bit (bit 4 of byte 9)
}

void state_set_local_mute(bool muted) {
    uint8_t *st = state[tier_audio_slot()];
    if (muted) {
        st[8] = 1; // MuteLight::On (solid orange)
        st[9] |= (1 << 4); // MicMute bit
    } else {
        st[8] = 0; // MuteLight::Off
        st[9] &= ~(1 << 4); // Clear MicMute bit
    }
}

void state_slot_reset(uint8_t slot) {
    if (slot >= BT_MAX_SLOTS) return;
    memcpy(state[slot], state_init_data, sizeof(state_init_data));
#if BT_MAX_SLOTS > 1
    state[slot][kPlayerIndicatorsOffset] = slot_player_leds[slot];
#endif
}

void state_init() {
    for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
        state_slot_reset(slot);
    }
    state_reset_mute();
}

void state_get(uint8_t slot, uint8_t *data, const uint8_t size) {
    if (slot >= BT_MAX_SLOTS) return;
    if (size > sizeof(state[0])) {
        // Each row is 63 bytes; copying more would OOB-read state and OOB-write
        // caller's buffer. Refuse rather than memcpy past the source.
        printf("[StateMgr] Error: state_get size %u > %u; refused\n",
               size, static_cast<unsigned>(sizeof(state[0])));
        return;
    }
    memcpy(data, state[slot], size);
}

void state_update(uint8_t slot, const uint8_t *data, const uint8_t size) {
    if (slot >= BT_MAX_SLOTS) return;
    uint8_t *st = state[slot];
    // macOS sends a shorter SetStateData (47 bytes) than the full struct; the
    // trailing fields it omits are unused here, so accept anything >= 47 and let
    // the memcpy below over-read into zero-init padding. Rejecting short reports
    // broke rumble/LED control from macOS hosts.
    // (Ported from upstream awalol/DS5Dongle c47b7ed.)
    if (size < 47) {
        printf(
            "[StateMgr] Error: SetStateData needs at least 47 bytes, got %u\n",
            static_cast<unsigned>(size)
        );
        return;
    }

    SetStateData update{};
    memcpy(&update, data, sizeof(update));

    const auto copy_if_allowed = [&](const bool allowed, const size_t offset, const size_t length) {
        if (allowed) {
            memcpy(st + offset, data + offset, length);
        }
    };
    auto set_bit = [](uint8_t &byte, const int bit, const bool value) {
        byte = (byte & ~(1 << bit)) | (value << bit);
    };

    // Some games (e.g. Ninja Gaiden 4) send non-zero rumble values without
    // setting UseRumbleNotHaptics/EnableRumbleEmulation. Force rumble mode on
    // in that case so the emulation values below aren't gated out and dropped.
    // (Ported from upstream awalol/DS5Dongle 45b5a4f.)
    if (update.RumbleEmulationLeft > 0 || update.RumbleEmulationRight > 0) {
        update.UseRumbleNotHaptics = true;
    }
    set_bit(st[0], 0, update.EnableRumbleEmulation);
    set_bit(st[0], 1, update.UseRumbleNotHaptics);
    set_bit(st[38], 2, update.EnableImprovedRumbleEmulation);
    copy_if_allowed(
        update.UseRumbleNotHaptics ||
            update.EnableRumbleEmulation ||
            update.EnableImprovedRumbleEmulation,
        offsetof(SetStateData, RumbleEmulationRight),
        2
    );

    // Deliberately NOT forwarded from host: VolumeHeadphones/VolumeSpeaker/VolumeMic,
    // AudioControl, MotorPowerLevel, AudioControl2, HapticLowPassFilter -- the
    // firmware owns audio routing; forwarding these caused regressions.

    // Hybrid mute bookkeeping is an audio-path concern; only the audio slot's
    // reports may flip the global mute-ownership flags.
    const bool is_audio_slot = (slot == tier_audio_slot());
    if (is_audio_slot &&
        ((update.AllowMuteLight && update.MuteLightMode == MuteLight::On) ||
         (update.AllowAudioMute && update.MicMute))) {
        g_host_hid_manages_mute = true;
    }

    if (is_audio_slot && g_host_hid_manages_mute) {
        copy_if_allowed(
            update.AllowMuteLight,
            offsetof(SetStateData, MuteLightMode),
            sizeof(update.MuteLightMode)
        );

        copy_if_allowed(
            update.AllowAudioMute,
            kMuteControlOffset,
            sizeof(uint8_t)
        );

        if (update.AllowMuteLight) {
            g_firmware_mic_muted = (update.MuteLightMode == MuteLight::On);
        } else if (update.AllowAudioMute) {
            g_firmware_mic_muted = (update.MicMute != 0);
        }
    }

    copy_if_allowed(
        update.AllowRightTriggerFFB,
        offsetof(SetStateData, RightTriggerFFB),
        sizeof(update.RightTriggerFFB)
    );
    copy_if_allowed(
        update.AllowLeftTriggerFFB,
        offsetof(SetStateData, LeftTriggerFFB),
        sizeof(update.LeftTriggerFFB)
    );

    // MotorPowerLevel / AudioControl2 / HapticLowPassFilter also deliberately
    // NOT forwarded (see note above).

    copy_if_allowed(
        update.AllowColorLightFadeAnimation,
        offsetof(SetStateData, LightFadeAnimation),
        sizeof(update.LightFadeAnimation)
    );
    copy_if_allowed(
        update.AllowLightBrightnessChange,
        offsetof(SetStateData, LightBrightness),
        sizeof(update.LightBrightness)
    );
    copy_if_allowed(
        update.AllowPlayerIndicators,
        kPlayerIndicatorsOffset,
        sizeof(uint8_t)
    );
    copy_if_allowed(
        update.AllowLedColor,
        offsetof(SetStateData, LedRed),
        sizeof(update.LedRed) * 3
    );
}
