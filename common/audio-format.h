/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_FORMAT_H
#define AUDIO_FORMAT_H

#include <alsa/asoundlib.h>
#include <glib.h>

#include <audio-manager/audio-manager-types.h>

/**
 * Internal PCM sample format shared by common ALSA helpers and backends.
 */
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0, /**< Unknown or unsupported format */
    AUDIO_FORMAT_S8,          /**< Signed 8 bit PCM */
    AUDIO_FORMAT_U8,          /**< Unsigned 8 bit PCM */
    AUDIO_FORMAT_S16_LE,      /**< Signed 16 bit little endian PCM */
    AUDIO_FORMAT_S16_BE,      /**< Signed 16 bit big endian PCM */
    AUDIO_FORMAT_S24_LE,      /**< Signed 24 bit little endian PCM in 32 bit container */
    AUDIO_FORMAT_S24_BE,      /**< Signed 24 bit big endian PCM in 32 bit container */
    AUDIO_FORMAT_S24_3LE,     /**< Packed signed 24 bit little endian PCM */
    AUDIO_FORMAT_S24_3BE,     /**< Packed signed 24 bit big endian PCM */
    AUDIO_FORMAT_S32_LE,      /**< Signed 32 bit little endian PCM */
    AUDIO_FORMAT_S32_BE,      /**< Signed 32 bit big endian PCM */
    AUDIO_FORMAT_FLOAT32_LE,  /**< 32 bit floating point little endian PCM */
    AUDIO_FORMAT_FLOAT32_BE,  /**< 32 bit floating point big endian PCM */
} AudioFormat;

/**
 * Parse an internal format from a configuration string.
 */
gboolean
audio_format_from_string(const gchar *string,
                         AudioFormat *format);

/**
 * Convert an internal format to an ALSA PCM format.
 */
snd_pcm_format_t
audio_format_to_alsa(AudioFormat format);

/**
 * Convert an ALSA PCM format to the internal format enum.
 */
AudioFormat
audio_format_from_alsa(snd_pcm_format_t format);

/**
 * Convert a public stream sample format to the common internal format.
 *
 * @param format Public stream sample format
 * @return Matching internal format or AUDIO_FORMAT_UNKNOWN
 */
AudioFormat
audio_format_from_sample_format(AudioManagerSampleFormat format);

/**
 * Convert a common internal format to the public stream sample format.
 *
 * @param format Internal PCM format
 * @return Matching public stream sample format
 */
AudioManagerSampleFormat
audio_format_to_sample_format(AudioFormat format);

#endif /* AUDIO_FORMAT_H */
