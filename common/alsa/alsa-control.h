/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef ALSA_CONTROL_H
#define ALSA_CONTROL_H

#include <glib.h>

#include "common/alsa/alsa-card.h"

/**
 * Set an ALSA control from a route/configuration string value.
 */
gint
audio_alsa_control_set_from_string(AudioAlsaCard *card,
                                   const gchar *name,
                                   const gchar *value);

/**
 * Set all values of an integer/enumerated control to one index/value.
 */
gint
audio_alsa_control_set_index(AudioAlsaCard *card,
                             const gchar *name,
                             glong value);

/**
 * Read the first integer value from an ALSA control.
 */
gint
audio_alsa_control_get_integer(AudioAlsaCard *card,
                               const gchar *name,
                               glong *value);

#endif /* ALSA_CONTROL_H */
