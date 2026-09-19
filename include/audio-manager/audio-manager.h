/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <glib.h>

#include <audio-manager/audio-manager-error.h>
#include <audio-manager/audio-manager-types.h>

G_BEGIN_DECLS

typedef struct AudioManager AudioManager;
typedef struct AudioManagerStream AudioManagerStream;
typedef struct AudioManagerCallStream AudioManagerCallStream;

/**
 * Top level audio-manager configuration.
 *
 * Initialize with AUDIO_MANAGER_CONFIG_INIT before setting fields.
 */
typedef struct {
    gsize struct_size;          /**< Size of this structure supplied by the caller */
    const gchar *config_path;   /**< Path to the global audio-manager configuration file */
    GMainContext *main_context; /**< Main context used for asynchronous backend I/O, or NULL for default */
} AudioManagerConfig;

#define AUDIO_MANAGER_CONFIG_INIT { .struct_size = sizeof(AudioManagerConfig) }

/**
 * Create a new audio manager.
 *
 * @param config Initialized manager configuration
 * @param error Error return location
 * @return A new AudioManager instance or NULL on failure
 */
AudioManager *
audio_manager_new(const AudioManagerConfig *config,
                  GError **error);

/**
 * Free an audio manager and its backend resources.
 *
 * Callers must stop and close generic stream I/O before freeing the manager.
 * Outstanding cellular call PCM stream handles are orphaned safely, but blocked I/O
 * must still be quiesced by the caller.
 *
 * @param manager Manager instance to free
 */
void
audio_manager_free(AudioManager *manager);

/**
 * Get the registered name of the active backend.
 *
 * @param manager Manager instance
 * @return Backend name owned by the library, or NULL
 */
const gchar *
audio_manager_get_backend_name(AudioManager *manager);

/**
 * Query capabilities and supported device/role masks from the active backend.
 *
 * @param manager Manager instance
 * @param capabilities Initialized capabilities structure to fill
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_get_capabilities(AudioManager *manager,
                               AudioManagerCapabilities *capabilities);

/**
 * Get the cellular call transports supported for an input/output pair.
 *
 * Clients may use this to implement their own transport policy. For example,
 * a sound server may prefer hostless and fall back to hostful.
 *
 * @param manager Manager instance
 * @param output Cellular call output device
 * @param input Cellular call input device
 * @return Mask of AudioManagerCallTransport values, or 0 when unsupported
 */
AudioManagerCallTransportMask
audio_manager_get_supported_cellular_call_transports(AudioManager *manager,
                                                     AudioManagerOutputDevice output,
                                                     AudioManagerInputDevice input);

/**
 * Select the physical output device.
 *
 * @param manager Manager instance
 * @param device Output device to select
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_set_output_device(AudioManager *manager,
                                AudioManagerOutputDevice device);

/**
 * Get the currently selected physical output device.
 *
 * @param manager Manager instance
 * @return Current output device or AUDIO_MANAGER_OUTPUT_NONE
 */
AudioManagerOutputDevice
audio_manager_get_output_device(AudioManager *manager);

/**
 * Select input and output devices as one backend operation.
 *
 * This is preferred when both sides must change atomically. During an active
 * cellular call this preserves the currently selected transport. Clients that
 * also need to change transport must use audio_manager_call_set_route(). When
 * the active route exposes host PCM, callers must close its call stream handles
 * before changing the cellular route, otherwise this returns -EBUSY.
 *
 * @param manager Manager instance
 * @param output Output device to select
 * @param input Input device to select
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_set_devices(AudioManager *manager,
                          AudioManagerOutputDevice output,
                          AudioManagerInputDevice input);

/**
 * Select the physical input device.
 *
 * @param manager Manager instance
 * @param device Input device to select
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_set_input_device(AudioManager *manager,
                               AudioManagerInputDevice device);

/**
 * Get the currently selected physical input device.
 *
 * @param manager Manager instance
 * @return Current input device or AUDIO_MANAGER_INPUT_NONE
 */
AudioManagerInputDevice
audio_manager_get_input_device(AudioManager *manager);

/**
 * Open a generic host PCM stream.
 *
 * @param manager Manager instance
 * @param config Initialized stream configuration
 * @param error Error return location
 * @return New stream handle or NULL on failure
 */
AudioManagerStream *
audio_manager_stream_open(AudioManager *manager,
                          const AudioManagerStreamConfig *config,
                          GError **error);

/**
 * Prepare/start a generic PCM stream.
 *
 * Playback streams may remain armed until the first write so an empty hardware
 * buffer is not explicitly started.
 *
 * @param stream Stream handle
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_stream_start(AudioManagerStream *stream);

/**
 * Stop a generic PCM stream.
 *
 * @param stream Stream handle
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_stream_stop(AudioManagerStream *stream);

/**
 * Get the actual negotiated stream configuration.
 *
 * @param stream Stream handle
 * @param config Initialized configuration structure to fill
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_stream_get_config(AudioManagerStream *stream,
                                AudioManagerStreamConfig *config);

/**
 * Get the hardware/backend stream delay.
 *
 * @param stream Stream handle
 * @param delay_frames Return location for delay in PCM frames
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_stream_get_delay(AudioManagerStream *stream,
                               gint64 *delay_frames);

/**
 * Get a stream timestamp and currently available frame count.
 *
 * @param stream Stream handle
 * @param timestamp_nsec Return location for monotonic timestamp in nanoseconds
 * @param available_frames Return location for available frames
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_stream_get_timestamp(AudioManagerStream *stream,
                                   guint64 *timestamp_nsec,
                                   guint64 *available_frames);

/**
 * Write interleaved PCM to a playback stream.
 *
 * One I/O thread should own a stream's read/write operation. The caller must
 * quiesce blocked I/O before stop/close/free.
 *
 * @param stream Playback stream
 * @param data PCM buffer
 * @param bytes Number of bytes to write
 * @return Number of bytes written or a negative errno value
 */
gssize
audio_manager_stream_write(AudioManagerStream *stream,
                           gconstpointer data,
                           gsize bytes);

/**
 * Read interleaved PCM from a capture stream.
 *
 * One I/O thread should own a stream's read/write operation. The caller must
 * quiesce blocked I/O before stop/close/free.
 *
 * @param stream Capture stream
 * @param data Destination PCM buffer
 * @param bytes Number of bytes to read
 * @return Number of bytes read or a negative errno value
 */
gssize
audio_manager_stream_read(AudioManagerStream *stream,
                          gpointer data,
                          gsize bytes);

/**
 * Close a generic PCM stream.
 *
 * @param stream Stream handle to close
 */
void
audio_manager_stream_close(AudioManagerStream *stream);

/**
 * Open one direction of raw modem PCM for an active cellular call using host PCM.
 *
 * Only one stream per direction may be open at a time.
 *
 * @param manager Manager instance
 * @param direction Modem PCM direction
 * @param error Error return location
 * @return New call stream handle or NULL on failure
 */
AudioManagerCallStream *
audio_manager_call_stream_open(AudioManager *manager,
                               AudioManagerCallStreamDirection direction,
                               GError **error);

/**
 * Start a cellular host PCM stream.
 *
 * @param stream Call stream handle
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_stream_start(AudioManagerCallStream *stream);

/**
 * Stop a cellular host PCM stream.
 *
 * @param stream Call stream handle
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_stream_stop(AudioManagerCallStream *stream);

/**
 * Get the negotiated configuration of a cellular host PCM stream.
 *
 * @param stream Call stream handle
 * @param config Initialized configuration structure to fill
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_stream_get_config(AudioManagerCallStream *stream,
                                     AudioManagerCallStreamConfig *config);

/**
 * Read modem downlink PCM from a cellular host PCM stream.
 *
 * @param stream Downlink call stream
 * @param data Destination PCM buffer
 * @param bytes Number of bytes to read
 * @return Number of bytes read or a negative errno value
 */
gssize
audio_manager_call_stream_read(AudioManagerCallStream *stream,
                               gpointer data,
                               gsize bytes);

/**
 * Write modem uplink PCM to a cellular host PCM stream.
 *
 * @param stream Uplink call stream
 * @param data PCM buffer
 * @param bytes Number of bytes to write
 * @return Number of bytes written or a negative errno value
 */
gssize
audio_manager_call_stream_write(AudioManagerCallStream *stream,
                                gconstpointer data,
                                gsize bytes);

/**
 * Close a cellular host PCM stream handle.
 *
 * @param stream Call stream handle to close
 */
void
audio_manager_call_stream_close(AudioManagerCallStream *stream);

/**
 * Start the backend's hostless FM route.
 *
 * @param manager Manager instance
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_fm_start(AudioManager *manager);

/**
 * Stop the backend's hostless FM route.
 *
 * @param manager Manager instance
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_fm_stop(AudioManager *manager);

/**
 * Start the backend's hostless hardware loopback route.
 *
 * @param manager Manager instance
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_loopback_start(AudioManager *manager);

/**
 * Stop the backend's hostless hardware loopback route.
 *
 * @param manager Manager instance
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_loopback_stop(AudioManager *manager);

/**
 * Start cellular speech for the currently selected call devices using one
 * explicitly requested transport.
 *
 * The call fails with -ENOTSUP when the active backend does not support the
 * selected input/output pair with the requested transport. Audio Manager does
 * not fall back to another transport.
 *
 * @param manager Manager instance
 * @param transport Requested cellular transport
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_start(AudioManager *manager,
                         AudioManagerCallTransport transport);

/**
 * Change the route and/or transport of an active cellular call atomically.
 *
 * Host PCM call streams must be closed before changing away from their current
 * route or transport. The call fails with -ENOTSUP when the backend does not
 * support the exact requested combination.
 *
 * @param manager Manager instance
 * @param output Cellular call output device
 * @param input Cellular call input device
 * @param transport Requested cellular transport
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_set_route(AudioManager *manager,
                             AudioManagerOutputDevice output,
                             AudioManagerInputDevice input,
                             AudioManagerCallTransport transport);

/**
 * Stop the active cellular speech session.
 *
 * @param manager Manager instance
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_stop(AudioManager *manager);

/**
 * Check whether a cellular call is active from the core manager perspective.
 *
 * @param manager Manager instance
 * @return TRUE when a cellular speech session is active
 */
gboolean
audio_manager_call_is_active(AudioManager *manager);

/**
 * Get the currently selected cellular call transport.
 *
 * @param manager Manager instance
 * @return Current call transport
 */
AudioManagerCallTransport
audio_manager_call_get_transport(AudioManager *manager);

/**
 * Get the vendor speech engine state.
 *
 * @param manager Manager instance
 * @return Current call state
 */
AudioManagerCallState
audio_manager_call_get_state(AudioManager *manager);

/**
 * Get current modem speech bandwidth.
 *
 * @param manager Manager instance
 * @return Current speech band or AUDIO_MANAGER_SPEECH_BAND_UNKNOWN
 */
AudioManagerSpeechBand
audio_manager_call_get_band(AudioManager *manager);

/**
 * Configure Bluetooth speech for a cellular call.
 *
 * PulseAudio/BlueZ remains responsible for HFP/SCO connection policy. This
 * operation only supplies the negotiated speech codec/NREC state to the vendor
 * cellular audio backend.
 *
 * @param manager Manager instance
 * @param config Initialized Bluetooth call configuration
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_set_bluetooth_config(AudioManager *manager,
                                        const AudioManagerBluetoothCallConfig *config);

/**
 * Set vendor cellular call volume.
 *
 * Backends may implement this in hardware or modem speech control. When the
 * selected cellular route uses host PCM, audible volume is normally owned by
 * the sound server processing that PCM.
 *
 * @param manager Manager instance
 * @param volume Normalized volume in the range 0.0 to 1.0
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_set_volume(AudioManager *manager,
                              gdouble volume);

/**
 * Get the last requested cellular call volume.
 *
 * @param manager Manager instance
 * @return Normalized volume in the range 0.0 to 1.0
 */
gdouble
audio_manager_call_get_volume(AudioManager *manager);

/**
 * Set modem side cellular uplink mute.
 *
 * @param manager Manager instance
 * @param muted TRUE to mute audio sent to the remote party
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_set_uplink_mute(AudioManager *manager,
                                   gboolean muted);

/**
 * Set modem side cellular downlink mute.
 *
 * @param manager Manager instance
 * @param muted TRUE to mute audio received from the remote party
 * @return 0 on success or a negative errno value
 */
gint
audio_manager_call_set_downlink_mute(AudioManager *manager,
                                     gboolean muted);

/**
 * Get the last requested cellular uplink mute state.
 *
 * @param manager Manager instance
 * @return TRUE when uplink mute is enabled
 */
gboolean
audio_manager_call_get_uplink_mute(AudioManager *manager);

/**
 * Get the last requested cellular downlink mute state.
 *
 * @param manager Manager instance
 * @return TRUE when downlink mute is enabled
 */
gboolean
audio_manager_call_get_downlink_mute(AudioManager *manager);

G_END_DECLS

#endif /* AUDIO_MANAGER_H */
