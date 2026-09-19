/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-speech-generation.h"

#include <errno.h>

#include <audio-manager/audio-manager-error.h>

struct MtkSpeechGeneration {
    const MtkSpeechGenerationOps *ops;
    gpointer data;
};

static const MtkSpeechGenerationOps *
find_generation(const gchar *name)
{
    if (g_strcmp0(name, mtk_speech_gen97_ops.name) == 0)
        return &mtk_speech_gen97_ops;

    return NULL;
}

MtkSpeechGeneration *
mtk_speech_generation_new(const MtkConfig *config,
                          AudioAlsaCard *card,
                          GError **error)
{
    MtkSpeechGeneration *generation;
    const MtkSpeechGenerationOps *ops;

    if (config == NULL || config->generation == NULL)
        return NULL;

    ops = find_generation(config->generation);
    if (ops == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_NOT_SUPPORTED,
                    "Unsupported MediaTek speech generation '%s'",
                    config->generation);
        return NULL;
    }

    generation = g_new0(MtkSpeechGeneration, 1);
    generation->ops = ops;
    generation->data = ops->create(config, card, error);
    if (generation->data == NULL) {
        g_free(generation);
        return NULL;
    }

    return generation;
}

void
mtk_speech_generation_free(MtkSpeechGeneration *generation)
{
    if (generation == NULL)
        return;

    if (generation->ops != NULL && generation->ops->destroy != NULL)
        generation->ops->destroy(generation->data);

    g_free(generation);
}

gint
mtk_speech_generation_reset(MtkSpeechGeneration *generation)
{
    if (generation == NULL || generation->ops == NULL ||
        generation->ops->reset == NULL)
        return -EINVAL;

    return generation->ops->reset(generation->data);
}

gint
mtk_speech_generation_prepare_info(MtkSpeechGeneration *generation,
                                   AudioManagerOutputDevice output_device,
                                   AudioManagerInputDevice input_device,
                                   MtkSphInfo *info)
{
    if (generation == NULL || generation->ops == NULL ||
        generation->ops->prepare_info == NULL)
        return -EINVAL;

    return generation->ops->prepare_info(generation->data,
                                         output_device,
                                         input_device,
                                         info);
}

void
mtk_speech_generation_set_volume_index(MtkSpeechGeneration *generation,
                                       guint index)
{
    if (generation != NULL && generation->ops != NULL &&
        generation->ops->set_volume_index != NULL)
        generation->ops->set_volume_index(generation->data, index);
}

void
mtk_speech_generation_set_bluetooth(MtkSpeechGeneration *generation,
                                    gboolean wideband,
                                    gboolean nrec)
{
    if (generation != NULL && generation->ops != NULL &&
        generation->ops->set_bluetooth != NULL)
        generation->ops->set_bluetooth(generation->data, wideband, nrec);
}

gint
mtk_speech_generation_read_md_data(MtkSpeechGeneration *generation,
                                   gpointer buffer,
                                   guint16 *data_type,
                                   guint16 *data_size,
                                   guint16 payload_length,
                                   guint32 read_index)
{
    if (generation == NULL || generation->ops == NULL ||
        generation->ops->read_md_data == NULL)
        return -ENOTSUP;
    return generation->ops->read_md_data(generation->data, buffer, data_type,
                                         data_size, payload_length, read_index);
}
