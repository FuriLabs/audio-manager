/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_AUDIO_PARAM_H
#define MTK_AUDIO_PARAM_H

#include <glib.h>

typedef struct MtkAudioParam MtkAudioParam;

/**
 * Load a MediaTek AudioParam XML file.
 */
MtkAudioParam *
mtk_audio_param_load(const gchar *path,
                     GError **error);

/**
 * Free a parsed MediaTek AudioParam file.
 */
void
mtk_audio_param_free(MtkAudioParam *param);

/**
 * Get a parameter value from one exact AudioParam path.
 */
const gchar *
mtk_audio_param_get_param(MtkAudioParam *param,
                          const gchar *path,
                          const gchar *name);

/**
 * Get a parameter from the first exact path that contains it.
 */
const gchar *
mtk_audio_param_get_first_param(MtkAudioParam *param,
                                const gchar *const *paths,
                                gsize path_count,
                                const gchar *name,
                                const gchar **matched_path);

/**
 * Return the number of paths in an AudioParam tree.
 */
gsize
mtk_audio_param_get_path_count(MtkAudioParam *param);

/**
 * Return an AudioParam path by index.
 */
const gchar *
mtk_audio_param_get_path(MtkAudioParam *param,
                         gsize index);

/**
 * Parse a comma separated signed 32 bit integer list.
 */
gint
mtk_audio_param_parse_i32_list(const gchar *value,
                               gint32 **values,
                               gsize *count);

/**
 * Parse a comma separated unsigned 16 bit integer list.
 */
gint
mtk_audio_param_parse_u16_list(const gchar *value,
                               guint16 **values,
                               gsize *count);

#endif /* MTK_AUDIO_PARAM_H */
