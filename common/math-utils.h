/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MATH_UTILS_H
#define MATH_UTILS_H

#include <glib.h>

/**
 * Clamp a normalized volume to the public 0.0..1.0 range.
 *
 * @param volume Volume value
 * @return Clamped volume
 */
gdouble
audio_clamp_volume(gdouble volume);

/**
 * Convert normalized volume to an index in a fixed size table.
 *
 * @param volume Normalized volume
 * @param count Number of table entries
 * @return Closest table index
 */
gsize
audio_volume_to_index(gdouble volume,
                      gsize count);

#endif /* AUDIO_MANAGER_COMMON_MATH_UTILS_H */
