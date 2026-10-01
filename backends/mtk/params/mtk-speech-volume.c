/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/params/mtk-speech-volume.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <audio-manager/audio-manager-error.h>

#include "backends/mtk/params/mtk-audio-param.h"
#include "common/alsa/alsa-control.h"
#include "common/math-utils.h"

#define MTK_GAIN_DEVICE_VOLUME_STEPS 8U
#define MTK_GAIN_UL_CHANNELS 4U
#define MTK_GAIN_ANALOG_HANDSET 0
#define MTK_GAIN_ANALOG_HEADPHONE 1
#define MTK_GAIN_ANALOG_SPEAKER 2
#define MTK_GAIN_ANALOG_LINEOUT 3

#define LOAD_INT(field, name) do { \
    ret = load_int(volume->volume_spec, path, name, &volume->field); \
    if (ret < 0) { bad_param = name; goto bad; } \
} while (0)
#define LOAD_LIST(field, count_field, name) do { \
    ret = load_list(volume->volume_spec, path, name, &volume->field, &volume->count_field); \
    if (ret < 0) { bad_param = name; goto bad; } \
} while (0)
#define LOAD_STRING(field, name) do { \
    ret = load_string(volume->volume_spec, path, name, &volume->field); \
    if (ret < 0) { bad_param = name; goto bad; } \
} while (0)

struct MtkSpeechVolume {
    MtkAudioParam *speech_vol;
    MtkAudioParam *dl_map;
    MtkAudioParam *ul_map;
    MtkAudioParam *volume_spec;
    MtkAudioParam *playback_vol_ana;
    MtkAudioParam *record_vol;

    gint step_per_db;
    gint digital_db_min;
    gint digital_db_max;
    gint volume_step;
    gint ul_pga_gain_map_max;
    gint ul_hw_pga_max_index;
    gint ul_gain_offset;
    gint sidetone_index_max;

    gint32 *voice_gain_db;
    gint32 *voice_gain_idx;
    gsize voice_gain_count;
    gchar *voice_mixer;

    gint32 *headphone_gain_db;
    gint32 *headphone_gain_idx;
    gsize headphone_gain_count;
    gchar *headphone_mixer;

    gint32 *lineout_gain_db;
    gint32 *lineout_gain_idx;
    gsize lineout_gain_count;

    gint32 *speaker_gain_db;
    gint32 *speaker_gain_idx;
    gsize speaker_gain_count;
    gchar *speaker_mixer;

    gchar *pga_mixer[MTK_GAIN_UL_CHANNELS];
    gint32 *sidetone_map;
    gsize sidetone_map_count;
};

static gint
parse_scalar(const gchar *value, gint *out)
{
    gchar *end = NULL;
    gint64 v;

    if (value == NULL || out == NULL)
        return -EINVAL;
    errno = 0;
    v = g_ascii_strtoll(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || v < INT_MIN || v > INT_MAX)
        return -EINVAL;
    *out = (gint)v;
    return 0;
}

static gint
load_list(MtkAudioParam *param,
          const gchar *path,
          const gchar *name,
          gint32 **values,
          gsize *count)
{
    const gchar *value = mtk_audio_param_get_param(param, path, name);
    return mtk_audio_param_parse_i32_list(value, values, count);
}

static gint
load_string(MtkAudioParam *param,
            const gchar *path,
            const gchar *name,
            gchar **out)
{
    const gchar *value = mtk_audio_param_get_param(param, path, name);
    if (value == NULL)
        return -ENOENT;
    *out = g_strdup(value);
    return 0;
}

static gint
load_int(MtkAudioParam *param,
         const gchar *path,
         const gchar *name,
         gint *out)
{
    return parse_scalar(mtk_audio_param_get_param(param, path, name), out);
}

static gint
load_volume_spec(MtkSpeechVolume *volume, GError **error)
{
    const gchar *path = "Common_SPK_LO";
    const gchar *bad_param = NULL;
    gsize count;
    gint ret;

    LOAD_INT(step_per_db, "step_per_db");
    LOAD_INT(digital_db_min, "play_digi_range_min");
    LOAD_INT(digital_db_max, "play_digi_range_max");
    ret = load_int(volume->volume_spec, path, "volume_step", &volume->volume_step);
    if (ret < 0) {
        const gchar *v = mtk_audio_param_get_param(volume->volume_spec, path, "volume_step");
        gchar *end = NULL;
        gdouble d = v != NULL ? g_ascii_strtod(v, &end) : 0.0;
        if (v == NULL || end == v || *end != '\0') {
            bad_param = "volume_step";
            goto bad;
        }
        volume->volume_step = (gint)d;
    }
    LOAD_INT(ul_pga_gain_map_max, "ul_pga_gain_map_max");
    LOAD_INT(ul_hw_pga_max_index, "ul_hw_pga_max_idx");
    LOAD_INT(ul_gain_offset, "ul_gain_offset");
    LOAD_INT(sidetone_index_max, "stf_idx_range_max");

    LOAD_LIST(voice_gain_db, voice_gain_count, "voice_buffer_gain_db");
    count = 0;
    ret = load_list(volume->volume_spec, path, "voice_buffer_gain_idx",
                    &volume->voice_gain_idx, &count);
    if (ret < 0 || count != volume->voice_gain_count) {
        bad_param = "voice_buffer_gain_idx";
        goto bad;
    }
    LOAD_STRING(voice_mixer, "voice_buffer_mixer_name");

    LOAD_LIST(headphone_gain_db, headphone_gain_count, "audio_buffer_gain_db");
    count = 0;
    ret = load_list(volume->volume_spec, path, "audio_buffer_gain_idx",
                    &volume->headphone_gain_idx, &count);
    if (ret < 0 || count != volume->headphone_gain_count) {
        bad_param = "audio_buffer_gain_idx";
        goto bad;
    }
    LOAD_STRING(headphone_mixer, "audio_buffer_mixer_name");

    LOAD_LIST(lineout_gain_db, lineout_gain_count, "lineout_buffer_gain_db");
    count = 0;
    ret = load_list(volume->volume_spec, path, "lineout_buffer_gain_idx",
                    &volume->lineout_gain_idx, &count);
    if (ret < 0 || count != volume->lineout_gain_count) {
        bad_param = "lineout_buffer_gain_idx";
        goto bad;
    }
    LOAD_LIST(speaker_gain_db, speaker_gain_count, "spk_gain_db");
    count = 0;
    ret = load_list(volume->volume_spec, path, "spk_gain_idx",
                    &volume->speaker_gain_idx, &count);
    if (ret < 0 || count != volume->speaker_gain_count) {
        bad_param = "spk_gain_idx";
        goto bad;
    }
    LOAD_STRING(speaker_mixer, "spk_mixer_name");

    LOAD_STRING(pga_mixer[0], "ul_pga_l_mixer_name");
    LOAD_STRING(pga_mixer[1], "ul_pga_r_mixer_name");
    LOAD_STRING(pga_mixer[2], "ul_pga_3_mixer_name");
    /* fourth mixer is optional (MT6877?) */
    load_string(volume->volume_spec, path, "ul_pga_4_mixer_name", &volume->pga_mixer[3]);

    LOAD_LIST(sidetone_map, sidetone_map_count, "stf_gain_map");

    return 0;

bad:
    g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                "MediaTek Volume_AudioParam.xml parameter '%s' is missing or invalid for '%s'",
                bad_param != NULL ? bad_param : "unknown", path);
    return -EINVAL;
}

static MtkAudioParam *
load_param(const gchar *directory, const gchar *name, GError **error)
{
    gchar *path = g_build_filename(directory, name, NULL);
    MtkAudioParam *param = mtk_audio_param_load(path, error);
    g_free(path);
    return param;
}

MtkSpeechVolume *
mtk_speech_volume_new(const gchar *audio_param_directory,
                      GError **error)
{
    MtkSpeechVolume *volume;
    GError *optional_error = NULL;

    if (audio_param_directory == NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek AudioParamDirectory is required for speech gain");
        return NULL;
    }

    volume = g_new0(MtkSpeechVolume, 1);
    volume->speech_vol = load_param(audio_param_directory, "SpeechVol_AudioParam.xml", error);
    if (volume->speech_vol == NULL)
        goto fail;
    volume->dl_map = load_param(audio_param_directory, "VolumeGainMap_AudioParam.xml", error);
    if (volume->dl_map == NULL)
        goto fail;
    volume->ul_map = load_param(audio_param_directory, "VolumeGainMapUL_AudioParam.xml", error);
    if (volume->ul_map == NULL)
        goto fail;
    volume->volume_spec = load_param(audio_param_directory, "Volume_AudioParam.xml", error);
    if (volume->volume_spec == NULL)
        goto fail;

    if (load_volume_spec(volume, error) < 0)
        goto fail;

    /* Out of call media gain tables are optional */
    volume->playback_vol_ana = load_param(audio_param_directory,
                                          "PlaybackVolAna_AudioParam.xml",
                                          &optional_error);
    if (optional_error != NULL) {
        g_debug("MediaTek PlaybackVolAna unavailable: %s", optional_error->message);
        g_clear_error(&optional_error);
    }
    volume->record_vol = load_param(audio_param_directory,
                                    "RecordVol_AudioParam.xml",
                                    &optional_error);
    if (optional_error != NULL) {
        g_debug("MediaTek RecordVol unavailable: %s", optional_error->message);
        g_clear_error(&optional_error);
    }

    g_debug("MediaTek speech gain tables loaded step_per_db=%d volume_step=%d",
            volume->step_per_db, volume->volume_step);
    return volume;

fail:
    mtk_speech_volume_free(volume);
    return NULL;
}

void
mtk_speech_volume_free(MtkSpeechVolume *volume)
{
    gsize i;

    if (volume == NULL)
        return;
    mtk_audio_param_free(volume->speech_vol);
    mtk_audio_param_free(volume->dl_map);
    mtk_audio_param_free(volume->ul_map);
    mtk_audio_param_free(volume->volume_spec);
    mtk_audio_param_free(volume->playback_vol_ana);
    mtk_audio_param_free(volume->record_vol);
    g_free(volume->voice_gain_db);
    g_free(volume->voice_gain_idx);
    g_free(volume->voice_mixer);
    g_free(volume->headphone_gain_db);
    g_free(volume->headphone_gain_idx);
    g_free(volume->headphone_mixer);
    g_free(volume->lineout_gain_db);
    g_free(volume->lineout_gain_idx);
    g_free(volume->speaker_gain_db);
    g_free(volume->speaker_gain_idx);
    g_free(volume->speaker_mixer);
    for (i = 0; i < MTK_GAIN_UL_CHANNELS; i++)
        g_free(volume->pga_mixer[i]);
    g_free(volume->sidetone_map);
    g_free(volume);
}

static gint
analog_db_to_index(MtkSpeechVolume *volume,
                   gint analog_type,
                   gint db,
                   const gchar **control,
                   glong *index)
{
    const gint32 *db_map = NULL;
    const gint32 *idx_map = NULL;
    gsize count = 0;
    const gchar *mixer = NULL;
    gsize i;
    gint best_delta = INT_MAX;
    gsize best = 0;

    switch (analog_type) {
    case MTK_GAIN_ANALOG_HANDSET:
        db_map = volume->voice_gain_db;
        idx_map = volume->voice_gain_idx;
        count = volume->voice_gain_count;
        mixer = volume->voice_mixer;
        break;
    case MTK_GAIN_ANALOG_HEADPHONE:
        db_map = volume->headphone_gain_db;
        idx_map = volume->headphone_gain_idx;
        count = volume->headphone_gain_count;
        mixer = volume->headphone_mixer;
        break;
    case MTK_GAIN_ANALOG_SPEAKER:
        db_map = volume->speaker_gain_db;
        idx_map = volume->speaker_gain_idx;
        count = volume->speaker_gain_count;
        mixer = volume->speaker_mixer;
        break;
    case MTK_GAIN_ANALOG_LINEOUT:
        db_map = volume->lineout_gain_db;
        idx_map = volume->lineout_gain_idx;
        count = volume->lineout_gain_count;
        mixer = volume->speaker_mixer;
        break;
    default:
        *control = NULL;
        *index = 0;
        return 0;
    }

    if (db_map == NULL || idx_map == NULL || count == 0 || mixer == NULL)
        return -ENOENT;
    for (i = 0; i < count; i++) {
        gint delta = abs((gint)db_map[i] - db);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
            if (delta == 0)
                break;
        }
    }
    *control = mixer;
    *index = idx_map[best];
    return 0;
}

static gint
profile_scalar(MtkAudioParam *param, const gchar *path, const gchar *name, gint *out)
{
    return parse_scalar(mtk_audio_param_get_param(param, path, name), out);
}

static gint
calculate_sidetone(MtkSpeechVolume *volume,
                   const gchar *speech_profile,
                   gint analog_db,
                   gint sw_agc,
                   glong *gain,
                   glong *positive_db)
{
    gint sidetone;
    gint idx;

    /* MTK does not apply local sidetone on the handsfree speaker path. */
    if (g_strcmp0(speech_profile, "SPK") == 0) {
        *gain = 0;
        *positive_db = 0;
        return 0;
    }

    if (profile_scalar(volume->speech_vol, speech_profile, "stf_gain", &sidetone) < 0)
        return -ENOENT;
    if (sidetone < 0)
        sidetone = 0;
    if (sidetone > volume->sidetone_index_max)
        sidetone = volume->sidetone_index_max;

    *positive_db = 0;
    if (sidetone == 0) {
        *gain = 0;
        return 0;
    }

    idx = analog_db - (sidetone + sw_agc) + 49 - volume->ul_gain_offset;
    if (idx < 0) {
        idx += 6;
        *positive_db = 6;
    }
    if (idx < 0)
        idx = 0;
    if (idx > volume->sidetone_index_max)
        idx = volume->sidetone_index_max;
    if ((gsize)idx >= volume->sidetone_map_count)
        return -ERANGE;
    *gain = volume->sidetone_map[idx];
    return 0;
}

gint
mtk_speech_volume_calculate(MtkSpeechVolume *volume,
                            const gchar *speech_profile,
                            const gchar *gain_profile,
                            gdouble normalized_volume,
                            MtkSpeechGain *gain)
{
    gint32 *dl_indices = NULL;
    gsize dl_count = 0;
    gint32 *dl_digital = NULL;
    gint32 *dl_analog = NULL;
    gsize digital_count = 0;
    gsize analog_count = 0;
    gint32 *swagc = NULL;
    gint32 *pga_map = NULL;
    gsize swagc_count = 0;
    gsize pga_count = 0;
    gint analog_type;
    gint ul_gain;
    gint mic_index_max;
    gint dl_index;
    gint digital_db;
    gint analog_db;
    gint mic_delta;
    gint sw_agc;
    gsize i;
    gint ret = 0;
    const gchar *failure_stage = NULL;
    const gchar *ul_profile;

    if (volume == NULL || speech_profile == NULL || gain_profile == NULL || gain == NULL)
        return -EINVAL;
    memset(gain, 0, sizeof(*gain));

    gain->ui_index = (guint)audio_volume_to_index(normalized_volume,
                                                         MTK_GAIN_DEVICE_VOLUME_STEPS);
    if (gain->ui_index > 7)
        gain->ui_index = 7;
    gain->update_parameter_volume_index = gain->ui_index != 0;
    if (gain->update_parameter_volume_index)
        gain->parameter_volume_index = gain->ui_index - 1;

    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->speech_vol, speech_profile, "dl_gain"),
                                         &dl_indices,
                                         &dl_count);
    if (ret < 0 || dl_count < 7) {
        failure_stage = "SpeechVol dl_gain";
        ret = -ENOENT;
        goto out;
    }
    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->dl_map, gain_profile, "dl_digital_gain"),
                                         &dl_digital,
                                         &digital_count);
    if (ret < 0) {
        failure_stage = "VolumeGainMap dl_digital_gain";
        goto out;
    }
    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->dl_map, gain_profile, "dl_analog_gain"),
                                         &dl_analog,
                                         &analog_count);
    if (ret < 0) {
        failure_stage = "VolumeGainMap dl_analog_gain";
        goto out;
    }
    if (profile_scalar(volume->dl_map, gain_profile, "dl_analog_type", &analog_type) < 0) {
        failure_stage = "VolumeGainMap dl_analog_type";
        ret = -ENOENT;
        goto out;
    }

    /* MTK table has step 0 as hard mute while SpeechVol stores 1..7. */
    dl_index = (gint)dl_indices[gain->ui_index == 0 ? 0 : gain->ui_index - 1];
    if (dl_index < 0 || (gsize)dl_index >= digital_count || (gsize)dl_index >= analog_count) {
        failure_stage = "downlink gain index";
        ret = -ERANGE;
        goto out;
    }
    digital_db = gain->ui_index == 0 ? -64 : (gint)dl_digital[dl_index];
    analog_db = (gint)dl_analog[dl_index];
    if (digital_db > volume->digital_db_max)
        gain->modem_downlink_gain = 0;
    else if (digital_db <= volume->digital_db_min)
        gain->modem_downlink_gain = (gint16)-volume->volume_step;
    else
        gain->modem_downlink_gain = (gint16)(digital_db * volume->step_per_db);
    ret = analog_db_to_index(volume, analog_type, analog_db,
                             &gain->downlink_analog_control,
                             &gain->downlink_analog_index);
    if (ret < 0) {
        failure_stage = "downlink analog mapping";
        goto out;
    }

    if (profile_scalar(volume->speech_vol, speech_profile, "ul_gain", &ul_gain) < 0) {
        failure_stage = "SpeechVol ul_gain";
        ret = -ENOENT;
        goto out;
    }
    ul_profile = gain_profile;

    if (g_str_has_prefix(gain_profile, "RCV_"))
        ul_profile = "RCV";

    if (profile_scalar(volume->ul_map, ul_profile,
                       "mic_idx_range_max", &mic_index_max) < 0) {
        failure_stage = "VolumeGainMapUL mic_idx_range_max";
        ret = -ENOENT;
        goto out;
    }

    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->ul_map, ul_profile, "swagc_gain_map"),
                                         &swagc,
                                         &swagc_count);
    if (ret < 0) {
        failure_stage = "VolumeGainMapUL swagc_gain_map";
        goto out;
    }
    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->ul_map, ul_profile, "ul_pga_gain_map"),
                                         &pga_map,
                                         &pga_count);
    if (ret < 0)
        failure_stage = "VolumeGainMapUL ul_pga_gain_map";
    if (ret < 0)
        goto out;

    if (ul_gain < 0)
        ul_gain = 0;
    if (ul_gain > mic_index_max)
        ul_gain = mic_index_max;
    mic_delta = mic_index_max - ul_gain;
    if ((gsize)mic_delta >= swagc_count || (gsize)mic_delta >= pga_count) {
        failure_stage = "uplink gain index";
        ret = -ERANGE;
        goto out;
    }
    sw_agc = (gint)swagc[mic_delta];
    gain->modem_uplink_gain = (gint16)(sw_agc << 2);

    /* BT/USB have no local ADC PGA */
    if (g_strcmp0(gain_profile, "BT") != 0 && g_strcmp0(gain_profile, "USB") != 0) {
        if (volume->ul_hw_pga_max_index <= 0) {
            failure_stage = "uplink PGA divisor";
            ret = -EINVAL;
            goto out;
        }
        gain->uplink_pga_count = 3;
        for (i = 0; i < gain->uplink_pga_count; i++) {
            gain->uplink_pga_control[i] = volume->pga_mixer[i];
            gain->uplink_pga_index[i] = (volume->ul_pga_gain_map_max - (gint)pga_map[mic_delta]) /
                                         volume->ul_hw_pga_max_index;
        }
    }

    ret = calculate_sidetone(volume, speech_profile, analog_db, sw_agc,
                             &gain->sidetone_gain,
                             &gain->sidetone_positive_gain_db);
    if (ret < 0) {
        failure_stage = "sidetone";
        goto out;
    }

    g_debug("MediaTek speech gain profile=%s gain_profile=%s volume=%.3f ui=%u "
            "dl_idx=%d dl_digital=%d dl_msg=%d analog_db=%d analog_ctl=%s "
            "analog_idx=%ld mic_idx_max=%d ul_idx=%d sw_agc=%d ul_msg=%d "
            "sidetone=%ld positive=%ld",
            speech_profile, gain_profile, normalized_volume, gain->ui_index,
            dl_index, digital_db, gain->modem_downlink_gain, analog_db,
            gain->downlink_analog_control != NULL ? gain->downlink_analog_control : "none",
            gain->downlink_analog_index, mic_index_max, mic_delta, sw_agc,
            gain->modem_uplink_gain, gain->sidetone_gain,
            gain->sidetone_positive_gain_db);

out:
    if (ret < 0)
        g_debug("MediaTek speech gain calculation failed profile=%s gain_profile=%s stage='%s': %s",
                speech_profile, gain_profile,
                failure_stage != NULL ? failure_stage : "unknown",
                g_strerror(-ret));
    g_free(dl_indices);
    g_free(dl_digital);
    g_free(dl_analog);
    g_free(swagc);
    g_free(pga_map);
    return ret;
}

static const gchar *
media_role_name(AudioManagerCaptureRole role)
{
    switch (role) {
    case AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER:
        return "Camera recording";
    case AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION:
        return "VR";
    case AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED:
        return "Unprocessed";
    case AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT:
    default:
        return "Sound recording";
    }
}

static const gchar *
media_output_profile(AudioManagerOutputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return "RCV";
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return "SPK";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return "HP";
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return "HS";
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return "BT_A2DP";
    case AUDIO_MANAGER_OUTPUT_USB:
        return "USB";
    default:
        return NULL;
    }
}

static const gchar *
media_playback_param(AudioManagerOutputDevice device,
                     const gchar **control,
                     MtkSpeechVolume *volume)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        *control = volume->voice_mixer;
        return "receiver_pga";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        *control = volume->headphone_mixer;
        return "headset_pga";
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        *control = volume->speaker_mixer;
        return "speaker_pga";
    default:
        *control = NULL;
        return NULL;
    }
}

gint
mtk_speech_volume_apply_media_playback_gain(MtkSpeechVolume *volume,
                                            AudioAlsaCard *card,
                                            AudioManagerOutputDevice device)
{
    const gchar *profile;
    const gchar *name;
    const gchar *control;
    const gchar *value;
    gchar *path;
    gint gain;
    gint ret;

    if (volume == NULL || card == NULL)
        return -EINVAL;
    if (volume->playback_vol_ana == NULL)
        return -ENOENT;

    profile = media_output_profile(device);
    name = media_playback_param(device, &control, volume);
    if (profile == NULL || name == NULL || control == NULL)
        return -ENOTSUP;

    path = g_strdup_printf("Scene,Default,Volume type,Music,Profile,%s", profile);
    value = mtk_audio_param_get_param(volume->playback_vol_ana, path, name);
    if (value == NULL && device == AUDIO_MANAGER_OUTPUT_SPEAKER) {
        g_free(path);
        path = g_strdup("Scene,Default,Volume type,Music,Profile,SPK_LO");
        value = mtk_audio_param_get_param(volume->playback_vol_ana, path, name);
    }
    if (value == NULL) {
        const gchar *categories[] = {
            "Scene", "Default", "Volume type", "Music", "Profile",
            profile,
            device == AUDIO_MANAGER_OUTPUT_SPEAKER ? "SPK_LO" : NULL,
        };
        const gchar *matched_path = NULL;

        value = mtk_audio_param_get_best_param(volume->playback_vol_ana,
                                               categories,
                                               G_N_ELEMENTS(categories),
                                               name,
                                               &matched_path);
        if (value != NULL && matched_path != NULL) {
            g_free(path);
            path = g_strdup(matched_path);
        }
    }
    ret = parse_scalar(value, &gain);
    if (ret == 0 && gain < 0) {
        /*
         * PlaybackVolAna uses negative PGA values to mean that this analog
         * stage is not applicable for the selected route.
         */
        g_debug("MediaTek media playback gain skipped device=%d path='%s' control='%s' raw=%d",
                device, path, control, gain);
        ret = 0;
        goto out;
    }

    if (ret == 0) {
        gsize gain_count = 0;

        switch (device) {
        case AUDIO_MANAGER_OUTPUT_RECEIVER:
            gain_count = volume->voice_gain_count;
            break;
        case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        case AUDIO_MANAGER_OUTPUT_HEADSET:
            gain_count = volume->headphone_gain_count;
            break;
        case AUDIO_MANAGER_OUTPUT_SPEAKER:
            gain_count = volume->speaker_gain_count;
            break;
        default:
            break;
        }

        /* clamp the PlaybackVolAna ordinal to the codec gain table before writing the mixer */
        if (gain_count == 0)
            ret = -ENOENT;
        else if ((gsize)gain >= gain_count)
            gain = (gint)gain_count - 1;
    }

    if (ret == 0)
        ret = audio_alsa_control_set_index(card, control, gain);
    if (ret == 0)
        g_debug("MediaTek media playback gain applied device=%d path='%s' control='%s' index=%d",
                device, path, control, gain);

out:
    g_free(path);
    return ret;
}

static const gchar *
record_profile(AudioManagerInputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_INPUT_BUILTIN_MIC:
        return "SPK";
    case AUDIO_MANAGER_INPUT_HEADSET_MIC:
        return "HS";
    case AUDIO_MANAGER_INPUT_BLUETOOTH:
        return "BT";
    case AUDIO_MANAGER_INPUT_USB:
        return "USB";
    default:
        return NULL;
    }
}

static const gchar *
find_record_gain(MtkSpeechVolume *volume,
                 const gchar *role,
                 const gchar *profile,
                 const gchar *fallback_profile,
                 gchar **matched_path)
{
    const gchar *value;
    gchar *path;

    path = g_strdup_printf("Scene,Default,Application,%s,Profile,%s", role, profile);
    value = mtk_audio_param_get_param(volume->record_vol, path, "ul_gain");
    if (value == NULL && fallback_profile != NULL) {
        g_free(path);
        path = g_strdup_printf("Scene,Default,Application,%s,Profile,%s",
                               role, fallback_profile);
        value = mtk_audio_param_get_param(volume->record_vol, path, "ul_gain");
    }
    if (value == NULL) {
        const gchar *categories[] = {
            "Scene", "Default", "Application", role, "Profile", profile,
            fallback_profile,
        };
        const gchar *best_path = NULL;

        value = mtk_audio_param_get_best_param(volume->record_vol,
                                                categories,
                                                G_N_ELEMENTS(categories),
                                                "ul_gain",
                                                &best_path);
        if (value != NULL && best_path != NULL) {
            g_free(path);
            path = g_strdup(best_path);
        }
    }
    if (value == NULL) {
        g_free(path);
        return NULL;
    }
    *matched_path = path;
    return value;
}

gint
mtk_speech_volume_apply_capture_gain(MtkSpeechVolume *volume,
                                     AudioAlsaCard *card,
                                     AudioManagerCaptureRole role,
                                     AudioManagerInputDevice device,
                                     const gchar *builtin_gain_profile,
                                     const gchar *headset_gain_profile,
                                     guint channels)
{
    const gchar *logical_profile;
    const gchar *map_profile;
    const gchar *value;
    const gchar *role_name;
    gchar *matched_path = NULL;
    gint32 *ul_gain = NULL;
    gint32 *pga_map = NULL;
    gsize ul_gain_count = 0;
    gsize pga_count = 0;
    gint mic_max;
    guint i;
    gint ret;

    if (volume == NULL || card == NULL)
        return -EINVAL;
    if (volume->record_vol == NULL || volume->ul_map == NULL)
        return -ENOENT;
    if (device == AUDIO_MANAGER_INPUT_BLUETOOTH || device == AUDIO_MANAGER_INPUT_USB)
        return 0;

    logical_profile = record_profile(device);
    if (logical_profile == NULL)
        return -ENOTSUP;
    map_profile = device == AUDIO_MANAGER_INPUT_BUILTIN_MIC ?
                  (builtin_gain_profile != NULL ? builtin_gain_profile : "SPK_LO") :
                  (headset_gain_profile != NULL ? headset_gain_profile : "HS");

    role_name = media_role_name(role);
    value = find_record_gain(volume, role_name, logical_profile, map_profile,
                             &matched_path);
    if (value == NULL)
        return -ENOENT;
    ret = mtk_audio_param_parse_i32_list(value, &ul_gain, &ul_gain_count);
    if (ret < 0 || ul_gain_count == 0) {
        ret = -EINVAL;
        goto out;
    }
    ret = profile_scalar(volume->ul_map, map_profile, "mic_idx_range_max", &mic_max);
    if (ret < 0 && device == AUDIO_MANAGER_INPUT_BUILTIN_MIC) {
        map_profile = "SPK_LO";
        ret = profile_scalar(volume->ul_map, map_profile, "mic_idx_range_max", &mic_max);
    }
    if (ret < 0)
        goto out;
    ret = mtk_audio_param_parse_i32_list(mtk_audio_param_get_param(volume->ul_map, map_profile, "ul_pga_gain_map"),
                                         &pga_map,
                                         &pga_count);
    if (ret < 0)
        goto out;
    if (volume->ul_hw_pga_max_index <= 0) {
        ret = -EINVAL;
        goto out;
    }

    channels = MIN(channels, (guint)MTK_GAIN_UL_CHANNELS);
    for (i = 0; i < channels; i++) {
        gint target = (gint)ul_gain[MIN((gsize)i, ul_gain_count - 1)];
        gint delta;
        gint adc_gain;

        target = CLAMP(target, 0, mic_max);
        delta = mic_max - target;
        if ((gsize)delta >= pga_count) {
            ret = -ERANGE;
            goto out;
        }
        if (volume->pga_mixer[i] == NULL)
            continue;
        adc_gain = (volume->ul_pga_gain_map_max - (gint)pga_map[delta]) /
                   volume->ul_hw_pga_max_index;
        ret = audio_alsa_control_set_index(card, volume->pga_mixer[i], adc_gain);
        if (ret < 0)
            goto out;
    }
    g_debug("MediaTek capture gain applied role=%s device=%d record_path='%s' ul_profile=%s channels=%u",
            role_name, device, matched_path, map_profile, channels);
    ret = 0;

out:
    g_free(matched_path);
    g_free(ul_gain);
    g_free(pga_map);
    return ret;
}
