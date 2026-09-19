/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef AUDIO_MANAGER_TYPES_H
#define AUDIO_MANAGER_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * Public API version for the installed header set.
 */
#define AUDIO_MANAGER_API_VERSION 1U

/**
 * Physical output devices.
 */
typedef enum {
    AUDIO_MANAGER_OUTPUT_NONE = 0,     /**< No output device selected */
    AUDIO_MANAGER_OUTPUT_RECEIVER,     /**< Built-in earpiece receiver */
    AUDIO_MANAGER_OUTPUT_SPEAKER,      /**< Built-in loudspeaker */
    AUDIO_MANAGER_OUTPUT_HEADPHONES,   /**< Wired headphones without microphone */
    AUDIO_MANAGER_OUTPUT_HEADSET,      /**< Wired headset output */
    AUDIO_MANAGER_OUTPUT_BLUETOOTH,    /**< Bluetooth call output */
    AUDIO_MANAGER_OUTPUT_USB,          /**< USB call output or vendor USB route */
} AudioManagerOutputDevice;

/**
 * Physical input devices.
 */
typedef enum {
    AUDIO_MANAGER_INPUT_NONE = 0,          /**< No input device selected */
    AUDIO_MANAGER_INPUT_BUILTIN_MIC,       /**< Built-in microphone array */
    AUDIO_MANAGER_INPUT_HEADSET_MIC,       /**< Wired headset microphone */
    AUDIO_MANAGER_INPUT_BLUETOOTH,         /**< Bluetooth call input */
    AUDIO_MANAGER_INPUT_USB,               /**< USB call input or vendor USB route */
} AudioManagerInputDevice;

/**
 * Generic PCM stream direction.
 */
typedef enum {
    AUDIO_MANAGER_STREAM_PLAYBACK = 0, /**< Client writes PCM to hardware */
    AUDIO_MANAGER_STREAM_CAPTURE,      /**< Client reads PCM from hardware */
} AudioManagerStreamDirection;

/**
 * Playback behavior requested by a generic PCM consumer.
 *
 * LOW_LATENCY and POWER_SAVING are optional backend capabilities. Unsupported
 * or invalid roles fall back to the backend's default primary path.
 */
typedef enum {
    AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT = 0,       /**< Backend default playback path */
    AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY,           /**< Primary general purpose playback path */
    AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY,       /**< Low latency playback path */
    AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING,      /**< Larger buffer power saving playback path */
} AudioManagerPlaybackRole;

/**
 * Capture hardware/tuning role requested by a generic PCM consumer.
 *
 * These roles describe vendor hardware gain, routing, channel, and buffering behavior.
 */
typedef enum {
    AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT = 0,        /**< Normal sound recording */
    AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER,          /**< Camera/camcorder recording */
    AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION,  /**< Voice recognition capture */
    AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED,        /**< Least processed hardware capture path */
} AudioManagerCaptureRole;

/**
 * Direction of modem PCM exposed when the selected cellular route uses host PCM.
 */
typedef enum {
    AUDIO_MANAGER_CALL_STREAM_DOWNLINK = 0, /**< Modem/network audio read by the client */
    AUDIO_MANAGER_CALL_STREAM_UPLINK,       /**< Client PCM written toward the modem/network */
} AudioManagerCallStreamDirection;

/**
 * Cellular call transport requested by a client.
 *
 * Backends may support one or both transports for each cellular route.
 */
typedef enum {
    AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS = 0, /**< Vendor hardware routes call PCM without client I/O */
    AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL,      /**< Client reads/writes modem facing PCM */
} AudioManagerCallTransport;

/**
 * PCM sample formats.
 */
typedef enum {
    AUDIO_MANAGER_SAMPLE_S16_LE = 0, /**< Signed 16-bit little endian PCM */
    AUDIO_MANAGER_SAMPLE_S24_LE,     /**< Signed 24-bit little endian PCM */
    AUDIO_MANAGER_SAMPLE_S32_LE,     /**< Signed 32-bit little endian PCM */
} AudioManagerSampleFormat;

/**
 * Bluetooth speech codecs that a cellular backend may configure.
 */
typedef enum {
    AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_UNKNOWN = 0, /**< Unknown or unspecified codec */
    AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_CVSD,        /**< Narrowband CVSD */
    AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC,        /**< Wideband mSBC */
    AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_LC3_SWB,     /**< Super wideband LC3-SWB */
} AudioManagerBluetoothCallCodec;

/**
 * Speech bandwidth reported by the cellular modem.
 */
typedef enum {
    AUDIO_MANAGER_SPEECH_BAND_UNKNOWN = 0, /**< Unknown speech bandwidth */
    AUDIO_MANAGER_SPEECH_BAND_NARROW,      /**< Narrowband speech */
    AUDIO_MANAGER_SPEECH_BAND_WIDE,        /**< Wideband speech */
    AUDIO_MANAGER_SPEECH_BAND_SUPER_WIDE,  /**< Super wideband speech */
} AudioManagerSpeechBand;

/**
 * Current lifecycle state of the vendor cellular speech engine.
 */
typedef enum {
    AUDIO_MANAGER_CALL_STATE_IDLE = 0,          /**< No cellular speech session */
    AUDIO_MANAGER_CALL_STATE_PREPARING,         /**< Preparing routes and parameters */
    AUDIO_MANAGER_CALL_STATE_STARTING,          /**< Waiting for speech start acknowledgement */
    AUDIO_MANAGER_CALL_STATE_ACTIVE,            /**< Cellular speech session is active */
    AUDIO_MANAGER_CALL_STATE_DEVICE_CHANGING,   /**< Active call device/transport change in progress */
    AUDIO_MANAGER_CALL_STATE_STOPPING,          /**< Waiting for speech stop acknowledgement */
    AUDIO_MANAGER_CALL_STATE_ERROR,             /**< Speech engine entered an error state */
    AUDIO_MANAGER_CALL_STATE_RECOVERING,        /**< Recovering the vendor/modem speech path */
} AudioManagerCallState;

typedef guint64 AudioManagerCapabilityFlags;          /**< Set of AUDIO_MANAGER_CAP_* bits */
typedef guint64 AudioManagerOutputDeviceMask;         /**< Set of AudioManagerOutputDevice values */
typedef guint64 AudioManagerInputDeviceMask;          /**< Set of AudioManagerInputDevice values */
typedef guint64 AudioManagerPlaybackRoleMask;         /**< Set of AudioManagerPlaybackRole values */
typedef guint64 AudioManagerCaptureRoleMask;          /**< Set of AudioManagerCaptureRole values */
typedef guint64 AudioManagerBluetoothCallCodecMask;   /**< Set of AudioManagerBluetoothCallCodec values */
typedef guint64 AudioManagerCallTransportMask;        /**< Set of AudioManagerCallTransport values */

#define AUDIO_MANAGER_CAP_STREAMS                     (G_GUINT64_CONSTANT(1) << 0)  /**< Generic host PCM streams */
#define AUDIO_MANAGER_CAP_STREAM_TIMING               (G_GUINT64_CONSTANT(1) << 1)  /**< Stream delay and timestamp queries */
#define AUDIO_MANAGER_CAP_ATOMIC_DEVICE_SWITCH        (G_GUINT64_CONSTANT(1) << 2)  /**< Atomic input/output route switching */
#define AUDIO_MANAGER_CAP_CAPTURE_ROLES               (G_GUINT64_CONSTANT(1) << 3)  /**< Capture hardware role selection */
#define AUDIO_MANAGER_CAP_LOW_LATENCY_PLAYBACK        (G_GUINT64_CONSTANT(1) << 4)  /**< Low latency playback role */
#define AUDIO_MANAGER_CAP_POWER_SAVING_PLAYBACK       (G_GUINT64_CONSTANT(1) << 5)  /**< Power saving playback role */
#define AUDIO_MANAGER_CAP_CELLULAR_CALL               (G_GUINT64_CONSTANT(1) << 6)  /**< Cellular call routing */
#define AUDIO_MANAGER_CAP_CALL_VOLUME                 (G_GUINT64_CONSTANT(1) << 7)  /**< Vendor cellular call volume */
#define AUDIO_MANAGER_CAP_CALL_UPLINK_MUTE            (G_GUINT64_CONSTANT(1) << 8)  /**< Modem uplink mute */
#define AUDIO_MANAGER_CAP_CALL_DOWNLINK_MUTE          (G_GUINT64_CONSTANT(1) << 9)  /**< Modem downlink mute */
#define AUDIO_MANAGER_CAP_BLUETOOTH_CALL_CONFIG       (G_GUINT64_CONSTANT(1) << 10) /**< Bluetooth speech codec/NREC configuration */
#define AUDIO_MANAGER_CAP_HOSTLESS_FM                 (G_GUINT64_CONSTANT(1) << 11) /**< Hostless FM route */
#define AUDIO_MANAGER_CAP_HOSTLESS_LOOPBACK           (G_GUINT64_CONSTANT(1) << 12) /**< Hostless hardware loopback */

#define AUDIO_MANAGER_OUTPUT_DEVICE_MASK(device) (G_GUINT64_CONSTANT(1) << (guint)(device))
#define AUDIO_MANAGER_INPUT_DEVICE_MASK(device) (G_GUINT64_CONSTANT(1) << (guint)(device))
#define AUDIO_MANAGER_PLAYBACK_ROLE_MASK(role) (G_GUINT64_CONSTANT(1) << (guint)(role))
#define AUDIO_MANAGER_CAPTURE_ROLE_MASK(role) (G_GUINT64_CONSTANT(1) << (guint)(role))
#define AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MASK(codec) (G_GUINT64_CONSTANT(1) << (guint)(codec))
#define AUDIO_MANAGER_CALL_TRANSPORT_MASK(transport) (G_GUINT64_CONSTANT(1) << (guint)(transport))

/**
 * Capabilities and supported values reported by the active backend.
 *
 * Initialize with AUDIO_MANAGER_CAPABILITIES_INIT before passing this structure
 * to audio_manager_get_capabilities().
 */
typedef struct {
    gsize struct_size;                                      /**< Size of this structure supplied by the caller */
    AudioManagerCapabilityFlags flags;                      /**< AUDIO_MANAGER_CAP_* flags */
    AudioManagerOutputDeviceMask output_devices;            /**< Supported generic media output devices */
    AudioManagerInputDeviceMask input_devices;              /**< Supported generic media input devices */
    AudioManagerPlaybackRoleMask playback_roles;            /**< Supported playback roles */
    AudioManagerCaptureRoleMask capture_roles;              /**< Supported capture roles */
    AudioManagerBluetoothCallCodecMask bluetooth_call_codecs; /**< Supported Bluetooth call codecs */
    AudioManagerOutputDeviceMask cellular_call_output_devices; /**< Output devices supported for cellular calls */
    AudioManagerInputDeviceMask cellular_call_input_devices;   /**< Input devices supported for cellular calls */
} AudioManagerCapabilities;

#define AUDIO_MANAGER_CAPABILITIES_INIT { .struct_size = sizeof(AudioManagerCapabilities) }

/**
 * Configuration for a generic host PCM stream.
 *
 * period_size and period_count may be zero to use backend defaults. After
 * opening a stream, audio_manager_stream_get_config() reports the actual ALSA
 * or backend negotiated values.
 */
typedef struct {
    gsize struct_size;                         /**< Size of this structure supplied by the caller */
    AudioManagerStreamDirection direction;    /**< Playback or capture */
    AudioManagerSampleFormat format;          /**< Requested PCM format */
    guint rate;                               /**< Requested sample rate in Hz */
    guint channels;                           /**< Requested channel count */
    guint period_size;                        /**< Negotiated period size in frames, or 0 for default */
    guint period_count;                       /**< Negotiated period count, or 0 for default */
    AudioManagerPlaybackRole playback_role;   /**< Optional playback role */
    AudioManagerCaptureRole capture_role;     /**< Optional capture hardware role */
    guint buffer_size;                        /**< Negotiated total buffer size in frames */
    gsize frame_bytes;                        /**< Negotiated bytes per interleaved PCM frame */
} AudioManagerStreamConfig;

#define AUDIO_MANAGER_STREAM_CONFIG_INIT { .struct_size = sizeof(AudioManagerStreamConfig) }

/**
 * Negotiated configuration for a cellular call host PCM stream.
 */
typedef struct {
    gsize struct_size;                              /**< Size of this structure supplied by the caller */
    AudioManagerCallStreamDirection direction;     /**< Modem downlink or uplink */
    AudioManagerSampleFormat format;               /**< PCM sample format */
    guint rate;                                    /**< Sample rate in Hz */
    guint channels;                                /**< Channel count */
    guint period_size;                             /**< Period size in frames */
    guint period_count;                            /**< Number of periods */
    gsize frame_bytes;                             /**< Bytes per interleaved PCM frame */
} AudioManagerCallStreamConfig;

#define AUDIO_MANAGER_CALL_STREAM_CONFIG_INIT { .struct_size = sizeof(AudioManagerCallStreamConfig) }

/**
 * Bluetooth speech configuration for a cellular call route.
 */
typedef struct {
    gsize struct_size;                         /**< Size of this structure supplied by the caller */
    AudioManagerBluetoothCallCodec codec;     /**< Negotiated HFP speech codec */
    gboolean nrec;                            /**< TRUE when headset network echo reduction is enabled */
} AudioManagerBluetoothCallConfig;

#define AUDIO_MANAGER_BLUETOOTH_CALL_CONFIG_INIT { .struct_size = sizeof(AudioManagerBluetoothCallConfig) }

G_END_DECLS

#endif /* AUDIO_MANAGER_TYPES_H */
