/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/config/config.h"

GKeyFile *
audio_config_load(const gchar *path,
                  GError **error)
{
    GKeyFile *config;

    config = g_key_file_new();
    if (!g_key_file_load_from_file(config,
                                   path,
                                   G_KEY_FILE_NONE,
                                   error)) {
        g_key_file_unref(config);
        return NULL;
    }

    return config;
}

gchar *
audio_config_get_string(GKeyFile *config,
                        const gchar *group,
                        const gchar *key,
                        const gchar *default_value)
{
    GError *error = NULL;
    gchar *value;

    value = g_key_file_get_string(config, group, key, &error);
    if (error == NULL)
        return value;

    g_clear_error(&error);
    return default_value != NULL ? g_strdup(default_value) : NULL;
}

gint
audio_config_get_integer(GKeyFile *config,
                         const gchar *group,
                         const gchar *key,
                         gint default_value)
{
    GError *error = NULL;
    gint value;

    value = g_key_file_get_integer(config, group, key, &error);
    if (error == NULL)
        return value;

    g_clear_error(&error);
    return default_value;
}

gboolean
audio_config_get_boolean(GKeyFile *config,
                         const gchar *group,
                         const gchar *key,
                         gboolean default_value)
{
    GError *error = NULL;
    gboolean value;

    value = g_key_file_get_boolean(config, group, key, &error);
    if (error == NULL)
        return value;

    g_clear_error(&error);
    return default_value;
}
