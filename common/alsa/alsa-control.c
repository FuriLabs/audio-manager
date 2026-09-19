/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/alsa/alsa-control.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void
control_id_init(snd_ctl_elem_id_t *id,
                const gchar *name)
{
    snd_ctl_elem_id_clear(id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
}

gint
audio_alsa_control_set_from_string(AudioAlsaCard *card,
                                   const gchar *name,
                                   const gchar *value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;
    snd_ctl_elem_type_t type;
    gboolean enabled;
    gchar *end;
    glong integer;
    gint64 integer64;
    guint count;
    guint items;
    guint i;
    gint found;
    gint ret;

    if (card == NULL || card->ctl == NULL || name == NULL || value == NULL)
        return -EINVAL;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    control_id_init(id, name);
    snd_ctl_elem_info_set_id(info, id);

    g_debug("ALSA control info request name='%s'", name);
    ret = snd_ctl_elem_info(card->ctl, info);
    g_debug("ALSA control info answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    snd_ctl_elem_value_set_id(control, id);
    g_debug("ALSA control read request name='%s'", name);
    ret = snd_ctl_elem_read(card->ctl, control);
    g_debug("ALSA control read answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    type = snd_ctl_elem_info_get_type(info);
    count = snd_ctl_elem_info_get_count(info);

    switch (type) {
    case SND_CTL_ELEM_TYPE_BOOLEAN:
        enabled = g_ascii_strcasecmp(value, "true") == 0 ||
                  g_ascii_strcasecmp(value, "on") == 0 ||
                  strcmp(value, "1") == 0;

        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_boolean(control, i, enabled);
        break;
    case SND_CTL_ELEM_TYPE_INTEGER:
        end = NULL;
        integer = strtol(value, &end, 0);

        if (end == value || *end != '\0')
            return -EINVAL;

        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_integer(control, i, integer);
        break;
    case SND_CTL_ELEM_TYPE_INTEGER64:
        end = NULL;
        integer64 = g_ascii_strtoll(value, &end, 0);

        if (end == value || *end != '\0')
            return -EINVAL;

        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_integer64(control, i, integer64);
        break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
        items = snd_ctl_elem_info_get_items(info);
        found = -1;

        for (i = 0; i < items; i++) {
            snd_ctl_elem_info_set_item(info, i);
            g_debug("ALSA control enum info request name='%s' item=%u", name, i);
            ret = snd_ctl_elem_info(card->ctl, info);
            g_debug("ALSA control enum info answer name='%s' item=%u ret=%d (%s)",
                    name, i, ret, ret < 0 ? snd_strerror(ret) : "ok");
            if (ret < 0)
                return ret;

            if (g_strcmp0(snd_ctl_elem_info_get_item_name(info), value) == 0) {
                found = (gint)i;
                break;
            }
        }

        if (found < 0)
            return -EINVAL;

        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_enumerated(control, i, (guint)found);
        break;
    default:
        return -ENOTSUP;
    }

    g_debug("ALSA control write request name='%s' value='%s'", name, value);
    ret = snd_ctl_elem_write(card->ctl, control);
    g_debug("ALSA control write answer name='%s' value='%s' ret=%d (%s)",
            name, value, ret, ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gint
audio_alsa_control_set_index(AudioAlsaCard *card,
                             const gchar *name,
                             glong value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;
    snd_ctl_elem_type_t type;
    guint count;
    guint i;
    gint ret;

    if (card == NULL || card->ctl == NULL || name == NULL)
        return -EINVAL;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    control_id_init(id, name);
    snd_ctl_elem_info_set_id(info, id);
    g_debug("ALSA control info request name='%s'", name);
    ret = snd_ctl_elem_info(card->ctl, info);
    g_debug("ALSA control info answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    snd_ctl_elem_value_set_id(control, id);
    g_debug("ALSA control read request name='%s'", name);
    ret = snd_ctl_elem_read(card->ctl, control);
    g_debug("ALSA control read answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    type = snd_ctl_elem_info_get_type(info);
    count = snd_ctl_elem_info_get_count(info);

    switch (type) {
    case SND_CTL_ELEM_TYPE_BOOLEAN:
        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_boolean(control, i, value != 0);
        break;
    case SND_CTL_ELEM_TYPE_INTEGER:
        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_integer(control, i, value);
        break;
    case SND_CTL_ELEM_TYPE_INTEGER64:
        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_integer64(control, i, value);
        break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
        if (value < 0 || (gulong)value >= snd_ctl_elem_info_get_items(info))
            return -ERANGE;
        for (i = 0; i < count; i++)
            snd_ctl_elem_value_set_enumerated(control, i, (guint)value);
        break;
    default:
        return -ENOTSUP;
    }

    g_debug("ALSA control write request name='%s' index=%ld", name, value);
    ret = snd_ctl_elem_write(card->ctl, control);
    g_debug("ALSA control write answer name='%s' index=%ld ret=%d (%s)",
            name, value, ret, ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gint
audio_alsa_control_get_integer(AudioAlsaCard *card,
                               const gchar *name,
                               glong *value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;
    gint ret;

    if (card == NULL || card->ctl == NULL || name == NULL || value == NULL)
        return -EINVAL;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    control_id_init(id, name);
    snd_ctl_elem_info_set_id(info, id);
    g_debug("ALSA control info request name='%s'", name);
    ret = snd_ctl_elem_info(card->ctl, info);
    g_debug("ALSA control info answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    snd_ctl_elem_value_set_id(control, id);
    g_debug("ALSA control read request name='%s'", name);
    ret = snd_ctl_elem_read(card->ctl, control);
    g_debug("ALSA control read answer name='%s' ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    switch (snd_ctl_elem_info_get_type(info)) {
    case SND_CTL_ELEM_TYPE_BOOLEAN:
        *value = snd_ctl_elem_value_get_boolean(control, 0);
        break;
    case SND_CTL_ELEM_TYPE_INTEGER:
        *value = snd_ctl_elem_value_get_integer(control, 0);
        break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
        *value = snd_ctl_elem_value_get_enumerated(control, 0);
        break;
    default:
        return -ENOTSUP;
    }

    g_debug("ALSA control read value name='%s' value=%ld", name, *value);
    return 0;
}
