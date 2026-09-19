/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>

/**
 * Load a configuration file.
 *
 * @param path File path
 * @param error Error return location
 * @return Loaded GKeyFile or NULL on failure
 */
GKeyFile *
audio_config_load(const gchar *path,
                  GError **error);

/**
 * Get a string value or duplicate the supplied default.
 */
gchar *
audio_config_get_string(GKeyFile *config,
                        const gchar *group,
                        const gchar *key,
                        const gchar *default_value);

/**
 * Get an integer value with fallback.
 */
gint
audio_config_get_integer(GKeyFile *config,
                         const gchar *group,
                         const gchar *key,
                         gint default_value);

/**
 * Get a boolean value with fallback.
 */
gboolean
audio_config_get_boolean(GKeyFile *config,
                         const gchar *group,
                         const gchar *key,
                         gboolean default_value);

#endif /* CONFIG_H */
