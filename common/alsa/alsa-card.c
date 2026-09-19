/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/alsa/alsa-card.h"

#include <errno.h>
#include <string.h>

#include <audio-manager/audio-manager-error.h>

static gboolean
card_name_matches(snd_ctl_card_info_t *info,
                  const gchar *name)
{
    return g_strcmp0(snd_ctl_card_info_get_id(info), name) == 0 ||
           g_strcmp0(snd_ctl_card_info_get_name(info), name) == 0 ||
           g_strcmp0(snd_ctl_card_info_get_longname(info), name) == 0;
}

static gboolean
pcm_name_matches(const gchar *value,
                 const gchar *name)
{
    gsize name_length;

    if (g_strcmp0(value, name) == 0)
        return TRUE;

    if (value == NULL || name == NULL)
        return FALSE;

    name_length = strlen(name);

    return strncmp(value, name, name_length) == 0 &&
           strcmp(value + name_length, " (*)") == 0;
}

AudioAlsaCard *
audio_alsa_card_open(const gchar *name,
                     GError **error)
{
    AudioAlsaCard *card = NULL;
    gint index = -1;
    gint ret;

    if (name == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "ALSA card name is missing");
        return NULL;
    }

    for (;;) {
        snd_ctl_card_info_t *info;
        snd_ctl_t *ctl;
        gchar hw_name[32];

        g_debug("ALSA card next request current=%d", index);
        ret = snd_card_next(&index);
        g_debug("ALSA card next answer ret=%d next=%d (%s)",
                ret, index, ret < 0 ? snd_strerror(ret) : "ok");
        if (ret < 0 || index < 0)
            break;

        snd_ctl_card_info_alloca(&info);
        g_snprintf(hw_name, sizeof(hw_name), "hw:%d", index);

        g_debug("ALSA control open request name=%s", hw_name);
        ret = snd_ctl_open(&ctl, hw_name, 0);
        g_debug("ALSA control open answer name=%s ret=%d (%s)",
                hw_name, ret, ret < 0 ? snd_strerror(ret) : "ok");
        if (ret < 0)
            continue;

        g_debug("ALSA card info request name=%s", hw_name);
        ret = snd_ctl_card_info(ctl, info);
        g_debug("ALSA card info answer name=%s ret=%d id='%s' name='%s' longname='%s' (%s)",
                hw_name,
                ret,
                ret >= 0 ? snd_ctl_card_info_get_id(info) : "",
                ret >= 0 ? snd_ctl_card_info_get_name(info) : "",
                ret >= 0 ? snd_ctl_card_info_get_longname(info) : "",
                ret < 0 ? snd_strerror(ret) : "ok");
        if (ret < 0 || !card_name_matches(info, name)) {
            g_debug("ALSA control close request name=%s", hw_name);
            ret = snd_ctl_close(ctl);
            g_debug("ALSA control close answer name=%s ret=%d (%s)",
                    hw_name, ret, ret < 0 ? snd_strerror(ret) : "ok");
            continue;
        }

        card = g_new0(AudioAlsaCard, 1);
        card->index = index;
        card->ctl = ctl;
        card->name = g_strdup(snd_ctl_card_info_get_name(info));
        card->id = g_strdup(snd_ctl_card_info_get_id(info));
        break;
    }

    if (card == NULL)
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to find ALSA card '%s'",
                    name);

    return card;
}

void
audio_alsa_card_close(AudioAlsaCard *card)
{
    gint ret;

    if (card == NULL)
        return;

    if (card->ctl != NULL) {
        g_debug("ALSA control close request card=%d", card->index);
        ret = snd_ctl_close(card->ctl);
        g_debug("ALSA control close answer card=%d ret=%d (%s)",
                card->index, ret, ret < 0 ? snd_strerror(ret) : "ok");
    }

    g_free(card->name);
    g_free(card->id);
    g_free(card);
}

gint
audio_alsa_card_find_pcm(AudioAlsaCard *card,
                         const gchar *name,
                         AudioDirection direction)
{
    snd_pcm_info_t *info;
    snd_pcm_stream_t stream;
    gint device = -1;
    gint ret;

    if (card == NULL || name == NULL)
        return -EINVAL;

    stream = direction == AUDIO_DIRECTION_OUTPUT ?
             SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;

    snd_pcm_info_alloca(&info);

    for (;;) {
        g_debug("ALSA PCM next device request card=%d current=%d", card->index, device);
        ret = snd_ctl_pcm_next_device(card->ctl, &device);
        g_debug("ALSA PCM next device answer card=%d ret=%d next=%d (%s)",
                card->index, ret, device, ret < 0 ? snd_strerror(ret) : "ok");
        if (ret < 0)
            return ret;
        if (device < 0)
            break;

        snd_pcm_info_set_device(info, (guint)device);
        snd_pcm_info_set_subdevice(info, 0);
        snd_pcm_info_set_stream(info, stream);

        g_debug("ALSA PCM info request card=%d device=%d direction=%s",
                card->index, device,
                direction == AUDIO_DIRECTION_OUTPUT ? "playback" : "capture");
        ret = snd_ctl_pcm_info(card->ctl, info);
        if (ret == -ENOENT) {
            g_debug("ALSA PCM info answer card=%d device=%d ret=%d (%s)",
                    card->index, device, ret, snd_strerror(ret));
            continue;
        }
        g_debug("ALSA PCM info answer card=%d device=%d ret=%d id='%s' name='%s' subname='%s' (%s)",
                card->index,
                device,
                ret,
                ret >= 0 ? snd_pcm_info_get_id(info) : "",
                ret >= 0 ? snd_pcm_info_get_name(info) : "",
                ret >= 0 ? snd_pcm_info_get_subdevice_name(info) : "",
                ret < 0 ? snd_strerror(ret) : "ok");
        if (ret < 0)
            return ret;

        if (pcm_name_matches(snd_pcm_info_get_id(info), name) ||
            pcm_name_matches(snd_pcm_info_get_name(info), name)) {
            g_debug("ALSA PCM resolved logical='%s' to card=%d device=%d direction=%s",
                    name, card->index, device,
                    direction == AUDIO_DIRECTION_OUTPUT ? "playback" : "capture");
            return device;
        }
    }

    return -ENOENT;
}
