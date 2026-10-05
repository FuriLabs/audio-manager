/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <audio-manager/audio-manager.h>

#include <errno.h>

#include "backends/backend.h"
#include "common/config/config.h"
#include "common/math-utils.h"
#include "src/audio-manager-private.h"

static void audio_manager_detach_call_streams(AudioManager *manager,
                                              gboolean orphan);

static void
audio_manager_sync_devices(AudioManager *manager)
{
    if (manager == NULL || manager->backend_ops == NULL || manager->backend == NULL)
        return;

    if (manager->backend_ops->get_output_device != NULL)
        manager->output_device = manager->backend_ops->get_output_device(manager->backend);
    if (manager->backend_ops->get_input_device != NULL)
        manager->input_device = manager->backend_ops->get_input_device(manager->backend);
}

AudioManager *
audio_manager_new(const AudioManagerConfig *config,
                  GError **error)
{
    AudioManager *manager;
    AudioManagerBackendCreateInfo create_info = { 0 };
    const AudioManagerBackendOps *backend_ops;
    GMainContext *main_context = NULL;
    GKeyFile *key_file;
    gchar *backend_name;
    gchar *device_config_path;

    if (config == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerConfig, config_path) ||
        config->config_path == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "A configuration path and initialized AudioManagerConfig are required");
        return NULL;
    }

    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerConfig, main_context))
        main_context = config->main_context;

    key_file = audio_config_load(config->config_path, error);
    if (key_file == NULL)
        return NULL;

    backend_name = audio_config_get_string(key_file,
                                           "AudioManager",
                                           "Backend",
                                           NULL);
    device_config_path = audio_config_get_string(key_file,
                                                 "AudioManager",
                                                 "DeviceConfig",
                                                 NULL);

    if (backend_name == NULL || device_config_path == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_CONFIG,
                    "AudioManager.Backend and AudioManager.DeviceConfig are required");
        g_free(backend_name);
        g_free(device_config_path);
        g_key_file_unref(key_file);
        return NULL;
    }

    backend_ops = audio_manager_backend_find(backend_name);
    if (backend_ops == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_BACKEND,
                    "Unknown audio backend '%s'",
                    backend_name);
        g_free(backend_name);
        g_free(device_config_path);
        g_key_file_unref(key_file);
        return NULL;
    }

    manager = g_new0(AudioManager, 1);
    g_rec_mutex_init(&manager->control_lock);
    manager->config = key_file;
    manager->backend_ops = backend_ops;
    manager->main_context = g_main_context_ref(main_context != NULL ?
                                               main_context :
                                               g_main_context_default());
    manager->output_device = AUDIO_MANAGER_OUTPUT_NONE;
    manager->input_device = AUDIO_MANAGER_INPUT_NONE;
    manager->call_volume = 1.0;
    manager->call_transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;

    create_info.main_context = manager->main_context;
    manager->backend = backend_ops->create(&create_info,
                                           device_config_path,
                                           error);
    if (manager->backend == NULL) {
        g_main_context_unref(manager->main_context);
        g_key_file_unref(manager->config);
        g_rec_mutex_clear(&manager->control_lock);
        g_free(manager);
        manager = NULL;
    } else {
        audio_manager_sync_devices(manager);
    }

    g_free(backend_name);
    g_free(device_config_path);

    return manager;
}

void
audio_manager_free(AudioManager *manager)
{
    if (manager == NULL)
        return;

    g_rec_mutex_lock(&manager->control_lock);
    audio_manager_detach_call_streams(manager, TRUE);

    if (manager->backend != NULL &&
        manager->backend_ops != NULL &&
        manager->backend_ops->destroy != NULL)
        manager->backend_ops->destroy(manager->backend);
    manager->backend = NULL;
    g_rec_mutex_unlock(&manager->control_lock);

    if (manager->config != NULL)
        g_key_file_unref(manager->config);

    if (manager->main_context != NULL)
        g_main_context_unref(manager->main_context);

    g_rec_mutex_clear(&manager->control_lock);
    g_free(manager);
}

const gchar *
audio_manager_get_backend_name(AudioManager *manager)
{
    const gchar *name;

    if (manager == NULL)
        return NULL;

    g_rec_mutex_lock(&manager->control_lock);
    name = manager->backend_ops != NULL ? manager->backend_ops->name : NULL;
    g_rec_mutex_unlock(&manager->control_lock);
    return name;
}

gint
audio_manager_get_capabilities(AudioManager *manager,
                               AudioManagerCapabilities *capabilities)
{
    AudioManagerCapabilities full = AUDIO_MANAGER_CAPABILITIES_INIT;
    gint ret;

    if (manager == NULL || capabilities == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(capabilities, AudioManagerCapabilities, flags))
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->get_capabilities == NULL) {
        ret = -ENOTSUP;
    } else {
        ret = manager->backend_ops->get_capabilities(manager->backend, &full);
        if (ret == 0)
            AUDIO_MANAGER_STRUCT_COPY_OUT(capabilities, &full, AudioManagerCapabilities);
    }
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

AudioManagerCallTransportMask
audio_manager_get_supported_cellular_call_transports(AudioManager *manager,
                                                     AudioManagerOutputDevice output,
                                                     AudioManagerInputDevice input)
{
    AudioManagerCallTransportMask transports = 0;

    if (manager == NULL)
        return 0;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops != NULL &&
        manager->backend_ops->call_transport_supported != NULL) {
        if (manager->backend_ops->call_transport_supported(manager->backend,
                                                           output,
                                                           input,
                                                           AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS))
            transports |= AUDIO_MANAGER_CALL_TRANSPORT_MASK(AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS);

        if (manager->backend_ops->call_transport_supported(manager->backend,
                                                           output,
                                                           input,
                                                           AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL))
            transports |= AUDIO_MANAGER_CALL_TRANSPORT_MASK(AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL);
    }
    g_rec_mutex_unlock(&manager->control_lock);
    return transports;
}

gint
audio_manager_set_output_device(AudioManager *manager,
                                AudioManagerOutputDevice device)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->call_active) {
        ret = audio_manager_set_devices(manager, device, manager->input_device);
        goto out;
    }
    if (manager->backend_ops == NULL || manager->backend_ops->set_output_device == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->set_output_device(manager->backend, device);
    if (ret == 0)
        audio_manager_sync_devices(manager);

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

AudioManagerOutputDevice
audio_manager_get_output_device(AudioManager *manager)
{
    AudioManagerOutputDevice device;

    if (manager == NULL)
        return AUDIO_MANAGER_OUTPUT_NONE;
    g_rec_mutex_lock(&manager->control_lock);
    device = manager->output_device;
    g_rec_mutex_unlock(&manager->control_lock);
    return device;
}

gint
audio_manager_call_set_route(AudioManager *manager,
                             AudioManagerOutputDevice output,
                             AudioManagerInputDevice input,
                             AudioManagerCallTransport transport)
{
    gint volume_ret;
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (!manager->call_active) {
        ret = -ENOTCONN;
        goto out;
    }
    if (manager->backend_ops == NULL ||
        manager->backend_ops->call_set_route == NULL ||
        manager->backend_ops->call_transport_supported == NULL) {
        ret = -ENOTSUP;
        goto out;
    }
    if (!manager->backend_ops->call_transport_supported(manager->backend,
                                                        output,
                                                        input,
                                                        transport)) {
        ret = -ENOTSUP;
        goto out;
    }

    if ((manager->call_streams[0] != NULL || manager->call_streams[1] != NULL) &&
        (output != manager->output_device || input != manager->input_device ||
         transport != manager->call_transport)) {
        ret = -EBUSY;
        goto out;
    }

    ret = manager->backend_ops->call_set_route(manager->backend,
                                               output,
                                               input,
                                               transport);
    if (ret < 0)
        goto out;

    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        manager->backend_ops->call_set_volume != NULL) {
        volume_ret = manager->backend_ops->call_set_volume(manager->backend,
                                                           manager->call_volume);
        if (volume_ret < 0 && volume_ret != -ENOTSUP)
            g_warning("failed to apply call volume after route change: %s",
                      g_strerror(-volume_ret));
    }

    manager->call_transport = transport;
    audio_manager_sync_devices(manager);

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_set_devices(AudioManager *manager,
                          AudioManagerOutputDevice output,
                          AudioManagerInputDevice input)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    if (manager->call_active) {
        ret = audio_manager_call_set_route(manager,
                                           output,
                                           input,
                                           manager->call_transport);
        goto out;
    }

    if (manager->backend_ops->set_devices == NULL) {
        if (manager->backend_ops->set_output_device == NULL ||
            manager->backend_ops->set_input_device == NULL) {
            ret = -ENOTSUP;
            goto out;
        }
        ret = manager->backend_ops->set_output_device(manager->backend, output);
        if (ret < 0)
            goto out;
        ret = manager->backend_ops->set_input_device(manager->backend, input);
    } else {
        ret = manager->backend_ops->set_devices(manager->backend, output, input);
    }
    if (ret == 0)
        audio_manager_sync_devices(manager);

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_set_input_device(AudioManager *manager,
                               AudioManagerInputDevice device)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->call_active) {
        ret = audio_manager_set_devices(manager, manager->output_device, device);
        goto out;
    }
    if (manager->backend_ops == NULL || manager->backend_ops->set_input_device == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->set_input_device(manager->backend, device);
    if (ret == 0)
        audio_manager_sync_devices(manager);

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

AudioManagerInputDevice
audio_manager_get_input_device(AudioManager *manager)
{
    AudioManagerInputDevice device;

    if (manager == NULL)
        return AUDIO_MANAGER_INPUT_NONE;
    g_rec_mutex_lock(&manager->control_lock);
    device = manager->input_device;
    g_rec_mutex_unlock(&manager->control_lock);
    return device;
}

AudioManagerStream *
audio_manager_stream_open(AudioManager *manager,
                          const AudioManagerStreamConfig *config,
                          GError **error)
{
    AudioManagerStreamConfig normalized = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerStream *stream;

    if (manager == NULL || config == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, channels)) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "An initialized stream configuration is required");
        return NULL;
    }
    if (config->rate == 0 || config->channels == 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Stream rate and channel count must be non zero");
        return NULL;
    }

    normalized.direction = config->direction;
    normalized.format = config->format;
    normalized.rate = config->rate;
    normalized.channels = config->channels;
    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, period_size))
        normalized.period_size = config->period_size;
    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, period_count))
        normalized.period_count = config->period_count;
    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, playback_role))
        normalized.playback_role = config->playback_role;
    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, capture_role))
        normalized.capture_role = config->capture_role;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->stream_open == NULL) {
        g_rec_mutex_unlock(&manager->control_lock);
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_NOT_SUPPORTED,
                    "Audio backend does not support host PCM streams");
        return NULL;
    }

    stream = g_new0(AudioManagerStream, 1);
    stream->manager = manager;
    stream->config = normalized;
    g_mutex_init(&stream->lock);
    stream->backend_stream = manager->backend_ops->stream_open(manager->backend,
                                                               &normalized,
                                                               error);
    if (stream->backend_stream == NULL) {
        g_mutex_clear(&stream->lock);
        g_free(stream);
        g_rec_mutex_unlock(&manager->control_lock);
        return NULL;
    }
    if (manager->backend_ops->stream_get_config != NULL) {
        AudioManagerStreamConfig actual = AUDIO_MANAGER_STREAM_CONFIG_INIT;
        if (manager->backend_ops->stream_get_config(manager->backend,
                                                    stream->backend_stream,
                                                    &actual) == 0)
            stream->config = actual;
    }
    g_rec_mutex_unlock(&manager->control_lock);
    return stream;
}

gint
audio_manager_stream_start(AudioManagerStream *stream)
{
    gint ret;

    if (stream == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    if (stream->manager == NULL || stream->manager->backend_ops == NULL ||
        stream->manager->backend_ops->stream_start == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (stream->started) {
        ret = 0;
        goto out;
    }
    ret = stream->manager->backend_ops->stream_start(stream->manager->backend,
                                                     stream->backend_stream);
    if (ret == 0)
        stream->started = TRUE;

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gint
audio_manager_stream_stop(AudioManagerStream *stream)
{
    gint ret = 0;

    if (stream == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    if (!stream->started)
        goto out;
    if (stream->manager == NULL || stream->manager->backend_ops == NULL ||
        stream->manager->backend_ops->stream_stop == NULL) {
        ret = -ENODEV;
        goto out;
    }
    ret = stream->manager->backend_ops->stream_stop(stream->manager->backend,
                                                    stream->backend_stream);
    if (ret == 0)
        stream->started = FALSE;

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gint
audio_manager_stream_get_config(AudioManagerStream *stream,
                                AudioManagerStreamConfig *config)
{
    if (stream == NULL || config == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerStreamConfig, direction))
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    AUDIO_MANAGER_STRUCT_COPY_OUT(config, &stream->config, AudioManagerStreamConfig);
    g_mutex_unlock(&stream->lock);
    return 0;
}

gint
audio_manager_stream_get_delay(AudioManagerStream *stream,
                               gint64 *delay_frames)
{
    AudioManager *manager;
    gint ret;

    if (stream == NULL || delay_frames == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (manager->backend_ops == NULL ||
        manager->backend_ops->stream_get_delay == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->stream_get_delay(manager->backend,
                                                 stream->backend_stream,
                                                 delay_frames);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gint
audio_manager_stream_get_timestamp(AudioManagerStream *stream,
                                   guint64 *timestamp_nsec,
                                   guint64 *available_frames)
{
    AudioManager *manager;
    gint ret;

    if (stream == NULL || timestamp_nsec == NULL || available_frames == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (manager->backend_ops == NULL ||
        manager->backend_ops->stream_get_timestamp == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->stream_get_timestamp(manager->backend,
                                                     stream->backend_stream,
                                                     timestamp_nsec,
                                                     available_frames);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gssize
audio_manager_stream_write(AudioManagerStream *stream,
                           gconstpointer data,
                           gsize bytes)
{
    AudioManager *manager;
    gssize ret;

    if (stream == NULL || data == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (stream->config.direction != AUDIO_MANAGER_STREAM_PLAYBACK) {
        ret = -EINVAL;
        goto out;
    }
    if (manager->backend_ops == NULL ||
        manager->backend_ops->stream_write == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->stream_write(manager->backend,
                                             stream->backend_stream,
                                             data,
                                             bytes);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gssize
audio_manager_stream_read(AudioManagerStream *stream,
                          gpointer data,
                          gsize bytes)
{
    AudioManager *manager;
    gssize ret;

    if (stream == NULL || data == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (stream->config.direction != AUDIO_MANAGER_STREAM_CAPTURE) {
        ret = -EINVAL;
        goto out;
    }
    if (manager->backend_ops == NULL ||
        manager->backend_ops->stream_read == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->stream_read(manager->backend,
                                            stream->backend_stream,
                                            data,
                                            bytes);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

void
audio_manager_stream_close(AudioManagerStream *stream)
{
    AudioManager *manager;

    if (stream == NULL)
        return;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager != NULL && manager->backend_ops != NULL) {
        if (stream->started && manager->backend_ops->stream_stop != NULL)
            manager->backend_ops->stream_stop(manager->backend,
                                              stream->backend_stream);
        if (manager->backend_ops->stream_close != NULL)
            manager->backend_ops->stream_close(manager->backend,
                                               stream->backend_stream);
    }
    stream->manager = NULL;
    stream->backend_stream = NULL;
    stream->started = FALSE;
    g_mutex_unlock(&stream->lock);
    g_mutex_clear(&stream->lock);
    g_free(stream);
}

static AudioManagerCallStream **
audio_manager_call_stream_slot(AudioManager *manager,
                               AudioManagerCallStreamDirection direction)
{
    if (manager == NULL)
        return NULL;

    switch (direction) {
    case AUDIO_MANAGER_CALL_STREAM_DOWNLINK:
        return &manager->call_streams[0];
    case AUDIO_MANAGER_CALL_STREAM_UPLINK:
        return &manager->call_streams[1];
    default:
        return NULL;
    }
}

static void
audio_manager_call_stream_detach(AudioManagerCallStream *stream,
                                 gboolean orphan)
{
    AudioManager *manager;
    gint ret;

    if (stream == NULL)
        return;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;

    if (stream->backend_stream != NULL && manager != NULL &&
        manager->backend_ops != NULL) {
        if (stream->started && manager->backend_ops->call_stream_stop != NULL) {
            ret = manager->backend_ops->call_stream_stop(manager->backend,
                                                         stream->backend_stream);
            if (ret < 0)
                g_warning("failed to stop cellular call stream during detach: %s",
                          g_strerror(-ret));
        }

        if (manager->backend_ops->call_stream_close != NULL)
            manager->backend_ops->call_stream_close(manager->backend,
                                                    stream->backend_stream);
    }

    stream->backend_stream = NULL;
    stream->started = FALSE;
    if (orphan)
        stream->manager = NULL;
    g_mutex_unlock(&stream->lock);
}

static void
audio_manager_detach_call_streams(AudioManager *manager,
                                  gboolean orphan)
{
    AudioManagerCallStream *stream;
    guint i;

    if (manager == NULL)
        return;

    for (i = 0; i < G_N_ELEMENTS(manager->call_streams); i++) {
        stream = manager->call_streams[i];
        manager->call_streams[i] = NULL;
        audio_manager_call_stream_detach(stream, orphan);
    }
}

AudioManagerCallStream *
audio_manager_call_stream_open(AudioManager *manager,
                               AudioManagerCallStreamDirection direction,
                               GError **error)
{
    AudioManagerCallStream **slot;
    AudioManagerCallStream *stream = NULL;
    gint ret;

    if (manager == NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "An audio manager instance is required");
        return NULL;
    }

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL ||
        manager->backend_ops->call_stream_open == NULL ||
        manager->backend_ops->call_stream_get_config == NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_NOT_SUPPORTED,
                    "Audio backend does not expose cellular host PCM streams");
        goto out;
    }
    if (!manager->call_active) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "A cellular call must be active before opening a call PCM stream");
        goto out;
    }
    if (manager->call_transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "The selected cellular route does not expose host PCM streams");
        goto out;
    }

    slot = audio_manager_call_stream_slot(manager, direction);
    if (slot == NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown cellular call stream direction");
        goto out;
    }
    if (*slot != NULL) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "A cellular call stream for this direction is already open");
        goto out;
    }

    stream = g_new0(AudioManagerCallStream, 1);
    stream->manager = manager;
    stream->direction = direction;
    stream->config.struct_size = sizeof(stream->config);
    g_mutex_init(&stream->lock);

    stream->backend_stream = manager->backend_ops->call_stream_open(manager->backend,
                                                                    direction,
                                                                    error);
    if (stream->backend_stream == NULL)
        goto fail;

    ret = manager->backend_ops->call_stream_get_config(manager->backend,
                                                       stream->backend_stream,
                                                       &stream->config);
    if (ret < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to query cellular call stream configuration: %s",
                    g_strerror(-ret));
        manager->backend_ops->call_stream_close(manager->backend,
                                                stream->backend_stream);
        stream->backend_stream = NULL;
        goto fail;
    }

    if (stream->config.rate == 0 || stream->config.channels == 0 ||
        stream->config.period_size == 0 || stream->config.period_count == 0 ||
        stream->config.frame_bytes == 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Cellular call stream configuration is invalid");
        manager->backend_ops->call_stream_close(manager->backend,
                                                stream->backend_stream);
        stream->backend_stream = NULL;
        goto fail;
    }

    *slot = stream;
    goto out;

fail:
    g_mutex_clear(&stream->lock);
    g_free(stream);
    stream = NULL;

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return stream;
}

gint
audio_manager_call_stream_start(AudioManagerCallStream *stream)
{
    AudioManager *manager;
    gint ret;

    if (stream == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (!manager->call_active ||
        manager->call_transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        ret = -ENOTCONN;
        goto out;
    }
    if (stream->started) {
        ret = 0;
        goto out;
    }
    if (manager->backend_ops->call_stream_start == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->call_stream_start(manager->backend,
                                                  stream->backend_stream);
    if (ret == 0)
        stream->started = TRUE;

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gint
audio_manager_call_stream_stop(AudioManagerCallStream *stream)
{
    AudioManager *manager;
    gint ret = 0;

    if (stream == NULL)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (stream->backend_stream == NULL || !stream->started)
        goto out;
    if (manager == NULL || manager->backend_ops == NULL ||
        manager->backend_ops->call_stream_stop == NULL) {
        ret = -ENODEV;
        goto out;
    }

    ret = manager->backend_ops->call_stream_stop(manager->backend,
                                                 stream->backend_stream);
    if (ret == 0)
        stream->started = FALSE;

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gint
audio_manager_call_stream_get_config(AudioManagerCallStream *stream,
                                     AudioManagerCallStreamConfig *config)
{
    if (stream == NULL || config == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerCallStreamConfig, direction))
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    AUDIO_MANAGER_STRUCT_COPY_OUT(config, &stream->config, AudioManagerCallStreamConfig);
    g_mutex_unlock(&stream->lock);
    return 0;
}

gssize
audio_manager_call_stream_read(AudioManagerCallStream *stream,
                               gpointer data,
                               gsize bytes)
{
    AudioManager *manager;
    gssize ret;

    if (stream == NULL || data == NULL ||
        stream->direction != AUDIO_MANAGER_CALL_STREAM_DOWNLINK)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (!stream->started || !manager->call_active ||
        manager->call_transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        ret = -ENOTCONN;
        goto out;
    }
    if (bytes == 0 || bytes % stream->config.frame_bytes != 0) {
        ret = -EINVAL;
        goto out;
    }
    if (manager->backend_ops->call_stream_read == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->call_stream_read(manager->backend,
                                                 stream->backend_stream,
                                                 data,
                                                 bytes);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

gssize
audio_manager_call_stream_write(AudioManagerCallStream *stream,
                                gconstpointer data,
                                gsize bytes)
{
    AudioManager *manager;
    gssize ret;

    if (stream == NULL || data == NULL ||
        stream->direction != AUDIO_MANAGER_CALL_STREAM_UPLINK)
        return -EINVAL;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (manager == NULL || stream->backend_stream == NULL) {
        ret = -ENODEV;
        goto out;
    }
    if (!stream->started || !manager->call_active ||
        manager->call_transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        ret = -ENOTCONN;
        goto out;
    }
    if (bytes == 0 || bytes % stream->config.frame_bytes != 0) {
        ret = -EINVAL;
        goto out;
    }
    if (manager->backend_ops->call_stream_write == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->call_stream_write(manager->backend,
                                                  stream->backend_stream,
                                                  data,
                                                  bytes);

out:
    g_mutex_unlock(&stream->lock);
    return ret;
}

void
audio_manager_call_stream_close(AudioManagerCallStream *stream)
{
    AudioManager *manager;
    AudioManagerCallStream **slot;
    gint ret;

    if (stream == NULL)
        return;

    g_mutex_lock(&stream->lock);
    manager = stream->manager;
    if (stream->backend_stream != NULL && manager != NULL &&
        manager->backend_ops != NULL) {
        if (stream->started && manager->backend_ops->call_stream_stop != NULL) {
            ret = manager->backend_ops->call_stream_stop(manager->backend,
                                                         stream->backend_stream);
            if (ret < 0)
                g_warning("failed to stop hostful call stream during close: %s",
                          g_strerror(-ret));
        }
        if (manager->backend_ops->call_stream_close != NULL)
            manager->backend_ops->call_stream_close(manager->backend,
                                                    stream->backend_stream);
    }
    stream->backend_stream = NULL;
    stream->started = FALSE;
    g_mutex_unlock(&stream->lock);

    if (manager != NULL) {
        g_rec_mutex_lock(&manager->control_lock);
        slot = audio_manager_call_stream_slot(manager, stream->direction);
        if (slot != NULL && *slot == stream)
            *slot = NULL;
        g_rec_mutex_unlock(&manager->control_lock);
    }

    g_mutex_clear(&stream->lock);
    g_free(stream);
}

gint
audio_manager_fm_start(AudioManager *manager)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    ret = manager->backend_ops != NULL && manager->backend_ops->fm_start != NULL ?
          manager->backend_ops->fm_start(manager->backend) : -ENOTSUP;
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_fm_stop(AudioManager *manager)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    ret = manager->backend_ops != NULL && manager->backend_ops->fm_stop != NULL ?
          manager->backend_ops->fm_stop(manager->backend) : -ENOTSUP;
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_loopback_start(AudioManager *manager)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    ret = manager->backend_ops != NULL && manager->backend_ops->loopback_start != NULL ?
          manager->backend_ops->loopback_start(manager->backend) : -ENOTSUP;
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_loopback_stop(AudioManager *manager)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    ret = manager->backend_ops != NULL && manager->backend_ops->loopback_stop != NULL ?
          manager->backend_ops->loopback_stop(manager->backend) : -ENOTSUP;
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_call_start(AudioManager *manager,
                         AudioManagerCallTransport transport)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL ||
        manager->backend_ops->call_start == NULL ||
        manager->backend_ops->call_transport_supported == NULL) {
        ret = -ENOTSUP;
        goto out;
    }
    if (manager->call_active) {
        ret = -EALREADY;
        goto out;
    }
    if (!manager->backend_ops->call_transport_supported(manager->backend,
                                                        manager->output_device,
                                                        manager->input_device,
                                                        transport)) {
        ret = -ENOTSUP;
        goto out;
    }

    ret = manager->backend_ops->call_start(manager->backend, transport);
    if (ret < 0)
        goto out;

    manager->call_active = TRUE;
    manager->call_transport = transport;
    audio_manager_sync_devices(manager);

    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        manager->backend_ops->call_set_volume != NULL) {
        gint volume_ret = manager->backend_ops->call_set_volume(manager->backend,
                                                                manager->call_volume);
        if (volume_ret < 0 && volume_ret != -ENOTSUP)
            g_warning("failed to apply initial call volume: %s",
                      g_strerror(-volume_ret));
    }

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_call_stop(AudioManager *manager)
{
    AudioManagerCallState state = AUDIO_MANAGER_CALL_STATE_IDLE;
    gint ret;

    if (manager == NULL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->call_stop == NULL) {
        ret = -ENOTSUP;
        goto out;
    }

    if (!manager->call_active) {
        if (manager->backend_ops->call_get_state != NULL)
            state = manager->backend_ops->call_get_state(manager->backend);
        if (state == AUDIO_MANAGER_CALL_STATE_IDLE) {
            ret = 0;
            goto out;
        }
    }

    audio_manager_detach_call_streams(manager, FALSE);

    ret = manager->backend_ops->call_stop(manager->backend);
    if (ret == 0)
        manager->call_active = FALSE;
    else if (manager->backend_ops->call_get_state != NULL &&
             manager->backend_ops->call_get_state(manager->backend) == AUDIO_MANAGER_CALL_STATE_IDLE)
        manager->call_active = FALSE;

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gboolean
audio_manager_call_is_active(AudioManager *manager)
{
    gboolean active;

    if (manager == NULL)
        return FALSE;
    g_rec_mutex_lock(&manager->control_lock);
    active = manager->call_active;
    g_rec_mutex_unlock(&manager->control_lock);
    return active;
}

AudioManagerCallTransport
audio_manager_call_get_transport(AudioManager *manager)
{
    AudioManagerCallTransport transport;

    if (manager == NULL)
        return AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
    g_rec_mutex_lock(&manager->control_lock);
    transport = manager->call_transport;
    g_rec_mutex_unlock(&manager->control_lock);
    return transport;
}

AudioManagerCallState
audio_manager_call_get_state(AudioManager *manager)
{
    AudioManagerCallState state;

    if (manager == NULL)
        return AUDIO_MANAGER_CALL_STATE_IDLE;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops != NULL && manager->backend_ops->call_get_state != NULL)
        state = manager->backend_ops->call_get_state(manager->backend);
    else
        state = manager->call_active ?
                AUDIO_MANAGER_CALL_STATE_ACTIVE : AUDIO_MANAGER_CALL_STATE_IDLE;
    g_rec_mutex_unlock(&manager->control_lock);
    return state;
}

AudioManagerSpeechBand
audio_manager_call_get_band(AudioManager *manager)
{
    AudioManagerSpeechBand band;

    if (manager == NULL)
        return AUDIO_MANAGER_SPEECH_BAND_UNKNOWN;
    g_rec_mutex_lock(&manager->control_lock);
    band = manager->backend_ops != NULL && manager->backend_ops->call_get_band != NULL ?
           manager->backend_ops->call_get_band(manager->backend) :
           AUDIO_MANAGER_SPEECH_BAND_UNKNOWN;
    g_rec_mutex_unlock(&manager->control_lock);
    return band;
}

gint
audio_manager_call_set_bluetooth_config(AudioManager *manager,
                                        const AudioManagerBluetoothCallConfig *config)
{
    AudioManagerBluetoothCallConfig normalized = AUDIO_MANAGER_BLUETOOTH_CALL_CONFIG_INIT;
    gint ret;

    if (manager == NULL || config == NULL ||
        !AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerBluetoothCallConfig, codec))
        return -EINVAL;

    normalized.codec = config->codec;
    if (AUDIO_MANAGER_STRUCT_HAS_FIELD(config, AudioManagerBluetoothCallConfig, nrec))
        normalized.nrec = config->nrec;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL ||
        manager->backend_ops->call_set_bluetooth_config == NULL)
        ret = -ENOTSUP;
    else
        ret = manager->backend_ops->call_set_bluetooth_config(manager->backend,
                                                              &normalized);
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_call_set_volume(AudioManager *manager,
                              gdouble volume)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;

    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->call_set_volume == NULL) {
        ret = -ENOTSUP;
        goto out;
    }
    if (manager->call_active &&
        manager->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        ret = -ENOTSUP;
        goto out;
    }

    volume = audio_clamp_volume(volume);
    if (!manager->call_active) {
        manager->call_volume = volume;
        ret = 0;
        goto out;
    }

    ret = manager->backend_ops->call_set_volume(manager->backend, volume);
    if (ret == 0)
        manager->call_volume = volume;

out:
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gdouble
audio_manager_call_get_volume(AudioManager *manager)
{
    gdouble volume;

    if (manager == NULL)
        return 0.0;
    g_rec_mutex_lock(&manager->control_lock);
    volume = manager->call_volume;
    g_rec_mutex_unlock(&manager->control_lock);
    return volume;
}

gint
audio_manager_call_set_uplink_mute(AudioManager *manager,
                                   gboolean muted)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->call_set_uplink_mute == NULL) {
        ret = -ENOTSUP;
    } else {
        ret = manager->backend_ops->call_set_uplink_mute(manager->backend, muted);
        if (ret == 0)
            manager->uplink_muted = muted;
    }
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gint
audio_manager_call_set_downlink_mute(AudioManager *manager,
                                     gboolean muted)
{
    gint ret;

    if (manager == NULL)
        return -EINVAL;
    g_rec_mutex_lock(&manager->control_lock);
    if (manager->backend_ops == NULL || manager->backend_ops->call_set_downlink_mute == NULL) {
        ret = -ENOTSUP;
    } else {
        ret = manager->backend_ops->call_set_downlink_mute(manager->backend, muted);
        if (ret == 0)
            manager->downlink_muted = muted;
    }
    g_rec_mutex_unlock(&manager->control_lock);
    return ret;
}

gboolean
audio_manager_call_get_uplink_mute(AudioManager *manager)
{
    gboolean muted;

    if (manager == NULL)
        return FALSE;
    g_rec_mutex_lock(&manager->control_lock);
    muted = manager->uplink_muted;
    g_rec_mutex_unlock(&manager->control_lock);
    return muted;
}

gboolean
audio_manager_call_get_downlink_mute(AudioManager *manager)
{
    gboolean muted;

    if (manager == NULL)
        return FALSE;
    g_rec_mutex_lock(&manager->control_lock);
    muted = manager->downlink_muted;
    g_rec_mutex_unlock(&manager->control_lock);
    return muted;
}
