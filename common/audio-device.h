/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_DEVICE_H
#define AUDIO_DEVICE_H

#include <glib.h>

#include <audio-manager/audio-manager-types.h>

/**
 * Convert an output device enum to its configuration string.
 *
 * @param device Output device value
 * @return Static device name or "unknown"
 */
const gchar *
audio_output_device_to_string(AudioManagerOutputDevice device);

/**
 * Parse an output device configuration string.
 *
 * @param string Device name
 * @param device Return location for parsed device
 * @return TRUE when the string is recognized
 */
gboolean
audio_output_device_from_string(const gchar *string,
                                AudioManagerOutputDevice *device);

/**
 * Convert an input device enum to its configuration string.
 *
 * @param device Input device value
 * @return Static device name or "unknown"
 */
const gchar *
audio_input_device_to_string(AudioManagerInputDevice device);

/**
 * Parse an input device configuration string.
 *
 * @param string Device name
 * @param device Return location for parsed device
 * @return TRUE when the string is recognized
 */
gboolean
audio_input_device_from_string(const gchar *string,
                               AudioManagerInputDevice *device);

#endif /* AUDIO_DEVICE_H */
