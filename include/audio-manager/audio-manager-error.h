/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_MANAGER_ERROR_H
#define AUDIO_MANAGER_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * Error codes returned through GError by audio-manager operations.
 */
typedef enum {
    AUDIO_MANAGER_ERROR_FAILED = 1,       /**< Generic operation failure */
    AUDIO_MANAGER_ERROR_CONFIG,           /**< Invalid or incomplete configuration */
    AUDIO_MANAGER_ERROR_BACKEND,          /**< Backend creation or backend operation failure */
    AUDIO_MANAGER_ERROR_NOT_SUPPORTED,    /**< Operation is not supported by the active backend */
} AudioManagerError;

/**
 * Error domain used by audio-manager.
 */
#define AUDIO_MANAGER_ERROR audio_manager_error_quark()

/**
 * Get the audio-manager error domain.
 *
 * @return The audio-manager GQuark
 */
GQuark
audio_manager_error_quark(void);

G_END_DECLS

#endif /* AUDIO_MANAGER_ERROR_H */
