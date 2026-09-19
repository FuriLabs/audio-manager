/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/backend.h"

#include <errno.h>

static GHashTable *backend_registry;
static GMutex backend_registry_lock;
static gsize backend_registry_initialized;

static void
backend_registry_init(void)
{
    backend_registry = g_hash_table_new(g_str_hash, g_str_equal);
    g_mutex_init(&backend_registry_lock);
}

static void
backend_registry_ensure(void)
{
    if (g_once_init_enter(&backend_registry_initialized)) {
        backend_registry_init();
        g_once_init_leave(&backend_registry_initialized, 1);
    }
}

gint
audio_manager_backend_register(const AudioManagerBackendOps *ops)
{
    if (ops == NULL || ops->name == NULL || *ops->name == '\0' ||
        ops->create == NULL || ops->destroy == NULL)
        return -EINVAL;

    backend_registry_ensure();
    g_mutex_lock(&backend_registry_lock);
    if (g_hash_table_contains(backend_registry, ops->name)) {
        g_mutex_unlock(&backend_registry_lock);
        return -EEXIST;
    }
    g_hash_table_insert(backend_registry, (gpointer)ops->name, (gpointer)ops);
    g_mutex_unlock(&backend_registry_lock);
    return 0;
}

const AudioManagerBackendOps *
audio_manager_backend_find(const gchar *name)
{
    const AudioManagerBackendOps *ops;

    if (name == NULL)
        return NULL;

    backend_registry_ensure();
    g_mutex_lock(&backend_registry_lock);
    ops = g_hash_table_lookup(backend_registry, name);
    g_mutex_unlock(&backend_registry_lock);
    return ops;
}
