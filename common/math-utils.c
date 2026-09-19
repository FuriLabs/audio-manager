/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/math-utils.h"

gdouble
audio_clamp_volume(gdouble volume)
{
    if (volume < 0.0)
        return 0.0;
    if (volume > 1.0)
        return 1.0;

    return volume;
}

gsize
audio_volume_to_index(gdouble volume,
                      gsize count)
{
    if (count == 0)
        return 0;

    volume = audio_clamp_volume(volume);
    return (gsize)(volume * (gdouble)(count - 1) + 0.5);
}
