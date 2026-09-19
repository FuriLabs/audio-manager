/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_MANAGER_PRIVATE_H
#define AUDIO_MANAGER_PRIVATE_H

#include <glib.h>
#include <stddef.h>
#include <string.h>

#include <audio-manager/audio-manager.h>

struct AudioManagerBackend;
struct AudioManagerBackendOps;

#define AUDIO_MANAGER_STRUCT_HAS_FIELD(ptr, type, field) \
    ((ptr) != NULL && \
     (ptr)->struct_size >= offsetof(type, field) + sizeof(((type *)0)->field))

#define AUDIO_MANAGER_STRUCT_COPY_OUT(dst, src, type) \
    G_STMT_START { \
        gsize _audio_manager_copy_size = MIN((dst)->struct_size, sizeof(type)); \
        gsize _audio_manager_struct_size = (dst)->struct_size; \
        memcpy((dst), (src), _audio_manager_copy_size); \
        (dst)->struct_size = _audio_manager_struct_size; \
    } G_STMT_END

struct AudioManagerStream {
    struct AudioManager *manager;
    gpointer backend_stream;
    AudioManagerStreamConfig config;
    gboolean started;
    GMutex lock;
};

struct AudioManagerCallStream {
    struct AudioManager *manager;
    gpointer backend_stream;
    AudioManagerCallStreamDirection direction;
    AudioManagerCallStreamConfig config;
    gboolean started;
    GMutex lock;
};

struct AudioManager {
    GMainContext *main_context;
    GKeyFile *config;

    const struct AudioManagerBackendOps *backend_ops;
    struct AudioManagerBackend *backend;

    AudioManagerOutputDevice output_device;
    AudioManagerInputDevice input_device;

    gdouble call_volume;

    gboolean uplink_muted;
    gboolean downlink_muted;
    gboolean call_active;
    AudioManagerCallTransport call_transport;
    AudioManagerCallStream *call_streams[2];

    GRecMutex control_lock;
};

#endif /* AUDIO_MANAGER_PRIVATE_H */
