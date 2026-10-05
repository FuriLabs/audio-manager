/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/params/mtk-speech-param.h"

#include <errno.h>
#include <string.h>

#include <audio-manager/audio-manager-error.h>

#include "backends/mtk/params/mtk-audio-param.h"

#define MTK_SPEECH_PARAM_SIZE (48U * 1024U)
#define MTK_SPEECH_PARAM_EMI_RESERVED (3U * 1024U)
#define MTK_SPEECH_PARAM_PARSER_VERSION 0x0001U
#define MTK_SPEECH_PARAM_DEBUG_NUMBER 0x1010U

#define MTK_CHECK_GENERAL_BEGIN 0x000bU
#define MTK_CHECK_GENERAL_END   0x0b0bU
#define MTK_CHECK_DMNR_BEGIN    0x000cU
#define MTK_CHECK_DMNR_END      0x0c0cU
#define MTK_CHECK_MAGIC_BEGIN   0x000dU
#define MTK_CHECK_MAGIC_END     0x0d0dU
#define MTK_CHECK_SPEECH_BEGIN  0x000aU
#define MTK_CHECK_SPEECH_END    0x0a0aU

typedef struct __attribute__((packed)) {
    guint16 parser_version;
    guint16 checking_number;
    guint16 debug_number;
    guint16 reserved[5];
} MtkSpeechParamHeader;

struct MtkSpeechParamProvider {
    const MtkConfig *config;
    MtkAudioParam *speech;
    MtkAudioParam *dmnr;
    MtkAudioParam *general;
    MtkAudioParam *magic;
    MtkAudioParam *network;
    gchar *network_names[16];
    guint16 parser_count;
    guint volume_index;
    gboolean bt_wideband;
    gboolean bt_nrec;
};

static MtkAudioParam *
load_audio_param(const MtkConfig *config, const gchar *name, GError **error)
{
    MtkAudioParam *param;
    gchar *path;

    path = g_build_filename(config->audio_param_directory, name, NULL);
    param = mtk_audio_param_load(path, error);
    g_free(path);
    return param;
}

static gint
append_bytes(guint8 *buffer, gsize capacity, gsize *offset,
             gconstpointer data, gsize size)
{
    if (buffer == NULL || offset == NULL ||
        size > capacity || *offset > capacity - size)
        return -ENOSPC;
    if (data != NULL)
        memcpy(buffer + *offset, data, size);
    else
        memset(buffer + *offset, 0, size);
    *offset += size;
    return 0;
}

static gint
append_u16(guint8 *buffer, gsize capacity, gsize *offset, guint16 value)
{
    return append_bytes(buffer, capacity, offset, &value, sizeof(value));
}

static gint
append_param_u16(MtkAudioParam *param,
                 const gchar *const *paths,
                 gsize path_count,
                 const gchar *name,
                 guint8 *buffer,
                 gsize capacity,
                 gsize *offset,
                 gboolean reserve_same_size)
{
    const gchar *matched = NULL;
    const gchar *value;
    guint16 *data = NULL;
    gsize count = 0;
    gint ret;

    value = mtk_audio_param_get_first_param(param,
                                            paths,
                                            path_count,
                                            name,
                                            &matched);
    if (value == NULL) {
        g_debug("SpeechParser missing required parameter name=%s", name);
        return -ENOENT;
    }
    ret = mtk_audio_param_parse_u16_list(value, &data, &count);
    if (ret < 0) {
        g_debug("SpeechParser invalid parameter name=%s path='%s': %s",
                name, matched != NULL ? matched : "", g_strerror(-ret));
        return ret;
    }
    if (count > G_MAXUINT16) {
        g_free(data);
        return -EOVERFLOW;
    }

    ret = append_u16(buffer, capacity, offset, (guint16)count);
    if (ret == 0)
        ret = append_bytes(buffer, capacity, offset, data,
                           count * sizeof(*data));
    if (ret == 0 && reserve_same_size)
        ret = append_bytes(buffer, capacity, offset, NULL,
                           count * sizeof(*data));
    g_free(data);
    return ret;
}

static gint
parse_u16_scalar(const gchar *value, guint16 *out)
{
    gchar *end = NULL;
    guint64 parsed;

    if (value == NULL || out == NULL)
        return -EINVAL;
    errno = 0;
    parsed = g_ascii_strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > G_MAXUINT16)
        return -EINVAL;
    *out = (guint16)parsed;
    return 0;
}

static const gchar *
network_name_from_path(const gchar *path)
{
    const gchar *comma;

    if (path == NULL || *path == '\0')
        return NULL;
    comma = strrchr(path, ',');
    return comma != NULL ? comma + 1 : path;
}

static gint
init_network_map(MtkSpeechParamProvider *provider)
{
    const gchar *first = NULL;
    gsize path_count;
    gsize i;
    guint bit;

    path_count = mtk_audio_param_get_path_count(provider->network);
    for (i = 0; i < path_count; i++) {
        const gchar *path = mtk_audio_param_get_path(provider->network, i);
        const gchar *name = network_name_from_path(path);
        const gchar *value;
        guint16 support;

        if (name == NULL || *name == '\0')
            continue;
        value = mtk_audio_param_get_param(provider->network, path,
                                          "speech_network_support");
        if (parse_u16_scalar(value, &support) < 0)
            continue;
        if (first == NULL)
            first = name;
        for (bit = 0; bit < G_N_ELEMENTS(provider->network_names); bit++) {
            if ((support & (1U << bit)) != 0 && provider->network_names[bit] == NULL)
                provider->network_names[bit] = g_strdup(name);
        }
    }

    if (first == NULL)
        return -ENOENT;
    for (bit = 0; bit < G_N_ELEMENTS(provider->network_names); bit++) {
        if (provider->network_names[bit] == NULL)
            provider->network_names[bit] = g_strdup(first);
    }
    return 0;
}

static void
init_header(MtkSpeechParamHeader *header,
            guint16 check,
            MtkSpeechParamProvider *provider,
            AudioManagerOutputDevice output_device)
{
    memset(header, 0, sizeof(*header));
    header->parser_version = MTK_SPEECH_PARAM_PARSER_VERSION;
    header->checking_number = check;
    header->debug_number = MTK_SPEECH_PARAM_DEBUG_NUMBER;
    if (check == MTK_CHECK_GENERAL_BEGIN) {
        header->reserved[2] = provider->parser_count++;
        header->reserved[3] = (guint16)output_device;
        header->reserved[4] = (guint16)provider->volume_index;
        if (provider->parser_count >= 0x100U)
            provider->parser_count = 0;
    }
}

static gint
build_general(MtkSpeechParamProvider *provider,
              AudioManagerOutputDevice output_device,
              guint8 *buffer,
              gsize *offset)
{
    MtkSpeechParamHeader header;
    const gchar *paths[] = { "CategoryLayer,Common", "Common", "" };
    gint ret;

    init_header(&header, MTK_CHECK_GENERAL_BEGIN, provider, output_device);
    ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset,
                       &header, sizeof(header));
    if (ret == 0)
        ret = append_param_u16(provider->general, paths, G_N_ELEMENTS(paths),
                               "speech_common_para", buffer,
                               MTK_SPEECH_PARAM_SIZE, offset, FALSE);
    if (ret == 0)
        ret = append_param_u16(provider->general, paths, G_N_ELEMENTS(paths),
                               "debug_info", buffer,
                               MTK_SPEECH_PARAM_SIZE, offset, FALSE);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 60);
    if (ret == 0)
        ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, MTK_CHECK_GENERAL_END);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 6);
    return ret;
}

static gint
build_dmnr(MtkSpeechParamProvider *provider,
           guint8 *buffer,
           gsize *offset)
{
    static const gchar *const bands[] = { "NB", "WB", "SWB" };
    static const gchar *const profiles[] = { "Handset", "MagiConference" };
    MtkSpeechParamHeader header;
    gsize band;
    gsize profile;
    gint ret;

    init_header(&header, MTK_CHECK_DMNR_BEGIN, provider, AUDIO_MANAGER_OUTPUT_NONE);
    ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, &header, sizeof(header));
    if (ret < 0)
        return ret;

    for (band = 0; band < G_N_ELEMENTS(bands); band++) {
        for (profile = 0; profile < G_N_ELEMENTS(profiles); profile++) {
            gchar *hal_path = g_strdup_printf("Band,%s,Profile,%s",
                                              bands[band], profiles[profile]);
            gchar *device_path = g_strdup_printf("%s,%s",
                                                 bands[band], profiles[profile]);
            const gchar *paths[] = { hal_path, device_path, bands[band], profiles[profile], "" };
            guint16 data_header = (guint16)(((1U << band) << 12) + profile + 1U);

            ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, data_header);
            if (ret == 0)
                ret = append_param_u16(provider->dmnr,
                                       paths, G_N_ELEMENTS(paths),
                                       "dmnr_para", buffer,
                                       MTK_SPEECH_PARAM_SIZE, offset, TRUE);
            g_free(device_path);
            g_free(hal_path);
            if (ret < 0)
                return ret;
        }
    }

    /* reserve two 242 word units for the absent FB DMNR band. */
    ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset,
                       NULL, (242U * 2U) * 2U);
    if (ret == 0)
        ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, MTK_CHECK_DMNR_END);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 6);
    return ret;
}

static gint
build_magic(MtkSpeechParamProvider *provider,
            guint8 *buffer,
            gsize *offset)
{
    MtkSpeechParamHeader header;
    const gchar *paths[] = { "CategoryLayer,Common", "Common", "" };
    gint ret;

    init_header(&header, MTK_CHECK_MAGIC_BEGIN, provider, AUDIO_MANAGER_OUTPUT_NONE);
    ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, &header, sizeof(header));
    if (ret == 0)
        ret = append_param_u16(provider->magic, paths, G_N_ELEMENTS(paths),
                               "shape_rx_fir_para", buffer,
                               MTK_SPEECH_PARAM_SIZE, offset, FALSE);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 60);
    if (ret == 0)
        ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, MTK_CHECK_MAGIC_END);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 6);
    return ret;
}

static const gchar *
speech_profile(MtkSpeechParamProvider *provider,
               AudioManagerOutputDevice output_device)
{
    switch (output_device) {
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return "Handsfree";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return "3_pole_Headset";
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return "4_pole_Headset";
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        if (!provider->bt_nrec)
            return "BT_NREC_Off";
        return provider->bt_wideband ? "BT_NREC_On_WB" : "BT_NREC_On_NB";
    case AUDIO_MANAGER_OUTPUT_USB:
        return "Usb_Headset";
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
    default:
        return "Normal";
    }
}

static guint16
speech_data_header(gsize band, gsize network)
{
    return (guint16)(((band + 1U) << 12) | (1U << network));
}

static gint
build_speech(MtkSpeechParamProvider *provider,
             AudioManagerOutputDevice output_device,
             guint8 *buffer,
             gsize *offset)
{
    static const gchar *const bands[] = { "NB", "WB", "SWB" };
    static const gchar *const names[] = {
        "speech_mode_para", "sph_in_fir", "sph_out_fir",
        "sph_in_iir_mic1_dsp", "sph_in_iir_mic2_dsp",
        "sph_in_iir_enh_dsp", "sph_out_iir_enh_dsp",
        "speech_mode_para_ext"
    };
    MtkSpeechParamHeader header;
    const gchar *profile = speech_profile(provider, output_device);
    gchar volume[16];
    gsize network;
    gsize band;
    gint ret;

    init_header(&header, MTK_CHECK_SPEECH_BEGIN, provider, output_device);
    ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, &header, sizeof(header));
    if (ret < 0)
        return ret;

    g_snprintf(volume, sizeof(volume), "%u", provider->volume_index);

    for (network = 0; network < 8; network++) {
        /*
         * The GEN95/GEN97 speech parameter image has fixed regions for
         * network slots. Slots 0/1 must fit before 19 KiB, and slots
         * 2/3/4 must fit before 35 KiB. Network slots 2 and 5 start at
         * those offsets. Check for overflow before moving the write offset
         * forward so we never overwrite an earlier region.
         */
        if (network == 2) {
            if (*offset > 19U * 1024U) {
                g_debug("SpeechParser network 0/1 region overflow offset=%zu limit=%u",
                        *offset, 19U * 1024U);
                return -ENOSPC;
            }
            *offset = 19U * 1024U;
        } else if (network == 5) {
            if (*offset > 35U * 1024U) {
                g_debug("SpeechParser network 2/3/4 region overflow offset=%zu limit=%u",
                        *offset, 35U * 1024U);
                return -ENOSPC;
            }
            *offset = 35U * 1024U;
        }

        for (band = 0; band < G_N_ELEMENTS(bands); band++) {
            const gchar *network_name = provider->network_names[network];
            gchar *hal_path = g_strdup_printf("Band,%s,Profile,%s,VolIndex,%s,Network,%s",
                                              bands[band], profile, volume, network_name);
            gchar *device_full = g_strdup_printf("%s,%s,%s,%s",
                                                 bands[band], profile, volume, network_name);
            gchar *device_volume = g_strdup_printf("%s,%s,%s",
                                                   bands[band], profile, volume);
            gchar *device_band = g_strdup_printf("%s,%s", bands[band], profile);
            const gchar *paths[] = {
                hal_path, device_full, device_volume, device_band, profile, bands[band], ""
            };
            gsize n;
            guint16 header_value = speech_data_header(band, network);

            ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, header_value);
            if (ret < 0) {
                g_free(device_band);
                g_free(device_volume);
                g_free(device_full);
                g_free(hal_path);
                return ret;
            }
            for (n = 0; n < G_N_ELEMENTS(names); n++) {
                ret = append_param_u16(provider->speech,
                                       paths, G_N_ELEMENTS(paths),
                                       names[n], buffer,
                                       MTK_SPEECH_PARAM_SIZE, offset, FALSE);
                if (ret < 0)
                    break;
            }
            g_free(device_band);
            g_free(device_volume);
            g_free(device_full);
            g_free(hal_path);
            if (ret < 0)
                return ret;
            ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 2);
            if (ret < 0)
                return ret;
        }
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 540U * 2U);
        if (ret < 0)
            return ret;
    }

    ret = append_u16(buffer, MTK_SPEECH_PARAM_SIZE, offset, MTK_CHECK_SPEECH_END);
    if (ret == 0)
        ret = append_bytes(buffer, MTK_SPEECH_PARAM_SIZE, offset, NULL, 6);
    return ret;
}

MtkSpeechParamProvider *
mtk_speech_param_provider_new(const MtkConfig *config, GError **error)
{
    MtkSpeechParamProvider *provider;

    if (config == NULL || config->audio_param_directory == NULL)
        return NULL;

    provider = g_new0(MtkSpeechParamProvider, 1);
    provider->config = config;
    provider->volume_index = 6;
    provider->bt_nrec = TRUE;

    provider->speech = load_audio_param(config, "Speech_AudioParam.xml", error);
    if (provider->speech == NULL)
        goto fail;
    provider->dmnr = load_audio_param(config, "SpeechDMNR_AudioParam.xml", error);
    if (provider->dmnr == NULL)
        goto fail;
    provider->general = load_audio_param(config, "SpeechGeneral_AudioParam.xml", error);
    if (provider->general == NULL)
        goto fail;
    provider->magic = load_audio_param(config, "SpeechMagiClarity_AudioParam.xml", error);
    if (provider->magic == NULL)
        goto fail;
    provider->network = load_audio_param(config, "SpeechNetwork_AudioParam.xml", error);
    if (provider->network == NULL)
        goto fail;
    if (init_network_map(provider) < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek SpeechNetwork_AudioParam.xml has no usable network mapping");
        goto fail;
    }

    return provider;

fail:
    mtk_speech_param_provider_free(provider);
    return NULL;
}

void
mtk_speech_param_provider_free(MtkSpeechParamProvider *provider)
{
    guint i;

    if (provider == NULL)
        return;
    mtk_audio_param_free(provider->speech);
    mtk_audio_param_free(provider->dmnr);
    mtk_audio_param_free(provider->general);
    mtk_audio_param_free(provider->magic);
    mtk_audio_param_free(provider->network);
    for (i = 0; i < G_N_ELEMENTS(provider->network_names); i++)
        g_free(provider->network_names[i]);
    g_free(provider);
}

void
mtk_speech_param_provider_set_volume_index(MtkSpeechParamProvider *provider,
                                           guint index)
{
    if (provider != NULL)
        provider->volume_index = index < 6U ? index : 6U;
}

void
mtk_speech_param_provider_set_bluetooth(MtkSpeechParamProvider *provider,
                                        gboolean wideband,
                                        gboolean nrec)
{
    if (provider == NULL)
        return;
    provider->bt_wideband = wideband;
    provider->bt_nrec = nrec;
}

gint
mtk_speech_param_provider_build(MtkSpeechParamProvider *provider,
                                AudioManagerOutputDevice output_device,
                                AudioManagerInputDevice input_device,
                                guint8 **data,
                                gsize *size)
{
    guint8 *buffer;
    gsize offset = MTK_SPEECH_PARAM_EMI_RESERVED;
    gint ret;

    (void)input_device;

    if (provider == NULL || data == NULL || size == NULL)
        return -EINVAL;
    *data = NULL;
    *size = 0;

    buffer = g_malloc0(MTK_SPEECH_PARAM_SIZE);
    ret = build_general(provider, output_device, buffer, &offset);
    if (ret == 0)
        ret = build_dmnr(provider, buffer, &offset);
    if (ret == 0)
        ret = build_magic(provider, buffer, &offset);
    if (ret == 0)
        ret = build_speech(provider, output_device, buffer, &offset);
    if (ret < 0) {
        g_free(buffer);
        return ret;
    }

    g_debug("SpeechParser built MTK GEN95/97 parameter image bytes=%u final_offset=%zu profile=%s volume=%u",
            MTK_SPEECH_PARAM_SIZE, offset,
            speech_profile(provider, output_device), provider->volume_index);
    *data = buffer;
    *size = MTK_SPEECH_PARAM_SIZE;
    return 0;
}
