/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-speech-generation.h"

#include <errno.h>

#include "backends/mtk/params/mtk-speech-param.h"
#include "backends/mtk/speech/mtk-ccci-shm.h"
#include "backends/mtk/speech/mtk-usip.h"

typedef struct {
    const MtkConfig *config;
    AudioAlsaCard *card;
    MtkCcciShm *ccci_shm;
    MtkUsip *usip;
    MtkSpeechParamProvider *params;
} MtkSpeechGen97;

static gpointer
gen97_create(const MtkConfig *config,
             AudioAlsaCard *card,
             GError **error)
{
    MtkSpeechGen97 *gen97;

    gen97 = g_new0(MtkSpeechGen97, 1);
    gen97->config = config;
    gen97->card = card;

    gen97->params = mtk_speech_param_provider_new(config, error);
    if (gen97->params == NULL)
        goto fail;

    return gen97;

fail:
    if (gen97 != NULL) {
        mtk_speech_param_provider_free(gen97->params);
        mtk_usip_free(gen97->usip);
        mtk_ccci_shm_free(gen97->ccci_shm);
        g_free(gen97);
    }
    return NULL;
}

static gint
gen97_open_usip(MtkSpeechGen97 *gen97)
{
    GError *error = NULL;

    if (!gen97->config->use_usip || gen97->usip != NULL)
        return 0;

    g_debug("initializing MediaTek USIP speech shared memory");

    gen97->usip = mtk_usip_new(gen97->config->usip_device,
                               gen97->card,
                               gen97->config->usip_init_control,
                               gen97->config->usip_write_index_control,
                               &error);
    if (gen97->usip == NULL) {
        if (error != NULL) {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        return -EIO;
    }

    return 0;
}

static gint
gen97_open_ccci_shared_memory(MtkSpeechGen97 *gen97)
{
    GError *error = NULL;

    if (!gen97->config->use_ccci_shared_memory || gen97->ccci_shm != NULL)
        return 0;

    g_debug("initializing MediaTek CCCI speech shared memory");

    gen97->ccci_shm = mtk_ccci_shm_new(gen97->config->shared_memory_device,
                                       gen97->card,
                                       gen97->config->ccci_shm_init_control,
                                       &error);
    if (gen97->ccci_shm == NULL) {
        if (error != NULL) {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        return -EIO;
    }

    return 0;
}

static void
gen97_destroy(gpointer data)
{
    MtkSpeechGen97 *gen97 = data;

    if (gen97 == NULL)
        return;

    mtk_speech_param_provider_free(gen97->params);
    mtk_usip_free(gen97->usip);
    mtk_ccci_shm_free(gen97->ccci_shm);
    g_free(gen97);
}

static gint
gen97_reset(gpointer data)
{
    MtkSpeechGen97 *gen97 = data;
    gint ret = 0;
    gint tmp;

    if (gen97 == NULL)
        return -EINVAL;

    if (gen97->ccci_shm != NULL) {
        tmp = mtk_ccci_shm_reset(gen97->ccci_shm);
        if (tmp < 0)
            ret = tmp;
    }

    if (gen97->usip != NULL) {
        tmp = mtk_usip_reset(gen97->usip);
        if (tmp < 0 && ret == 0)
            ret = tmp;
    }

    return ret;
}

static gint
gen97_prepare_info(gpointer data,
                   AudioManagerOutputDevice output_device,
                   AudioManagerInputDevice input_device,
                   MtkSphInfo *info)
{
    MtkSpeechGen97 *gen97 = data;
    guint8 *params = NULL;
    gsize params_size = 0;
    guint32 write_index = 0;
    gint ret;

    if (gen97 == NULL || info == NULL)
        return -EINVAL;

    info->sph_param_path = MTK_SPH_PARAM_VIA_PAYLOAD;
    info->sph_param_valid = MTK_SPH_PARAM_INVALID;

    /*
     * CCCI MD->AP data can arrive as soon as SPH_ON is processed. Initialize
     * and format the CCCI ring before sending any speech command so a later
     * mailbox notification never has to initialize (and reset) a ring that
     * already contains modem data.
     */
    if (gen97->config->use_ccci_shared_memory) {
        ret = gen97_open_ccci_shared_memory(gen97);
        if (ret < 0)
            g_warning("GEN97 CCCI shared memory unavailable: %s",
                      g_strerror(-ret));
    }

    ret = mtk_speech_param_provider_build(gen97->params,
                                          output_device,
                                          input_device,
                                          &params,
                                          &params_size);
    if (ret < 0 || params == NULL || params_size == 0) {
        if (ret < 0)
            g_debug("GEN97 speech parameter image unavailable: %s. using modem defaults",
                    g_strerror(-ret));
        else
            g_debug("GEN97 speech parameters unavailable. using modem defaults");
        g_free(params);
        return 0;
    }
    if (params_size > G_MAXUINT32 || params_size > G_MAXUINT16) {
        g_warning("GEN97 speech parameter image is too large (%zu bytes). using modem defaults",
                  params_size);
        g_free(params);
        return 0;
    }

    /*
     * USIP and CCCI SHM can both carry the speech parameter image. GEN97
     * prefers USIP when configured so CCCI SHM is selected only when USIP is
     * disabled.
     */
    if (gen97->config->use_usip)
        ret = gen97_open_usip(gen97);
    else if (gen97->ccci_shm != NULL)
        ret = 0;
    else
        ret = -ENODEV;
    if (ret < 0) {
        g_warning("GEN97 shared memory unavailable: %s. using modem defaults",
                  g_strerror(-ret));
        g_free(params);
        return 0;
    }

    if (gen97->config->use_usip && gen97->usip != NULL) {
        ret = mtk_usip_write_speech_params(gen97->usip,
                                           params,
                                           params_size,
                                           &write_index);
        if (ret == 0) {
            info->sph_param_path = MTK_SPH_PARAM_VIA_SHM_USIP;
            info->sph_param_valid = MTK_SPH_PARAM_VALID;
            info->sph_param_usip_length = (guint32)params_size;
            info->sph_param_usip_index = write_index;
            g_debug("GEN97 speech params ready via USIP length=%zu write_index=%u",
                    params_size, write_index);
        }
    } else if (gen97->config->use_ccci_shared_memory && gen97->ccci_shm != NULL) {
        ret = mtk_ccci_shm_write_speech_params(gen97->ccci_shm,
                                               params,
                                               params_size,
                                               &write_index);
        if (ret == 0) {
            info->sph_param_path = MTK_SPH_PARAM_VIA_SHM_CCCI;
            info->sph_param_valid = MTK_SPH_PARAM_VALID;
            info->sph_param_length = (guint16)params_size;
            info->sph_param_index = (guint16)write_index;
            g_debug("GEN97 speech params ready via CCCI SHM length=%zu write_index=%u",
                    params_size, write_index);
        }
    } else {
        ret = -ENODEV;
    }

    if (ret < 0) {
        g_warning("GEN97 speech parameter upload failed: %s. using modem defaults",
                  g_strerror(-ret));
        info->sph_param_path = MTK_SPH_PARAM_VIA_PAYLOAD;
        info->sph_param_valid = MTK_SPH_PARAM_INVALID;
        ret = 0;
    }

    g_free(params);
    return ret;
}

static void
gen97_set_volume_index(gpointer data, guint index)
{
    MtkSpeechGen97 *gen97 = data;
    if (gen97 != NULL)
        mtk_speech_param_provider_set_volume_index(gen97->params, index);
}

static void
gen97_set_bluetooth(gpointer data, gboolean wideband, gboolean nrec)
{
    MtkSpeechGen97 *gen97 = data;
    if (gen97 != NULL)
        mtk_speech_param_provider_set_bluetooth(gen97->params, wideband, nrec);
}

static gint
gen97_read_md_data(gpointer data,
                   gpointer buffer,
                   guint16 *data_type,
                   guint16 *data_size,
                   guint16 payload_length,
                   guint32 read_index)
{
    MtkSpeechGen97 *gen97 = data;

    if (gen97 == NULL)
        return -EINVAL;
    if (gen97->ccci_shm == NULL)
        return -ENODEV;
    return mtk_ccci_shm_read_md_data(gen97->ccci_shm, buffer, data_type,
                                     data_size, payload_length, read_index);
}

const MtkSpeechGenerationOps mtk_speech_gen97_ops = {
    .name = "gen97",
    .create = gen97_create,
    .destroy = gen97_destroy,
    .reset = gen97_reset,
    .prepare_info = gen97_prepare_info,
    .set_volume_index = gen97_set_volume_index,
    .set_bluetooth = gen97_set_bluetooth,
    .read_md_data = gen97_read_md_data,
};
