/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <audio-manager/audio-manager-error.h>

GQuark
audio_manager_error_quark(void)
{
    return g_quark_from_static_string("audio-manager-error-quark");
}
