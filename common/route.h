/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef ROUTE_H
#define ROUTE_H

#include <glib.h>

#include "common/alsa/alsa-card.h"

typedef struct AudioRouteSet AudioRouteSet;

/**
 * Create an empty mixer route set.
 */
AudioRouteSet *
audio_route_set_new(void);

/**
 * Free a mixer route set.
 */
void
audio_route_set_free(AudioRouteSet *routes);

/**
 * Add one mixer control operation to a named route/state pair.
 *
 * @return 0 on success or a negative errno value
 */
gint
audio_route_set_add_operation(AudioRouteSet *routes,
                              const gchar *route_name,
                              const gchar *route_state,
                              const gchar *control,
                              const gchar *value);

/**
 * Apply one named route/state pair to an ALSA card.
 */
gint
audio_route_set_apply(AudioRouteSet *routes,
                      AudioAlsaCard *card,
                      const gchar *route_name,
                      const gchar *route_state);

/**
 * Apply a route when present and succeed when it is absent.
 */
gint
audio_route_set_apply_optional(AudioRouteSet *routes,
                               AudioAlsaCard *card,
                               const gchar *route_name,
                               const gchar *route_state);

/**
 * Apply a comma separated list of routes in order.
 */
gint
audio_route_set_apply_list(AudioRouteSet *routes,
                           AudioAlsaCard *card,
                           const gchar *route_list,
                           const gchar *route_state);

#endif /* ROUTE_H */
