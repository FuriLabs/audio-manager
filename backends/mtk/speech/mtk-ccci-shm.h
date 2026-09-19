/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef CCCI_SHM_H
#define CCCI_SHM_H

#include <glib.h>

#include "common/alsa/alsa-card.h"

typedef struct MtkCcciShm MtkCcciShm;

/**
 * Open the MediaTek CCCI raw audio shared memory interface.
 */
MtkCcciShm *
mtk_ccci_shm_new(const gchar *device,
                 AudioAlsaCard *card,
                 const gchar *init_control,
                 GError **error);

/**
 * Close and unmap CCCI shared memory.
 */
void
mtk_ccci_shm_free(MtkCcciShm *shm);

/**
 * Reset shared memory state.
 */
gint
mtk_ccci_shm_reset(MtkCcciShm *shm);

/**
 * Write a speech parameter image into the CCCI shared memory region.
 */
gint
mtk_ccci_shm_write_speech_params(MtkCcciShm *shm,
                                 gconstpointer data,
                                 gsize size,
                                 guint32 *write_index);

/**
 * Read modem to AP typed data from CCCI shared memory.
 */
gint
mtk_ccci_shm_read_md_data(MtkCcciShm *shm,
                          gpointer data,
                          guint16 *data_type,
                          guint16 *data_size,
                          guint16 payload_length,
                          guint32 read_index);

#endif /* CCCI_SHM_H */
