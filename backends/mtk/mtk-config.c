/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/mtk-config.h"

#include <audio-manager/audio-manager-error.h>

#include "common/audio-device.h"
#include "common/config/config.h"

static gint
get_ext_device(GKeyFile *key_file,
               const gchar *key)
{
    return audio_config_get_integer(key_file,
                                    "MediaTek.Speech.Device",
                                    key,
                                    -1);
}

static gboolean
speech_rate_supported(guint rate)
{
    return rate == 8000 ||
           rate == 16000 ||
           rate == 32000 ||
           rate == 48000;
}

MtkConfig *
mtk_config_load(const gchar *path,
                GError **error)
{
    MtkConfig *config;
    GKeyFile *key_file;
    gchar *format;
    gchar *default_output;
    gchar *default_input;

    key_file = audio_config_load(path, error);
    if (key_file == NULL)
        return NULL;

    config = g_new0(MtkConfig, 1);
    config->path = g_strdup(path);

    config->card = audio_config_get_string(key_file,
                                           "ALSA",
                                           "Card",
                                           NULL);
    config->generation = audio_config_get_string(key_file,
                                                 "MediaTek",
                                                 "Generation",
                                                 NULL);
    config->audio_device_config = audio_config_get_string(key_file,
                                                          "MediaTek",
                                                          "AudioDeviceConfig",
                                                          NULL);
    config->audio_param_directory = audio_config_get_string(key_file,
                                                            "MediaTek",
                                                            "AudioParamDirectory",
                                                            NULL);

    config->control_device = audio_config_get_string(key_file,
                                                     "MediaTek.Speech",
                                                     "ControlDevice",
                                                     NULL);
    config->shared_memory_device = audio_config_get_string(key_file,
                                                           "MediaTek.Speech",
                                                           "SharedMemoryDevice",
                                                           NULL);
    config->usip_device = audio_config_get_string(key_file,
                                                  "MediaTek.Speech",
                                                  "UsipDevice",
                                                  NULL);
    config->ccci_shm_init_control = audio_config_get_string(key_file,
                                                            "MediaTek.Speech",
                                                            "CcciShmInitControl",
                                                            NULL);
    config->usip_init_control = audio_config_get_string(key_file,
                                                        "MediaTek.Speech",
                                                        "UsipInitControl",
                                                        NULL);
    config->usip_write_index_control = audio_config_get_string(key_file,
                                                               "MediaTek.Speech",
                                                               "UsipWriteIndexControl",
                                                               NULL);

    config->hostless_pcm = audio_config_get_string(key_file,
                                                   "MediaTek.Speech",
                                                   "HostlessPcm",
                                                   NULL);
    config->speech_rate = audio_config_get_integer(key_file,
                                                   "MediaTek.Speech",
                                                   "SampleRate",
                                                   0);
    config->speech_channels = audio_config_get_integer(key_file,
                                                       "MediaTek.Speech",
                                                       "Channels",
                                                       0);
    config->speech_period_size = audio_config_get_integer(key_file,
                                                          "MediaTek.Speech",
                                                          "PeriodSize",
                                                          0);
    config->speech_period_count = audio_config_get_integer(key_file,
                                                           "MediaTek.Speech",
                                                           "PeriodCount",
                                                           0);

    format = audio_config_get_string(key_file,
                                     "MediaTek.Speech",
                                     "Format",
                                     NULL);
    if (format == NULL || !audio_format_from_string(format, &config->speech_format)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown or missing MediaTek speech PCM format '%s'",
                    format != NULL ? format : "(null)");
        g_free(format);
        g_key_file_unref(key_file);
        mtk_config_free(config);
        return NULL;
    }
    g_free(format);

    config->use_usip = audio_config_get_boolean(key_file,
                                                "MediaTek.Speech",
                                                "UseUsip",
                                                FALSE);
    config->use_ccci_shared_memory = audio_config_get_boolean(key_file,
                                                              "MediaTek.Speech",
                                                              "UseCcciSharedMemory",
                                                              FALSE);
    config->smart_pa_count = audio_config_get_integer(key_file,
                                                       "MediaTek.Speech",
                                                       "SmartPaCount",
                                                       0);

    default_output = audio_config_get_string(key_file,
                                             "Call",
                                             "DefaultOutput",
                                             NULL);
    default_input = audio_config_get_string(key_file,
                                            "Call",
                                            "DefaultInput",
                                            NULL);

    if (!audio_output_device_from_string(default_output,
                                         &config->default_call_output) ||
        config->default_call_output == AUDIO_MANAGER_OUTPUT_NONE) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown or missing default call output '%s'",
                    default_output != NULL ? default_output : "(null)");
        g_free(default_output);
        g_free(default_input);
        g_key_file_unref(key_file);
        mtk_config_free(config);
        return NULL;
    }

    if (!audio_input_device_from_string(default_input,
                                        &config->default_call_input) ||
        config->default_call_input == AUDIO_MANAGER_INPUT_NONE) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown or missing default call input '%s'",
                    default_input != NULL ? default_input : "(null)");
        g_free(default_output);
        g_free(default_input);
        g_key_file_unref(key_file);
        mtk_config_free(config);
        return NULL;
    }

    g_free(default_output);
    g_free(default_input);

    config->receiver_output = audio_config_get_string(key_file,
                                                      "Routes",
                                                      "ReceiverOutput",
                                                      NULL);
    config->speaker_output = audio_config_get_string(key_file,
                                                     "Routes",
                                                     "SpeakerOutput",
                                                     NULL);
    config->headphones_output = audio_config_get_string(key_file,
                                                        "Routes",
                                                        "HeadphonesOutput",
                                                        NULL);
    config->headset_output = audio_config_get_string(key_file,
                                                     "Routes",
                                                     "HeadsetOutput",
                                                     NULL);
    config->usb_output = audio_config_get_string(key_file,
                                                 "Routes",
                                                 "UsbOutput",
                                                 NULL);
    config->builtin_mic_input = audio_config_get_string(key_file,
                                                        "Routes",
                                                        "BuiltinMicInput",
                                                        NULL);
    config->headset_mic_input = audio_config_get_string(key_file,
                                                        "Routes",
                                                        "HeadsetMicInput",
                                                        NULL);
    config->usb_input = audio_config_get_string(key_file,
                                                "Routes",
                                                "UsbInput",
                                                NULL);

    config->modem_uplink = audio_config_get_string(key_file,
                                                   "Routes",
                                                   "ModemUplink",
                                                   "ADDA_UL_TO_MD1");
    config->modem_downlink = audio_config_get_string(key_file,
                                                     "Routes",
                                                     "ModemDownlink",
                                                     "MD1_TO_ADDA_DL");

    config->receiver_ext_device = get_ext_device(key_file, "Receiver");
    config->speaker_ext_device = get_ext_device(key_file, "Speaker");
    config->headphones_ext_device = get_ext_device(key_file, "Headphones");
    config->headset_ext_device = get_ext_device(key_file, "Headset");
    config->bluetooth_ext_device = get_ext_device(key_file, "Bluetooth");
    config->usb_ext_device = get_ext_device(key_file, "USB");

    config->receiver_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "ReceiverProfile",
                                NULL);
    config->speaker_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "SpeakerProfile",
                                NULL);
    config->headphones_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "HeadphonesProfile",
                                NULL);
    config->headset_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "HeadsetProfile",
                                NULL);
    config->bluetooth_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "BluetoothProfile",
                                "BT");
    config->usb_speech_volume_profile =
        audio_config_get_string(key_file,
                                "MediaTek.Speech.Volume",
                                "UsbProfile",
                                NULL);

    config->receiver_gain_profile = audio_config_get_string(key_file,
                                                            "MediaTek.Speech.Volume",
                                                            "ReceiverGainProfile",
                                                            "RCV_NORMAL");
    config->speaker_gain_profile = audio_config_get_string(key_file,
                                                           "MediaTek.Speech.Volume",
                                                           "SpeakerGainProfile",
                                                           "SPK_LO");
    config->headphones_gain_profile = audio_config_get_string(key_file,
                                                              "MediaTek.Speech.Volume",
                                                              "HeadphonesGainProfile",
                                                              "HS");
    config->headset_gain_profile = audio_config_get_string(key_file,
                                                           "MediaTek.Speech.Volume",
                                                           "HeadsetGainProfile",
                                                           "HS");
    config->bluetooth_gain_profile = audio_config_get_string(key_file,
                                                             "MediaTek.Speech.Volume",
                                                             "BluetoothGainProfile",
                                                             "BT");
    config->usb_gain_profile = audio_config_get_string(key_file,
                                                       "MediaTek.Speech.Volume",
                                                       "UsbGainProfile",
                                                       "USB");

    config->playback_pcm = audio_config_get_string(key_file,
                                                   "MediaTek.Streams",
                                                   "PlaybackPcm",
                                                   "Playback_1");
    config->capture_pcm = audio_config_get_string(key_file,
                                                  "MediaTek.Streams",
                                                  "CapturePcm",
                                                  "Capture_1");
    config->playback_route = audio_config_get_string(key_file,
                                                     "MediaTek.Streams",
                                                     "PlaybackRoute",
                                                     "PLAYBACK1_TO_ADDA_DL");
    config->capture_route = audio_config_get_string(key_file,
                                                    "MediaTek.Streams",
                                                    "CaptureRoute",
                                                    "ADDA_TO_CAPTURE1");
    config->playback_scenario_control = audio_config_get_string(key_file,
                                                                "MediaTek.Streams",
                                                                "PlaybackScenarioControl",
                                                                "primary_play_scenario");
    config->fast_playback_pcm = audio_config_get_string(key_file,
                                                        "MediaTek.Streams",
                                                        "FastPlaybackPcm",
                                                        "Playback_1");
    config->fast_playback_route = audio_config_get_string(key_file,
                                                          "MediaTek.Streams",
                                                          "FastPlaybackRoute",
                                                          "PLAYBACK1_TO_ADDA_DL");
    config->fast_playback_scenario_control = audio_config_get_string(key_file,
                                                                     "MediaTek.Streams",
                                                                     "FastPlaybackScenarioControl",
                                                                     "fast_play_scenario");
    config->deep_buffer_playback_pcm = audio_config_get_string(key_file,
                                                               "MediaTek.Streams",
                                                               "DeepBufferPlaybackPcm",
                                                               "Playback_3");
    config->deep_buffer_playback_route = audio_config_get_string(key_file,
                                                                 "MediaTek.Streams",
                                                                 "DeepBufferPlaybackRoute",
                                                                 "PLAYBACK3_TO_ADDA_DL");
    config->deep_buffer_playback_scenario_control = audio_config_get_string(key_file,
                                                                            "MediaTek.Streams",
                                                                            "DeepBufferPlaybackScenarioControl",
                                                                            "deep_buffer_scenario");
    config->capture_xrun_control = audio_config_get_string(key_file,
                                                           "MediaTek.Streams",
                                                           "CaptureXrunControl",
                                                           "record_xrun_assert");
    config->playback_period_size = audio_config_get_integer(key_file,
                                                            "MediaTek.Streams",
                                                            "PlaybackPeriodSize",
                                                            2048);
    config->playback_period_count = audio_config_get_integer(key_file,
                                                             "MediaTek.Streams",
                                                             "PlaybackPeriodCount",
                                                             2);
    config->fast_playback_period_size = audio_config_get_integer(key_file,
                                                                 "MediaTek.Streams",
                                                                 "FastPlaybackPeriodSize",
                                                                 256);
    config->fast_playback_period_count = audio_config_get_integer(key_file,
                                                                  "MediaTek.Streams",
                                                                  "FastPlaybackPeriodCount",
                                                                  2);
    config->deep_buffer_playback_period_size = audio_config_get_integer(key_file,
                                                                        "MediaTek.Streams",
                                                                        "DeepBufferPlaybackPeriodSize",
                                                                        2048);
    config->deep_buffer_playback_period_count = audio_config_get_integer(key_file,
                                                                         "MediaTek.Streams",
                                                                         "DeepBufferPlaybackPeriodCount",
                                                                         2);
    config->capture_rate = audio_config_get_integer(key_file,
                                                    "MediaTek.Streams",
                                                    "CaptureRate",
                                                    48000);
    config->capture_channels = audio_config_get_integer(key_file,
                                                        "MediaTek.Streams",
                                                        "CaptureChannels",
                                                        2);
    config->capture_period_size = audio_config_get_integer(key_file,
                                                           "MediaTek.Streams",
                                                           "CapturePeriodSize",
                                                           1024);
    config->capture_period_count = audio_config_get_integer(key_file,
                                                            "MediaTek.Streams",
                                                            "CapturePeriodCount",
                                                            4);

    config->call_downlink_pcm = audio_config_get_string(key_file,
                                                        "MediaTek.Call.Hostful",
                                                        "DownlinkPcm",
                                                        "Capture_2");
    config->call_downlink_route = audio_config_get_string(key_file,
                                                          "MediaTek.Call.Hostful",
                                                          "DownlinkRoute",
                                                          "MD1_TO_CAPTURE2");
    config->call_uplink_pcm = audio_config_get_string(key_file,
                                                      "MediaTek.Call.Hostful",
                                                      "UplinkPcm",
                                                      "Playback_2");
    config->call_uplink_route = audio_config_get_string(key_file,
                                                        "MediaTek.Call.Hostful",
                                                        "UplinkRoute",
                                                        "PLAYBACK2_TO_MD1");
    config->call_hostful_rate = audio_config_get_integer(key_file,
                                                         "MediaTek.Call.Hostful",
                                                         "SampleRate",
                                                         config->speech_rate);
    config->call_hostful_channels = audio_config_get_integer(key_file,
                                                             "MediaTek.Call.Hostful",
                                                             "Channels",
                                                             2);
    config->call_hostful_period_size = audio_config_get_integer(key_file,
                                                                "MediaTek.Call.Hostful",
                                                                "PeriodSize",
                                                                240);
    config->call_hostful_period_count = audio_config_get_integer(key_file,
                                                                 "MediaTek.Call.Hostful",
                                                                 "PeriodCount",
                                                                 8);
    format = audio_config_get_string(key_file,
                                     "MediaTek.Call.Hostful",
                                     "Format",
                                     "S16_LE");
    if (format == NULL || !audio_format_from_string(format, &config->call_hostful_format)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown MediaTek hostful call PCM format '%s'",
                    format != NULL ? format : "(null)");
        g_free(format);
        g_key_file_unref(key_file);
        mtk_config_free(config);
        return NULL;
    }
    g_free(format);

    config->hostless_fm_pcm = audio_config_get_string(key_file,
                                                      "MediaTek.Hostless",
                                                      "FmPcm",
                                                      "Hostless_FM");
    config->hostless_loopback_pcm = audio_config_get_string(key_file,
                                                            "MediaTek.Hostless",
                                                            "LoopbackPcm",
                                                            "Hostless_LPBK");
    config->fm_route = audio_config_get_string(key_file,
                                               "MediaTek.Hostless",
                                               "FmRoute",
                                               "CONNSYS_TO_ADDA_DL");
    config->loopback_route = audio_config_get_string(key_file,
                                                     "MediaTek.Hostless",
                                                     "LoopbackRoute",
                                                     "ADDA_UL_TO_ADDA_DL");
    config->fm_period_size = audio_config_get_integer(key_file,
                                                      "MediaTek.Hostless",
                                                      "FmPeriodSize",
                                                      3072);
    config->hostless_aux_rate = audio_config_get_integer(key_file,
                                                         "MediaTek.Hostless",
                                                         "SampleRate",
                                                         48000);
    config->hostless_aux_channels = audio_config_get_integer(key_file,
                                                             "MediaTek.Hostless",
                                                             "Channels",
                                                             2);
    config->hostless_aux_period_size = audio_config_get_integer(key_file,
                                                                "MediaTek.Hostless",
                                                                "PeriodSize",
                                                                1024);
    config->hostless_aux_period_count = audio_config_get_integer(key_file,
                                                                 "MediaTek.Hostless",
                                                                 "PeriodCount",
                                                                 2);
    format = audio_config_get_string(key_file,
                                     "MediaTek.Hostless",
                                     "Format",
                                     "S16_LE");
    if (format == NULL || !audio_format_from_string(format, &config->hostless_aux_format)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown MediaTek hostless PCM format '%s'",
                    format != NULL ? format : "(null)");
        g_free(format);
        g_key_file_unref(key_file);
        mtk_config_free(config);
        return NULL;
    }
    g_free(format);

    config->smartpa_profile_control = audio_config_get_string(key_file,
                                                              "MediaTek.SmartPA",
                                                              "ProfileControl",
                                                              "aw87xxx_profile_switch_0");
    config->smartpa_monitor_control = audio_config_get_string(key_file,
                                                              "MediaTek.SmartPA",
                                                              "MonitorControl",
                                                              "aw87xxx_monitor_switch_0");
    config->smartpa_vmax_control = audio_config_get_string(key_file,
                                                           "MediaTek.SmartPA",
                                                           "VmaxControl",
                                                           "aw87xxx_vmax_get_0");
    config->smartpa_spin_control = audio_config_get_string(key_file,
                                                           "MediaTek.SmartPA",
                                                           "SpinControl",
                                                           "aw87xxx_spin_switch");

    g_key_file_unref(key_file);

    if (config->generation == NULL ||
        config->audio_device_config == NULL ||
        config->audio_param_directory == NULL ||
        config->control_device == NULL ||
        config->hostless_pcm == NULL ||
        config->speech_channels == 0 ||
        config->speech_period_size == 0 ||
        config->speech_period_count == 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek generation, device audio param configuration, "
                    "speech control device and PCM configuration are required");
        mtk_config_free(config);
        return NULL;
    }

    if (!speech_rate_supported(config->speech_rate)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unsupported MediaTek speech sample rate %u",
                    config->speech_rate);
        mtk_config_free(config);
        return NULL;
    }

    if (config->smart_pa_count > G_MAXUINT8) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek SmartPaCount %u exceeds protocol limit",
                    config->smart_pa_count);
        mtk_config_free(config);
        return NULL;
    }

    if (config->use_ccci_shared_memory &&
        (config->shared_memory_device == NULL || config->ccci_shm_init_control == NULL)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek CCCI shared memory configuration is incomplete");
        mtk_config_free(config);
        return NULL;
    }

    if (config->use_usip &&
        (config->usip_device == NULL ||
         config->usip_init_control == NULL ||
         config->usip_write_index_control == NULL)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek USIP configuration is incomplete");
        mtk_config_free(config);
        return NULL;
    }

    return config;
}

void
mtk_config_free(MtkConfig *config)
{
    if (config == NULL)
        return;

    g_free(config->path);
    g_free(config->card);
    g_free(config->generation);
    g_free(config->audio_device_config);
    g_free(config->audio_param_directory);
    g_free(config->control_device);
    g_free(config->shared_memory_device);
    g_free(config->usip_device);
    g_free(config->ccci_shm_init_control);
    g_free(config->usip_init_control);
    g_free(config->usip_write_index_control);
    g_free(config->hostless_pcm);

    g_free(config->receiver_output);
    g_free(config->speaker_output);
    g_free(config->headphones_output);
    g_free(config->headset_output);
    g_free(config->usb_output);
    g_free(config->builtin_mic_input);
    g_free(config->headset_mic_input);
    g_free(config->usb_input);
    g_free(config->modem_uplink);
    g_free(config->modem_downlink);

    g_free(config->receiver_speech_volume_profile);
    g_free(config->speaker_speech_volume_profile);
    g_free(config->headphones_speech_volume_profile);
    g_free(config->headset_speech_volume_profile);
    g_free(config->bluetooth_speech_volume_profile);
    g_free(config->usb_speech_volume_profile);

    g_free(config->receiver_gain_profile);
    g_free(config->speaker_gain_profile);
    g_free(config->headphones_gain_profile);
    g_free(config->headset_gain_profile);
    g_free(config->bluetooth_gain_profile);
    g_free(config->usb_gain_profile);

    g_free(config->playback_pcm);
    g_free(config->capture_pcm);
    g_free(config->playback_route);
    g_free(config->capture_route);
    g_free(config->playback_scenario_control);
    g_free(config->fast_playback_pcm);
    g_free(config->fast_playback_route);
    g_free(config->fast_playback_scenario_control);
    g_free(config->deep_buffer_playback_pcm);
    g_free(config->deep_buffer_playback_route);
    g_free(config->deep_buffer_playback_scenario_control);
    g_free(config->capture_xrun_control);
    g_free(config->call_downlink_pcm);
    g_free(config->call_downlink_route);
    g_free(config->call_uplink_pcm);
    g_free(config->call_uplink_route);
    g_free(config->hostless_fm_pcm);
    g_free(config->hostless_loopback_pcm);
    g_free(config->fm_route);
    g_free(config->loopback_route);
    g_free(config->smartpa_profile_control);
    g_free(config->smartpa_monitor_control);
    g_free(config->smartpa_vmax_control);
    g_free(config->smartpa_spin_control);
    g_free(config);
}
