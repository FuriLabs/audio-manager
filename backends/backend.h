/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef BACKEND_H
#define BACKEND_H

#include <glib.h>

#include <audio-manager/audio-manager.h>

typedef struct AudioManagerBackend AudioManagerBackend;

/**
 * Core services provided to a backend when it is created.
 */
typedef struct {
    GMainContext *main_context; /**< Main context for backend I/O */
} AudioManagerBackendCreateInfo;

/**
 * Virtual function table implemented by audio hardware backends.
 *
 * Optional operations may be NULL.
 * Backends should report optional feature support through get_capabilities()
 * so clients do not need to probe by error.
 */
typedef struct AudioManagerBackendOps {
    const gchar *name; /**< Stable configuration name, for example "mtk" */

    /**
     * Create backend state.
     *
     * @param info Core services available to the backend
     * @param device_config_path Backend specific device configuration path
     * @param error Error return location
     * @return New backend instance or NULL on failure
     */
    AudioManagerBackend *(*create)(const AudioManagerBackendCreateInfo *info,
                                   const gchar *device_config_path,
                                   GError **error);

    /**
     * Destroy backend state.
     *
     * @param backend Backend instance
     */
    void (*destroy)(AudioManagerBackend *backend);

    /**
     * Query optional capabilities and supported masks.
     */
    gint (*get_capabilities)(AudioManagerBackend *backend,
                             AudioManagerCapabilities *capabilities);

    /**
     * Get the backend's currently selected output device.
     */
    AudioManagerOutputDevice (*get_output_device)(AudioManagerBackend *backend);

    /**
     * Get the backend's currently selected input device.
     */
    AudioManagerInputDevice (*get_input_device)(AudioManagerBackend *backend);

    /**
     * Select one physical output device.
     */
    gint (*set_output_device)(AudioManagerBackend *backend,
                              AudioManagerOutputDevice device);

    /**
     * Select one physical input device.
     */
    gint (*set_input_device)(AudioManagerBackend *backend,
                             AudioManagerInputDevice device);

    /**
     * Select input and output devices atomically when supported.
     */
    gint (*set_devices)(AudioManagerBackend *backend,
                        AudioManagerOutputDevice output,
                        AudioManagerInputDevice input);

    /**
     * Open a generic host PCM stream.
     */
    gpointer (*stream_open)(AudioManagerBackend *backend,
                            const AudioManagerStreamConfig *config,
                            GError **error);

    /**
     * Prepare/start a generic host PCM stream.
     */
    gint (*stream_start)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Stop a generic host PCM stream.
     */
    gint (*stream_stop)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Return negotiated generic stream parameters.
     */
    gint (*stream_get_config)(AudioManagerBackend *backend,
                              gpointer stream,
                              AudioManagerStreamConfig *config);

    /**
     * Return generic stream delay in frames.
     */
    gint (*stream_get_delay)(AudioManagerBackend *backend,
                             gpointer stream,
                             gint64 *delay_frames);

    /**
     * Return generic stream timestamp and available frame count.
     */
    gint (*stream_get_timestamp)(AudioManagerBackend *backend,
                                 gpointer stream,
                                 guint64 *timestamp_nsec,
                                 guint64 *available_frames);

    /**
     * Write PCM to a generic playback stream.
     */
    gssize (*stream_write)(AudioManagerBackend *backend,
                           gpointer stream,
                           gconstpointer data,
                           gsize bytes);

    /**
     * Read PCM from a generic capture stream.
     */
    gssize (*stream_read)(AudioManagerBackend *backend,
                          gpointer stream,
                          gpointer data,
                          gsize bytes);

    /**
     * Close a generic host PCM stream.
     */
    void (*stream_close)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Open raw modem PCM for one direction of a hostful call.
     */
    gpointer (*call_stream_open)(AudioManagerBackend *backend,
                                 AudioManagerCallStreamDirection direction,
                                 GError **error);

    /**
     * Start a hostful call PCM stream.
     */
    gint (*call_stream_start)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Stop a hostful call PCM stream.
     */
    gint (*call_stream_stop)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Return negotiated hostful call stream parameters.
     */
    gint (*call_stream_get_config)(AudioManagerBackend *backend,
                                   gpointer stream,
                                   AudioManagerCallStreamConfig *config);

    /**
     * Read modem downlink PCM.
     */
    gssize (*call_stream_read)(AudioManagerBackend *backend,
                               gpointer stream,
                               gpointer data,
                               gsize bytes);

    /**
     * Write modem uplink PCM.
     */
    gssize (*call_stream_write)(AudioManagerBackend *backend,
                                gpointer stream,
                                gconstpointer data,
                                gsize bytes);

    /**
     * Close a hostful call PCM stream.
     */
    void (*call_stream_close)(AudioManagerBackend *backend, gpointer stream);

    /**
     * Start hostless FM hardware routing.
     */
    gint (*fm_start)(AudioManagerBackend *backend);

    /**
     * Stop hostless FM hardware routing.
     */
    gint (*fm_stop)(AudioManagerBackend *backend);

    /**
     * Start hostless hardware loopback.
     */
    gint (*loopback_start)(AudioManagerBackend *backend);

    /**
     * Stop hostless hardware loopback.
     */
    gint (*loopback_stop)(AudioManagerBackend *backend);

    /**
     * Report whether a cellular route can use an explicitly requested transport.
     */
    gboolean (*call_transport_supported)(AudioManagerBackend *backend,
                                         AudioManagerOutputDevice output,
                                         AudioManagerInputDevice input,
                                         AudioManagerCallTransport transport);

    /**
     * Start vendor cellular speech using the transport requested by the client.
     */
    gint (*call_start)(AudioManagerBackend *backend,
                       AudioManagerCallTransport transport);

    /**
     * Stop vendor cellular speech.
     */
    gint (*call_stop)(AudioManagerBackend *backend);

    /**
     * Atomically change the active cellular route and selected transport.
     */
    gint (*call_set_route)(AudioManagerBackend *backend,
                           AudioManagerOutputDevice output,
                           AudioManagerInputDevice input,
                           AudioManagerCallTransport transport);

    /**
     * Get the current vendor speech engine state.
     */
    AudioManagerCallState (*call_get_state)(AudioManagerBackend *backend);

    /**
     * Get modem reported speech bandwidth.
     */
    AudioManagerSpeechBand (*call_get_band)(AudioManagerBackend *backend);

    /**
     * Configure backend Bluetooth cellular speech.
     */
    gint (*call_set_bluetooth_config)(AudioManagerBackend *backend,
                                      const AudioManagerBluetoothCallConfig *config);

    /**
     * Set backend cellular call volume.
     */
    gint (*call_set_volume)(AudioManagerBackend *backend,
                            gdouble volume);

    /**
     * Set modem side uplink mute.
     */
    gint (*call_set_uplink_mute)(AudioManagerBackend *backend,
                                 gboolean muted);

    /**
     * Set modem side downlink mute.
     */
    gint (*call_set_downlink_mute)(AudioManagerBackend *backend,
                                   gboolean muted);
} AudioManagerBackendOps;

/**
 * Register a backend implementation by name.
 *
 * Backends normally call this from their library constructor. Registered
 * operation tables must remain valid for the lifetime of the process.
 *
 * @param ops Backend operation table
 * @return 0 on success, -EEXIST for a duplicate name, or another negative errno value
 */
gint
audio_manager_backend_register(const AudioManagerBackendOps *ops);

/**
 * Look up a registered backend by configuration name.
 *
 * @param name Backend name
 * @return Registered backend operation table or NULL
 */
const AudioManagerBackendOps *
audio_manager_backend_find(const gchar *name);

#endif /* BACKEND_H */
