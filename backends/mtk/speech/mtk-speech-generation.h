/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_SPEECH_GENERATION_H
#define MTK_SPEECH_GENERATION_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

#include "backends/mtk/mtk-config.h"
#include "backends/mtk/speech/mtk-speech-protocol.h"
#include "common/alsa/alsa-card.h"

typedef struct MtkSpeechGeneration MtkSpeechGeneration;

/**
 * Generation specific MediaTek speech parameter/shared memory implementation.
 *
 * This separates the generic MTK speech state machine from GEN93/95/97 style
 * parameter image and AP/modem data layouts.
 */
typedef struct MtkSpeechGenerationOps {
    const gchar *name; /**< Generation name used by the device configuration */

    /**
     * Create generation specific state.
     */
    gpointer (*create)(const MtkConfig *config,
                       AudioAlsaCard *card,
                       GError **error);

    /**
     * Destroy generation specific state.
     */
    void (*destroy)(gpointer data);

    /**
     * Reset shared memory/parameter transport state after modem recovery.
     */
    gint (*reset)(gpointer data);

    /**
     * Build MtkSphInfo for a speech start and device change command.
     */
    gint (*prepare_info)(gpointer data,
                         AudioManagerOutputDevice output_device,
                         AudioManagerInputDevice input_device,
                         MtkSphInfo *info);

    /**
     * Select the current speech parameter volume index.
     */
    void (*set_volume_index)(gpointer data,
                             guint index);

    /**
     * Select Bluetooth WB/NREC parameter state.
     */
    void (*set_bluetooth)(gpointer data,
                          gboolean wideband,
                          gboolean nrec);

    /**
     * Read typed modem to AP data from the generation transport.
     */
    gint (*read_md_data)(gpointer data,
                         gpointer buffer,
                         guint16 *data_type,
                         guint16 *data_size,
                         guint16 payload_length,
                         guint32 read_index);

} MtkSpeechGenerationOps;

/**
 * Create the configured MediaTek speech generation implementation.
 */
MtkSpeechGeneration *
mtk_speech_generation_new(const MtkConfig *config,
                          AudioAlsaCard *card,
                          GError **error);

/**
 * Free the speech generation.
 */
void
mtk_speech_generation_free(MtkSpeechGeneration *generation);

/**
 * Reset generation specific state.
 */
gint
mtk_speech_generation_reset(MtkSpeechGeneration *generation);

/**
 * Build the speech information structure for the selected endpoints.
 */
gint
mtk_speech_generation_prepare_info(MtkSpeechGeneration *generation,
                                   AudioManagerOutputDevice output_device,
                                   AudioManagerInputDevice input_device,
                                   MtkSphInfo *info);

/**
 * Set the current parameter volume index.
 */
void
mtk_speech_generation_set_volume_index(MtkSpeechGeneration *generation,
                                       guint index);

/**
 * Set generation specific Bluetooth speech parameter state.
 */
void
mtk_speech_generation_set_bluetooth(MtkSpeechGeneration *generation,
                                    gboolean wideband,
                                    gboolean nrec);

/**
 * Read typed modem data through the generation transport.
 */
gint
mtk_speech_generation_read_md_data(MtkSpeechGeneration *generation,
                                   gpointer buffer,
                                   guint16 *data_type,
                                   guint16 *data_size,
                                   guint16 payload_length,
                                   guint32 read_index);

/**
 * GEN97 implementation exported to the generation registry.
 */
extern const MtkSpeechGenerationOps mtk_speech_gen97_ops;

#endif /* MTK_SPEECH_GENERATION_H */
