/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_SPEECH_PARAM_H
#define MTK_SPEECH_PARAM_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

#include "backends/mtk/mtk-config.h"

typedef struct MtkSpeechParamProvider MtkSpeechParamProvider;

/**
 * Create the MediaTek speech parameter image provider.
 */
MtkSpeechParamProvider *
mtk_speech_param_provider_new(const MtkConfig *config,
                              GError **error);

/**
 * Free a speech parameter provider.
 */
void
mtk_speech_param_provider_free(MtkSpeechParamProvider *provider);

/**
 * Set the current speech parameter volume index.
 */
void
mtk_speech_param_provider_set_volume_index(MtkSpeechParamProvider *provider,
                                           guint index);

/**
 * Set Bluetooth WB/NREC parameter selection.
 */
void
mtk_speech_param_provider_set_bluetooth(MtkSpeechParamProvider *provider,
                                        gboolean wideband,
                                        gboolean nrec);

/**
 * Build the modem speech parameter image for the selected devices.
 *
 * @param provider Parameter provider
 * @param output_device Selected call output
 * @param input_device Selected call input
 * @param data Return location for newly allocated parameter image
 * @param size Return location for image size
 * @return 0 on success or a negative errno value
 */
gint
mtk_speech_param_provider_build(MtkSpeechParamProvider *provider,
                                AudioManagerOutputDevice output_device,
                                AudioManagerInputDevice input_device,
                                guint8 **data,
                                gsize *size);

#endif /* MTK_SPEECH_PARAM_H */
