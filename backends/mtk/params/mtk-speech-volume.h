/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_SPEECH_VOLUME_H
#define MTK_SPEECH_VOLUME_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

#include "common/alsa/alsa-card.h"

typedef struct MtkSpeechVolume MtkSpeechVolume;

/**
 * Calculated call gain state per volume step.
 */
typedef struct {
    guint ui_index;                         /**< index 0..7, zero is the hard mute step */
    guint parameter_volume_index;           /**< Speech parameter index used for UI steps 1..7 */
    gboolean update_parameter_volume_index; /**< TRUE when a new parameter image is required */

    gint16 modem_downlink_gain;             /**< Gain sent through SetDownlinkGain */
    gint16 modem_uplink_gain;               /**< Gain sent through SetUplinkGain */

    const gchar *downlink_analog_control;   /**< Local analog playback mixer control */
    glong downlink_analog_index;            /**< Mixer index for local analog playback */

    const gchar *uplink_pga_control[4];     /**< Local microphone PGA controls */
    glong uplink_pga_index[4];              /**< PGA indices matching uplink_pga_control */
    gsize uplink_pga_count;                 /**< Number of active PGA controls */

    glong sidetone_gain;                    /**< AP side sidetone mixer value */
    glong sidetone_positive_gain_db;        /**< Positive sidetone gain helper value */
} MtkSpeechGain;

/**
 * Load MediaTek speech/media hardware gain tables from AudioParam config.
 *
 * @param audio_param_directory Directory containing vendor AudioParam config files
 * @param error Error return location
 * @return New volume parser or NULL on failure
 */
MtkSpeechVolume *
mtk_speech_volume_new(const gchar *audio_param_directory,
                      GError **error);

/**
 * Free MediaTek speech volume tables.
 */
void
mtk_speech_volume_free(MtkSpeechVolume *volume);

/**
 * Calculate modem, analog, microphone PGA and sidetone gains for a call.
 *
 * @return 0 on success or a negative errno value
 */
gint
mtk_speech_volume_calculate(MtkSpeechVolume *volume,
                            AudioManagerSpeechBand band,
                            guint network,
                            const gchar *speech_profile,
                            const gchar *gain_profile,
                            gdouble normalized_volume,
                            MtkSpeechGain *gain);

/**
 * Apply the MediaTek out of call playback hardware gain baseline.
 */
gint
mtk_speech_volume_apply_media_playback_gain(MtkSpeechVolume *volume,
                                            AudioAlsaCard *card,
                                            AudioManagerOutputDevice device);

/**
 * Apply MediaTek out of call microphone/PGA gain for a capture role/device.
 */
gint
mtk_speech_volume_apply_capture_gain(MtkSpeechVolume *volume,
                                     AudioAlsaCard *card,
                                     AudioManagerCaptureRole role,
                                     AudioManagerInputDevice device,
                                     const gchar *builtin_gain_profile,
                                     const gchar *headset_gain_profile,
                                     guint channels);

#endif /* MTK_SPEECH_VOLUME_H */
