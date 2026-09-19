/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_DEVICE_PARSER_H
#define MTK_DEVICE_PARSER_H

#include <glib.h>

#include "common/route.h"

/**
 * Parsed MediaTek mixer route XML configuration.
 */
typedef struct {
    gchar *card_name;       /**< ALSA card selector from the XML */
    AudioRouteSet *routes;  /**< Parsed route operations */
} MtkDeviceConfig;

/**
 * Load MediaTek mixer routes from the vendor device config.
 *
 * @param path XML path
 * @param error Error return location
 * @return Parsed device configuration or NULL on failure
 */
MtkDeviceConfig *
mtk_device_config_load(const gchar *path,
                       GError **error);

/**
 * Free a parsed MediaTek device configuration.
 */
void
mtk_device_config_free(MtkDeviceConfig *config);

#endif /* MTK_DEVICE_PARSER_H */
