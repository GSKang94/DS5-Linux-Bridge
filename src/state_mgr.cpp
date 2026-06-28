//
// Created by awalol on 2026/5/15.
//

#include <cstddef>
#include <cstring>

#include "utils.h"

namespace {
    constexpr size_t kAudioControlOffset = offsetof(SetStateData, MuteLightMode) - sizeof(uint8_t);
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kMotorPowerLevelOffset = offsetof(SetStateData, HostTimestamp) + sizeof(uint32_t);
    constexpr size_t kAudioControl2Offset = kMotorPowerLevelOffset + sizeof(uint8_t);
    constexpr size_t kHapticLowPassFilterOffset = offsetof(SetStateData, LightFadeAnimation) - 2 * sizeof(uint8_t);
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

volatile bool g_firmware_mic_muted = false;
volatile bool g_host_hid_manages_mute = false;
volatile uint8_t g_last_uac_mute = 0xFF;

uint8_t state[63]{};

void state_reset_mute() {
    g_firmware_mic_muted = false;
    g_host_hid_manages_mute = false;
    g_last_uac_mute = 0xFF;
    state[8] = 0; // MuteLight::Off
    state[9] &= ~(1 << 4); // Clear MicMute bit (bit 4 of byte 9)
}

void state_set_local_mute(bool muted) {
    if (muted) {
        state[8] = 1; // MuteLight::On (solid orange)
        state[9] |= (1 << 4); // MicMute bit
    } else {
        state[8] = 0; // MuteLight::Off
        state[9] &= ~(1 << 4); // Clear MicMute bit
    }
}

void state_init() {
    memcpy(state, state_init_data, sizeof(state));
    state_reset_mute();
}

void state_get(uint8_t *data, const uint8_t size) {
    if (size > sizeof(state)) {
        // state[] is 63 bytes; copying more would OOB-read state and OOB-write
        // caller's buffer. Refuse rather than memcpy past the source.
        printf("[StateMgr] Error: state_get size %u > %u; refused\n",
               size, static_cast<unsigned>(sizeof(state)));
        return;
    }
    memcpy(data, state, size);
}

void state_update(const uint8_t *data, const uint8_t size) {
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
            memcpy(state + offset, data + offset, length);
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
    set_bit(state[0], 0, update.EnableRumbleEmulation);
    set_bit(state[0], 1, update.UseRumbleNotHaptics);
    set_bit(state[38], 2, update.EnableImprovedRumbleEmulation);
    copy_if_allowed(
        update.UseRumbleNotHaptics ||
            update.EnableRumbleEmulation ||
            update.EnableImprovedRumbleEmulation,
        offsetof(SetStateData, RumbleEmulationRight),
        2
    );

    /*copy_if_allowed(
        update.AllowHeadphoneVolume,
        offsetof(SetStateData, VolumeHeadphones),
        sizeof(update.VolumeHeadphones)
    );*/
    /*copy_if_allowed(
        update.AllowSpeakerVolume,
        offsetof(SetStateData, VolumeSpeaker),
        sizeof(update.VolumeSpeaker)
    );*/
    /*copy_if_allowed(
        update.AllowMicVolume,
        offsetof(SetStateData, VolumeMic),
        sizeof(update.VolumeMic)
    );*/
    /*copy_if_allowed(
        update.AllowAudioControl,
        kAudioControlOffset,
        sizeof(uint8_t)
    );*/

    if ((update.AllowMuteLight && update.MuteLightMode == MuteLight::On) ||
        (update.AllowAudioMute && update.MicMute)) {
        g_host_hid_manages_mute = true;
    }

    if (g_host_hid_manages_mute) {
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

    /*copy_if_allowed(
        update.AllowMotorPowerLevel,
        kMotorPowerLevelOffset,
        sizeof(uint8_t)
    );*/
    /*copy_if_allowed(
        update.AllowAudioControl2,
        kAudioControl2Offset,
        sizeof(uint8_t)
    );*/
    /*copy_if_allowed(
        update.AllowHapticLowPassFilter,
        kHapticLowPassFilterOffset,
        sizeof(uint8_t)
    );*/

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
