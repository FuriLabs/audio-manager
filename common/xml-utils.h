/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef XML_UTILS_H
#define XML_UTILS_H

#include <glib.h>
#include <libxml/tree.h>

/**
 * Duplicate one XML property
 *
 * @param node XML node containing the property
 * @param name Property name
 * @return Newly allocated property value or NULL when absent
 */
gchar *
audio_xml_property_dup(xmlNode *node,
                       const gchar *name);

#endif /* XML_UTILS_H */
