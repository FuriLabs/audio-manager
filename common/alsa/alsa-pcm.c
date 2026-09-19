/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "common/alsa/alsa-pcm.h"

#include <errno.h>

#include <audio-manager/audio-manager-error.h>

#define PCM_SET_DEBUG(label, expression) do { \
    g_debug("ALSA PCM %s request", label); \
    ret = (expression); \
    g_debug("ALSA PCM %s answer ret=%d (%s)", label, ret, \
            ret < 0 ? snd_strerror(ret) : "ok"); \
    if (ret < 0) \
        return ret; \
} while (0)

struct AudioAlsaPcm {
    snd_pcm_t *handle;
    gint device;
    AudioDirection direction;
    AudioFormat format;
    guint rate;
    guint channels;
    guint period_size;
    guint period_count;
    guint buffer_size;
    gsize frame_bytes;
    gchar *name;
};

static const gchar *
direction_name(AudioDirection direction)
{
    return direction == AUDIO_DIRECTION_OUTPUT ? "playback" : "capture";
}

static gint
pcm_configure(AudioAlsaPcm *pcm,
              const AudioAlsaPcmConfig *config)
{
    snd_pcm_t *handle = pcm->handle;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_format_t format;
    snd_pcm_uframes_t period_size;
    snd_pcm_format_t actual_format;
    snd_pcm_uframes_t actual_period = 0;
    snd_pcm_uframes_t actual_buffer = 0;
    guint rate;
    guint periods;
    guint actual_rate = 0;
    guint actual_channels = 0;
    guint actual_periods = 0;
    gint actual_dir = 0;
    gint dir = 0;
    gint ret;

    format = audio_format_to_alsa(config->format);
    if (format == SND_PCM_FORMAT_UNKNOWN)
        return -EINVAL;

    snd_pcm_hw_params_alloca(&hw);
    g_debug("ALSA PCM hw_params_any request");
    ret = snd_pcm_hw_params_any(handle, hw);
    g_debug("ALSA PCM hw_params_any answer ret=%d (%s)",
            ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    PCM_SET_DEBUG("set_access=RW_INTERLEAVED",
                  snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    PCM_SET_DEBUG("set_format",
                  snd_pcm_hw_params_set_format(handle, hw, format));
    PCM_SET_DEBUG("set_channels",
                  snd_pcm_hw_params_set_channels(handle, hw, config->channels));

    rate = config->rate;
    g_debug("ALSA PCM set_rate_near request rate=%u", rate);
    ret = snd_pcm_hw_params_set_rate_near(handle, hw, &rate, &dir);
    g_debug("ALSA PCM set_rate_near answer ret=%d rate=%u dir=%d (%s)",
            ret, rate, dir, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;
    if (rate != config->rate)
        return -EINVAL;

    period_size = config->period_size;
    g_debug("ALSA PCM set_period_size_near request frames=%lu",
            (gulong)period_size);
    ret = snd_pcm_hw_params_set_period_size_near(handle, hw, &period_size, &dir);
    g_debug("ALSA PCM set_period_size_near answer ret=%d frames=%lu dir=%d (%s)",
            ret, (gulong)period_size, dir,
            ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    periods = config->period_count;
    g_debug("ALSA PCM set_periods_near request periods=%u", periods);
    ret = snd_pcm_hw_params_set_periods_near(handle, hw, &periods, &dir);
    g_debug("ALSA PCM set_periods_near answer ret=%d periods=%u dir=%d (%s)",
            ret, periods, dir, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    g_debug("ALSA PCM hw_params commit request");
    ret = snd_pcm_hw_params(handle, hw);
    g_debug("ALSA PCM hw_params commit answer ret=%d (%s)",
            ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    /* keep the values ALSA actually negotiated */
    snd_pcm_hw_params_current(handle, hw);
    if (snd_pcm_hw_params_get_format(hw, &actual_format) == 0)
        pcm->format = audio_format_from_alsa(actual_format);
    if (snd_pcm_hw_params_get_rate(hw, &actual_rate, &actual_dir) == 0)
        pcm->rate = actual_rate;
    if (snd_pcm_hw_params_get_channels(hw, &actual_channels) == 0)
        pcm->channels = actual_channels;
    if (snd_pcm_hw_params_get_period_size(hw, &actual_period, &actual_dir) == 0)
        pcm->period_size = (guint)actual_period;
    if (snd_pcm_hw_params_get_periods(hw, &actual_periods, &actual_dir) == 0)
        pcm->period_count = actual_periods;
    if (snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer) == 0)
        pcm->buffer_size = (guint)actual_buffer;

    if (pcm->format != AUDIO_FORMAT_UNKNOWN) {
        gint width = snd_pcm_format_physical_width(audio_format_to_alsa(pcm->format));
        if (width > 0)
            pcm->frame_bytes = ((gsize)width / 8U) * pcm->channels;
    }

    if (config->disable_stop_threshold || config->start_threshold > 0) {
        snd_pcm_uframes_t boundary;

        snd_pcm_sw_params_alloca(&sw);
        PCM_SET_DEBUG("sw_params_current", snd_pcm_sw_params_current(handle, sw));
        if (config->disable_stop_threshold) {
            PCM_SET_DEBUG("sw_params_get_boundary", snd_pcm_sw_params_get_boundary(sw, &boundary));
            g_debug("ALSA PCM set_stop_threshold request boundary=%lu",
                    (gulong)boundary);
            ret = snd_pcm_sw_params_set_stop_threshold(handle, sw, boundary);
            g_debug("ALSA PCM set_stop_threshold answer ret=%d (%s)",
                    ret, ret < 0 ? snd_strerror(ret) : "ok");
            if (ret < 0)
                return ret;
        }
        if (config->start_threshold > 0) {
            g_debug("ALSA PCM set_start_threshold request frames=%u",
                    config->start_threshold);
            ret = snd_pcm_sw_params_set_start_threshold(handle, sw,
                                                         config->start_threshold);
            g_debug("ALSA PCM set_start_threshold answer ret=%d (%s)",
                    ret, ret < 0 ? snd_strerror(ret) : "ok");
            if (ret < 0)
                return ret;
        }
        PCM_SET_DEBUG("sw_params commit", snd_pcm_sw_params(handle, sw));
    }

    g_debug("ALSA PCM prepare request");
    ret = snd_pcm_prepare(handle);
    g_debug("ALSA PCM prepare answer ret=%d (%s)",
            ret, ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

AudioAlsaPcm *
audio_alsa_pcm_open(AudioAlsaCard *card,
                    const AudioAlsaPcmConfig *config,
                    GError **error)
{
    AudioAlsaPcm *pcm;
    snd_pcm_stream_t stream;
    snd_pcm_format_t alsa_format;
    gint flags = 0;
    gchar name[32];
    gint device;
    gint width;
    gint ret;

    if (card == NULL || config == NULL || config->device == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "Invalid ALSA PCM configuration");
        return NULL;
    }

    device = audio_alsa_card_find_pcm(card,
                                      config->device,
                                      config->direction);
    if (device < 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to find PCM '%s' on card '%s'",
                    config->device,
                    card->name);
        return NULL;
    }

    stream = config->direction == AUDIO_DIRECTION_OUTPUT ?
             SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
    if (config->nonblock)
        flags |= SND_PCM_NONBLOCK;

    g_snprintf(name, sizeof(name), "hw:%d,%d", card->index, device);

    pcm = g_new0(AudioAlsaPcm, 1);
    pcm->device = device;
    pcm->direction = config->direction;
    pcm->format = config->format;
    pcm->rate = config->rate;
    pcm->channels = config->channels;
    pcm->period_size = config->period_size;
    pcm->period_count = config->period_count;
    pcm->buffer_size = config->period_size * config->period_count;
    pcm->name = g_strdup(name);

    alsa_format = audio_format_to_alsa(config->format);
    width = snd_pcm_format_physical_width(alsa_format);
    if (width <= 0) {
        audio_alsa_pcm_close(pcm);
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Invalid physical width for PCM format");
        return NULL;
    }
    pcm->frame_bytes = ((gsize)width / 8U) * config->channels;

    g_debug("ALSA PCM open request name=%s logical='%s' direction=%s flags=0x%x "
            "rate=%u channels=%u period=%u count=%u frame_bytes=%zu",
            name, config->device, direction_name(config->direction), flags,
            config->rate, config->channels, config->period_size,
            config->period_count, pcm->frame_bytes);
    ret = snd_pcm_open(&pcm->handle, name, stream, flags);
    g_debug("ALSA PCM open answer name=%s ret=%d (%s)",
            name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to open PCM '%s': %s",
                    name,
                    snd_strerror(ret));
        audio_alsa_pcm_close(pcm);
        return NULL;
    }

    ret = pcm_configure(pcm, config);
    if (ret < 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to configure PCM '%s': %s",
                    name,
                    snd_strerror(ret));
        audio_alsa_pcm_close(pcm);
        return NULL;
    }

    return pcm;
}

void
audio_alsa_pcm_close(AudioAlsaPcm *pcm)
{
    gint ret;

    if (pcm == NULL)
        return;

    if (pcm->handle != NULL) {
        snd_pcm_t *handle = pcm->handle;

        pcm->handle = NULL;
        g_debug("ALSA PCM close request name=%s", pcm->name != NULL ? pcm->name : "?");
        ret = snd_pcm_close(handle);
        g_debug("ALSA PCM close answer name=%s ret=%d (%s)",
                pcm->name != NULL ? pcm->name : "?", ret,
                ret < 0 ? snd_strerror(ret) : "ok");
    }

    g_free(pcm->name);
    g_free(pcm);
}

gint
audio_alsa_pcm_prepare(AudioAlsaPcm *pcm)
{
    gint ret;

    if (pcm == NULL || pcm->handle == NULL)
        return -EINVAL;
    g_debug("ALSA PCM prepare request name=%s", pcm->name);
    ret = snd_pcm_prepare(pcm->handle);
    g_debug("ALSA PCM prepare answer name=%s ret=%d (%s)",
            pcm->name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gint
audio_alsa_pcm_start(AudioAlsaPcm *pcm)
{
    gint ret;

    if (pcm == NULL || pcm->handle == NULL)
        return -EINVAL;

    g_debug("ALSA PCM start request name=%s", pcm->name);
    ret = snd_pcm_start(pcm->handle);
    g_debug("ALSA PCM start answer name=%s ret=%d (%s)",
            pcm->name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gint
audio_alsa_pcm_stop(AudioAlsaPcm *pcm)
{
    gint ret;

    if (pcm == NULL || pcm->handle == NULL)
        return -EINVAL;

    g_debug("ALSA PCM drop request name=%s", pcm->name);
    ret = snd_pcm_drop(pcm->handle);
    g_debug("ALSA PCM drop answer name=%s ret=%d (%s)",
            pcm->name, ret, ret < 0 ? snd_strerror(ret) : "ok");
    if (ret < 0)
        return ret;

    return audio_alsa_pcm_prepare(pcm);
}

static gssize
pcm_transfer(AudioAlsaPcm *pcm,
             gpointer buffer,
             gsize bytes,
             gboolean write_data)
{
    gsize frame_bytes;
    gsize total_frames;
    gsize done_frames = 0;

    if (pcm == NULL || pcm->handle == NULL || buffer == NULL)
        return -EINVAL;
    if ((write_data && pcm->direction != AUDIO_DIRECTION_OUTPUT) ||
        (!write_data && pcm->direction != AUDIO_DIRECTION_INPUT))
        return -EINVAL;

    frame_bytes = pcm->frame_bytes;
    if (frame_bytes == 0 || bytes % frame_bytes != 0)
        return -EINVAL;
    total_frames = bytes / frame_bytes;

    while (done_frames < total_frames) {
        snd_pcm_sframes_t ret;
        guint8 *p = (guint8 *)buffer + done_frames * frame_bytes;
        snd_pcm_uframes_t remain = (snd_pcm_uframes_t)(total_frames - done_frames);

        g_debug("ALSA PCM %s request name=%s frames=%lu bytes=%zu",
                write_data ? "write" : "read", pcm->name,
                (gulong)remain, (gsize)remain * frame_bytes);
        ret = write_data ?
              snd_pcm_writei(pcm->handle, p, remain) :
              snd_pcm_readi(pcm->handle, p, remain);
        g_debug("ALSA PCM %s answer name=%s ret_frames=%ld (%s)",
                write_data ? "write" : "read", pcm->name,
                (glong)ret, ret < 0 ? snd_strerror((gint)ret) : "ok");
        if (ret == -EINTR)
            continue;
        if (ret == -EPIPE || ret == -ESTRPIPE) {
            gint recover;
            g_debug("ALSA PCM recover request name=%s error=%ld", pcm->name, (glong)ret);
            recover = snd_pcm_recover(pcm->handle, (gint)ret, 1);
            g_debug("ALSA PCM recover answer name=%s ret=%d (%s)",
                    pcm->name, recover,
                    recover < 0 ? snd_strerror(recover) : "ok");
            if (recover < 0)
                return recover;
            continue;
        }
        if (ret < 0)
            return (gssize)ret;
        if (ret == 0)
            break;
        done_frames += (gsize)ret;
    }

    return (gssize)(done_frames * frame_bytes);
}

gssize
audio_alsa_pcm_write(AudioAlsaPcm *pcm,
                     gconstpointer data,
                     gsize bytes)
{
    return pcm_transfer(pcm, (gpointer )data, bytes, TRUE);
}

gssize
audio_alsa_pcm_read(AudioAlsaPcm *pcm,
                    gpointer data,
                    gsize bytes)
{
    return pcm_transfer(pcm, data, bytes, FALSE);
}

gint
audio_alsa_pcm_avail_delay(AudioAlsaPcm *pcm,
                           snd_pcm_sframes_t *avail,
                           snd_pcm_sframes_t *delay)
{
    gint ret;

    if (pcm == NULL || pcm->handle == NULL || avail == NULL || delay == NULL)
        return -EINVAL;
    g_debug("ALSA PCM avail_delay request name=%s", pcm->name);
    ret = snd_pcm_avail_delay(pcm->handle, avail, delay);
    g_debug("ALSA PCM avail_delay answer name=%s ret=%d avail=%ld delay=%ld (%s)",
            pcm->name, ret, (glong)*avail, (glong)*delay,
            ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gint
audio_alsa_pcm_get_htimestamp(AudioAlsaPcm *pcm,
                              snd_pcm_uframes_t *avail,
                              struct timespec *timestamp)
{
    gint ret;

    if (pcm == NULL || pcm->handle == NULL || avail == NULL || timestamp == NULL)
        return -EINVAL;
    g_debug("ALSA PCM htimestamp request name=%s", pcm->name);
    ret = snd_pcm_htimestamp(pcm->handle, avail, timestamp);
    g_debug("ALSA PCM htimestamp answer name=%s ret=%d avail=%lu ts=%ld.%09ld (%s)",
            pcm->name, ret, (gulong)*avail,
            (glong)timestamp->tv_sec, timestamp->tv_nsec,
            ret < 0 ? snd_strerror(ret) : "ok");
    return ret;
}

gsize
audio_alsa_pcm_frame_bytes(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->frame_bytes : 0;
}

guint
audio_alsa_pcm_rate(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->rate : 0;
}

guint
audio_alsa_pcm_channels(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->channels : 0;
}

guint
audio_alsa_pcm_period_size(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->period_size : 0;
}

guint
audio_alsa_pcm_period_count(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->period_count : 0;
}

guint
audio_alsa_pcm_buffer_size(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->buffer_size : 0;
}

AudioFormat
audio_alsa_pcm_format(AudioAlsaPcm *pcm)
{
    return pcm != NULL ? pcm->format : AUDIO_FORMAT_UNKNOWN;
}
