/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/xml-utils.h"

gchar *
audio_xml_property_dup(xmlNode *node,
                       const gchar *name)
{
    xmlChar *value;
    gchar *copy;

    if (node == NULL || name == NULL)
        return NULL;

    value = xmlGetProp(node, BAD_CAST name);
    if (value == NULL)
        return NULL;

    copy = g_strdup((const gchar *)value);
    xmlFree(value);
    return copy;
}
