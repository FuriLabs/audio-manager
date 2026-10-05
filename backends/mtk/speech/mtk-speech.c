/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-speech.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "backends/mtk/speech/mtk-ccci.h"
#include "backends/mtk/speech/mtk-speech-generation.h"
#include "backends/mtk/speech/mtk-speech-protocol.h"
#include "common/alsa/alsa-pcm.h"

#define MTK_SPEECH_ACK_TIMEOUT_MS 60000U
#define MTK_SPEECH_ROUTING_UNMUTE_MS 150U
#define MTK_SPEECH_MODEM_MONITOR_MS 2000U
#define MTK_SPEECH_MODEM_READY_RETRY_MS 20U
#define MTK_SPEECH_MODEM_READY_TIMEOUT_MS 500U
#define MTK_SPEECH_CCCI_REOPEN_RETRY_MS 10U
#define MTK_SPEECH_CCCI_REOPEN_MAX_TRIES 200U

typedef struct __attribute__((packed)) {
    guint16 header_md_version;
    guint16 md_version;
    guint8 reserved[28];
} MtkMdAliveInfo;

#define MTK_SPEECH_CODEC_INFO_LENGTH 92U

typedef struct __attribute__((packed)) {
    gchar codec_info[MTK_SPEECH_CODEC_INFO_LENGTH];
    gchar codec_operation[MTK_SPEECH_CODEC_INFO_LENGTH];
} MtkNetworkCodecInfo;

struct MtkSpeech {
    const MtkConfig *config;
    AudioAlsaCard *card;
    GMainContext *main_context;

    MtkCcci *ccci;
    MtkSpeechGeneration *generation;
    AudioAlsaPcm *playback;
    AudioAlsaPcm *capture;

    AudioManagerCallState state;
    gboolean modem_active;
    gboolean desired_active;
    gboolean modem_epof;
    gboolean recovery_callback_running;
    gboolean uplink_muted;
    gboolean downlink_muted;
    gboolean routing_muted;
    gboolean last_start_failed_pcm;
    GSource *routing_unmute_source;
    gboolean deferred_device_change;
    gboolean deferred_volume_update;
    gboolean deferred_uplink_mute;
    gboolean deferred_downlink_mute;
    gboolean bluetooth_wideband;
    gboolean bluetooth_nrec;

    guint8 network;
    gboolean network_valid;
    AudioManagerSpeechBand band;

    AudioManagerOutputDevice output_device;
    AudioManagerInputDevice input_device;
    AudioManagerOutputDevice deferred_output_device;
    AudioManagerInputDevice deferred_input_device;
    AudioManagerCallTransport transport;

    guint16 pending_msg_id;
    guint16 expected_ack_id;

    GSource *ack_timeout_source;
    GSource *modem_monitor_source;
    GSource *reconnect_source;
    guint reconnect_attempts;

    MtkSpeechRecoveryFunc recovery_callback;
    gpointer recovery_user_data;
    MtkSpeechNetworkChangeFunc network_change_callback;
    gpointer network_change_user_data;
};

static void speech_enter_recovery(MtkSpeech *speech,
                                  const gchar *reason);
static gint speech_request_reopen(MtkSpeech *speech);

static const gchar *
speech_state_name(AudioManagerCallState state)
{
    switch (state) {
    case AUDIO_MANAGER_CALL_STATE_IDLE:
        return "idle";
    case AUDIO_MANAGER_CALL_STATE_PREPARING:
        return "preparing";
    case AUDIO_MANAGER_CALL_STATE_STARTING:
        return "starting";
    case AUDIO_MANAGER_CALL_STATE_ACTIVE:
        return "active";
    case AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING:
        return "device-changing";
    case AUDIO_MANAGER_CALL_STATE_STOPPING:
        return "stopping";
    case AUDIO_MANAGER_CALL_STATE_ERROR:
        return "error";
    case AUDIO_MANAGER_CALL_STATE_RECOVERING:
        return "recovering";
    }

    return "unknown";
}

static void
speech_set_state(MtkSpeech *speech,
                 AudioManagerCallState state)
{
    if (speech->state == state)
        return;

    g_debug("MediaTek speech state %s -> %s",
            speech_state_name(speech->state),
            speech_state_name(state));
    speech->state = state;
}

static void
speech_source_clear(GSource **source)
{
    if (source == NULL || *source == NULL)
        return;

    g_source_destroy(*source);
    g_source_unref(*source);
    *source = NULL;
}

static void
speech_source_finish(GSource **source)
{
    if (source == NULL || *source == NULL)
        return;

    g_source_unref(*source);
    *source = NULL;
}

static GSource *
speech_timeout_source_new(MtkSpeech *speech,
                          guint timeout_ms,
                          GSourceFunc callback)
{
    GSource *source;

    source = g_timeout_source_new(timeout_ms);
    g_source_set_callback(source, callback, speech, NULL);
    g_source_attach(source, speech->main_context);
    return source;
}

static void
speech_close_pcm(MtkSpeech *speech)
{
    AudioAlsaPcm *capture;
    AudioAlsaPcm *playback;

    if (speech == NULL)
        return;

    capture = speech->capture;
    playback = speech->playback;
    speech->capture = NULL;
    speech->playback = NULL;

    audio_alsa_pcm_close(capture);
    if (playback != capture)
        audio_alsa_pcm_close(playback);
}

static void
speech_stop_pcm(MtkSpeech *speech)
{
    AudioAlsaPcm *capture;
    AudioAlsaPcm *playback;

    if (speech == NULL)
        return;

    capture = speech->capture;
    playback = speech->playback;
    speech->capture = NULL;
    speech->playback = NULL;

    if (capture != NULL)
        audio_alsa_pcm_stop(capture);
    if (playback != NULL && playback != capture)
        audio_alsa_pcm_stop(playback);

    audio_alsa_pcm_close(capture);
    if (playback != capture)
        audio_alsa_pcm_close(playback);
}

static void
speech_clear_pending_ack(MtkSpeech *speech)
{
    speech_source_clear(&speech->ack_timeout_source);
    speech->pending_msg_id = 0;
    speech->expected_ack_id = 0;
}

static gboolean
speech_ack_timeout(gpointer user_data)
{
    MtkSpeech *speech = user_data;
    guint16 pending_msg_id = speech->pending_msg_id;
    const gchar *name = mtk_speech_message_name(pending_msg_id);

    speech_source_finish(&speech->ack_timeout_source);
    speech->pending_msg_id = 0;
    speech->expected_ack_id = 0;

    g_warning("MediaTek speech command %s (0x%04x) timed out waiting for modem ACK",
              name != NULL ? name : "unknown",
              pending_msg_id);

    if (pending_msg_id == MTK_MSG_A2M_SPH_OFF || !speech->desired_active) {
        speech->modem_active = FALSE;
        speech_stop_pcm(speech);
        if (speech->generation != NULL)
            mtk_speech_generation_reset(speech->generation);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
        speech_source_clear(&speech->modem_monitor_source);
        return G_SOURCE_REMOVE;
    }

    speech_enter_recovery(speech, "ACK timeout");
    return G_SOURCE_REMOVE;
}

static gint
speech_begin_pending_ack(MtkSpeech *speech,
                         guint16 msg_id)
{
    guint16 expected_ack;

    if (!mtk_speech_message_needs_ack(msg_id))
        return -EINVAL;

    if (speech->pending_msg_id != 0) {
        const gchar *pending_name = mtk_speech_message_name(speech->pending_msg_id);
        const gchar *new_name = mtk_speech_message_name(msg_id);

        g_warning("MediaTek speech command %s (0x%04x) blocked while waiting for %s (0x%04x)",
                  new_name != NULL ? new_name : "unknown",
                  msg_id,
                  pending_name != NULL ? pending_name : "unknown",
                  speech->pending_msg_id);
        return -EBUSY;
    }

    expected_ack = mtk_speech_message_expected_ack(msg_id);
    if (expected_ack == 0)
        return -EINVAL;

    speech->pending_msg_id = msg_id;
    speech->expected_ack_id = expected_ack;
    speech_source_clear(&speech->ack_timeout_source);
    speech->ack_timeout_source = speech_timeout_source_new(speech,
                                                           MTK_SPEECH_ACK_TIMEOUT_MS,
                                                           speech_ack_timeout);
    return 0;
}

static void
speech_send_failed(MtkSpeech *speech,
                   gint error)
{
    if (error == -EPIPE || error == -ENODEV || error == -EIO)
        speech_enter_recovery(speech, "CCCI send failure");
}

static gint
speech_send_routing_mute_start(MtkSpeech *speech)
{
    gint ret;

    if (speech == NULL || speech->ccci == NULL)
        return -EINVAL;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_MUTE_SPH_DL,
                                1,
                                0);
    if (ret < 0) {
        speech_send_failed(speech, ret);
        return ret;
    }

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_MUTE_SPH_UL,
                                1,
                                0);
    if (ret < 0)
        speech_send_failed(speech, ret);

    return ret;
}

static gint
speech_restore_routing_mute(MtkSpeech *speech)
{
    gint ret;

    if (speech == NULL || speech->ccci == NULL)
        return -EINVAL;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_MUTE_SPH_UL,
                                speech->uplink_muted ? 1 : 0,
                                0);
    if (ret < 0) {
        speech_send_failed(speech, ret);
        return ret;
    }

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_MUTE_SPH_DL,
                                speech->downlink_muted ? 1 : 0,
                                0);
    if (ret < 0)
        speech_send_failed(speech, ret);

    return ret;
}

static gboolean
speech_routing_unmute(gpointer user_data)
{
    MtkSpeech *speech = user_data;
    gint ret;

    speech_source_finish(&speech->routing_unmute_source);
    if (!speech->routing_muted)
        return G_SOURCE_REMOVE;

    speech->routing_muted = FALSE;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE ||
        speech->state == AUDIO_MANAGER_CALL_STATE_STOPPING ||
        speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
        return G_SOURCE_REMOVE;

    ret = speech_restore_routing_mute(speech);
    if (ret < 0)
        g_warning("failed to restore MediaTek speech mute state after routing: %s",
                  g_strerror(-ret));

    return G_SOURCE_REMOVE;
}

static gint
speech_send_mailbox_with_ack(MtkSpeech *speech,
                             guint16 msg_id,
                             guint16 param_16bit,
                             guint32 param_32bit)
{
    gint ret;

    ret = speech_begin_pending_ack(speech, msg_id);
    if (ret < 0)
        return ret;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                msg_id,
                                param_16bit,
                                param_32bit);
    if (ret < 0) {
        speech_clear_pending_ack(speech);
        speech_send_failed(speech, ret);
    }

    return ret;
}

static gint
speech_send_payload_with_ack(MtkSpeech *speech,
                             guint16 msg_id,
                             guint16 payload_type,
                             gconstpointer payload,
                             guint16 payload_size)
{
    gint ret;

    ret = speech_begin_pending_ack(speech, msg_id);
    if (ret < 0)
        return ret;

    ret = mtk_ccci_send_payload(speech->ccci,
                                msg_id,
                                payload_type,
                                payload,
                                payload_size);
    if (ret < 0) {
        speech_clear_pending_ack(speech);
        speech_send_failed(speech, ret);
    }

    return ret;
}

static gint
speech_sample_rate_enum(guint rate)
{
    switch (rate) {
    case 8000:
        return MTK_SPH_SAMPLE_RATE_08K;
    case 16000:
        return MTK_SPH_SAMPLE_RATE_16K;
    case 32000:
        return MTK_SPH_SAMPLE_RATE_32K;
    case 48000:
        return MTK_SPH_SAMPLE_RATE_48K;
    default:
        return -EINVAL;
    }
}

static guint
speech_device_sample_rate(const MtkSpeech *speech,
                          AudioManagerOutputDevice output_device)
{
    if (output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return speech->bluetooth_wideband ? 16000U : 8000U;

    return speech->config->speech_rate;
}

static gint
speech_ext_device(const MtkSpeech *speech,
                  AudioManagerOutputDevice output_device,
                  guint16 *ext_device)
{
    if (ext_device == NULL)
        return -EINVAL;

    switch (output_device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        if (speech->config->receiver_ext_device < 0 ||
            speech->config->receiver_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->receiver_ext_device;
        return 0;
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        if (speech->config->speaker_ext_device < 0 ||
            speech->config->speaker_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->speaker_ext_device;
        return 0;
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        if (speech->config->headphones_ext_device < 0 ||
            speech->config->headphones_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->headphones_ext_device;
        return 0;
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        if (speech->config->headset_ext_device < 0 ||
            speech->config->headset_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->headset_ext_device;
        return 0;
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        if (speech->config->bluetooth_ext_device < 0 ||
            speech->config->bluetooth_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->bluetooth_ext_device;
        return 0;
    case AUDIO_MANAGER_OUTPUT_USB:
        if (speech->config->usb_ext_device < 0 ||
            speech->config->usb_ext_device > G_MAXUINT16)
            return -ENOTSUP;
        *ext_device = (guint16)speech->config->usb_ext_device;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static guint8
speech_mute_info(const MtkSpeech *speech)
{
    guint8 mute_info = 0;

    if (speech->uplink_muted)
        mute_info |= MTK_SPH_ON_MUTE_BIT_UL;
    if (speech->downlink_muted)
        mute_info |= MTK_SPH_ON_MUTE_BIT_DL;

    return mute_info;
}

static gint
speech_info_init(MtkSpeech *speech,
                 MtkSphInfo *info,
                 AudioManagerOutputDevice output_device,
                 AudioManagerInputDevice input_device)
{
    guint16 ext_device;
    gint sample_rate;
    gint ret;

    memset(info, 0, sizeof(*info));

    sample_rate = speech_sample_rate_enum(speech_device_sample_rate(speech,
                                                                    output_device));
    if (sample_rate < 0)
        return sample_rate;

    ret = speech_ext_device(speech, output_device, &ext_device);
    if (ret < 0)
        return ret;

    info->application = MTK_SPH_APPLICATION_NORMAL;
    info->bt_info = MTK_SPH_BT_OFF;
    if (output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        info->bt_info = speech->bluetooth_wideband ?
                        MTK_SPH_BT_MSBC : MTK_SPH_BT_CVSD;
    info->sample_rate_enum = (guint8)sample_rate;
    info->opendsp_flag = 0;

    info->ext_dev_info = ext_device;
    info->mute_info = speech_mute_info(speech);
    info->num_smart_pa = (guint8)speech->config->smart_pa_count;

    return mtk_speech_generation_prepare_info(speech->generation,
                                              output_device,
                                              input_device,
                                              info);
}

static gint
speech_flush_deferred_controls(MtkSpeech *speech)
{
    MtkSphInfo info;
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE ||
        speech->pending_msg_id != 0)
        return 0;

    if (speech->deferred_device_change) {
        AudioManagerOutputDevice output_device = speech->deferred_output_device;
        AudioManagerInputDevice input_device = speech->deferred_input_device;

        speech->deferred_device_change = FALSE;
        ret = mtk_speech_device_change(speech, output_device, input_device);
        if (ret == -EBUSY) {
            speech->deferred_output_device = output_device;
            speech->deferred_input_device = input_device;
            speech->deferred_device_change = TRUE;
        }
        return ret;
    }

    if (speech->deferred_volume_update) {
        ret = speech_info_init(speech, &info,
                               speech->output_device,
                               speech->input_device);
        if (ret < 0)
            return ret;
        ret = speech_send_payload_with_ack(speech,
                                           MTK_MSG_A2M_DYNAMIC_PAR_IN_STRUCT_SHM,
                                           MTK_SHARE_BUFF_DATA_TYPE_CCCI_SPH_INFO,
                                           &info,
                                           sizeof(info));
        if (ret == 0)
            speech->deferred_volume_update = FALSE;
        return ret;
    }

    if (speech->deferred_uplink_mute) {
        ret = speech_send_mailbox_with_ack(speech,
                                           MTK_MSG_A2M_MUTE_SPH_UL,
                                           speech->uplink_muted ? 1 : 0,
                                           0);
        if (ret == 0)
            speech->deferred_uplink_mute = FALSE;
        return ret;
    }

    if (speech->deferred_downlink_mute) {
        ret = speech_send_mailbox_with_ack(speech,
                                           MTK_MSG_A2M_MUTE_SPH_DL,
                                           speech->downlink_muted ? 1 : 0,
                                           0);
        if (ret == 0)
            speech->deferred_downlink_mute = FALSE;
        return ret;
    }

    return 0;
}

static gint
speech_open_pcm(MtkSpeech *speech,
                AudioManagerOutputDevice output_device,
                GError **error)
{
    AudioAlsaPcmConfig config = { 0 };

    if (output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return -ENOTSUP;

    config.device = speech->config->hostless_pcm;
    config.format = speech->config->speech_format;
    config.rate = speech->config->speech_rate;
    config.channels = speech->config->speech_channels;
    config.period_size = speech->config->speech_period_size;
    config.period_count = speech->config->speech_period_count;
    config.nonblock = FALSE;
    config.disable_stop_threshold = TRUE;

    config.direction = AUDIO_DIRECTION_INPUT;
    speech->capture = audio_alsa_pcm_open(speech->card, &config, error);
    if (speech->capture == NULL)
        return -EIO;

    config.direction = AUDIO_DIRECTION_OUTPUT;
    speech->playback = audio_alsa_pcm_open(speech->card, &config, error);
    if (speech->playback == NULL) {
        audio_alsa_pcm_close(speech->capture);
        speech->capture = NULL;
        return -EIO;
    }

    return 0;
}

static gboolean
speech_pcm_needs_reopen(MtkSpeech *speech,
                        AudioManagerOutputDevice output_device)
{
    gboolean old_bluetooth;
    gboolean new_bluetooth;

    if (speech->transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS)
        return FALSE;

    old_bluetooth = speech->output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    new_bluetooth = output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH;

    if (new_bluetooth)
        return speech->capture != NULL || speech->playback != NULL;

    if (old_bluetooth)
        return TRUE;

    return speech->capture == NULL || speech->playback == NULL;
}

static gint
speech_reopen_pcm(MtkSpeech *speech,
                  AudioManagerOutputDevice output_device)
{
    GError *error = NULL;
    gint ret;

    speech_stop_pcm(speech);

    if (output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return 0;

    ret = speech_open_pcm(speech, output_device, &error);
    if (ret < 0) {
        if (error != NULL) {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        return ret;
    }

    ret = audio_alsa_pcm_start(speech->playback);
    if (ret < 0)
        goto fail;

    ret = audio_alsa_pcm_start(speech->capture);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    speech_stop_pcm(speech);
    return ret;
}

static gint
speech_transport_open(MtkSpeech *speech,
                      GError **error);

static gint
speech_generation_open(MtkSpeech *speech,
                       GError **error)
{
    if (speech->generation != NULL)
        return 0;

    g_debug("initializing MediaTek speech generation '%s'",
            speech->config->generation);

    speech->generation = mtk_speech_generation_new(speech->config,
                                                   speech->card,
                                                   error);
    if (speech->generation == NULL)
        return -EIO;

    mtk_speech_generation_set_bluetooth(speech->generation,
                                        speech->bluetooth_wideband,
                                        speech->bluetooth_nrec);

    return 0;
}

static gboolean
speech_wait_modem_ready(MtkSpeech *speech)
{
    guint waited_ms = 0;

    while (waited_ms < MTK_SPEECH_MODEM_READY_TIMEOUT_MS) {
        if (mtk_ccci_modem_ready(speech->ccci))
            return TRUE;

        usleep(MTK_SPEECH_MODEM_READY_RETRY_MS * 1000U);
        waited_ms += MTK_SPEECH_MODEM_READY_RETRY_MS;
    }

    return mtk_ccci_modem_ready(speech->ccci);
}

static gboolean
speech_reconnect_tick(gpointer user_data)
{
    MtkSpeech *speech = user_data;
    GError *error = NULL;
    gint ret;

    if (speech->ccci == NULL) {
        speech_source_finish(&speech->reconnect_source);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ERROR);
        return G_SOURCE_REMOVE;
    }

    if (mtk_ccci_is_connected(speech->ccci)) {
        speech_source_finish(&speech->reconnect_source);
        speech->reconnect_attempts = 0;
        return G_SOURCE_REMOVE;
    }

    speech->reconnect_attempts++;
    ret = mtk_ccci_reopen(speech->ccci, &error);
    if (ret == 0) {
        g_message("MediaTek CCCI transport reopened after %u attempt%s",
                  speech->reconnect_attempts,
                  speech->reconnect_attempts == 1 ? "" : "s");
        speech_source_finish(&speech->reconnect_source);
        speech->reconnect_attempts = 0;

        if (speech->desired_active &&
            !speech->modem_epof &&
            mtk_ccci_modem_ready(speech->ccci))
            speech_request_reopen(speech);

        return G_SOURCE_REMOVE;
    }

    if (error != NULL)
        g_error_free(error);

    if (speech->reconnect_attempts >= MTK_SPEECH_CCCI_REOPEN_MAX_TRIES) {
        g_warning("MediaTek CCCI transport recovery failed after %u attempts",
                  speech->reconnect_attempts);
        speech_source_finish(&speech->reconnect_source);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ERROR);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
speech_start_reconnect(MtkSpeech *speech)
{
    if (speech->reconnect_source != NULL)
        return;

    speech->reconnect_attempts = 0;
    speech->reconnect_source = speech_timeout_source_new(speech,
                                                         MTK_SPEECH_CCCI_REOPEN_RETRY_MS,
                                                         speech_reconnect_tick);
}

static gboolean
speech_modem_monitor(gpointer user_data)
{
    MtkSpeech *speech = user_data;

    if (!speech->desired_active) {
        speech_source_finish(&speech->modem_monitor_source);
        return G_SOURCE_REMOVE;
    }

    if (speech->ccci == NULL || !mtk_ccci_is_connected(speech->ccci)) {
        speech_enter_recovery(speech, "CCCI transport unavailable");
        speech_start_reconnect(speech);
        return G_SOURCE_CONTINUE;
    }

    if (!mtk_ccci_modem_ready(speech->ccci)) {
        speech_enter_recovery(speech, "modem status is not ready");
        return G_SOURCE_CONTINUE;
    }

    if (speech->state == AUDIO_MANAGER_CALL_STATE_RECOVERING &&
        !speech->modem_epof)
        speech_request_reopen(speech);

    return G_SOURCE_CONTINUE;
}

static void
speech_start_modem_monitor(MtkSpeech *speech)
{
    if (speech->modem_monitor_source != NULL)
        return;

    speech->modem_monitor_source = speech_timeout_source_new(speech,
                                                             MTK_SPEECH_MODEM_MONITOR_MS,
                                                             speech_modem_monitor);
}

static void
speech_enter_recovery(MtkSpeech *speech,
                      const gchar *reason)
{
    if (speech == NULL)
        return;

    /* recovery restarts speech using the backend's current route */
    speech->deferred_device_change = FALSE;

    if (!speech->desired_active) {
        speech_clear_pending_ack(speech);
        speech->modem_active = FALSE;
        speech_stop_pcm(speech);
        if (speech->generation != NULL)
            mtk_speech_generation_reset(speech->generation);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
        return;
    }

    if (speech->state == AUDIO_MANAGER_CALL_STATE_RECOVERING) {
        if (speech->ccci == NULL || !mtk_ccci_is_connected(speech->ccci))
            speech_start_reconnect(speech);
        return;
    }

    g_warning("MediaTek speech entering recovery: %s", reason);

    speech_clear_pending_ack(speech);
    speech->modem_active = FALSE;
    speech_stop_pcm(speech);
    if (speech->generation != NULL)
        mtk_speech_generation_reset(speech->generation);
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_RECOVERING);

    if (speech->ccci == NULL || !mtk_ccci_is_connected(speech->ccci))
        speech_start_reconnect(speech);
}

static gint
speech_request_reopen(MtkSpeech *speech)
{
    gint ret;

    if (!speech->desired_active ||
        speech->state != AUDIO_MANAGER_CALL_STATE_RECOVERING)
        return 0;

    if (speech->recovery_callback_running)
        return -EBUSY;

    if (speech->ccci == NULL ||
        !mtk_ccci_is_connected(speech->ccci) ||
        !mtk_ccci_modem_ready(speech->ccci) ||
        speech->modem_epof)
        return -EAGAIN;

    if (speech->recovery_callback == NULL)
        return -ENOSYS;

    speech->recovery_callback_running = TRUE;
    ret = speech->recovery_callback(speech,
                                    speech->recovery_user_data);
    speech->recovery_callback_running = FALSE;

    if (ret < 0 && ret != -EAGAIN) {
        g_warning("MediaTek phone call path recovery failed: %s",
                  g_strerror(-ret));
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ERROR);
    }

    return ret;
}

static void
speech_ccci_fault(MtkCcci *ccci,
                  GIOCondition condition,
                  gpointer user_data)
{
    MtkSpeech *speech = user_data;

    (void)ccci;

    g_warning("MediaTek CCCI transport fault, condition 0x%x", condition);
    speech_enter_recovery(speech, "CCCI fd fault");
    speech_start_reconnect(speech);
}

static gint
speech_read_shared_message(MtkSpeech *speech,
                           const MtkCcciMessage *message,
                           guint16 expected_type,
                           gpointer buffer,
                           guint16 expected_size)
{
    guint16 type = 0;
    guint16 size = expected_size;
    gint ret;

    if (message->has_payload) {
        if (message->payload_type != expected_type ||
            message->payload_size != expected_size ||
            message->payload_index != message->payload_total)
            return -EPROTO;
        memcpy(buffer, message->payload, expected_size);
        return 0;
    }

    if (message->param_16bit < MTK_CCCI_MAX_MD_PAYLOAD_HEADER_SIZE)
        return -EPROTO;

    ret = mtk_speech_generation_read_md_data(speech->generation,
                                             buffer,
                                             &type,
                                             &size,
                                             message->param_16bit,
                                             message->param_32bit);
    if (ret < 0)
        return ret;
    if (type != expected_type || size != expected_size)
        return -EPROTO;
    return 0;
}

static void
speech_process_modem_alive(MtkSpeech *speech,
                           const MtkCcciMessage *message)
{
    MtkMdAliveInfo info;
    gboolean was_recovering;
    gint ret;

    memset(&info, 0, sizeof(info));
    ret = speech_read_shared_message(speech,
                                     message,
                                     MTK_SHARE_BUFF_DATA_TYPE_CCCI_MD_ALIVE_INFO,
                                     &info,
                                     sizeof(info));
    if (ret < 0) {
        g_warning("failed to read MediaTek MD alive payload: %s",
                  g_strerror(-ret));
        return;
    }

    g_debug("MediaTek modem alive RX header_version=0x%04x md_version=0x%04x source=%s",
            info.header_md_version,
            info.md_version,
            message->has_payload ? "inline" : "CCCI SHM");

    was_recovering = speech->state == AUDIO_MANAGER_CALL_STATE_RECOVERING ||
                     speech->modem_epof;
    speech->modem_epof = FALSE;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_MD_ALIVE_ACK_BACK,
                                0,
                                0);
    if (ret < 0)
        g_warning("failed to acknowledge MediaTek modem alive notification: %s",
                  g_strerror(-ret));
    else
        g_debug("MediaTek modem alive notification acknowledged");

    if (speech->desired_active && was_recovering) {
        if (speech->state != AUDIO_MANAGER_CALL_STATE_RECOVERING)
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_RECOVERING);
        speech_request_reopen(speech);
    } else if (speech->desired_active) {
        g_debug("MediaTek modem alive notification received during %s",
                speech_state_name(speech->state));
    }
}

static AudioManagerSpeechBand
speech_band_from_mtk(guint8 band)
{
    switch (band) {
    case 0:
        return AUDIO_MANAGER_SPEECH_BAND_NARROW;
    case 1:
        return AUDIO_MANAGER_SPEECH_BAND_WIDE;
    case 2:
        return AUDIO_MANAGER_SPEECH_BAND_SUPER_WIDE;
    default:
        return AUDIO_MANAGER_SPEECH_BAND_UNKNOWN;
    }
}

static void
speech_process_network_status(MtkSpeech *speech,
                              const MtkCcciMessage *message)
{
    guint16 status = message->param_16bit;
    gboolean network_support = (status & (1U << 15)) != 0;
    guint8 band_raw;
    guint8 network_raw;
    AudioManagerSpeechBand band;
    gboolean network_changed;
    gboolean band_changed;

    if (network_support)
        band_raw = (guint8)((status >> 4) & 0x3U);
    else
        band_raw = (guint8)((status >> 3) & 0x7U);

    network_raw = (guint8)(status & 0x0fU);
    band = speech_band_from_mtk(band_raw);
    network_changed = !speech->network_valid || network_raw != speech->network;
    band_changed = band != speech->band;

    speech->network = network_raw;
    speech->network_valid = TRUE;
    speech->band = band;

    g_debug("MediaTek speech network status 0x%04x: network=%u band=%u%s%s",
            status,
            network_raw,
            band_raw,
            network_changed ? " (network changed)" : "",
            band_changed ? " (band changed)" : "");

    if ((network_changed || band_changed) &&
        speech->network_change_callback != NULL)
        speech->network_change_callback(speech,
                                        status,
                                        speech->network_change_user_data);
}

static void
speech_process_network_codec_info(MtkSpeech *speech,
                                  const MtkCcciMessage *message)
{
    MtkNetworkCodecInfo info;
    gchar codec_info[MTK_SPEECH_CODEC_INFO_LENGTH + 1];
    gchar codec_operation[MTK_SPEECH_CODEC_INFO_LENGTH + 1];
    gint ret;

    memset(&info, 0, sizeof(info));
    ret = speech_read_shared_message(speech,
                                     message,
                                     MTK_SHARE_BUFF_DATA_TYPE_CCCI_NW_CODEC_INFO,
                                     &info,
                                     sizeof(info));
    if (ret < 0) {
        g_warning("failed to read MediaTek network codec payload: %s",
                  g_strerror(-ret));
        return;
    }

    memcpy(codec_info, info.codec_info, MTK_SPEECH_CODEC_INFO_LENGTH);
    codec_info[MTK_SPEECH_CODEC_INFO_LENGTH] = '\0';
    memcpy(codec_operation, info.codec_operation, MTK_SPEECH_CODEC_INFO_LENGTH);
    codec_operation[MTK_SPEECH_CODEC_INFO_LENGTH] = '\0';

    g_debug("MediaTek network codec RX source=%s codec=\"%s\" operation=\"%s\"",
            message->has_payload ? "inline" : "CCCI SHM",
            codec_info,
            codec_operation);

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_NW_CODEC_INFO_READ_ACK,
                                0,
                                0);
    if (ret < 0)
        g_warning("failed to acknowledge MediaTek network codec info: %s",
                  g_strerror(-ret));
}

static void
speech_ccci_message(MtkCcci *ccci,
                    const MtkCcciMessage *message,
                    gpointer user_data)
{
    MtkSpeech *speech = user_data;
    const gchar *name;
    gint ret;

    (void)ccci;

    name = mtk_speech_message_name(message->msg_id);

    switch (message->msg_id) {
    case MTK_MSG_M2A_NETWORK_STATUS_NOTIFY:
        speech_process_network_status(speech, message);
        return;
    case MTK_MSG_M2A_NW_CODEC_INFO_NOTIFY:
        speech_process_network_codec_info(speech, message);
        return;
    case MTK_MSG_M2A_EPOF_NOTIFY:
        g_warning("MediaTek modem EPOF notification received");
        ret = mtk_ccci_send_mailbox(speech->ccci,
                                    MTK_MSG_A2M_EPOF_ACK,
                                    0,
                                    0);
        if (ret < 0)
            g_warning("failed to acknowledge MediaTek EPOF: %s",
                      g_strerror(-ret));
        speech->modem_epof = TRUE;
        speech_enter_recovery(speech, "modem EPOF");
        return;
    case MTK_MSG_M2A_MD_ALIVE:
        speech_process_modem_alive(speech, message);
        return;
    case MTK_MSG_M2A_EM_DATA_REQUEST:
        g_debug("MediaTek EM data request received (GEN97 stub)");
        return;
    default:
        break;
    }

    switch (message->msg_id) {
    case MTK_MSG_M2A_SPH_ON_ACK:
    case MTK_MSG_M2A_SPH_OFF_ACK:
    case MTK_MSG_M2A_SPH_DEV_CHANGE_ACK:
    case MTK_MSG_M2A_MUTE_SPH_UL_ACK:
    case MTK_MSG_M2A_MUTE_SPH_DL_ACK:
    case MTK_MSG_M2A_DYNAMIC_PAR_IN_STRUCT_SHM_ACK:
        if (speech->expected_ack_id == 0) {
            g_debug("ignoring stale MediaTek speech ACK %s (0x%04x)",
                    name != NULL ? name : "unknown",
                    message->msg_id);
            return;
        }

        if (message->msg_id != speech->expected_ack_id) {
            const gchar *expected_name = mtk_speech_message_name(speech->expected_ack_id);

            g_debug("ignoring out of order MediaTek speech ACK %s (0x%04x) while waiting for %s (0x%04x)",
                    name != NULL ? name : "unknown",
                    message->msg_id,
                    expected_name != NULL ? expected_name : "unknown",
                    speech->expected_ack_id);
            return;
        }

        speech_clear_pending_ack(speech);
        break;
    default:
        g_debug("MediaTek CCCI message %s%s0x%04x",
                name != NULL ? name : "",
                name != NULL ? " " : "",
                message->msg_id);
        return;
    }

    switch (message->msg_id) {
    case MTK_MSG_M2A_SPH_ON_ACK:
        speech->modem_active = TRUE;
        if (!speech->desired_active) {
            if (speech->state != AUDIO_MANAGER_CALL_STATE_STOPPING)
                speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_STOPPING);

            ret = speech_send_mailbox_with_ack(speech,
                                               MTK_MSG_A2M_SPH_OFF,
                                               0,
                                               0);
            if (ret < 0) {
                g_warning("failed to stop MediaTek speech after delayed speech on ACK: %s",
                          g_strerror(-ret));
                speech->modem_active = FALSE;
                speech_source_clear(&speech->modem_monitor_source);
                if (speech->generation != NULL)
                    mtk_speech_generation_reset(speech->generation);
                speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
            } else {
                g_debug("MediaTek modem acknowledged speech on after stop request.");
            }
        } else if (speech->state == AUDIO_MANAGER_CALL_STATE_STARTING)
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ACTIVE);
        else
            g_warning("speech on ACK received in state %s",
                      speech_state_name(speech->state));
        g_debug("MediaTek modem acknowledged speech on");
        break;
    case MTK_MSG_M2A_SPH_OFF_ACK:
        speech->modem_active = FALSE;
        if (speech->generation != NULL)
            mtk_speech_generation_reset(speech->generation);
        speech_source_clear(&speech->modem_monitor_source);
        if (speech->state == AUDIO_MANAGER_CALL_STATE_STOPPING ||
            speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
        else
            g_warning("speech off ACK received in state %s",
                      speech_state_name(speech->state));
        g_debug("MediaTek modem acknowledged speech off");
        break;
    case MTK_MSG_M2A_SPH_DEV_CHANGE_ACK:
        if (speech->state == AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING)
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ACTIVE);
        else
            g_warning("speech device change ACK received in state %s",
                      speech_state_name(speech->state));
        g_debug("MediaTek modem acknowledged speech device change");
        break;
    case MTK_MSG_M2A_MUTE_SPH_UL_ACK:
        g_debug("MediaTek modem acknowledged uplink mute change");
        break;
    case MTK_MSG_M2A_MUTE_SPH_DL_ACK:
        g_debug("MediaTek modem acknowledged downlink mute change");
        break;
    case MTK_MSG_M2A_DYNAMIC_PAR_IN_STRUCT_SHM_ACK:
        g_debug("MediaTek modem acknowledged dynamic speech parameter update");
        break;
    default:
        break;
    }

    ret = speech_flush_deferred_controls(speech);
    if (ret < 0 && ret != -EBUSY)
        g_warning("failed to flush deferred MediaTek speech controls: %s",
                  g_strerror(-ret));
}

static gint
speech_transport_open(MtkSpeech *speech,
                      GError **error)
{
    if (speech->ccci != NULL)
        return 0;

    g_debug("initializing MediaTek speech transport");

    speech->ccci = mtk_ccci_new(speech->config->control_device,
                                speech->main_context,
                                speech_ccci_message,
                                speech_ccci_fault,
                                speech,
                                error);
    if (speech->ccci == NULL)
        return -EIO;

    return 0;
}

MtkSpeech *
mtk_speech_new(const MtkConfig *config,
               AudioAlsaCard *card,
               GMainContext *main_context,
               MtkSpeechRecoveryFunc recovery_callback,
               gpointer recovery_user_data,
               MtkSpeechNetworkChangeFunc network_change_callback,
               gpointer network_change_user_data,
               GError **error)
{
    MtkSpeech *speech;

    (void)error;

    if (config == NULL || card == NULL || main_context == NULL)
        return NULL;

    speech = g_new0(MtkSpeech, 1);
    speech->config = config;
    speech->card = card;
    speech->main_context = main_context;
    speech->state = AUDIO_MANAGER_CALL_STATE_IDLE;
    speech->band = AUDIO_MANAGER_SPEECH_BAND_UNKNOWN;
    speech->recovery_callback = recovery_callback;
    speech->recovery_user_data = recovery_user_data;
    speech->network_change_callback = network_change_callback;
    speech->network_change_user_data = network_change_user_data;

    return speech;
}

void
mtk_speech_free(MtkSpeech *speech)
{
    if (speech == NULL)
        return;

    speech->desired_active = FALSE;
    speech_source_clear(&speech->ack_timeout_source);
    speech_source_clear(&speech->modem_monitor_source);
    speech_source_clear(&speech->reconnect_source);
    speech_source_clear(&speech->routing_unmute_source);

    if (mtk_speech_is_active(speech))
        mtk_speech_stop(speech);

    speech_stop_pcm(speech);
    mtk_speech_generation_free(speech->generation);
    mtk_ccci_free(speech->ccci);
    g_free(speech);
}

static gint
speech_start_path(MtkSpeech *speech,
                  AudioManagerOutputDevice output_device,
                  AudioManagerInputDevice input_device,
                  gboolean recovering)
{
    MtkSphInfo info;
    GError *error = NULL;
    gint ret;

    ret = speech_generation_open(speech, &error);
    if (ret < 0) {
        if (error != NULL) {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        return ret;
    }

    if (speech->transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
        ret = speech_open_pcm(speech, output_device, &error);
        if (ret < 0) {
            speech->last_start_failed_pcm = TRUE;
            if (error != NULL) {
                g_warning("%s", error->message);
                g_error_free(error);
            }
            return ret;
        }

        /* kernel space starts modem output before modem input. */
        ret = audio_alsa_pcm_start(speech->playback);
        if (ret < 0) {
            speech->last_start_failed_pcm = TRUE;
            goto fail_pcm;
        }

        ret = audio_alsa_pcm_start(speech->capture);
        if (ret < 0) {
            speech->last_start_failed_pcm = TRUE;
            goto fail_pcm;
        }
    }

    ret = speech_info_init(speech, &info, output_device, input_device);
    if (ret < 0)
        goto fail_pcm;

    speech->output_device = output_device;
    speech->input_device = input_device;
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_STARTING);
    ret = speech_send_payload_with_ack(speech,
                                       MTK_MSG_A2M_SPH_ON,
                                       MTK_SHARE_BUFF_DATA_TYPE_CCCI_SPH_INFO,
                                       &info,
                                       sizeof(info));
    if (ret < 0)
        goto fail_pcm;

    speech_start_modem_monitor(speech);
    if (recovering)
        g_message("MediaTek phone call speech path reopened after modem recovery");
    return 0;

fail_pcm:
    speech_clear_pending_ack(speech);
    speech_stop_pcm(speech);
    return ret;
}

gint
mtk_speech_start(MtkSpeech *speech,
                 AudioManagerOutputDevice output_device,
                 AudioManagerInputDevice input_device,
                 AudioManagerCallTransport transport)
{
    GError *error = NULL;
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_IDLE)
        return speech->state == AUDIO_MANAGER_CALL_STATE_ERROR ?
               -EPIPE : -EBUSY;

    speech->desired_active = TRUE;
    speech->modem_epof = FALSE;
    speech->last_start_failed_pcm = FALSE;
    speech->deferred_device_change = FALSE;
    speech->transport = transport;
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_PREPARING);

    ret = speech_transport_open(speech, &error);
    if (ret < 0) {
        if (error != NULL) {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        goto fail_idle;
    }

    if (!speech_wait_modem_ready(speech)) {
        g_warning("MediaTek modem is not ready for speech after %u ms",
                  MTK_SPEECH_MODEM_READY_TIMEOUT_MS);
        ret = -EPIPE;
        goto fail_idle;
    }

    ret = speech_start_path(speech,
                            output_device,
                            input_device,
                            FALSE);
    if (ret < 0)
        goto fail_idle;

    return 0;

fail_idle:
    speech->desired_active = FALSE;
    speech_clear_pending_ack(speech);
    speech_stop_pcm(speech);
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
    return ret;
}

gint
mtk_speech_recover(MtkSpeech *speech,
                   AudioManagerOutputDevice output_device,
                   AudioManagerInputDevice input_device)
{
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (!speech->desired_active)
        return -ECANCELED;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_RECOVERING)
        return -EINVAL;
    if (speech->ccci == NULL ||
        !mtk_ccci_is_connected(speech->ccci) ||
        !mtk_ccci_modem_ready(speech->ccci) ||
        speech->modem_epof)
        return -EAGAIN;

    speech_stop_pcm(speech);
    if (speech->generation != NULL)
        mtk_speech_generation_reset(speech->generation);

    ret = speech_start_path(speech,
                            output_device,
                            input_device,
                            TRUE);
    if (ret < 0)
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ERROR);

    return ret;
}

gint
mtk_speech_stop(MtkSpeech *speech)
{
    gint ret = 0;
    gint tmp;

    if (speech == NULL)
        return -EINVAL;

    speech_source_clear(&speech->routing_unmute_source);
    speech->routing_muted = FALSE;
    speech->deferred_device_change = FALSE;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE)
        return 0;

    if (speech->state == AUDIO_MANAGER_CALL_STATE_RECOVERING ||
        speech->state == AUDIO_MANAGER_CALL_STATE_ERROR) {
        speech->desired_active = FALSE;
        speech_clear_pending_ack(speech);
        speech_source_clear(&speech->modem_monitor_source);
        speech_source_clear(&speech->reconnect_source);
        speech->modem_active = FALSE;
        speech_stop_pcm(speech);
        if (speech->generation != NULL)
            mtk_speech_generation_reset(speech->generation);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
        return 0;
    }

    if (speech->state == AUDIO_MANAGER_CALL_STATE_STARTING) {
        speech->desired_active = FALSE;
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_STOPPING);
        speech_source_clear(&speech->modem_monitor_source);
        speech_stop_pcm(speech);
        return 0;
    }

    if (speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE)
        return -EBUSY;

    speech->desired_active = FALSE;
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_STOPPING);
    tmp = speech_send_mailbox_with_ack(speech,
                                       MTK_MSG_A2M_SPH_OFF,
                                       0,
                                       0);
    if (tmp < 0) {
        ret = tmp;
        speech->modem_active = FALSE;
        speech_source_clear(&speech->modem_monitor_source);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_IDLE);
    }

    if (speech->capture != NULL) {
        tmp = audio_alsa_pcm_stop(speech->capture);
        if (tmp < 0 && ret == 0)
            ret = tmp;
    }

    if (speech->playback != NULL) {
        tmp = audio_alsa_pcm_stop(speech->playback);
        if (tmp < 0 && ret == 0)
            ret = tmp;
    }

    speech_close_pcm(speech);
    return ret;
}

gint
mtk_speech_device_change(MtkSpeech *speech,
                         AudioManagerOutputDevice output_device,
                         AudioManagerInputDevice input_device)
{
    MtkSphInfo info;
    AudioManagerOutputDevice old_output_device;
    AudioManagerInputDevice old_input_device;
    gboolean reopen_pcm;
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE)
        return 0;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
        return -EPIPE;

    if (output_device == speech->output_device &&
        input_device == speech->input_device) {
        speech->deferred_device_change = FALSE;
        return 0;
    }

    if (speech->state == AUDIO_MANAGER_CALL_STATE_STARTING ||
        speech->state == AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING ||
        (speech->state == AUDIO_MANAGER_CALL_STATE_ACTIVE &&
         speech->pending_msg_id != 0)) {
        speech->deferred_output_device = output_device;
        speech->deferred_input_device = input_device;
        speech->deferred_device_change = TRUE;
        g_debug("MediaTek speech device change queued output=%u input=%u state=%s",
                output_device, input_device, speech_state_name(speech->state));
        return 0;
    }

    if (speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE)
        return -EBUSY;

    ret = speech_info_init(speech, &info, output_device, input_device);
    if (ret < 0)
        return ret;

    old_output_device = speech->output_device;
    old_input_device = speech->input_device;
    reopen_pcm = speech_pcm_needs_reopen(speech, output_device);
    if (reopen_pcm) {
        ret = speech_reopen_pcm(speech, output_device);
        if (ret < 0)
            return ret;
    }

    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING);
    ret = speech_send_payload_with_ack(speech,
                                       MTK_MSG_A2M_SPH_DEV_CHANGE,
                                       MTK_SHARE_BUFF_DATA_TYPE_CCCI_SPH_INFO,
                                       &info,
                                       sizeof(info));
    if (ret < 0) {
        if (speech->state == AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING)
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ACTIVE);
        if (reopen_pcm) {
            gint rollback_ret = speech_reopen_pcm(speech, old_output_device);
            if (rollback_ret < 0)
                g_warning("failed to restore MediaTek speech PCM after device change failure: %s",
                          g_strerror(-rollback_ret));
        }
        speech->output_device = old_output_device;
        speech->input_device = old_input_device;
        return ret;
    }

    speech->output_device = output_device;
    speech->input_device = input_device;
    return 0;
}

gint
mtk_speech_set_transport(MtkSpeech *speech,
                         AudioManagerCallTransport transport,
                         AudioManagerOutputDevice output_device,
                         AudioManagerInputDevice input_device)
{
    MtkSphInfo info;
    AudioManagerCallTransport old_transport;
    AudioManagerOutputDevice old_output;
    AudioManagerInputDevice old_input;
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE)
        return speech->state == AUDIO_MANAGER_CALL_STATE_ERROR ? -EPIPE : -EBUSY;
    if (speech->transport == transport)
        return mtk_speech_device_change(speech, output_device, input_device);

    old_transport = speech->transport;
    old_output = speech->output_device;
    old_input = speech->input_device;

    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        /*
         * Hostful calls route modem speech through the AP facing speech interface
         * instead of the direct ADDA hostless path. The consumer opens Capture_2
         * and Playback_2 separately to read downlink and write uplink PCM.
         */
        ret = speech_info_init(speech, &info,
                               AUDIO_MANAGER_OUTPUT_USB,
                               AUDIO_MANAGER_INPUT_USB);
        if (ret < 0)
            return ret;
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING);
        ret = speech_send_payload_with_ack(speech,
                                           MTK_MSG_A2M_SPH_DEV_CHANGE,
                                           MTK_SHARE_BUFF_DATA_TYPE_CCCI_SPH_INFO,
                                           &info,
                                           sizeof(info));
        if (ret < 0) {
            speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ACTIVE);
            return ret;
        }
        speech_stop_pcm(speech);
        speech->transport = transport;
        speech->output_device = AUDIO_MANAGER_OUTPUT_USB;
        speech->input_device = AUDIO_MANAGER_INPUT_USB;
        return 0;
    }

    /*
     * For hostful -> hostless, make the local path ready before asking MD1 to
     * switch devices so there is no interval with no usable speech path. Bluetooth
     * hostless speech has no AP side PCM, so speech_reopen_pcm() only tears down
     * the hostful PCM.
     */
    ret = speech_reopen_pcm(speech, output_device);
    if (ret < 0)
        return ret;
    ret = speech_info_init(speech, &info, output_device, input_device);
    if (ret < 0) {
        speech_stop_pcm(speech);
        return ret;
    }
    speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING);
    ret = speech_send_payload_with_ack(speech,
                                       MTK_MSG_A2M_SPH_DEV_CHANGE,
                                       MTK_SHARE_BUFF_DATA_TYPE_CCCI_SPH_INFO,
                                       &info,
                                       sizeof(info));
    if (ret < 0) {
        speech_stop_pcm(speech);
        speech_set_state(speech, AUDIO_MANAGER_CALL_STATE_ACTIVE);
        speech->transport = old_transport;
        speech->output_device = old_output;
        speech->input_device = old_input;
        return ret;
    }
    speech->transport = transport;
    speech->output_device = output_device;
    speech->input_device = input_device;
    return 0;
}

gint
mtk_speech_set_parameter_volume_index(MtkSpeech *speech,
                                      guint index)
{
    if (speech == NULL)
        return -EINVAL;
    if (speech->generation == NULL)
        return 0;

    mtk_speech_generation_set_volume_index(speech->generation, index);
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE) {
        speech->deferred_volume_update = FALSE;
        return 0;
    }
    if (speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
        return -EPIPE;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_STOPPING)
        return -EBUSY;

    speech->deferred_volume_update = TRUE;
    g_debug("MediaTek dynamic speech parameter queued reason=volume index=%u", index);
    return speech_flush_deferred_controls(speech);
}

gint
mtk_speech_set_bluetooth_parameters(MtkSpeech *speech,
                                    gboolean wideband,
                                    gboolean nrec)
{
    gboolean changed;

    if (speech == NULL)
        return -EINVAL;

    changed = speech->bluetooth_wideband != wideband ||
              speech->bluetooth_nrec != nrec;
    speech->bluetooth_wideband = wideband;
    speech->bluetooth_nrec = nrec;

    g_debug("MediaTek speech BT parameter state wideband=%d nrec=%d",
            wideband, nrec);
    if (speech->generation != NULL)
        mtk_speech_generation_set_bluetooth(speech->generation, wideband, nrec);

    if (changed &&
        speech->state == AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        speech->transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        speech->output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return mtk_speech_device_change(speech,
                                        speech->output_device,
                                        speech->input_device);

    return 0;
}

gint
mtk_speech_set_downlink_gain(MtkSpeech *speech,
                             gint16 gain)
{
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_STARTING &&
        speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        speech->state != AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING)
        return 0;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_SPH_DL_DIGIT_VOLUME,
                                (guint16)gain,
                                0);
    if (ret < 0)
        speech_send_failed(speech, ret);
    return ret;
}

gint
mtk_speech_set_uplink_gain(MtkSpeech *speech,
                           gint16 gain)
{
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_STARTING &&
        speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        speech->state != AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING)
        return 0;

    ret = mtk_ccci_send_mailbox(speech->ccci,
                                MTK_MSG_A2M_SPH_UL_DIGIT_VOLUME,
                                (guint16)gain,
                                0);
    if (ret < 0)
        speech_send_failed(speech, ret);
    return ret;
}

gint
mtk_speech_set_uplink_mute(MtkSpeech *speech,
                           gboolean muted)
{
    if (speech == NULL)
        return -EINVAL;

    speech->uplink_muted = muted;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE) {
        speech->deferred_uplink_mute = FALSE;
        return 0;
    }
    if (speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
        return -EPIPE;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_STOPPING)
        return -EBUSY;

    speech->deferred_uplink_mute = TRUE;
    return speech_flush_deferred_controls(speech);
}

gint
mtk_speech_set_downlink_mute(MtkSpeech *speech,
                             gboolean muted)
{
    if (speech == NULL)
        return -EINVAL;

    speech->downlink_muted = muted;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_IDLE) {
        speech->deferred_downlink_mute = FALSE;
        return 0;
    }
    if (speech->state == AUDIO_MANAGER_CALL_STATE_ERROR)
        return -EPIPE;
    if (speech->state == AUDIO_MANAGER_CALL_STATE_STOPPING)
        return -EBUSY;

    speech->deferred_downlink_mute = TRUE;
    return speech_flush_deferred_controls(speech);
}

gint
mtk_speech_routing_mute_start(MtkSpeech *speech)
{
    gint ret;

    if (speech == NULL)
        return -EINVAL;
    if (speech->state != AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        speech->state != AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING)
        return 0;

    speech_source_clear(&speech->routing_unmute_source);
    speech->routing_muted = TRUE;

    ret = speech_send_routing_mute_start(speech);
    if (ret < 0)
        speech->routing_muted = FALSE;
    return ret;
}

gint
mtk_speech_routing_mute_end(MtkSpeech *speech)
{
    if (speech == NULL)
        return -EINVAL;
    if (!speech->routing_muted)
        return 0;

    speech_source_clear(&speech->routing_unmute_source);
    speech->routing_unmute_source = speech_timeout_source_new(speech,
                                                              MTK_SPEECH_ROUTING_UNMUTE_MS,
                                                              speech_routing_unmute);
    return 0;
}

gboolean
mtk_speech_last_start_failed_pcm(MtkSpeech *speech)
{
    return speech != NULL && speech->last_start_failed_pcm;
}

gboolean
mtk_speech_is_active(MtkSpeech *speech)
{
    if (speech == NULL)
        return FALSE;

    switch (speech->state) {
    case AUDIO_MANAGER_CALL_STATE_PREPARING:
    case AUDIO_MANAGER_CALL_STATE_STARTING:
    case AUDIO_MANAGER_CALL_STATE_ACTIVE:
    case AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING:
    case AUDIO_MANAGER_CALL_STATE_STOPPING:
    case AUDIO_MANAGER_CALL_STATE_RECOVERING:
        return TRUE;
    case AUDIO_MANAGER_CALL_STATE_IDLE:
    case AUDIO_MANAGER_CALL_STATE_ERROR:
        return FALSE;
    }

    return FALSE;
}

AudioManagerCallState
mtk_speech_get_state(MtkSpeech *speech)
{
    return speech != NULL ?
           speech->state :
           AUDIO_MANAGER_CALL_STATE_IDLE;
}

AudioManagerSpeechBand
mtk_speech_get_band(MtkSpeech *speech)
{
    return speech != NULL ?
           speech->band :
           AUDIO_MANAGER_SPEECH_BAND_UNKNOWN;
}

guint
mtk_speech_get_network(MtkSpeech *speech)
{
    return speech != NULL && speech->network_valid ? speech->network : 0;
}
