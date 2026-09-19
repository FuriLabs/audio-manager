/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_SPEECH_H
#define MTK_SPEECH_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

#include "backends/mtk/mtk-config.h"
#include "common/alsa/alsa-card.h"

typedef struct MtkSpeech MtkSpeech;

/**
 * Called when the MediaTek speech transport requests backend recovery.
 */
typedef gint (*MtkSpeechRecoveryFunc)(MtkSpeech *speech,
                                      gpointer user_data);

/**
 * Called when the modem reports a changed speech network status word.
 */
typedef void (*MtkSpeechNetworkChangeFunc)(MtkSpeech *speech,
                                           guint16 network_status,
                                           gpointer user_data);

/**
 * Create the MediaTek cellular speech controller.
 *
 * @param config MediaTek device configuration
 * @param card Open ALSA card
 * @param main_context Main context used for CCCI receive events
 * @param recovery_callback Recovery callback
 * @param recovery_user_data Data passed to the recovery callback
 * @param network_change_callback Modem network status callback
 * @param network_change_user_data Data passed to network callback
 * @param error Error return location
 * @return New speech controller or NULL on failure
 */
MtkSpeech *
mtk_speech_new(const MtkConfig *config,
               AudioAlsaCard *card,
               GMainContext *main_context,
               MtkSpeechRecoveryFunc recovery_callback,
               gpointer recovery_user_data,
               MtkSpeechNetworkChangeFunc network_change_callback,
               gpointer network_change_user_data,
               GError **error);

/**
 * Free the MediaTek cellular speech controller.
 */
void
mtk_speech_free(MtkSpeech *speech);

/**
 * Start MediaTek speech for the selected devices and transport.
 */
gint
mtk_speech_start(MtkSpeech *speech,
                 AudioManagerOutputDevice output_device,
                 AudioManagerInputDevice input_device,
                 AudioManagerCallTransport transport);

/**
 * Request MediaTek speech shutdown.
 */
gint
mtk_speech_stop(MtkSpeech *speech);

/**
 * Rebuild modem speech state after a CCCI/modem recovery.
 */
gint
mtk_speech_recover(MtkSpeech *speech,
                   AudioManagerOutputDevice output_device,
                   AudioManagerInputDevice input_device);

/**
 * Change speech input/output devices.
 */
gint
mtk_speech_device_change(MtkSpeech *speech,
                         AudioManagerOutputDevice output_device,
                         AudioManagerInputDevice input_device);

/**
 * Change hostless/hostful transport.
 */
gint
mtk_speech_set_transport(MtkSpeech *speech,
                         AudioManagerCallTransport transport,
                         AudioManagerOutputDevice output_device,
                         AudioManagerInputDevice input_device);

/**
 * Set the speech parameter volume index.
 */
gint
mtk_speech_set_parameter_volume_index(MtkSpeech *speech,
                                      guint index);

/**
 * Configure the MTK modem speech Bluetooth WB/NREC state.
 */
gint
mtk_speech_set_bluetooth_parameters(MtkSpeech *speech,
                                    gboolean wideband,
                                    gboolean nrec);

/**
 * Set modem speech downlink digital gain.
 */
gint
mtk_speech_set_downlink_gain(MtkSpeech *speech,
                             gint16 gain);

/**
 * Set modem speech uplink digital gain.
 */
gint
mtk_speech_set_uplink_gain(MtkSpeech *speech,
                           gint16 gain);

/**
 * Set modem speech uplink mute.
 */
gint
mtk_speech_set_uplink_mute(MtkSpeech *speech,
                           gboolean muted);

/**
 * Set modem speech downlink mute.
 */
gint
mtk_speech_set_downlink_mute(MtkSpeech *speech,
                             gboolean muted);

/**
 * Return TRUE when a speech session is active or transitioning.
 */
gboolean
mtk_speech_is_active(MtkSpeech *speech);

/**
 * Get the current MediaTek speech state machine state.
 */
AudioManagerCallState
mtk_speech_get_state(MtkSpeech *speech);

/**
 * Get modem reported speech bandwidth.
 */
AudioManagerSpeechBand
mtk_speech_get_band(MtkSpeech *speech);

#endif /* MTK_SPEECH_H */
