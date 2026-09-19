/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/route.h"

#include <errno.h>

#include "common/alsa/alsa-control.h"

typedef struct {
    gchar *control;
    gchar *value;
} AudioRouteOperation;

typedef struct {
    gchar *name;
    gchar *state;
    GPtrArray *operations;
} AudioRoute;

struct AudioRouteSet {
    GHashTable *routes;
};

static void
route_operation_free(gpointer data)
{
    AudioRouteOperation *operation = data;

    g_free(operation->control);
    g_free(operation->value);
    g_free(operation);
}

static void
route_free(gpointer data)
{
    AudioRoute *route = data;

    g_free(route->name);
    g_free(route->state);
    g_ptr_array_free(route->operations, TRUE);
    g_free(route);
}

static gchar *
route_key(const gchar *name,
          const gchar *state)
{
    return g_strdup_printf("%s\x1f%s", name, state);
}

AudioRouteSet *
audio_route_set_new(void)
{
    AudioRouteSet *routes;

    routes = g_new0(AudioRouteSet, 1);
    routes->routes = g_hash_table_new_full(g_str_hash,
                                           g_str_equal,
                                           g_free,
                                           route_free);
    return routes;
}

void
audio_route_set_free(AudioRouteSet *routes)
{
    if (routes == NULL)
        return;

    g_hash_table_unref(routes->routes);
    g_free(routes);
}

gint
audio_route_set_add_operation(AudioRouteSet *routes,
                              const gchar *route_name,
                              const gchar *route_state,
                              const gchar *control,
                              const gchar *value)
{
    AudioRouteOperation *operation;
    AudioRoute *route;
    gchar *key;

    if (routes == NULL || route_name == NULL || route_state == NULL ||
        control == NULL || value == NULL)
        return -EINVAL;

    key = route_key(route_name, route_state);
    route = g_hash_table_lookup(routes->routes, key);
    if (route == NULL) {
        route = g_new0(AudioRoute, 1);
        route->name = g_strdup(route_name);
        route->state = g_strdup(route_state);
        route->operations = g_ptr_array_new_with_free_func(route_operation_free);
        g_hash_table_insert(routes->routes, key, route);
    } else {
        g_free(key);
    }

    operation = g_new0(AudioRouteOperation, 1);
    operation->control = g_strdup(control);
    operation->value = g_strdup(value);
    g_ptr_array_add(route->operations, operation);

    return 0;
}

static gint
audio_route_set_apply_internal(AudioRouteSet *routes,
                               AudioAlsaCard *card,
                               const gchar *route_name,
                               const gchar *route_state,
                               gboolean ignore_missing_controls)
{
    AudioRoute *route;
    gchar *key;
    guint i;
    gint ret;

    if (route_name == NULL || *route_name == '\0')
        return 0;

    key = route_key(route_name, route_state);
    route = g_hash_table_lookup(routes->routes, key);
    g_free(key);

    if (route == NULL)
        return -ENOENT;

    for (i = 0; i < route->operations->len; i++) {
        AudioRouteOperation *operation = g_ptr_array_index(route->operations, i);

        ret = audio_alsa_control_set_from_string(card,
                                                 operation->control,
                                                 operation->value);
        if (ret < 0) {
            if (ignore_missing_controls && ret == -ENOENT) {
                g_debug("route %s/%s: control '%s' is not exposed by this card, skipping",
                        route_name,
                        route_state,
                        operation->control);
                continue;
            }

            g_warning("route %s/%s: failed to set '%s' to '%s': %s",
                      route_name,
                      route_state,
                      operation->control,
                      operation->value,
                      snd_strerror(ret));
            return ret;
        }
    }

    return 0;
}

gint
audio_route_set_apply(AudioRouteSet *routes,
                      AudioAlsaCard *card,
                      const gchar *route_name,
                      const gchar *route_state)
{
    return audio_route_set_apply_internal(routes,
                                          card,
                                          route_name,
                                          route_state,
                                          FALSE);
}

gint
audio_route_set_apply_optional(AudioRouteSet *routes,
                               AudioAlsaCard *card,
                               const gchar *route_name,
                               const gchar *route_state)
{
    return audio_route_set_apply_internal(routes,
                                          card,
                                          route_name,
                                          route_state,
                                          TRUE);
}

gint
audio_route_set_apply_list(AudioRouteSet *routes,
                           AudioAlsaCard *card,
                           const gchar *route_list,
                           const gchar *route_state)
{
    gchar **names;
    guint i;
    gint ret = 0;

    if (route_list == NULL || *route_list == '\0')
        return 0;

    names = g_strsplit(route_list, ",", -1);

    if (g_strcmp0(route_state, "turnoff") == 0) {
        guint count = g_strv_length(names);

        for (i = count; i > 0; i--) {
            gchar *name = g_strstrip(names[i - 1]);

            if (*name == '\0')
                continue;

            ret = audio_route_set_apply(routes, card, name, route_state);
            if (ret < 0)
                break;
        }
    } else {
        for (i = 0; names[i] != NULL; i++) {
            gchar *name = g_strstrip(names[i]);

            if (*name == '\0')
                continue;

            ret = audio_route_set_apply(routes, card, name, route_state);
            if (ret < 0)
                break;
        }
    }

    g_strfreev(names);
    return ret;
}
