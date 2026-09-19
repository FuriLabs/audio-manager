/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/mainloop/fd-source.h"

#include <glib-unix.h>

struct AudioFdSource {
    GSource *source;
};

AudioFdSource *
audio_fd_source_new(GMainContext *context,
                    gint fd,
                    GIOCondition condition,
                    AudioFdSourceFunc callback,
                    gpointer user_data)
{
    AudioFdSource *fd_source;

    if (context == NULL || fd < 0 || callback == NULL)
        return NULL;

    fd_source = g_new0(AudioFdSource, 1);
    fd_source->source = g_unix_fd_source_new(fd, condition);
    g_source_set_callback(fd_source->source,
                          G_SOURCE_FUNC(callback),
                          user_data,
                          NULL);
    g_source_attach(fd_source->source, context);

    return fd_source;
}

void
audio_fd_source_free(AudioFdSource *source)
{
    if (source == NULL)
        return;

    if (source->source != NULL) {
        g_source_destroy(source->source);
        g_source_unref(source->source);
    }

    g_free(source);
}
