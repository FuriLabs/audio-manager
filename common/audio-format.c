/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/audio-format.h"

#include <glib.h>
#include <string.h>

typedef struct {
    AudioFormat format;
    const gchar *name;
    snd_pcm_format_t alsa_format;
} AudioFormatInfo;

static const AudioFormatInfo format_info[] = {
    { AUDIO_FORMAT_S8, "S8", SND_PCM_FORMAT_S8 },
    { AUDIO_FORMAT_U8, "U8", SND_PCM_FORMAT_U8 },
    { AUDIO_FORMAT_S16_LE, "S16_LE", SND_PCM_FORMAT_S16_LE },
    { AUDIO_FORMAT_S16_BE, "S16_BE", SND_PCM_FORMAT_S16_BE },
    { AUDIO_FORMAT_S24_LE, "S24_LE", SND_PCM_FORMAT_S24_LE },
    { AUDIO_FORMAT_S24_BE, "S24_BE", SND_PCM_FORMAT_S24_BE },
    { AUDIO_FORMAT_S24_3LE, "S24_3LE", SND_PCM_FORMAT_S24_3LE },
    { AUDIO_FORMAT_S24_3BE, "S24_3BE", SND_PCM_FORMAT_S24_3BE },
    { AUDIO_FORMAT_S32_LE, "S32_LE", SND_PCM_FORMAT_S32_LE },
    { AUDIO_FORMAT_S32_BE, "S32_BE", SND_PCM_FORMAT_S32_BE },
    { AUDIO_FORMAT_FLOAT32_LE, "FLOAT32_LE", SND_PCM_FORMAT_FLOAT_LE },
    { AUDIO_FORMAT_FLOAT32_BE, "FLOAT32_BE", SND_PCM_FORMAT_FLOAT_BE },
};

gboolean
audio_format_from_string(const gchar *string,
                         AudioFormat *format)
{
    gsize i;

    if (string == NULL || format == NULL)
        return FALSE;

    for (i = 0; i < G_N_ELEMENTS(format_info); i++) {
        if (g_ascii_strcasecmp(format_info[i].name, string) == 0) {
            *format = format_info[i].format;
            return TRUE;
        }
    }

    return FALSE;
}

snd_pcm_format_t
audio_format_to_alsa(AudioFormat format)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(format_info); i++) {
        if (format_info[i].format == format)
            return format_info[i].alsa_format;
    }

    return SND_PCM_FORMAT_UNKNOWN;
}

AudioFormat
audio_format_from_alsa(snd_pcm_format_t format)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(format_info); i++) {
        if (format_info[i].alsa_format == format)
            return format_info[i].format;
    }

    return AUDIO_FORMAT_UNKNOWN;
}

AudioFormat
audio_format_from_sample_format(AudioManagerSampleFormat format)
{
    switch (format) {
    case AUDIO_MANAGER_SAMPLE_S16_LE:
        return AUDIO_FORMAT_S16_LE;
    case AUDIO_MANAGER_SAMPLE_S24_LE:
        return AUDIO_FORMAT_S24_LE;
    case AUDIO_MANAGER_SAMPLE_S32_LE:
        return AUDIO_FORMAT_S32_LE;
    default:
        return AUDIO_FORMAT_UNKNOWN;
    }
}

AudioManagerSampleFormat
audio_format_to_sample_format(AudioFormat format)
{
    switch (format) {
    case AUDIO_FORMAT_S16_LE:
        return AUDIO_MANAGER_SAMPLE_S16_LE;
    case AUDIO_FORMAT_S24_LE:
        return AUDIO_MANAGER_SAMPLE_S24_LE;
    case AUDIO_FORMAT_S32_LE:
        return AUDIO_MANAGER_SAMPLE_S32_LE;
    default:
        return AUDIO_MANAGER_SAMPLE_S16_LE;
    }
}
