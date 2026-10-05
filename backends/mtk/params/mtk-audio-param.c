/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/params/mtk-audio-param.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <audio-manager/audio-manager-error.h>

#include "common/xml-utils.h"

typedef struct {
    gchar *path;
    gchar *param_id;
} MtkAudioParamPath;

struct MtkAudioParam {
    xmlDoc *document;
    GHashTable *units;
    GPtrArray *paths;
    GHashTable *value_cache;
};

static void
param_path_free(gpointer data)
{
    MtkAudioParamPath *path = data;

    if (path == NULL)
        return;
    g_free(path->path);
    g_free(path->param_id);
    g_free(path);
}

static xmlNode *
find_child(xmlNode *parent, const gchar *name)
{
    xmlNode *node;

    for (node = parent != NULL ? parent->children : NULL;
         node != NULL; node = node->next) {
        if (node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(node->name, BAD_CAST name) == 0)
            return node;
    }
    return NULL;
}

MtkAudioParam *
mtk_audio_param_load(const gchar *path, GError **error)
{
    MtkAudioParam *param;
    xmlNode *root;
    xmlNode *pool;
    xmlNode *tree;
    xmlNode *node;

    if (path == NULL)
        return NULL;

    param = g_new0(MtkAudioParam, 1);
    param->document = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (param->document == NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Unable to parse MediaTek AudioParam '%s'", path);
        mtk_audio_param_free(param);
        return NULL;
    }

    root = xmlDocGetRootElement(param->document);
    if (root == NULL || xmlStrcmp(root->name, BAD_CAST "AudioParam") != 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Invalid MediaTek AudioParam '%s'", path);
        mtk_audio_param_free(param);
        return NULL;
    }

    param->units = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    param->paths = g_ptr_array_new_with_free_func(param_path_free);
    param->value_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    pool = find_child(root, "ParamUnitPool");
    for (node = pool != NULL ? pool->children : NULL; node != NULL; node = node->next) {
        gchar *id;
        if (node->type != XML_ELEMENT_NODE ||
            xmlStrcmp(node->name, BAD_CAST "ParamUnit") != 0)
            continue;
        id = audio_xml_property_dup(node, "param_id");
        if (id != NULL)
            g_hash_table_replace(param->units, id, node);
    }

    tree = find_child(root, "ParamTree");
    for (node = tree != NULL ? tree->children : NULL; node != NULL; node = node->next) {
        MtkAudioParamPath *entry;
        gchar *p;
        gchar *id;

        if (node->type != XML_ELEMENT_NODE ||
            xmlStrcmp(node->name, BAD_CAST "Param") != 0)
            continue;
        p = audio_xml_property_dup(node, "path");
        id = audio_xml_property_dup(node, "param_id");
        if (p == NULL || id == NULL) {
            g_free(p);
            g_free(id);
            continue;
        }
        entry = g_new0(MtkAudioParamPath, 1);
        entry->path = p;
        entry->param_id = id;
        g_ptr_array_add(param->paths, entry);
    }

    if (g_hash_table_size(param->units) == 0 || param->paths->len == 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek AudioParam '%s' has no parameter tree", path);
        mtk_audio_param_free(param);
        return NULL;
    }

    g_debug("AudioParam loaded '%s' units=%u paths=%u",
            path, g_hash_table_size(param->units), param->paths->len);
    return param;
}

void
mtk_audio_param_free(MtkAudioParam *param)
{
    if (param == NULL)
        return;
    if (param->paths != NULL)
        g_ptr_array_free(param->paths, TRUE);
    if (param->value_cache != NULL)
        g_hash_table_unref(param->value_cache);
    if (param->units != NULL)
        g_hash_table_unref(param->units);
    if (param->document != NULL)
        xmlFreeDoc(param->document);
    g_free(param);
}

static const gchar *
unit_param_value(MtkAudioParam *param, const gchar *param_id, const gchar *name)
{
    xmlNode *unit;
    xmlNode *node;

    unit = g_hash_table_lookup(param->units, param_id);
    for (node = unit != NULL ? unit->children : NULL; node != NULL; node = node->next) {
        gchar *param_name;
        xmlChar *value;
        const gchar *cached;

        if (node->type != XML_ELEMENT_NODE ||
            xmlStrcmp(node->name, BAD_CAST "Param") != 0)
            continue;
        param_name = audio_xml_property_dup(node, "name");
        if (g_strcmp0(param_name, name) != 0) {
            g_free(param_name);
            continue;
        }
        g_free(param_name);
        cached = g_hash_table_lookup(param->value_cache, node);
        if (cached != NULL)
            return cached;
        value = xmlGetProp(node, BAD_CAST "value");
        if (value == NULL)
            return NULL;
        cached = g_strdup((const gchar *)value);
        xmlFree(value);
        g_hash_table_insert(param->value_cache, node, (gpointer)cached);
        return cached;
    }
    return NULL;
}

const gchar *
mtk_audio_param_get_param(MtkAudioParam *param,
                          const gchar *path,
                          const gchar *name)
{
    guint i;

    if (param == NULL || path == NULL || name == NULL)
        return NULL;
    for (i = 0; i < param->paths->len; i++) {
        MtkAudioParamPath *entry = g_ptr_array_index(param->paths, i);
        if (g_strcmp0(entry->path, path) == 0)
            return unit_param_value(param, entry->param_id, name);
    }
    return NULL;
}

const gchar *
mtk_audio_param_get_first_param(MtkAudioParam *param,
                                const gchar *const *paths,
                                gsize path_count,
                                const gchar *name,
                                const gchar **matched_path)
{
    gsize i;

    if (matched_path != NULL)
        *matched_path = NULL;
    if (param == NULL || paths == NULL || name == NULL)
        return NULL;

    for (i = 0; i < path_count; i++) {
        const gchar *value;

        if (paths[i] == NULL)
            continue;
        value = mtk_audio_param_get_param(param, paths[i], name);
        if (value == NULL)
            continue;
        if (matched_path != NULL)
            *matched_path = paths[i];
        return value;
    }

    return NULL;
}

gsize
mtk_audio_param_get_path_count(MtkAudioParam *param)
{
    return param != NULL && param->paths != NULL ? param->paths->len : 0;
}

const gchar *
mtk_audio_param_get_path(MtkAudioParam *param,
                         gsize index)
{
    MtkAudioParamPath *entry;

    if (param == NULL || param->paths == NULL || index >= param->paths->len)
        return NULL;
    entry = g_ptr_array_index(param->paths, index);
    return entry->path;
}

gint
mtk_audio_param_parse_i32_list(const gchar *value,
                               gint32 **values,
                               gsize *count)
{
    GArray *array;
    gchar **parts;
    guint i;

    if (values == NULL || count == NULL || value == NULL)
        return -EINVAL;
    *values = NULL;
    *count = 0;
    array = g_array_new(FALSE, FALSE, sizeof(gint32));
    parts = g_strsplit(value, ",", -1);
    for (i = 0; parts[i] != NULL; i++) {
        gchar *end = NULL;
        gint64 v;
        gint32 vv;
        gchar *s = g_strstrip(parts[i]);
        if (*s == '\0')
            continue;
        errno = 0;
        v = g_ascii_strtoll(s, &end, 0);
        if (errno != 0 || end == s || *end != '\0' || v < G_MININT32 || v > G_MAXINT32) {
            g_strfreev(parts);
            g_array_free(array, TRUE);
            return -EINVAL;
        }
        vv = (gint32)v;
        g_array_append_val(array, vv);
    }
    g_strfreev(parts);
    *count = array->len;
    *values = (gint32 *)g_array_free(array, FALSE);
    if (*count == 0) {
        g_free(*values);
        *values = NULL;
        return -ENOENT;
    }

    return 0;
}

gint
mtk_audio_param_parse_u16_list(const gchar *value,
                               guint16 **values,
                               gsize *count)
{
    gint32 *tmp = NULL;
    gsize n = 0;
    gsize i;
    guint16 *out;
    gint ret;

    if (values == NULL || count == NULL)
        return -EINVAL;
    *values = NULL;
    *count = 0;
    ret = mtk_audio_param_parse_i32_list(value, &tmp, &n);
    if (ret < 0)
        return ret;
    out = g_new(guint16, n);
    for (i = 0; i < n; i++) {
        if (tmp[i] < 0 || tmp[i] > G_MAXUINT16) {
            g_free(tmp);
            g_free(out);
            return -ERANGE;
        }
        out[i] = (guint16)tmp[i];
    }
    g_free(tmp);
    *values = out;
    *count = n;
    return 0;
}
