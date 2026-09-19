/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/audio-state.h"

const gchar *
audio_call_state_to_string(AudioManagerCallState state)
{
    switch (state) {
    case AUDIO_MANAGER_CALL_STATE_IDLE:
        return "idle";
    case AUDIO_MANAGER_CALL_STATE_PREPARING:
        return "preparing";
    case AUDIO_MANAGER_CALL_STATE_STARTING:
        return "starting";
    case AUDIO_MANAGER_CALL_STATE_ACTIVE:
        return "active";
    case AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING:
        return "device-changing";
    case AUDIO_MANAGER_CALL_STATE_STOPPING:
        return "stopping";
    case AUDIO_MANAGER_CALL_STATE_ERROR:
        return "error";
    case AUDIO_MANAGER_CALL_STATE_RECOVERING:
        return "recovering";
    }

    return "unknown";
}

const gchar *
audio_speech_band_to_string(AudioManagerSpeechBand band)
{
    switch (band) {
    case AUDIO_MANAGER_SPEECH_BAND_NARROW:
        return "nb";
    case AUDIO_MANAGER_SPEECH_BAND_WIDE:
        return "wb";
    case AUDIO_MANAGER_SPEECH_BAND_SUPER_WIDE:
        return "swb";
    case AUDIO_MANAGER_SPEECH_BAND_UNKNOWN:
        return "unknown";
    }

    return "unknown";
}
