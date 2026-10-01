/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_CONFIG_H
#define MTK_CONFIG_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

#include "common/audio-format.h"

/**
 * Parsed MediaTek device configuration.
 */
typedef struct {
    gchar *path;                         /**< Source configuration path */

    /**
     * Core device/vendor configuration.
     */
    gchar *card;
    gchar *generation;
    gchar *audio_device_config;
    gchar *audio_param_directory;

    /**
     * Modem speech transport devices.
     */
    gchar *control_device;
    gchar *shared_memory_device;
    gchar *usip_device;

    /**
     * ALSA controls used to initialize shared memory transports.
     */
    gchar *ccci_shm_init_control;
    gchar *usip_init_control;
    gchar *usip_write_index_control;

    /**
     * Hostless speech PCM and audio format.
     */
    gchar *hostless_pcm;
    guint speech_rate;
    guint speech_channels;
    AudioFormat speech_format;
    guint speech_period_size;
    guint speech_period_count;

    gboolean use_usip;
    gboolean use_ccci_shared_memory;
    guint smart_pa_count;

    /**
     * Default cellular call endpoints.
     */
    AudioManagerOutputDevice default_call_output;
    AudioManagerInputDevice default_call_input;

    /**
     * Physical output route names.
     */
    gchar *receiver_output;
    gchar *speaker_output;
    gchar *headphones_output;
    gchar *headset_output;
    gchar *usb_output;

    /**
     * Physical input route names.
     */
    gchar *builtin_mic_input;
    gchar *headset_mic_input;
    gchar *usb_input;

    /**
     * Direct hostless modem uplink/downlink routes.
     */
    gchar *modem_uplink;
    gchar *modem_downlink;
    gchar *speaker_modem_downlink;

    /**
     * MediaTek external speech device IDs.
     */
    gint receiver_ext_device;
    gint speaker_ext_device;
    gint headphones_ext_device;
    gint headset_ext_device;
    gint bluetooth_ext_device;
    gint usb_ext_device;

    /**
     * Speech volume profile names.
     */
    gchar *receiver_speech_volume_profile;
    gchar *speaker_speech_volume_profile;
    gchar *headphones_speech_volume_profile;
    gchar *headset_speech_volume_profile;
    gchar *bluetooth_speech_volume_profile;
    gchar *usb_speech_volume_profile;

    /**
     * MediaTek speech UL/DL gain profile names.
     */
    gchar *receiver_gain_profile;
    gchar *speaker_gain_profile;
    gchar *headphones_gain_profile;
    gchar *headset_gain_profile;
    gchar *bluetooth_gain_profile;
    gchar *usb_gain_profile;

    /**
     * Generic hostful media PCM/route configuration.
     */
    gchar *playback_pcm;
    gchar *capture_pcm;
    gchar *playback_route;
    gchar *capture_route;
    gchar *playback_scenario_control;
    gchar *fast_playback_pcm;
    gchar *fast_playback_route;
    gchar *fast_playback_scenario_control;
    gchar *deep_buffer_playback_pcm;
    gchar *deep_buffer_playback_route;
    gchar *deep_buffer_playback_scenario_control;
    gchar *capture_xrun_control;
    guint playback_period_size;
    guint playback_period_count;
    guint fast_playback_period_size;
    guint fast_playback_period_count;
    guint deep_buffer_playback_period_size;
    guint deep_buffer_playback_period_count;
    guint capture_rate;
    guint capture_channels;
    guint capture_period_size;
    guint capture_period_count;

    /**
     * Hostful cellular modem PCM configuration.
     */
    gchar *call_downlink_pcm;
    gchar *call_downlink_route;
    gchar *call_uplink_pcm;
    gchar *call_uplink_route;
    guint call_hostful_rate;
    guint call_hostful_channels;
    AudioFormat call_hostful_format;
    guint call_hostful_period_size;
    guint call_hostful_period_count;

    /**
     * Hostless FM and hardware loopback configuration.
     */
    gchar *hostless_fm_pcm;
    gchar *hostless_loopback_pcm;
    gchar *fm_route;
    gchar *loopback_route;
    guint fm_period_size;
    guint hostless_aux_rate;
    guint hostless_aux_channels;
    AudioFormat hostless_aux_format;
    guint hostless_aux_period_size;
    guint hostless_aux_period_count;

    /**
     * SmartPA diagnostic/status controls.
     */
    gchar *smartpa_profile_control;
    gchar *smartpa_monitor_control;
    gchar *smartpa_vmax_control;
    gchar *smartpa_spin_control;
} MtkConfig;

/**
 * Load a MediaTek backend configuration file.
 *
 * @param path Device configuration path
 * @param error Error return location
 * @return Parsed configuration or NULL on failure
 */
MtkConfig *
mtk_config_load(const gchar *path,
                GError **error);

/**
 * Free a parsed MediaTek backend configuration.
 */
void
mtk_config_free(MtkConfig *config);

#endif /* MTK_CONFIG_H */
