/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef FD_SOURCE_H
#define FD_SOURCE_H

#include <glib.h>

typedef struct AudioFdSource AudioFdSource;

/**
 * Callback invoked when a watched file descriptor becomes ready.
 *
 * @param fd Watched file descriptor
 * @param condition GLib I/O condition
 * @param user_data Caller provided data
 * @return TRUE to keep the source, FALSE to remove it
 */
typedef gboolean (*AudioFdSourceFunc)(gint fd,
                                      GIOCondition condition,
                                      gpointer user_data);

/**
 * Attach file descriptor readiness to a GLib main context.
 *
 * @param context Main context to attach to
 * @param fd File descriptor to watch
 * @param condition Conditions to watch for
 * @param callback Readiness callback
 * @param user_data Data passed to the callback
 * @return New source or NULL on failure
 */
AudioFdSource *
audio_fd_source_new(GMainContext *context,
                    gint fd,
                    GIOCondition condition,
                    AudioFdSourceFunc callback,
                    gpointer user_data);

/**
 * Remove and free a file descriptor source.
 *
 * @param source Source to free
 */
void
audio_fd_source_free(AudioFdSource *source);

#endif /* FD_SOURCE_H */
