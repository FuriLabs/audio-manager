/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/backend.h"

#include <errno.h>
#include "backends/mtk/mtk-config.h"
#include "backends/mtk/params/mtk-speech-volume.h"
#include "backends/mtk/route/mtk-device-parser.h"
#include "backends/mtk/speech/mtk-speech.h"
#include "common/alsa/alsa-card.h"
#include "common/alsa/alsa-control.h"
#include "common/alsa/alsa-pcm.h"
#include "common/audio-device.h"
#include "common/audio-format.h"
#include "common/math-utils.h"

typedef struct {
    AudioAlsaPcm *pcm;
    AudioManagerStreamDirection direction;
    AudioManagerStreamConfig config;
    const gchar *route;
    const gchar *scenario_control;
    gboolean route_enabled;
    gboolean scenario_enabled;
    gboolean started;
} MtkHostStream;

typedef struct {
    AudioAlsaPcm *playback;
    AudioAlsaPcm *capture;
    const gchar *route;
    gboolean route_enabled;
} MtkHostlessDuplex;

typedef struct {
    AudioAlsaPcm *pcm;
    AudioManagerCallStreamDirection direction;
    AudioManagerCallStreamConfig config;
    const gchar *route;
    gboolean route_enabled;
} MtkCallStream;

struct AudioManagerBackend {
    MtkConfig *config;
    MtkDeviceConfig *device_config;
    AudioAlsaCard *card;
    MtkSpeechVolume *speech_volume;
    MtkSpeech *speech;

    AudioManagerOutputDevice output_device;
    AudioManagerInputDevice input_device;
    AudioManagerCallTransport call_transport;
    gdouble call_volume;
    guint playback_streams;
    guint capture_streams;
    AudioManagerCaptureRole active_capture_role;
    guint active_capture_channels;
    GHashTable *route_refs;
    GHashTable *scenario_refs;
    gboolean smart_pa_present;

    MtkHostlessDuplex *fm;
    MtkHostlessDuplex *loopback;
};

static AudioManagerCallState
mtk_backend_call_get_state(AudioManagerBackend *backend)
{
    return mtk_speech_get_state(backend->speech);
}

static gint
mtk_backend_call_set_volume(AudioManagerBackend *backend,
                            gdouble volume);

static gboolean
mtk_backend_call_transport_supported(AudioManagerBackend *backend,
                                     AudioManagerOutputDevice output,
                                     AudioManagerInputDevice input,
                                     AudioManagerCallTransport transport);

static gint
mtk_backend_call_set_route(AudioManagerBackend *backend,
                           AudioManagerOutputDevice output,
                           AudioManagerInputDevice input,
                           AudioManagerCallTransport transport);

static gint
mtk_hostless_duplex_close(AudioManagerBackend *backend,
                          MtkHostlessDuplex **duplex_ptr);

static gint
mtk_backend_recover_speech(MtkSpeech *speech,
                           gpointer user_data);

static void
mtk_backend_network_change(MtkSpeech *speech,
                           guint16 network_status,
                           gpointer user_data);

static void
log_smartpa_control(AudioAlsaCard *card,
                    const gchar *name)
{
    glong value;
    gint ret;

    if (name == NULL || *name == '\0')
        return;

    ret = audio_alsa_control_get_integer(card, name, &value);
    if (ret == 0)
        g_debug("MediaTek/Awinic SmartPA control '%s' value/index=%ld",
                name, value);
    else if (ret != -ENOENT)
        g_debug("MediaTek/Awinic SmartPA control '%s' read failed: %s",
                name, snd_strerror(ret));
}

static void
mtk_smartpa_log_state(const MtkConfig *config,
                      AudioAlsaCard *card)
{
    if (config == NULL || card == NULL)
        return;

    log_smartpa_control(card, config->smartpa_profile_control);
    log_smartpa_control(card, config->smartpa_monitor_control);
    log_smartpa_control(card, config->smartpa_vmax_control);
    log_smartpa_control(card, config->smartpa_spin_control);
}

static gboolean
mtk_smartpa_detect(const MtkConfig *config,
                   AudioAlsaCard *card)
{
    const gchar *controls[4];
    guint i;

    if (config == NULL || card == NULL)
        return FALSE;

    controls[0] = config->smartpa_profile_control;
    controls[1] = config->smartpa_monitor_control;
    controls[2] = config->smartpa_vmax_control;
    controls[3] = config->smartpa_spin_control;

    for (i = 0; i < G_N_ELEMENTS(controls); i++) {
        glong value;

        if (controls[i] == NULL || *controls[i] == '\0')
            continue;
        if (audio_alsa_control_get_integer(card, controls[i], &value) == 0)
            return TRUE;
    }

    return FALSE;
}

static gint
apply_media_output_gain(AudioManagerBackend *backend)
{
    if (backend == NULL)
        return -EINVAL;

    /*
     * Disable the analog speaker PGA when a SmartPA is present.
     * The external Awinic amplifier owns that stage, so writing PlaybackVolAna
     * speaker_pga is invalid on this hardware.
     */
    if (backend->smart_pa_present &&
        backend->output_device == AUDIO_MANAGER_OUTPUT_SPEAKER)
        return 0;

    return mtk_speech_volume_apply_media_playback_gain(backend->speech_volume,
                                                       backend->card,
                                                       backend->output_device);
}

static const gchar *
output_route(AudioManagerBackend *backend,
             AudioManagerOutputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return backend->config->receiver_output;
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return backend->config->speaker_output;
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return backend->config->headphones_output;
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return backend->config->headset_output;
    case AUDIO_MANAGER_OUTPUT_USB:
        return backend->config->usb_output;
    default:
        return NULL;
    }
}

static const gchar *
modem_downlink_route(AudioManagerBackend *backend,
                     AudioManagerOutputDevice device)
{
    if (device == AUDIO_MANAGER_OUTPUT_SPEAKER &&
        backend->config->speaker_modem_downlink != NULL &&
        *backend->config->speaker_modem_downlink != '\0')
        return backend->config->speaker_modem_downlink;

    return backend->config->modem_downlink;
}

static const gchar *
input_route(AudioManagerBackend *backend,
            AudioManagerInputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_INPUT_BUILTIN_MIC:
        return backend->config->builtin_mic_input;
    case AUDIO_MANAGER_INPUT_HEADSET_MIC:
        return backend->config->headset_mic_input;
    case AUDIO_MANAGER_INPUT_USB:
        return backend->config->usb_input;
    default:
        return NULL;
    }
}

static gint
apply_route(AudioManagerBackend *backend,
            const gchar *route,
            gboolean enabled)
{
    guint count;
    gint ret;

    if (route == NULL || *route == '\0')
        return 0;
    if (backend->route_refs == NULL)
        return audio_route_set_apply_list(backend->device_config->routes,
                                          backend->card,
                                          route,
                                          enabled ? "turnon" : "turnoff");

    count = GPOINTER_TO_UINT(g_hash_table_lookup(backend->route_refs, route));
    if (enabled) {
        if (count > 0) {
            g_hash_table_replace(backend->route_refs, g_strdup(route),
                                 GUINT_TO_POINTER(count + 1));
            return 0;
        }
        ret = audio_route_set_apply_list(backend->device_config->routes,
                                         backend->card, route, "turnon");
        if (ret < 0)
            return ret;
        g_hash_table_replace(backend->route_refs, g_strdup(route),
                             GUINT_TO_POINTER(1));
        return 0;
    }

    if (count == 0)
        return 0;
    if (count > 1) {
        g_hash_table_replace(backend->route_refs, g_strdup(route),
                             GUINT_TO_POINTER(count - 1));
        return 0;
    }
    ret = audio_route_set_apply_list(backend->device_config->routes,
                                     backend->card, route, "turnoff");
    if (ret < 0)
        return ret;
    g_hash_table_remove(backend->route_refs, route);
    return 0;
}

static gint
apply_playback_scenario(AudioManagerBackend *backend,
                        const gchar *control,
                        gboolean enabled)
{
    guint count;
    gint ret;

    if (control == NULL || *control == '\0')
        return 0;
    count = GPOINTER_TO_UINT(g_hash_table_lookup(backend->scenario_refs, control));
    if (enabled) {
        if (count > 0) {
            g_hash_table_replace(backend->scenario_refs, g_strdup(control),
                                 GUINT_TO_POINTER(count + 1));
            return 0;
        }
        ret = audio_alsa_control_set_index(backend->card, control, 1);
        if (ret < 0)
            return ret;
        g_hash_table_replace(backend->scenario_refs, g_strdup(control), GUINT_TO_POINTER(1));
        return 0;
    }
    if (count == 0)
        return 0;
    if (count > 1) {
        g_hash_table_replace(backend->scenario_refs, g_strdup(control),
                             GUINT_TO_POINTER(count - 1));
        return 0;
    }
    ret = audio_alsa_control_set_index(backend->card, control, 0);
    if (ret < 0)
        return ret;
    g_hash_table_remove(backend->scenario_refs, control);
    return 0;
}

static void
refresh_media_output_gain(AudioManagerBackend *backend)
{
    gint ret;
    if (backend->playback_streams == 0)
        return;
    ret = apply_media_output_gain(backend);
    if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP)
        g_warning("failed to refresh MediaTek media output gain: %s", g_strerror(-ret));
}

static void
refresh_capture_gain(AudioManagerBackend *backend)
{
    gint ret;
    if (backend->capture_streams == 0)
        return;
    ret = mtk_speech_volume_apply_capture_gain(backend->speech_volume,
                                               backend->card,
                                               backend->active_capture_role,
                                               backend->input_device,
                                               backend->config->speaker_gain_profile,
                                               backend->config->headset_gain_profile,
                                               backend->active_capture_channels);
    if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP)
        g_warning("failed to refresh MediaTek capture gain: %s", g_strerror(-ret));
}

static gint
set_output_device(AudioManagerBackend *backend,
                  AudioManagerOutputDevice device)
{
    AudioManagerOutputDevice old_device;
    const gchar *old_route;
    const gchar *new_route;
    const gchar *old_modem_downlink;
    const gchar *new_modem_downlink;
    gboolean active_hostless;
    gboolean modem_downlink_changed;
    gint ret;

    if (backend->output_device == device)
        return 0;

    /*
     * Bluetooth call audio is a paired hostless route. both the call input
     * and output must use Bluetooth together through the modem/CONNSYS speech
     * path. Don't allow this setter to move only the output into or out of Bluetooth
     * while speech is active.
     */
    active_hostless = mtk_speech_is_active(backend->speech) &&
                      backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
    if (active_hostless &&
        ((backend->output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH) !=
         (device == AUDIO_MANAGER_OUTPUT_BLUETOOTH)))
        return -ENOTSUP;

    old_device = backend->output_device;
    old_route = output_route(backend, old_device);
    new_route = output_route(backend, device);
    old_modem_downlink = modem_downlink_route(backend, old_device);
    new_modem_downlink = modem_downlink_route(backend, device);
    modem_downlink_changed = g_strcmp0(old_modem_downlink, new_modem_downlink) != 0;

    if (device != AUDIO_MANAGER_OUTPUT_NONE && new_route == NULL &&
        device != AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return -ENOTSUP;

    if (active_hostless &&
        device != AUDIO_MANAGER_OUTPUT_BLUETOOTH &&
        modem_downlink_changed) {
        ret = apply_route(backend, new_modem_downlink, TRUE);
        if (ret < 0)
            return ret;
        ret = apply_route(backend, old_modem_downlink, FALSE);
        if (ret < 0) {
            apply_route(backend, new_modem_downlink, FALSE);
            return ret;
        }
    }

    if (g_strcmp0(old_route, new_route) != 0) {
        ret = apply_route(backend, old_route, FALSE);
        if (ret < 0 && ret != -ENOENT)
            goto rollback_downlink;

        ret = apply_route(backend, new_route, TRUE);
        if (ret < 0) {
            apply_route(backend, old_route, TRUE);
            goto rollback_downlink;
        }
    }

    backend->output_device = device;

    if (device == AUDIO_MANAGER_OUTPUT_SPEAKER)
        mtk_smartpa_log_state(backend->config, backend->card);

    if (active_hostless) {
        ret = mtk_speech_device_change(backend->speech,
                                       device,
                                       backend->input_device);
        if (ret < 0) {
            if (g_strcmp0(old_route, new_route) != 0) {
                apply_route(backend, new_route, FALSE);
                apply_route(backend, old_route, TRUE);
            }
            backend->output_device = old_device;
            goto rollback_downlink;
        }

        ret = mtk_backend_call_set_volume(backend,
                                          backend->call_volume);
        if (ret < 0 && ret != -ENOTSUP)
            g_warning("failed to update call volume after route change: %s",
                      g_strerror(-ret));
    }

    refresh_media_output_gain(backend);
    return 0;

rollback_downlink:
    if (active_hostless &&
        device != AUDIO_MANAGER_OUTPUT_BLUETOOTH &&
        modem_downlink_changed) {
        apply_route(backend, new_modem_downlink, FALSE);
        apply_route(backend, old_modem_downlink, TRUE);
    }
    return ret;
}

static gint
set_input_device(AudioManagerBackend *backend,
                 AudioManagerInputDevice device)
{
    AudioManagerInputDevice old_device;
    const gchar *old_route;
    const gchar *new_route;
    gint ret;

    if (backend->input_device == device)
        return 0;

    /*
     * Bluetooth call audio is a paired hostless route: both the call input
     * and output must use Bluetooth together through the modem/CONNSYS speech
     * path. Don't allow this setter to move only the input into or out of Bluetooth
     * while speech is active.
     */
    if (mtk_speech_is_active(backend->speech) &&
        backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        ((backend->input_device == AUDIO_MANAGER_INPUT_BLUETOOTH) !=
         (device == AUDIO_MANAGER_INPUT_BLUETOOTH)))
        return -ENOTSUP;

    old_device = backend->input_device;
    old_route = input_route(backend, old_device);
    new_route = input_route(backend, device);

    if (device != AUDIO_MANAGER_INPUT_NONE && new_route == NULL &&
        device != AUDIO_MANAGER_INPUT_BLUETOOTH)
        return -ENOTSUP;

    if (g_strcmp0(old_route, new_route) != 0) {
        ret = apply_route(backend, old_route, FALSE);
        if (ret < 0 && ret != -ENOENT)
            return ret;

        ret = apply_route(backend, new_route, TRUE);
        if (ret < 0) {
            apply_route(backend, old_route, TRUE);
            return ret;
        }
    }

    backend->input_device = device;

    if (mtk_speech_is_active(backend->speech) &&
        backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
        ret = mtk_speech_device_change(backend->speech,
                                       backend->output_device,
                                       device);
        if (ret < 0) {
            if (g_strcmp0(old_route, new_route) != 0) {
                apply_route(backend, new_route, FALSE);
                apply_route(backend, old_route, TRUE);
            }
            backend->input_device = old_device;
            return ret;
        }
    }

    refresh_capture_gain(backend);
    return 0;
}

static gint
set_devices(AudioManagerBackend *backend,
            AudioManagerOutputDevice output,
            AudioManagerInputDevice input)
{
    AudioManagerOutputDevice old_output;
    AudioManagerInputDevice old_input;
    const gchar *old_output_route;
    const gchar *old_input_route;
    const gchar *new_output_route;
    const gchar *new_input_route;
    const gchar *old_modem_downlink;
    const gchar *new_modem_downlink;
    gboolean active_hostless;
    gboolean old_bt;
    gboolean new_bt;
    gboolean output_route_changed;
    gboolean input_route_changed;
    gboolean modem_downlink_changed;
    gboolean enabled_direct_routes = FALSE;
    gboolean switched_direct_route = FALSE;
    gint ret;

    if (backend == NULL)
        return -EINVAL;
    if (backend->output_device == output && backend->input_device == input)
        return 0;

    active_hostless = mtk_speech_is_active(backend->speech) &&
                      backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
    old_bt = backend->output_device == AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    new_bt = output == AUDIO_MANAGER_OUTPUT_BLUETOOTH;

    /*
     * Bluetooth and USB speech routes are duplex meaning their input and output
     * endpoints must move together rather than being mixed with a local side.
     */
    if (new_bt != (input == AUDIO_MANAGER_INPUT_BLUETOOTH))
        return -EINVAL;
    if ((output == AUDIO_MANAGER_OUTPUT_USB) !=
        (input == AUDIO_MANAGER_INPUT_USB))
        return -EINVAL;

    new_output_route = output_route(backend, output);
    new_input_route = input_route(backend, input);
    if (output != AUDIO_MANAGER_OUTPUT_NONE && new_output_route == NULL &&
        output != AUDIO_MANAGER_OUTPUT_BLUETOOTH &&
        output != AUDIO_MANAGER_OUTPUT_USB)
        return -ENOTSUP;
    if (input != AUDIO_MANAGER_INPUT_NONE && new_input_route == NULL &&
        input != AUDIO_MANAGER_INPUT_BLUETOOTH &&
        input != AUDIO_MANAGER_INPUT_USB)
        return -ENOTSUP;

    old_output = backend->output_device;
    old_input = backend->input_device;
    old_output_route = output_route(backend, old_output);
    old_input_route = input_route(backend, old_input);
    old_modem_downlink = modem_downlink_route(backend, old_output);
    new_modem_downlink = modem_downlink_route(backend, output);
    output_route_changed = g_strcmp0(old_output_route, new_output_route) != 0;
    input_route_changed = g_strcmp0(old_input_route, new_input_route) != 0;
    modem_downlink_changed = g_strcmp0(old_modem_downlink, new_modem_downlink) != 0;

    /*
     * Local call audio:
     *
     *       network
     *          |
     *         MD1
     *          |
     *       MTK AFE
     *          |
     *      output path
     *          |
     *   +------+------+------+
     *   |             |      |
     * Receiver     Speaker  Wired
     *
     * Bluetooth call audio:
     *
     *       network
     *          |
     *         MD1
     *          |
     *    Speech iface
     *          |
     *       CONNSYS
     *          |
     *     BT controller
     *          |
     *     BT headset
     */
    if (active_hostless && old_bt && !new_bt) {
        ret = apply_route(backend, backend->config->modem_uplink, TRUE);
        if (ret < 0)
            return ret;
        ret = apply_route(backend, new_modem_downlink, TRUE);
        if (ret < 0) {
            apply_route(backend, backend->config->modem_uplink, FALSE);
            return ret;
        }
        enabled_direct_routes = TRUE;
    } else if (active_hostless && !old_bt && !new_bt &&
               modem_downlink_changed) {
        ret = apply_route(backend, new_modem_downlink, TRUE);
        if (ret < 0)
            return ret;
        ret = apply_route(backend, old_modem_downlink, FALSE);
        if (ret < 0) {
            apply_route(backend, new_modem_downlink, FALSE);
            return ret;
        }
        switched_direct_route = TRUE;
    }

    if (input_route_changed) {
        ret = apply_route(backend, old_input_route, FALSE);
        if (ret < 0 && ret != -ENOENT)
            goto rollback_direct;
    }
    if (output_route_changed) {
        ret = apply_route(backend, old_output_route, FALSE);
        if (ret < 0 && ret != -ENOENT) {
            if (input_route_changed)
                apply_route(backend, old_input_route, TRUE);
            goto rollback_direct;
        }
        ret = apply_route(backend, new_output_route, TRUE);
        if (ret < 0) {
            apply_route(backend, old_output_route, TRUE);
            if (input_route_changed)
                apply_route(backend, old_input_route, TRUE);
            goto rollback_direct;
        }
    }
    if (input_route_changed) {
        ret = apply_route(backend, new_input_route, TRUE);
        if (ret < 0) {
            if (output_route_changed) {
                apply_route(backend, new_output_route, FALSE);
                apply_route(backend, old_output_route, TRUE);
            }
            apply_route(backend, old_input_route, TRUE);
            goto rollback_direct;
        }
    }

    backend->output_device = output;
    backend->input_device = input;

    if (active_hostless) {
        ret = mtk_speech_device_change(backend->speech, output, input);
        if (ret < 0) {
            if (input_route_changed) {
                apply_route(backend, new_input_route, FALSE);
                apply_route(backend, old_input_route, TRUE);
            }
            if (output_route_changed) {
                apply_route(backend, new_output_route, FALSE);
                apply_route(backend, old_output_route, TRUE);
            }
            backend->output_device = old_output;
            backend->input_device = old_input;
            goto rollback_direct;
        }
        if (!old_bt && new_bt) {
            apply_route(backend, old_modem_downlink, FALSE);
            apply_route(backend, backend->config->modem_uplink, FALSE);
        }
        ret = mtk_backend_call_set_volume(backend, backend->call_volume);
        if (ret < 0 && ret != -ENOTSUP)
            g_warning("failed to update call volume after device change: %s",
                      g_strerror(-ret));
    }

    if (output == AUDIO_MANAGER_OUTPUT_SPEAKER)
        mtk_smartpa_log_state(backend->config, backend->card);
    refresh_media_output_gain(backend);
    refresh_capture_gain(backend);
    return 0;

rollback_direct:
    if (switched_direct_route) {
        apply_route(backend, new_modem_downlink, FALSE);
        apply_route(backend, old_modem_downlink, TRUE);
    }
    if (enabled_direct_routes) {
        apply_route(backend, new_modem_downlink, FALSE);
        apply_route(backend, backend->config->modem_uplink, FALSE);
    }
    return ret;
}

static void
resolve_call_devices(AudioManagerBackend *backend,
                     AudioManagerOutputDevice *output,
                     AudioManagerInputDevice *input)
{
    if (*output == AUDIO_MANAGER_OUTPUT_BLUETOOTH &&
        *input == AUDIO_MANAGER_INPUT_NONE)
        *input = AUDIO_MANAGER_INPUT_BLUETOOTH;
    else if (*input == AUDIO_MANAGER_INPUT_BLUETOOTH &&
             *output == AUDIO_MANAGER_OUTPUT_NONE)
        *output = AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    else if (*output == AUDIO_MANAGER_OUTPUT_USB &&
             *input == AUDIO_MANAGER_INPUT_NONE)
        *input = AUDIO_MANAGER_INPUT_USB;
    else if (*input == AUDIO_MANAGER_INPUT_USB &&
             *output == AUDIO_MANAGER_OUTPUT_NONE)
        *output = AUDIO_MANAGER_OUTPUT_USB;

    if (*output == AUDIO_MANAGER_OUTPUT_NONE)
        *output = backend->config->default_call_output;
    if (*input == AUDIO_MANAGER_INPUT_NONE)
        *input = backend->config->default_call_input;
}

static gint
set_default_call_devices(AudioManagerBackend *backend)
{
    AudioManagerOutputDevice output = backend->output_device;
    AudioManagerInputDevice input = backend->input_device;

    resolve_call_devices(backend, &output, &input);
    return set_devices(backend, output, input);
}

static AudioManagerBackend *
mtk_backend_create(const AudioManagerBackendCreateInfo *info,
                   const gchar *device_config_path,
                   GError **error)
{
    AudioManagerBackend *backend;
    const gchar *card_name;
    gint ret;

    if (info == NULL || info->main_context == NULL)
        return NULL;

    backend = g_new0(AudioManagerBackend, 1);
    backend->call_volume = 1.0;
    backend->route_refs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    backend->scenario_refs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    backend->config = mtk_config_load(device_config_path, error);
    if (backend->config == NULL)
        goto fail;

    backend->device_config = mtk_device_config_load(backend->config->audio_device_config,
                                                    error);
    if (backend->device_config == NULL)
        goto fail;

    card_name = backend->config->card != NULL ?
                backend->config->card :
                backend->device_config->card_name;

    backend->card = audio_alsa_card_open(card_name, error);
    if (backend->card == NULL)
        goto fail;

    /* Apply top level MTK mixer defaults from audio device config */
    ret = audio_route_set_apply_optional(backend->device_config->routes,
                                         backend->card,
                                         "__init__",
                                         "setting");
    if (ret < 0 && ret != -ENOENT)
        g_warning("failed to apply MediaTek initial mixer state: %s",
                  snd_strerror(ret));

    mtk_smartpa_log_state(backend->config, backend->card);
    backend->smart_pa_present = mtk_smartpa_detect(backend->config, backend->card);
    if (backend->smart_pa_present)
        g_debug("MediaTek SmartPA detected, speaker analog PGA will not be configured");

    backend->speech_volume = mtk_speech_volume_new(backend->config->audio_param_directory, error);
    if (backend->speech_volume == NULL)
        goto fail;

    backend->speech = mtk_speech_new(backend->config,
                                     backend->card,
                                     info->main_context,
                                     mtk_backend_recover_speech,
                                     backend,
                                     mtk_backend_network_change,
                                     backend,
                                     error);
    if (backend->speech == NULL)
        goto fail;

    g_message("using MediaTek backend on ALSA card '%s' (%s)",
              backend->card->name,
              backend->config->generation);

    return backend;

fail:
    if (backend != NULL) {
        mtk_speech_free(backend->speech);
        mtk_speech_volume_free(backend->speech_volume);
        audio_alsa_card_close(backend->card);
        mtk_device_config_free(backend->device_config);
        mtk_config_free(backend->config);
        if (backend->route_refs != NULL)
            g_hash_table_unref(backend->route_refs);
        if (backend->scenario_refs != NULL)
            g_hash_table_unref(backend->scenario_refs);
        g_free(backend);
    }
    return NULL;
}

static void
mtk_backend_destroy(AudioManagerBackend *backend)
{
    if (backend == NULL)
        return;

    if (backend->speech != NULL && mtk_speech_is_active(backend->speech))
        mtk_speech_stop(backend->speech);

    mtk_hostless_duplex_close(backend, &backend->fm);
    mtk_hostless_duplex_close(backend, &backend->loopback);

    apply_route(backend, input_route(backend, backend->input_device), FALSE);
    apply_route(backend, output_route(backend, backend->output_device), FALSE);

    mtk_speech_free(backend->speech);
    mtk_speech_volume_free(backend->speech_volume);
    audio_alsa_card_close(backend->card);
    mtk_device_config_free(backend->device_config);
    mtk_config_free(backend->config);
    if (backend->route_refs != NULL)
        g_hash_table_unref(backend->route_refs);
    if (backend->scenario_refs != NULL)
        g_hash_table_unref(backend->scenario_refs);
    g_free(backend);
}

static gboolean
mtk_configured(const gchar *value)
{
    return value != NULL && *value != '\0';
}

static gint
mtk_backend_get_capabilities(AudioManagerBackend *backend,
                             AudioManagerCapabilities *capabilities)
{
    AudioManagerCapabilities result = AUDIO_MANAGER_CAPABILITIES_INIT;
    AudioManagerOutputDevice outputs[] = {
        AUDIO_MANAGER_OUTPUT_RECEIVER,
        AUDIO_MANAGER_OUTPUT_SPEAKER,
        AUDIO_MANAGER_OUTPUT_HEADPHONES,
        AUDIO_MANAGER_OUTPUT_HEADSET,
        AUDIO_MANAGER_OUTPUT_BLUETOOTH,
        AUDIO_MANAGER_OUTPUT_USB,
    };
    AudioManagerInputDevice inputs[] = {
        AUDIO_MANAGER_INPUT_BUILTIN_MIC,
        AUDIO_MANAGER_INPUT_HEADSET_MIC,
        AUDIO_MANAGER_INPUT_BLUETOOTH,
        AUDIO_MANAGER_INPUT_USB,
    };
    guint i;

    if (backend == NULL || capabilities == NULL)
        return -EINVAL;

    result.flags = AUDIO_MANAGER_CAP_STREAMS |
                   AUDIO_MANAGER_CAP_STREAM_TIMING |
                   AUDIO_MANAGER_CAP_ATOMIC_DEVICE_SWITCH |
                   AUDIO_MANAGER_CAP_CAPTURE_ROLES |
                   AUDIO_MANAGER_CAP_CALL_VOLUME |
                   AUDIO_MANAGER_CAP_CALL_UPLINK_MUTE |
                   AUDIO_MANAGER_CAP_CALL_DOWNLINK_MUTE;

    if (mtk_configured(backend->config->fast_playback_pcm))
        result.flags |= AUDIO_MANAGER_CAP_LOW_LATENCY_PLAYBACK;
    if (mtk_configured(backend->config->deep_buffer_playback_pcm))
        result.flags |= AUDIO_MANAGER_CAP_POWER_SAVING_PLAYBACK;
    if (backend->config->bluetooth_ext_device >= 0 &&
        backend->config->bluetooth_ext_device <= G_MAXUINT16)
        result.flags |= AUDIO_MANAGER_CAP_BLUETOOTH_CALL_CONFIG;
    if (mtk_configured(backend->config->hostless_fm_pcm) &&
        mtk_configured(backend->config->fm_route))
        result.flags |= AUDIO_MANAGER_CAP_HOSTLESS_FM;
    if (mtk_configured(backend->config->hostless_loopback_pcm) &&
        mtk_configured(backend->config->loopback_route))
        result.flags |= AUDIO_MANAGER_CAP_HOSTLESS_LOOPBACK;

    if (mtk_configured(backend->config->receiver_output))
        result.output_devices |= AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_RECEIVER);
    if (mtk_configured(backend->config->speaker_output))
        result.output_devices |= AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_SPEAKER);
    if (mtk_configured(backend->config->headphones_output))
        result.output_devices |= AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_HEADPHONES);
    if (mtk_configured(backend->config->headset_output))
        result.output_devices |= AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_HEADSET);

    if (mtk_configured(backend->config->builtin_mic_input))
        result.input_devices |= AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_BUILTIN_MIC);
    if (mtk_configured(backend->config->headset_mic_input))
        result.input_devices |= AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_HEADSET_MIC);

    for (i = 0; i < G_N_ELEMENTS(outputs); i++) {
        AudioManagerOutputDevice output = outputs[i];
        AudioManagerInputDevice input;

        switch (output) {
        case AUDIO_MANAGER_OUTPUT_HEADSET:
            input = AUDIO_MANAGER_INPUT_HEADSET_MIC;
            break;
        case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
            input = AUDIO_MANAGER_INPUT_BLUETOOTH;
            break;
        case AUDIO_MANAGER_OUTPUT_USB:
            input = AUDIO_MANAGER_INPUT_USB;
            break;
        default:
            input = AUDIO_MANAGER_INPUT_BUILTIN_MIC;
            break;
        }

        if (mtk_backend_call_transport_supported(backend, output, input,
                                                 AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) ||
            mtk_backend_call_transport_supported(backend, output, input,
                                                 AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL))
            result.cellular_call_output_devices |= AUDIO_MANAGER_OUTPUT_DEVICE_MASK(output);
    }

    for (i = 0; i < G_N_ELEMENTS(inputs); i++) {
        AudioManagerInputDevice input = inputs[i];
        AudioManagerOutputDevice output;

        switch (input) {
        case AUDIO_MANAGER_INPUT_HEADSET_MIC:
            output = AUDIO_MANAGER_OUTPUT_HEADSET;
            break;
        case AUDIO_MANAGER_INPUT_BLUETOOTH:
            output = AUDIO_MANAGER_OUTPUT_BLUETOOTH;
            break;
        case AUDIO_MANAGER_INPUT_USB:
            output = AUDIO_MANAGER_OUTPUT_USB;
            break;
        default:
            output = AUDIO_MANAGER_OUTPUT_RECEIVER;
            break;
        }

        if (mtk_backend_call_transport_supported(backend, output, input,
                                                 AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) ||
            mtk_backend_call_transport_supported(backend, output, input,
                                                 AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL))
            result.cellular_call_input_devices |= AUDIO_MANAGER_INPUT_DEVICE_MASK(input);
    }

    if (result.cellular_call_output_devices != 0 &&
        result.cellular_call_input_devices != 0)
        result.flags |= AUDIO_MANAGER_CAP_CELLULAR_CALL;

    result.playback_roles = AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT) |
                            AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY);
    if (result.flags & AUDIO_MANAGER_CAP_LOW_LATENCY_PLAYBACK)
        result.playback_roles |= AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY);
    if (result.flags & AUDIO_MANAGER_CAP_POWER_SAVING_PLAYBACK)
        result.playback_roles |= AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING);

    result.capture_roles = AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT) |
                           AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER) |
                           AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION) |
                           AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED);
    if (result.flags & AUDIO_MANAGER_CAP_BLUETOOTH_CALL_CONFIG)
        result.bluetooth_call_codecs =
            AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MASK(AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_CVSD) |
            AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MASK(AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC);

    *capabilities = result;
    return 0;
}

static AudioManagerOutputDevice
mtk_backend_get_output_device(AudioManagerBackend *backend)
{
    return backend != NULL ? backend->output_device : AUDIO_MANAGER_OUTPUT_NONE;
}

static AudioManagerInputDevice
mtk_backend_get_input_device(AudioManagerBackend *backend)
{
    return backend != NULL ? backend->input_device : AUDIO_MANAGER_INPUT_NONE;
}

static gint
mtk_backend_set_output_device(AudioManagerBackend *backend,
                              AudioManagerOutputDevice device)
{
    return set_output_device(backend, device);
}

static gint
mtk_backend_set_input_device(AudioManagerBackend *backend,
                             AudioManagerInputDevice device)
{
    return set_input_device(backend, device);
}

static gint
mtk_backend_set_devices(AudioManagerBackend *backend,
                        AudioManagerOutputDevice output,
                        AudioManagerInputDevice input)
{
    return set_devices(backend, output, input);
}

static void
mtk_backend_network_change(MtkSpeech *speech,
                           guint16 network_status,
                           gpointer user_data)
{
    AudioManagerBackend *backend = user_data;
    gint ret;

    if (backend == NULL || speech == NULL || !mtk_speech_is_active(speech))
        return;
    if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return;

    ret = mtk_backend_call_set_volume(backend,
                                      backend->call_volume);
    if (ret < 0 && ret != -ENOTSUP)
        g_warning("failed to refresh call volume after MediaTek network status 0x%04x: %s",
                  network_status,
                  g_strerror(-ret));
}

static gint
mtk_backend_recover_speech(MtkSpeech *speech,
                           gpointer user_data)
{
    AudioManagerBackend *backend = user_data;
    AudioManagerOutputDevice speech_output;
    AudioManagerInputDevice speech_input;
    gint ret;

    if (backend == NULL || speech == NULL)
        return -EINVAL;

    g_message("reopening MediaTek phone call audio path after modem recovery");

    if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        backend->output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
        apply_route(backend, modem_downlink_route(backend, backend->output_device), FALSE);
        apply_route(backend, backend->config->modem_uplink, FALSE);

        ret = apply_route(backend, backend->config->modem_uplink, TRUE);
        if (ret < 0)
            return ret;

        ret = apply_route(backend, modem_downlink_route(backend, backend->output_device), TRUE);
        if (ret < 0) {
            apply_route(backend, backend->config->modem_uplink, FALSE);
            return ret;
        }

        speech_output = backend->output_device;
        speech_input = backend->input_device;
    } else if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        speech_output = AUDIO_MANAGER_OUTPUT_USB;
        speech_input = AUDIO_MANAGER_INPUT_USB;
    } else {
        speech_output = AUDIO_MANAGER_OUTPUT_BLUETOOTH;
        speech_input = AUDIO_MANAGER_INPUT_BLUETOOTH;
    }

    ret = mtk_speech_recover(speech, speech_output, speech_input);
    if (ret < 0) {
        if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
            backend->output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
            apply_route(backend, modem_downlink_route(backend, backend->output_device), FALSE);
            apply_route(backend, backend->config->modem_uplink, FALSE);
        }
        return ret;
    }

    if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
        ret = mtk_backend_call_set_volume(backend, backend->call_volume);
        if (ret < 0 && ret != -ENOTSUP)
            g_warning("failed to restore call volume after modem recovery: %s",
                      g_strerror(-ret));
    }

    return 0;
}

static gboolean
valid_playback_role(AudioManagerPlaybackRole role)
{
    return role >= AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT &&
           role <= AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING;
}

static gboolean
valid_capture_role(AudioManagerCaptureRole role)
{
    return role >= AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT &&
           role <= AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED;
}

static void
select_playback_path(AudioManagerBackend *backend,
                     AudioManagerPlaybackRole requested,
                     const gchar **device,
                     const gchar **route,
                     const gchar **scenario,
                     guint *period_size,
                     guint *period_count,
                     AudioManagerPlaybackRole *effective)
{
    if (!valid_playback_role(requested))
        requested = AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;

    switch (requested) {
    case AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY:
        if (backend->config->fast_playback_pcm != NULL &&
            backend->config->fast_playback_route != NULL) {
            *device = backend->config->fast_playback_pcm;
            *route = backend->config->fast_playback_route;
            *scenario = backend->config->fast_playback_scenario_control;
            *period_size = backend->config->fast_playback_period_size;
            *period_count = backend->config->fast_playback_period_count;
            *effective = requested;
            return;
        }
        break;
    case AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING:
        if (backend->config->deep_buffer_playback_pcm != NULL &&
            backend->config->deep_buffer_playback_route != NULL) {
            *device = backend->config->deep_buffer_playback_pcm;
            *route = backend->config->deep_buffer_playback_route;
            *scenario = backend->config->deep_buffer_playback_scenario_control;
            *period_size = backend->config->deep_buffer_playback_period_size;
            *period_count = backend->config->deep_buffer_playback_period_count;
            *effective = requested;
            return;
        }
        break;
    case AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY:
    case AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT:
    default:
        break;
    }

    *device = backend->config->playback_pcm;
    *route = backend->config->playback_route;
    *scenario = backend->config->playback_scenario_control;
    *period_size = backend->config->playback_period_size;
    *period_count = backend->config->playback_period_count;
    *effective = requested == AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY ?
                 AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY :
                 AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;
}

static gpointer
mtk_backend_stream_open(AudioManagerBackend *backend,
                        const AudioManagerStreamConfig *config,
                        GError **error)
{
    AudioAlsaPcmConfig pcm_config = { 0 };
    MtkHostStream *stream;
    const gchar *device;
    const gchar *route;
    const gchar *scenario;
    AudioFormat format;
    guint period_size;
    guint period_count;
    guint rate;
    guint channels;
    gint ret;

    if (backend == NULL || config == NULL)
        return NULL;

    format = audio_format_from_sample_format(config->format);
    if (format == AUDIO_FORMAT_UNKNOWN) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Unsupported host stream sample format");
        return NULL;
    }

    stream = g_new0(MtkHostStream, 1);
    stream->direction = config->direction;
    stream->config = *config;
    rate = config->rate;
    channels = config->channels;

    if (config->direction == AUDIO_MANAGER_STREAM_PLAYBACK) {
        AudioManagerPlaybackRole effective;
        select_playback_path(backend, config->playback_role,
                             &device, &route, &scenario,
                             &period_size, &period_count, &effective);
        stream->config.playback_role = effective;
        stream->config.capture_role = AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
        if (config->period_size != 0)
            period_size = config->period_size;
        if (config->period_count != 0)
            period_count = config->period_count;
        pcm_config.direction = AUDIO_DIRECTION_OUTPUT;
    } else if (config->direction == AUDIO_MANAGER_STREAM_CAPTURE) {
        AudioManagerCaptureRole role = valid_capture_role(config->capture_role) ?
                                       config->capture_role :
                                       AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
        device = backend->config->capture_pcm;
        route = backend->config->capture_route;
        scenario = backend->config->capture_xrun_control;
        period_size = config->period_size != 0 ? config->period_size :
                      backend->config->capture_period_size;
        period_count = config->period_count != 0 ? config->period_count :
                       backend->config->capture_period_count;
        stream->config.capture_role = role;
        stream->config.playback_role = AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;

        rate = backend->config->capture_rate;
        channels = backend->config->capture_channels;
        if (role == AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED ||
            backend->input_device == AUDIO_MANAGER_INPUT_HEADSET_MIC)
            channels = 1;
        pcm_config.direction = AUDIO_DIRECTION_INPUT;
    } else {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "Unknown host stream direction");
        g_free(stream);
        return NULL;
    }

    if (device == NULL || route == NULL || period_size == 0 || period_count == 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                    "MediaTek host stream configuration is incomplete");
        g_free(stream);
        return NULL;
    }

    stream->route = route;
    stream->scenario_control = scenario;

    if (scenario != NULL) {
        ret = config->direction == AUDIO_MANAGER_STREAM_PLAYBACK ?
              apply_playback_scenario(backend, scenario, TRUE) :
              audio_alsa_control_set_index(backend->card, scenario, 0);
        if (ret < 0) {
            /*
             * Optional playback classes fall back to the primary path if the
             * kernel does not expose their scenario.
             */
            if (config->direction == AUDIO_MANAGER_STREAM_PLAYBACK &&
                stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT &&
                stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY) {
                g_debug("MediaTek playback role %d unsupported by scenario '%s', using primary",
                        stream->config.playback_role, scenario);
                select_playback_path(backend, AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT,
                                     &device, &route, &scenario,
                                     &period_size, &period_count,
                                     &stream->config.playback_role);
                stream->route = route;
                stream->scenario_control = scenario;
                if (config->period_size != 0)
                    period_size = config->period_size;
                if (config->period_count != 0)
                    period_count = config->period_count;
                ret = scenario != NULL ?
                      apply_playback_scenario(backend, scenario, TRUE) : 0;
            }
            if (ret < 0 && ret != -ENOENT) {
                g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                            "Unable to set MediaTek stream scenario '%s': %s",
                            scenario, snd_strerror(ret));
                g_free(stream);
                return NULL;
            }
        }
        stream->scenario_enabled = config->direction == AUDIO_MANAGER_STREAM_PLAYBACK && ret == 0;
    }

    ret = apply_route(backend, route, TRUE);
    if (ret < 0 && config->direction == AUDIO_MANAGER_STREAM_PLAYBACK &&
        stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT &&
        stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY) {
        if (stream->scenario_enabled && scenario != NULL)
            apply_playback_scenario(backend, scenario, FALSE);
        stream->scenario_enabled = FALSE;
        select_playback_path(backend, AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT,
                             &device, &route, &scenario,
                             &period_size, &period_count,
                             &stream->config.playback_role);
        stream->route = route;
        stream->scenario_control = scenario;
        if (config->period_size != 0)
            period_size = config->period_size;
        if (config->period_count != 0)
            period_count = config->period_count;
        if (scenario != NULL && apply_playback_scenario(backend, scenario, TRUE) == 0)
            stream->scenario_enabled = TRUE;
        ret = apply_route(backend, route, TRUE);
    }
    if (ret < 0) {
        if (stream->scenario_enabled)
            apply_playback_scenario(backend, scenario, FALSE);
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to enable MediaTek stream route '%s': %s",
                    route, snd_strerror(ret));
        g_free(stream);
        return NULL;
    }
    stream->route_enabled = TRUE;

    if (config->direction == AUDIO_MANAGER_STREAM_PLAYBACK) {
        ret = apply_media_output_gain(backend);
        if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP)
            g_warning("failed to apply MediaTek media output gain: %s", g_strerror(-ret));
    } else {
        ret = mtk_speech_volume_apply_capture_gain(backend->speech_volume,
                                                   backend->card,
                                                   stream->config.capture_role,
                                                   backend->input_device,
                                                   backend->config->speaker_gain_profile,
                                                   backend->config->headset_gain_profile,
                                                   channels);
        if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP)
            g_warning("failed to apply MediaTek capture gain: %s", g_strerror(-ret));
    }

    pcm_config.device = device;
    pcm_config.format = format;
    pcm_config.rate = rate;
    pcm_config.channels = channels;
    pcm_config.period_size = period_size;
    pcm_config.period_count = period_count;
    pcm_config.start_threshold = config->direction == AUDIO_MANAGER_STREAM_PLAYBACK ? period_size : 0;
    pcm_config.nonblock = FALSE;
    pcm_config.disable_stop_threshold = TRUE;

    if (config->direction == AUDIO_MANAGER_STREAM_PLAYBACK &&
        stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT &&
        stream->config.playback_role != AUDIO_MANAGER_PLAYBACK_ROLE_PRIMARY) {
        GError *role_error = NULL;

        stream->pcm = audio_alsa_pcm_open(backend->card, &pcm_config, &role_error);
        if (stream->pcm == NULL) {
            g_debug("MediaTek playback role %d PCM '%s' unavailable%s%s, using primary",
                    stream->config.playback_role, device,
                    role_error != NULL ? ": " : "",
                    role_error != NULL ? role_error->message : "");
            g_clear_error(&role_error);
            apply_route(backend, route, FALSE);
            if (stream->scenario_enabled && scenario != NULL)
                apply_playback_scenario(backend, scenario, FALSE);
            stream->scenario_enabled = FALSE;

            select_playback_path(backend, AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT,
                                 &device, &route, &scenario,
                                 &period_size, &period_count,
                                 &stream->config.playback_role);
            stream->route = route;
            stream->scenario_control = scenario;
            if (config->period_size != 0)
                period_size = config->period_size;
            if (config->period_count != 0)
                period_count = config->period_count;
            if (scenario != NULL && apply_playback_scenario(backend, scenario, TRUE) == 0)
                stream->scenario_enabled = TRUE;
            ret = apply_route(backend, route, TRUE);
            if (ret < 0) {
                if (stream->scenario_enabled && scenario != NULL)
                    apply_playback_scenario(backend, scenario, FALSE);
                g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                            "Unable to enable MediaTek primary stream route '%s': %s",
                            route, snd_strerror(ret));
                g_free(stream);
                return NULL;
            }
            pcm_config.device = device;
            pcm_config.period_size = period_size;
            pcm_config.period_count = period_count;
            pcm_config.start_threshold = period_size;
            stream->pcm = audio_alsa_pcm_open(backend->card, &pcm_config, error);
        }
    } else {
        stream->pcm = audio_alsa_pcm_open(backend->card, &pcm_config, error);
    }
    if (stream->pcm == NULL) {
        apply_route(backend, stream->route, FALSE);
        if (stream->scenario_enabled && stream->scenario_control != NULL)
            apply_playback_scenario(backend, stream->scenario_control, FALSE);
        g_free(stream);
        return NULL;
    }

    stream->config.format = audio_format_to_sample_format(audio_alsa_pcm_format(stream->pcm));
    stream->config.rate = audio_alsa_pcm_rate(stream->pcm);
    stream->config.channels = audio_alsa_pcm_channels(stream->pcm);
    stream->config.period_size = audio_alsa_pcm_period_size(stream->pcm);
    stream->config.period_count = audio_alsa_pcm_period_count(stream->pcm);
    stream->config.buffer_size = audio_alsa_pcm_buffer_size(stream->pcm);
    stream->config.frame_bytes = audio_alsa_pcm_frame_bytes(stream->pcm);

    if (stream->direction == AUDIO_MANAGER_STREAM_PLAYBACK)
        backend->playback_streams++;
    else {
        backend->capture_streams++;
        backend->active_capture_role = stream->config.capture_role;
        backend->active_capture_channels = stream->config.channels;
    }

    g_debug("MediaTek host stream opened direction=%s pcm='%s' route='%s' role=%d "
            "rate=%u channels=%u format=%d period=%u count=%u buffer=%u",
            config->direction == AUDIO_MANAGER_STREAM_PLAYBACK ? "playback" : "capture",
            device, route,
            config->direction == AUDIO_MANAGER_STREAM_PLAYBACK ?
            stream->config.playback_role : stream->config.capture_role,
            stream->config.rate, stream->config.channels, stream->config.format,
            stream->config.period_size, stream->config.period_count,
            stream->config.buffer_size);
    return stream;
}

static gint
mtk_backend_stream_start(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkHostStream *stream = stream_data;
    gint ret;

    (void)backend;
    if (stream == NULL)
        return -EINVAL;
    if (stream->started)
        return 0;
    if (stream->direction == AUDIO_MANAGER_STREAM_PLAYBACK) {
        stream->started = TRUE;
        g_debug("MediaTek playback stream armed");
        return 0;
    }
    ret = audio_alsa_pcm_start(stream->pcm);
    if (ret < 0)
        return ret;
    stream->started = TRUE;
    return 0;
}

static gint
mtk_backend_stream_stop(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkHostStream *stream = stream_data;
    gint ret;
    (void)backend;
    if (stream == NULL)
        return -EINVAL;
    if (!stream->started)
        return 0;
    ret = audio_alsa_pcm_stop(stream->pcm);
    if (ret == 0)
        stream->started = FALSE;
    return ret;
}

static gint
mtk_backend_stream_get_config(AudioManagerBackend *backend, gpointer stream_data,
                              AudioManagerStreamConfig *config)
{
    MtkHostStream *stream = stream_data;
    (void)backend;
    if (stream == NULL || config == NULL)
        return -EINVAL;
    *config = stream->config;
    return 0;
}

static gint
mtk_backend_stream_get_delay(AudioManagerBackend *backend, gpointer stream_data,
                             gint64 *delay_frames)
{
    MtkHostStream *stream = stream_data;
    snd_pcm_sframes_t avail = 0;
    snd_pcm_sframes_t delay = 0;
    gint ret;
    (void)backend;
    if (stream == NULL || delay_frames == NULL)
        return -EINVAL;
    ret = audio_alsa_pcm_avail_delay(stream->pcm, &avail, &delay);
    if (ret < 0)
        return ret;
    *delay_frames = delay;
    return 0;
}

static gint
mtk_backend_stream_get_timestamp(AudioManagerBackend *backend, gpointer stream_data,
                                 guint64 *timestamp_nsec,
                                 guint64 *available_frames)
{
    MtkHostStream *stream = stream_data;
    snd_pcm_uframes_t avail = 0;
    struct timespec ts = { 0 };
    gint ret;
    (void)backend;
    if (stream == NULL || timestamp_nsec == NULL || available_frames == NULL)
        return -EINVAL;
    ret = audio_alsa_pcm_get_htimestamp(stream->pcm, &avail, &ts);
    if (ret < 0)
        return ret;
    *timestamp_nsec = (guint64)ts.tv_sec * G_GUINT64_CONSTANT(1000000000) +
                      (guint64)ts.tv_nsec;
    *available_frames = (guint64)avail;
    return 0;
}

static gssize
mtk_backend_stream_write(AudioManagerBackend *backend, gpointer stream_data,
                         gconstpointer data, gsize bytes)
{
    MtkHostStream *stream = stream_data;
    (void)backend;
    if (stream == NULL || stream->direction != AUDIO_MANAGER_STREAM_PLAYBACK)
        return -EINVAL;
    return audio_alsa_pcm_write(stream->pcm, data, bytes);
}

static gssize
mtk_backend_stream_read(AudioManagerBackend *backend, gpointer stream_data,
                        gpointer data, gsize bytes)
{
    MtkHostStream *stream = stream_data;
    (void)backend;
    if (stream == NULL || stream->direction != AUDIO_MANAGER_STREAM_CAPTURE)
        return -EINVAL;
    return audio_alsa_pcm_read(stream->pcm, data, bytes);
}

static void
mtk_backend_stream_close(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkHostStream *stream = stream_data;

    if (stream == NULL)
        return;

    if (stream->started)
        audio_alsa_pcm_stop(stream->pcm);
    audio_alsa_pcm_close(stream->pcm);
    if (stream->route_enabled)
        apply_route(backend, stream->route, FALSE);
    if (stream->scenario_enabled && stream->scenario_control != NULL)
        apply_playback_scenario(backend, stream->scenario_control, FALSE);
    if (stream->direction == AUDIO_MANAGER_STREAM_CAPTURE &&
        stream->scenario_control != NULL)
        audio_alsa_control_set_index(backend->card, stream->scenario_control, 0);
    if (stream->direction == AUDIO_MANAGER_STREAM_PLAYBACK) {
        if (backend->playback_streams > 0)
            backend->playback_streams--;
    } else if (backend->capture_streams > 0) {
        backend->capture_streams--;
    }
    g_debug("MediaTek host stream closed direction=%s route='%s'",
            stream->direction == AUDIO_MANAGER_STREAM_PLAYBACK ? "playback" : "capture",
            stream->route != NULL ? stream->route : "");
    g_free(stream);
}

static const gchar *
call_stream_direction_name(AudioManagerCallStreamDirection direction)
{
    return direction == AUDIO_MANAGER_CALL_STREAM_DOWNLINK ? "downlink" : "uplink";
}

static gpointer
mtk_backend_call_stream_open(AudioManagerBackend *backend,
                             AudioManagerCallStreamDirection direction,
                             GError **error)
{
    AudioAlsaPcmConfig pcm_config = { 0 };
    const gchar *pcm_name;
    MtkCallStream *stream;
    gint ret;

    if (backend == NULL)
        return NULL;

    switch (direction) {
    case AUDIO_MANAGER_CALL_STREAM_DOWNLINK:
        pcm_name = backend->config->call_downlink_pcm;
        if (pcm_name == NULL || backend->config->call_downlink_route == NULL) {
            g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                        "MediaTek hostful call downlink configuration is incomplete");
            return NULL;
        }
        break;
    case AUDIO_MANAGER_CALL_STREAM_UPLINK:
        pcm_name = backend->config->call_uplink_pcm;
        if (pcm_name == NULL || backend->config->call_uplink_route == NULL) {
            g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_CONFIG,
                        "MediaTek hostful call uplink configuration is incomplete");
            return NULL;
        }
        break;
    default:
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_NOT_SUPPORTED,
                    "Unknown hostful call stream direction");
        return NULL;
    }

    stream = g_new0(MtkCallStream, 1);
    stream->direction = direction;
    stream->config.struct_size = sizeof(stream->config);
    stream->route = direction == AUDIO_MANAGER_CALL_STREAM_DOWNLINK ?
                    backend->config->call_downlink_route :
                    backend->config->call_uplink_route;

    ret = apply_route(backend, stream->route, TRUE);
    if (ret < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to enable MediaTek hostful call route '%s': %s",
                    stream->route, snd_strerror(ret));
        g_free(stream);
        return NULL;
    }
    stream->route_enabled = TRUE;

    pcm_config.device = pcm_name;
    pcm_config.direction = direction == AUDIO_MANAGER_CALL_STREAM_DOWNLINK ?
                           AUDIO_DIRECTION_INPUT : AUDIO_DIRECTION_OUTPUT;
    pcm_config.format = backend->config->call_hostful_format;
    pcm_config.rate = backend->config->call_hostful_rate;
    pcm_config.channels = backend->config->call_hostful_channels;
    pcm_config.period_size = backend->config->call_hostful_period_size;
    pcm_config.period_count = backend->config->call_hostful_period_count;
    if (direction == AUDIO_MANAGER_CALL_STREAM_UPLINK)
        pcm_config.start_threshold = pcm_config.period_size;
    pcm_config.nonblock = FALSE;
    pcm_config.disable_stop_threshold = TRUE;

    stream->pcm = audio_alsa_pcm_open(backend->card, &pcm_config, error);
    if (stream->pcm == NULL) {
        apply_route(backend, stream->route, FALSE);
        g_free(stream);
        return NULL;
    }

    stream->config.direction = direction;
    stream->config.format = audio_format_to_sample_format(audio_alsa_pcm_format(stream->pcm));
    stream->config.rate = audio_alsa_pcm_rate(stream->pcm);
    stream->config.channels = audio_alsa_pcm_channels(stream->pcm);
    stream->config.period_size = audio_alsa_pcm_period_size(stream->pcm);
    stream->config.period_count = audio_alsa_pcm_period_count(stream->pcm);
    stream->config.frame_bytes = audio_alsa_pcm_frame_bytes(stream->pcm);

    g_debug("MediaTek hostful call %s opened pcm='%s' route='%s' rate=%u channels=%u period=%u count=%u",
            call_stream_direction_name(direction), pcm_name, stream->route,
            pcm_config.rate, pcm_config.channels, pcm_config.period_size,
            pcm_config.period_count);
    return stream;
}

static gint
mtk_backend_call_stream_start(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkCallStream *stream = stream_data;
    (void)backend;
    if (stream == NULL)
        return -EINVAL;

    if (stream->direction == AUDIO_MANAGER_CALL_STREAM_UPLINK) {
        g_debug("MediaTek hostful call uplink armed");
        return 0;
    }

    return audio_alsa_pcm_start(stream->pcm);
}

static gint
mtk_backend_call_stream_stop(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkCallStream *stream = stream_data;
    (void)backend;
    if (stream == NULL)
        return -EINVAL;
    return audio_alsa_pcm_stop(stream->pcm);
}

static gint
mtk_backend_call_stream_get_config(AudioManagerBackend *backend,
                                   gpointer stream_data,
                                   AudioManagerCallStreamConfig *config)
{
    MtkCallStream *stream = stream_data;
    (void)backend;

    if (stream == NULL || config == NULL)
        return -EINVAL;

    *config = stream->config;
    return 0;
}

static gssize
mtk_backend_call_stream_read(AudioManagerBackend *backend, gpointer stream_data,
                             gpointer data, gsize bytes)
{
    MtkCallStream *stream = stream_data;
    (void)backend;
    if (stream == NULL || stream->direction != AUDIO_MANAGER_CALL_STREAM_DOWNLINK)
        return -EINVAL;
    return audio_alsa_pcm_read(stream->pcm, data, bytes);
}

static gssize
mtk_backend_call_stream_write(AudioManagerBackend *backend, gpointer stream_data,
                              gconstpointer data, gsize bytes)
{
    MtkCallStream *stream = stream_data;
    (void)backend;
    if (stream == NULL || stream->direction != AUDIO_MANAGER_CALL_STREAM_UPLINK)
        return -EINVAL;
    return audio_alsa_pcm_write(stream->pcm, data, bytes);
}

static void
mtk_backend_call_stream_close(AudioManagerBackend *backend, gpointer stream_data)
{
    MtkCallStream *stream = stream_data;

    if (stream == NULL)
        return;

    audio_alsa_pcm_close(stream->pcm);
    if (stream->route_enabled)
        apply_route(backend, stream->route, FALSE);
    g_debug("MediaTek hostful call %s closed route='%s'",
            call_stream_direction_name(stream->direction),
            stream->route != NULL ? stream->route : "");
    g_free(stream);
}

static MtkHostlessDuplex *
mtk_hostless_duplex_open(AudioManagerBackend *backend,
                         const gchar *pcm_name,
                         const gchar *route,
                         guint period_size,
                         GError **error)
{
    AudioAlsaPcmConfig config = { 0 };
    MtkHostlessDuplex *duplex;
    gint ret;

    if (backend == NULL || pcm_name == NULL || route == NULL)
        return NULL;

    duplex = g_new0(MtkHostlessDuplex, 1);
    duplex->route = route;

    ret = apply_route(backend, route, TRUE);
    if (ret < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to enable hostless route '%s': %s",
                    route, snd_strerror(ret));
        g_free(duplex);
        return NULL;
    }
    duplex->route_enabled = TRUE;

    config.device = pcm_name;
    config.format = backend->config->hostless_aux_format;
    config.rate = backend->config->hostless_aux_rate;
    config.channels = backend->config->hostless_aux_channels;
    config.period_size = period_size != 0 ? period_size :
                         backend->config->hostless_aux_period_size;
    config.period_count = backend->config->hostless_aux_period_count;
    config.disable_stop_threshold = TRUE;

    config.direction = AUDIO_DIRECTION_INPUT;
    duplex->capture = audio_alsa_pcm_open(backend->card, &config, error);
    if (duplex->capture == NULL)
        goto fail;

    config.direction = AUDIO_DIRECTION_OUTPUT;
    duplex->playback = audio_alsa_pcm_open(backend->card, &config, error);
    if (duplex->playback == NULL)
        goto fail;

    /* vendor hostless controllers start UL before DL. */
    ret = audio_alsa_pcm_start(duplex->capture);
    if (ret < 0)
        goto fail_ret;
    ret = audio_alsa_pcm_start(duplex->playback);
    if (ret < 0)
        goto fail_ret;

    g_debug("MediaTek hostless duplex started pcm='%s' route='%s' rate=%u channels=%u period=%u count=%u",
            pcm_name, route, config.rate, config.channels,
            config.period_size, config.period_count);
    return duplex;

fail_ret:
    g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                "Unable to start hostless PCM '%s': %s", pcm_name,
                snd_strerror(ret));
fail:
    if (duplex->playback != NULL)
        audio_alsa_pcm_close(duplex->playback);
    if (duplex->capture != NULL)
        audio_alsa_pcm_close(duplex->capture);
    if (duplex->route_enabled)
        apply_route(backend, route, FALSE);
    g_free(duplex);
    return NULL;
}

static gint
mtk_hostless_duplex_close(AudioManagerBackend *backend,
                          MtkHostlessDuplex **duplex_ptr)
{
    MtkHostlessDuplex *duplex;
    gint ret = 0;
    gint tmp;

    if (backend == NULL || duplex_ptr == NULL || *duplex_ptr == NULL)
        return 0;
    duplex = *duplex_ptr;

    if (duplex->playback != NULL) {
        tmp = audio_alsa_pcm_stop(duplex->playback);
        if (tmp < 0 && ret == 0)
            ret = tmp;
        audio_alsa_pcm_close(duplex->playback);
    }
    if (duplex->capture != NULL) {
        tmp = audio_alsa_pcm_stop(duplex->capture);
        if (tmp < 0 && ret == 0)
            ret = tmp;
        audio_alsa_pcm_close(duplex->capture);
    }
    if (duplex->route_enabled) {
        tmp = apply_route(backend, duplex->route, FALSE);
        if (tmp < 0 && ret == 0)
            ret = tmp;
    }
    g_debug("MediaTek hostless duplex stopped route='%s' ret=%d",
            duplex->route != NULL ? duplex->route : "", ret);
    g_free(duplex);
    *duplex_ptr = NULL;
    return ret;
}

static gint
mtk_backend_fm_start(AudioManagerBackend *backend)
{
    GError *error = NULL;

    if (backend->fm != NULL)
        return -EALREADY;
    backend->fm = mtk_hostless_duplex_open(backend,
                                           backend->config->hostless_fm_pcm,
                                           backend->config->fm_route,
                                           backend->config->fm_period_size,
                                           &error);
    if (backend->fm == NULL) {
        gint ret = -EIO;
        if (error != NULL) {
            g_warning("failed to start MediaTek FM hostless path: %s", error->message);
            g_error_free(error);
        }
        return ret;
    }
    return 0;
}

static gint
mtk_backend_fm_stop(AudioManagerBackend *backend)
{
    return mtk_hostless_duplex_close(backend, &backend->fm);
}

static gint
mtk_backend_loopback_start(AudioManagerBackend *backend)
{
    GError *error = NULL;

    if (backend->loopback != NULL)
        return -EALREADY;
    backend->loopback = mtk_hostless_duplex_open(backend,
                                                 backend->config->hostless_loopback_pcm,
                                                 backend->config->loopback_route,
                                                 backend->config->hostless_aux_period_size,
                                                 &error);
    if (backend->loopback == NULL) {
        if (error != NULL) {
            g_warning("failed to start MediaTek hostless loopback path: %s", error->message);
            g_error_free(error);
        }
        return -EIO;
    }
    return 0;
}

static gint
mtk_backend_loopback_stop(AudioManagerBackend *backend)
{
    return mtk_hostless_duplex_close(backend, &backend->loopback);
}

static gboolean
mtk_backend_call_pair_valid(AudioManagerOutputDevice output,
                            AudioManagerInputDevice input)
{
    if (output == AUDIO_MANAGER_OUTPUT_BLUETOOTH ||
        input == AUDIO_MANAGER_INPUT_BLUETOOTH)
        return output == AUDIO_MANAGER_OUTPUT_BLUETOOTH &&
               input == AUDIO_MANAGER_INPUT_BLUETOOTH;

    if (output == AUDIO_MANAGER_OUTPUT_USB || input == AUDIO_MANAGER_INPUT_USB)
        return output == AUDIO_MANAGER_OUTPUT_USB && input == AUDIO_MANAGER_INPUT_USB;

    return output != AUDIO_MANAGER_OUTPUT_NONE &&
           input != AUDIO_MANAGER_INPUT_NONE;
}

static gboolean
mtk_backend_hostful_available(AudioManagerBackend *backend)
{
    return backend != NULL &&
           mtk_configured(backend->config->call_downlink_pcm) &&
           mtk_configured(backend->config->call_downlink_route) &&
           mtk_configured(backend->config->call_uplink_pcm) &&
           mtk_configured(backend->config->call_uplink_route);
}

static gboolean
mtk_backend_call_transport_supported(AudioManagerBackend *backend,
                                     AudioManagerOutputDevice output,
                                     AudioManagerInputDevice input,
                                     AudioManagerCallTransport transport)
{
    if (backend == NULL)
        return FALSE;

    resolve_call_devices(backend, &output, &input);
    if (!mtk_backend_call_pair_valid(output, input))
        return FALSE;

    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL) {
        if (!mtk_backend_hostful_available(backend))
            return FALSE;
        if (output == AUDIO_MANAGER_OUTPUT_BLUETOOTH ||
            output == AUDIO_MANAGER_OUTPUT_USB)
            return TRUE;
        return output_route(backend, output) != NULL &&
               input_route(backend, input) != NULL;
    }
    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS)
        return FALSE;

    if (output == AUDIO_MANAGER_OUTPUT_USB)
        return FALSE;
    if (output == AUDIO_MANAGER_OUTPUT_BLUETOOTH)
        return backend->config->bluetooth_ext_device >= 0 &&
               backend->config->bluetooth_ext_device <= G_MAXUINT16;

    return output_route(backend, output) != NULL &&
           input_route(backend, input) != NULL;
}

static gint
mtk_backend_call_start(AudioManagerBackend *backend,
                       AudioManagerCallTransport transport)
{
    AudioManagerOutputDevice speech_output;
    AudioManagerInputDevice speech_input;
    gint ret;

    if (transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        transport != AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -EINVAL;

    ret = set_default_call_devices(backend);
    if (ret < 0)
        return ret;

    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
        /*
         * USB phone call audio is AP side and must use the hostful modem
         * PCM streams. Bluetooth is a hostless speech device and has no
         * direct MD1<->ADDA routes.
         */
        if (backend->output_device == AUDIO_MANAGER_OUTPUT_USB ||
            backend->input_device == AUDIO_MANAGER_INPUT_USB)
            return -ENOTSUP;

        if (backend->output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
            ret = apply_route(backend, backend->config->modem_uplink, TRUE);
            if (ret < 0)
                return ret;

            ret = apply_route(backend, modem_downlink_route(backend, backend->output_device), TRUE);
            if (ret < 0) {
                apply_route(backend, backend->config->modem_uplink, FALSE);
                return ret;
            }
        }

        speech_output = backend->output_device;
        speech_input = backend->input_device;
    } else {
        speech_output = AUDIO_MANAGER_OUTPUT_USB;
        speech_input = AUDIO_MANAGER_INPUT_USB;
    }

    ret = mtk_speech_start(backend->speech,
                           speech_output,
                           speech_input,
                           transport);
    if (ret < 0) {
        if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
            backend->output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
            apply_route(backend, modem_downlink_route(backend, backend->output_device), FALSE);
            apply_route(backend, backend->config->modem_uplink, FALSE);
        }
        return ret;
    }

    backend->call_transport = transport;

    g_debug("MediaTek cellular call started transport=%s",
            transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL ?
            "hostful" : "hostless");
    return 0;
}

static gint
mtk_backend_call_stop(AudioManagerBackend *backend)
{
    gint ret;
    gint tmp;

    ret = mtk_speech_stop(backend->speech);

    if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        backend->output_device != AUDIO_MANAGER_OUTPUT_BLUETOOTH) {
        tmp = apply_route(backend, modem_downlink_route(backend, backend->output_device), FALSE);
        if (tmp < 0 && ret == 0)
            ret = tmp;

        tmp = apply_route(backend, backend->config->modem_uplink, FALSE);
        if (tmp < 0 && ret == 0)
            ret = tmp;
    }

    return ret;
}

static gint
mtk_backend_call_set_route(AudioManagerBackend *backend,
                           AudioManagerOutputDevice output,
                           AudioManagerInputDevice input,
                           AudioManagerCallTransport transport)
{
    AudioManagerCallTransport old_transport;
    AudioManagerOutputDevice old_output;
    AudioManagerInputDevice old_input;
    const gchar *old_output_route;
    const gchar *old_input_route;
    const gchar *new_output_route;
    const gchar *new_input_route;
    const gchar *old_modem_downlink;
    const gchar *new_modem_downlink;
    gboolean old_direct;
    gboolean new_direct;
    gboolean modem_downlink_changed;
    gboolean switched_direct_route = FALSE;
    gint ret;

    if (backend == NULL)
        return -EINVAL;

    resolve_call_devices(backend, &output, &input);
    if (!mtk_backend_call_transport_supported(backend, output, input, transport))
        return -ENOTSUP;
    if (!mtk_speech_is_active(backend->speech))
        return -ENOTCONN;

    old_transport = backend->call_transport;
    old_output = backend->output_device;
    old_input = backend->input_device;
    if (old_transport == transport && old_output == output && old_input == input)
        return 0;

    old_output_route = output_route(backend, old_output);
    old_input_route = input_route(backend, old_input);
    new_output_route = output_route(backend, output);
    new_input_route = input_route(backend, input);
    old_modem_downlink = modem_downlink_route(backend, old_output);
    new_modem_downlink = modem_downlink_route(backend, output);
    old_direct = old_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
                 old_output != AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    new_direct = transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
                 output != AUDIO_MANAGER_OUTPUT_BLUETOOTH;
    modem_downlink_changed = g_strcmp0(old_modem_downlink, new_modem_downlink) != 0;

    if (new_direct && !old_direct) {
        ret = apply_route(backend, backend->config->modem_uplink, TRUE);
        if (ret < 0)
            return ret;
        ret = apply_route(backend, new_modem_downlink, TRUE);
        if (ret < 0) {
            apply_route(backend, backend->config->modem_uplink, FALSE);
            return ret;
        }
    } else if (old_direct && new_direct && modem_downlink_changed) {
        ret = apply_route(backend, new_modem_downlink, TRUE);
        if (ret < 0)
            return ret;
        ret = apply_route(backend, old_modem_downlink, FALSE);
        if (ret < 0) {
            apply_route(backend, new_modem_downlink, FALSE);
            return ret;
        }
        switched_direct_route = TRUE;
    }

    if (g_strcmp0(old_input_route, new_input_route) != 0) {
        ret = apply_route(backend, old_input_route, FALSE);
        if (ret < 0 && ret != -ENOENT)
            goto rollback_direct;
        ret = apply_route(backend, new_input_route, TRUE);
        if (ret < 0) {
            apply_route(backend, old_input_route, TRUE);
            goto rollback_direct;
        }
    }

    if (g_strcmp0(old_output_route, new_output_route) != 0) {
        ret = apply_route(backend, old_output_route, FALSE);
        if (ret < 0 && ret != -ENOENT)
            goto rollback_input;
        ret = apply_route(backend, new_output_route, TRUE);
        if (ret < 0) {
            apply_route(backend, old_output_route, TRUE);
            goto rollback_input;
        }
    }

    if (old_transport != transport) {
        ret = mtk_speech_set_transport(backend->speech,
                                       transport,
                                       transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL ?
                                           AUDIO_MANAGER_OUTPUT_USB : output,
                                       transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL ?
                                           AUDIO_MANAGER_INPUT_USB : input);
    } else if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
        ret = mtk_speech_device_change(backend->speech, output, input);
    } else {
        ret = 0;
    }
    if (ret < 0)
        goto rollback_output;

    backend->output_device = output;
    backend->input_device = input;
    backend->call_transport = transport;

    if (old_direct && !new_direct) {
        apply_route(backend, old_modem_downlink, FALSE);
        apply_route(backend, backend->config->modem_uplink, FALSE);
    }

    if (output == AUDIO_MANAGER_OUTPUT_SPEAKER)
        mtk_smartpa_log_state(backend->config, backend->card);
    refresh_media_output_gain(backend);
    refresh_capture_gain(backend);
    return 0;

rollback_output:
    if (g_strcmp0(old_output_route, new_output_route) != 0) {
        apply_route(backend, new_output_route, FALSE);
        apply_route(backend, old_output_route, TRUE);
    }
rollback_input:
    if (g_strcmp0(old_input_route, new_input_route) != 0) {
        apply_route(backend, new_input_route, FALSE);
        apply_route(backend, old_input_route, TRUE);
    }
rollback_direct:
    if (switched_direct_route) {
        apply_route(backend, new_modem_downlink, FALSE);
        apply_route(backend, old_modem_downlink, TRUE);
    } else if (new_direct && !old_direct) {
        apply_route(backend, new_modem_downlink, FALSE);
        apply_route(backend, backend->config->modem_uplink, FALSE);
    }
    return ret;
}

static gint
mtk_backend_call_set_bluetooth_config(AudioManagerBackend *backend,
                                      const AudioManagerBluetoothCallConfig *config)
{
    gboolean wideband;

    if (backend == NULL || config == NULL)
        return -EINVAL;

    switch (config->codec) {
    case AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_CVSD:
        wideband = FALSE;
        break;
    case AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC:
        wideband = TRUE;
        break;
    case AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_LC3_SWB:
        return -ENOTSUP;
    case AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_UNKNOWN:
    default:
        return -EINVAL;
    }

    return mtk_speech_set_bluetooth_parameters(backend->speech,
                                               wideband,
                                               config->nrec);
}

static const gchar *
call_gain_profile(AudioManagerBackend *backend)
{
    switch (backend->output_device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return backend->config->receiver_gain_profile;
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return backend->config->speaker_gain_profile;
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return backend->config->headphones_gain_profile;
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return backend->config->headset_gain_profile;
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return backend->config->bluetooth_gain_profile;
    case AUDIO_MANAGER_OUTPUT_USB:
        return backend->config->usb_gain_profile;
    default:
        return NULL;
    }
}

static const gchar *
call_volume_profile(AudioManagerBackend *backend)
{
    switch (backend->output_device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return backend->config->receiver_speech_volume_profile;
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return backend->config->speaker_speech_volume_profile;
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return backend->config->headphones_speech_volume_profile;
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return backend->config->headset_speech_volume_profile;
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return backend->config->bluetooth_speech_volume_profile;
    case AUDIO_MANAGER_OUTPUT_USB:
        return backend->config->usb_speech_volume_profile;
    default:
        return NULL;
    }
}

static gint
mtk_backend_apply_call_gain(AudioManagerBackend *backend,
                            const MtkSpeechGain *gain)
{
    gsize i;
    gint ret;

    if (gain->downlink_analog_control != NULL &&
        !(backend->smart_pa_present &&
          backend->output_device == AUDIO_MANAGER_OUTPUT_SPEAKER)) {
        ret = audio_alsa_control_set_index(backend->card,
                                           gain->downlink_analog_control,
                                           gain->downlink_analog_index);
        if (ret < 0)
            return ret;
    }

    ret = mtk_speech_set_downlink_gain(backend->speech,
                                       gain->modem_downlink_gain);
    if (ret < 0)
        return ret;

    if (gain->ui_index == 0)
        return 0;

    if (gain->update_parameter_volume_index) {
        ret = mtk_speech_set_parameter_volume_index(backend->speech,
                                                    gain->parameter_volume_index);
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < gain->uplink_pga_count; i++) {
        if (gain->uplink_pga_control[i] == NULL)
            continue;
        ret = audio_alsa_control_set_index(backend->card,
                                           gain->uplink_pga_control[i],
                                           gain->uplink_pga_index[i]);
        if (ret < 0)
            return ret;
    }

    ret = mtk_speech_set_uplink_gain(backend->speech,
                                     gain->modem_uplink_gain);
    if (ret < 0)
        return ret;

    ret = audio_alsa_control_set_index(backend->card,
                                       "Sidetone_Positive_Gain_dB",
                                       gain->sidetone_positive_gain_db);
    if (ret < 0 && ret != -ENOENT)
        return ret;

    ret = audio_alsa_control_set_index(backend->card,
                                       "Sidetone_Gain",
                                       gain->sidetone_gain);
    if (ret < 0 && ret != -ENOENT)
        return ret;

    return 0;
}

static gint
mtk_backend_call_set_volume(AudioManagerBackend *backend,
                            gdouble volume)
{
    MtkSpeechGain old_gain;
    MtkSpeechGain gain;
    const gchar *speech_profile;
    const gchar *gain_profile;
    gint rollback_ret;
    gint ret;

    if (backend->call_transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)
        return -ENOTSUP;

    speech_profile = call_volume_profile(backend);
    gain_profile = call_gain_profile(backend);
    if (speech_profile == NULL || gain_profile == NULL)
        return -ENOTSUP;

    ret = mtk_speech_volume_calculate(backend->speech_volume,
                                      mtk_speech_get_band(backend->speech),
                                      mtk_speech_get_network(backend->speech),
                                      speech_profile,
                                      gain_profile,
                                      backend->call_volume,
                                      &old_gain);
    if (ret < 0)
        return ret;

    ret = mtk_speech_volume_calculate(backend->speech_volume,
                                      mtk_speech_get_band(backend->speech),
                                      mtk_speech_get_network(backend->speech),
                                      speech_profile,
                                      gain_profile,
                                      volume,
                                      &gain);
    if (ret < 0)
        return ret;

    ret = mtk_backend_apply_call_gain(backend, &gain);
    if (ret < 0) {
        rollback_ret = mtk_backend_apply_call_gain(backend, &old_gain);
        if (rollback_ret < 0)
            g_warning("failed to restore previous MediaTek call gain after update failure: %s",
                      g_strerror(-rollback_ret));
        return ret;
    }

    backend->call_volume = volume;
    g_debug("MediaTek call gain applied volume=%.3f speech_profile=%s gain_profile=%s ui_index=%u",
            volume, speech_profile, gain_profile, gain.ui_index);
    return 0;
}

static gint
mtk_backend_call_set_uplink_mute(AudioManagerBackend *backend,
                                 gboolean muted)
{
    return mtk_speech_set_uplink_mute(backend->speech, muted);
}

static gint
mtk_backend_call_set_downlink_mute(AudioManagerBackend *backend,
                                   gboolean muted)
{
    return mtk_speech_set_downlink_mute(backend->speech, muted);
}

static AudioManagerSpeechBand
mtk_backend_call_get_band(AudioManagerBackend *backend)
{
    return mtk_speech_get_band(backend->speech);
}

static const AudioManagerBackendOps mtk_backend_ops = {
    .name = "mtk",
    .create = mtk_backend_create,
    .destroy = mtk_backend_destroy,
    .get_capabilities = mtk_backend_get_capabilities,
    .get_output_device = mtk_backend_get_output_device,
    .get_input_device = mtk_backend_get_input_device,
    .set_output_device = mtk_backend_set_output_device,
    .set_input_device = mtk_backend_set_input_device,
    .set_devices = mtk_backend_set_devices,
    .stream_open = mtk_backend_stream_open,
    .stream_start = mtk_backend_stream_start,
    .stream_stop = mtk_backend_stream_stop,
    .stream_get_config = mtk_backend_stream_get_config,
    .stream_get_delay = mtk_backend_stream_get_delay,
    .stream_get_timestamp = mtk_backend_stream_get_timestamp,
    .stream_write = mtk_backend_stream_write,
    .stream_read = mtk_backend_stream_read,
    .stream_close = mtk_backend_stream_close,
    .call_stream_open = mtk_backend_call_stream_open,
    .call_stream_start = mtk_backend_call_stream_start,
    .call_stream_stop = mtk_backend_call_stream_stop,
    .call_stream_get_config = mtk_backend_call_stream_get_config,
    .call_stream_read = mtk_backend_call_stream_read,
    .call_stream_write = mtk_backend_call_stream_write,
    .call_stream_close = mtk_backend_call_stream_close,
    .fm_start = mtk_backend_fm_start,
    .fm_stop = mtk_backend_fm_stop,
    .loopback_start = mtk_backend_loopback_start,
    .loopback_stop = mtk_backend_loopback_stop,
    .call_transport_supported = mtk_backend_call_transport_supported,
    .call_start = mtk_backend_call_start,
    .call_stop = mtk_backend_call_stop,
    .call_set_route = mtk_backend_call_set_route,
    .call_get_state = mtk_backend_call_get_state,
    .call_get_band = mtk_backend_call_get_band,
    .call_set_bluetooth_config = mtk_backend_call_set_bluetooth_config,
    .call_set_volume = mtk_backend_call_set_volume,
    .call_set_uplink_mute = mtk_backend_call_set_uplink_mute,
    .call_set_downlink_mute = mtk_backend_call_set_downlink_mute,
};

static void __attribute__((constructor))
mtk_audio_manager_register_backend(void)
{
    gint ret = audio_manager_backend_register(&mtk_backend_ops);
    if (ret < 0 && ret != -EEXIST)
        g_warning("failed to register MediaTek audio backend: %s", g_strerror(-ret));
}
