/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef ALSA_CARD_H
#define ALSA_CARD_H

#include <alsa/asoundlib.h>
#include <glib.h>

#include "common/audio-format.h"

/**
 * PCM direction used when resolving named ALSA devices.
 */
typedef enum {
    AUDIO_DIRECTION_OUTPUT = 0, /**< Playback PCM */
    AUDIO_DIRECTION_INPUT,      /**< Capture PCM */
} AudioDirection;

/**
 * Open ALSA card metadata and control handle.
 */
typedef struct AudioAlsaCard {
    gint index;      /**< ALSA card index */
    gchar *id;       /**< ALSA card ID */
    gchar *name;     /**< Human readable card name */
    snd_ctl_t *ctl;  /**< Open ALSA control handle */
} AudioAlsaCard;

/**
 * Open an ALSA card by ID/name or configured selector.
 *
 * @param name Card selector
 * @param error Error return location
 * @return Open card or NULL on failure
 */
AudioAlsaCard *
audio_alsa_card_open(const gchar *name,
                     GError **error);

/**
 * Close and free an ALSA card.
 */
void
audio_alsa_card_close(AudioAlsaCard *card);

/**
 * Find the PCM device index whose ALSA name matches a logical name.
 *
 * @param card Open card
 * @param name PCM name to find
 * @param direction Playback or capture direction
 * @return PCM device index or a negative errno value
 */
gint
audio_alsa_card_find_pcm(AudioAlsaCard *card,
                         const gchar *name,
                         AudioDirection direction);

#endif /* AUDIO_MANAGER_COMMON_ALSA_ALSA_CARD_H */
