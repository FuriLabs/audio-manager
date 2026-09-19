/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef ALSA_PCM_H
#define ALSA_PCM_H

#include <time.h>

#include <glib.h>

#include "common/alsa/alsa-card.h"
#include "common/audio-format.h"

/**
 * Configuration used when opening a named ALSA PCM.
 */
typedef struct {
    const gchar *device;              /**< Logical PCM name */
    AudioDirection direction;         /**< Playback or capture */
    AudioFormat format;               /**< PCM sample format */
    guint rate;                       /**< Sample rate in Hz */
    guint channels;                   /**< Channel count */
    guint period_size;                /**< Requested period size in frames */
    guint period_count;               /**< Requested period count */
    guint start_threshold;            /**< ALSA start threshold in frames */
    gboolean nonblock;                /**< Open PCM in non blocking mode */
    gboolean disable_stop_threshold;  /**< Disable automatic ALSA stop threshold */
} AudioAlsaPcmConfig;

typedef struct AudioAlsaPcm AudioAlsaPcm;

/**
 * Open and configure a named PCM on an ALSA card.
 */
AudioAlsaPcm *
audio_alsa_pcm_open(AudioAlsaCard *card,
                    const AudioAlsaPcmConfig *config,
                    GError **error);

/**
 * Close and free a PCM.
 */
void
audio_alsa_pcm_close(AudioAlsaPcm *pcm);

/**
 * Explicitly start an ALSA PCM.
 */
gint
audio_alsa_pcm_start(AudioAlsaPcm *pcm);

/**
 * Drop/stop an ALSA PCM.
 */
gint
audio_alsa_pcm_stop(AudioAlsaPcm *pcm);

/**
 * Prepare an ALSA PCM after open or XRUN recovery.
 */
gint
audio_alsa_pcm_prepare(AudioAlsaPcm *pcm);

/**
 * Write interleaved PCM, including XRUN/suspend recovery.
 */
gssize
audio_alsa_pcm_write(AudioAlsaPcm *pcm,
                     gconstpointer data,
                     gsize bytes);

/**
 * Read interleaved PCM, including XRUN/suspend recovery.
 */
gssize
audio_alsa_pcm_read(AudioAlsaPcm *pcm,
                    gpointer data,
                    gsize bytes);

/**
 * Query ALSA available frames and current delay.
 */
gint
audio_alsa_pcm_avail_delay(AudioAlsaPcm *pcm,
                           snd_pcm_sframes_t *avail,
                           snd_pcm_sframes_t *delay);

/**
 * Query ALSA hardware timestamp and available frame count.
 */
gint
audio_alsa_pcm_get_htimestamp(AudioAlsaPcm *pcm,
                              snd_pcm_uframes_t *avail,
                              struct timespec *timestamp);

/**
 * Get negotiated bytes per PCM frame.
 */
gsize
audio_alsa_pcm_frame_bytes(AudioAlsaPcm *pcm);

/**
 * Get negotiated sample rate.
 */
guint
audio_alsa_pcm_rate(AudioAlsaPcm *pcm);

/**
 * Get negotiated channel count.
 */
guint
audio_alsa_pcm_channels(AudioAlsaPcm *pcm);

/**
 * Get negotiated period size in frames.
 */
guint
audio_alsa_pcm_period_size(AudioAlsaPcm *pcm);

/**
 * Get negotiated number of periods.
 */
guint
audio_alsa_pcm_period_count(AudioAlsaPcm *pcm);

/**
 * Get negotiated total buffer size in frames.
 */
guint
audio_alsa_pcm_buffer_size(AudioAlsaPcm *pcm);

/**
 * Get negotiated internal PCM format.
 */
AudioFormat
audio_alsa_pcm_format(AudioAlsaPcm *pcm);

#endif /* ALSA_PCM_H */
