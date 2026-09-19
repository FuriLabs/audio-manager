/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/route/mtk-device-parser.h"

#include <errno.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <audio-manager/audio-manager-error.h>

#include "common/xml-utils.h"

static gint
parse_kctl(MtkDeviceConfig *config,
           xmlNode *node,
           const gchar *route_name,
           const gchar *route_state)
{
    gchar *name;
    gchar *value;
    gint ret;

    name = audio_xml_property_dup(node, "name");
    value = audio_xml_property_dup(node, "value");
    if (name == NULL || value == NULL) {
        g_free(name);
        g_free(value);
        return -EINVAL;
    }

    ret = audio_route_set_add_operation(config->routes,
                                        route_name,
                                        route_state,
                                        name,
                                        value);
    g_free(name);
    g_free(value);
    return ret;
}

MtkDeviceConfig *
mtk_device_config_load(const gchar *path,
                       GError **error)
{
    MtkDeviceConfig *config;
    xmlDoc *document;
    xmlNode *root;
    xmlNode *node;
    gint ret = 0;

    document = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (document == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Unable to parse MediaTek audio device XML '%s'",
                    path);
        return NULL;
    }

    root = xmlDocGetRootElement(document);
    if (root == NULL || xmlStrcmp(root->name, BAD_CAST "root") != 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Invalid MediaTek audio device XML '%s'",
                    path);
        xmlFreeDoc(document);
        return NULL;
    }

    config = g_new0(MtkDeviceConfig, 1);
    config->routes = audio_route_set_new();

    for (node = root->children; node != NULL; node = node->next) {
        xmlNode *child;

        if (node->type != XML_ELEMENT_NODE)
            continue;

        if (xmlStrcmp(node->name, BAD_CAST "card") == 0) {
            g_free(config->card_name);
            config->card_name = audio_xml_property_dup(node, "name");
            continue;
        }

        if (xmlStrcmp(node->name, BAD_CAST "mixercontrol") != 0)
            continue;

        for (child = node->children; child != NULL; child = child->next) {
            if (child->type != XML_ELEMENT_NODE)
                continue;

            if (xmlStrcmp(child->name, BAD_CAST "kctl") == 0) {
                ret = parse_kctl(config, child, "__init__", "setting");
                if (ret < 0)
                    goto fail;
                continue;
            }

            if (xmlStrcmp(child->name, BAD_CAST "path") == 0) {
                gchar *route_name = audio_xml_property_dup(child, "name");
                gchar *route_state = audio_xml_property_dup(child, "value");
                xmlNode *kctl;

                if (route_name == NULL || route_state == NULL) {
                    g_free(route_name);
                    g_free(route_state);
                    ret = -EINVAL;
                    goto fail;
                }

                for (kctl = child->children; kctl != NULL; kctl = kctl->next) {
                    if (kctl->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(kctl->name, BAD_CAST "kctl") != 0)
                        continue;

                    ret = parse_kctl(config,
                                     kctl,
                                     route_name,
                                     route_state);
                    if (ret < 0)
                        break;
                }

                g_free(route_name);
                g_free(route_state);
                if (ret < 0)
                    goto fail;
            }
        }
    }

    xmlFreeDoc(document);
    return config;

fail:
    g_set_error(error,
                AUDIO_MANAGER_ERROR,
                AUDIO_MANAGER_ERROR_CONFIG,
                "Malformed MediaTek mixer route in '%s'",
                path);
    xmlFreeDoc(document);
    mtk_device_config_free(config);
    return NULL;
}

void
mtk_device_config_free(MtkDeviceConfig *config)
{
    if (config == NULL)
        return;

    g_free(config->card_name);
    audio_route_set_free(config->routes);
    g_free(config);
}
