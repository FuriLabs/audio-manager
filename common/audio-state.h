/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_STATE_H
#define AUDIO_STATE_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

/**
 * Convert a call state enum to a static string.
 */
const gchar *
audio_call_state_to_string(AudioManagerCallState state);

/**
 * Convert a speech bandwidth enum to a static string.
 */
const gchar *
audio_speech_band_to_string(AudioManagerSpeechBand band);

#endif /* AUDIO_STATE_H */
