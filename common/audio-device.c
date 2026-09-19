/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/audio-device.h"

#include <string.h>

const gchar *
audio_output_device_to_string(AudioManagerOutputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_NONE:
        return "none";
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return "receiver";
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return "speaker";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return "headphones";
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return "headset";
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return "bluetooth";
    case AUDIO_MANAGER_OUTPUT_USB:
        return "usb";
    }

    return "unknown";
}

gboolean
audio_output_device_from_string(const gchar *string,
                                AudioManagerOutputDevice *device)
{
    if (string == NULL || device == NULL)
        return FALSE;

    if (strcmp(string, "none") == 0)
        *device = AUDIO_MANAGER_OUTPUT_NONE;
    else if (strcmp(string, "receiver") == 0)
        *device = AUDIO_MANAGER_OUTPUT_RECEIVER;
    else if (strcmp(string, "speaker") == 0)
        *device = AUDIO_MANAGER_OUTPUT_SPEAKER;
    else if (strcmp(string, "headphones") == 0)
        *device = AUDIO_MANAGER_OUTPUT_HEADPHONES;
    else if (strcmp(string, "headset") == 0)
        *device = AUDIO_MANAGER_OUTPUT_HEADSET;
    else if (strcmp(string, "bluetooth") == 0)
        *device = AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    else if (strcmp(string, "usb") == 0)
        *device = AUDIO_MANAGER_OUTPUT_USB;
    else
        return FALSE;

    return TRUE;
}

const gchar *
audio_input_device_to_string(AudioManagerInputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_INPUT_NONE:
        return "none";
    case AUDIO_MANAGER_INPUT_BUILTIN_MIC:
        return "builtin-mic";
    case AUDIO_MANAGER_INPUT_HEADSET_MIC:
        return "headset-mic";
    case AUDIO_MANAGER_INPUT_BLUETOOTH:
        return "bluetooth";
    case AUDIO_MANAGER_INPUT_USB:
        return "usb";
    }

    return "unknown";
}

gboolean
audio_input_device_from_string(const gchar *string,
                               AudioManagerInputDevice *device)
{
    if (string == NULL || device == NULL)
        return FALSE;

    if (strcmp(string, "none") == 0)
        *device = AUDIO_MANAGER_INPUT_NONE;
    else if (strcmp(string, "builtin-mic") == 0)
        *device = AUDIO_MANAGER_INPUT_BUILTIN_MIC;
    else if (strcmp(string, "headset-mic") == 0)
        *device = AUDIO_MANAGER_INPUT_HEADSET_MIC;
    else if (strcmp(string, "bluetooth") == 0)
        *device = AUDIO_MANAGER_INPUT_BLUETOOTH;
    else if (strcmp(string, "usb") == 0)
        *device = AUDIO_MANAGER_INPUT_USB;
    else
        return FALSE;

    return TRUE;
}
