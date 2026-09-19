/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_USIP_H
#define MTK_USIP_H

#include <glib.h>

#include "common/alsa/alsa-card.h"

typedef struct MtkUsip MtkUsip;

/**
 * Open the MediaTek USIP shared memory interface used for speech parameters.
 */
MtkUsip *
mtk_usip_new(const gchar *device,
             AudioAlsaCard *card,
             const gchar *init_control,
             const gchar *write_index_control,
             GError **error);

/**
 * Close the USIP mapping and controls.
 */
void
mtk_usip_free(MtkUsip *usip);

/**
 * Reset the USIP shared memory write state.
 */
gint
mtk_usip_reset(MtkUsip *usip);

/**
 * Write a complete speech parameter image into USIP shared memory.
 */
gint
mtk_usip_write_speech_params(MtkUsip *usip,
                             gconstpointer data,
                             gsize size,
                             guint32 *write_index);

#endif /* MTK_USIP_H */
