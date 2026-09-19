/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <audio-manager/audio-manager.h>

#include "common/audio-device.h"
#include "common/audio-state.h"

#define FURIOS_CONFIG_PATH "/usr/lib/furios/device/audio-manager.conf"
#define SYSTEM_CONFIG_PATH "/etc/audio-manager/audio-manager.conf"
#define DEFAULT_CONFIG_PATH "/usr/share/audio-manager/audio-manager.conf"

typedef struct {
    AudioManager *manager;
    GMainLoop *loop;
    AudioManagerCallStream *call_downlink;
    AudioManagerCallStream *call_uplink;
} AudioManagerCtl;

static void
usage(const gchar *program)
{
    fprintf(stderr,
            "usage: %s [-c CONFIG] [COMMAND [ARG ...]]\n"
            "\n"
            "If COMMAND is omitted, an interactive shell is started.\n"
            "Without -c, configuration is searched in this order:\n"
            "  %s\n"
            "  %s\n"
            "  %s\n"
            "\n"
            "commands:\n"
            "  output receiver|speaker|headphones|headset|bluetooth|usb\n"
            "  input builtin-mic|headset-mic|bluetooth|usb\n"
            "  devices OUTPUT INPUT\n"
            "  call start hostless|hostful\n"
            "  call route OUTPUT INPUT hostless|hostful\n"
            "  call stop\n"
            "  call bluetooth narrow|wide nrec|no-nrec\n"
            "  call volume 0.0..1.0\n"
            "  call downlink start|stop\n"
            "  call downlink info\n"
            "  call downlink read BYTES\n"
            "  call uplink start|stop\n"
            "  call uplink info\n"
            "  call uplink tone MILLISECONDS\n"
            "  mute uplink on|off\n"
            "  mute downlink on|off\n"
            "  status\n"
            "  help\n"
            "  quit\n",
            program,
            FURIOS_CONFIG_PATH,
            SYSTEM_CONFIG_PATH,
            DEFAULT_CONFIG_PATH);
}

static const gchar *
default_config_path(void)
{
    if (g_file_test(FURIOS_CONFIG_PATH, G_FILE_TEST_IS_REGULAR))
        return FURIOS_CONFIG_PATH;

    if (g_file_test(SYSTEM_CONFIG_PATH, G_FILE_TEST_IS_REGULAR))
        return SYSTEM_CONFIG_PATH;

    return DEFAULT_CONFIG_PATH;
}

static void
interactive_help(void)
{
    printf("commands:\n"
           "  output receiver|speaker|headphones|headset|bluetooth|usb\n"
           "  input builtin-mic|headset-mic|bluetooth|usb\n"
           "  devices OUTPUT INPUT\n"
           "  call start hostless|hostful\n"
           "  call route OUTPUT INPUT hostless|hostful\n"
           "  call stop\n"
           "  call bluetooth narrow|wide nrec|no-nrec\n"
           "  call volume 0.0..1.0\n"
           "  call downlink start|stop\n"
           "  call downlink info\n"
           "  call downlink read BYTES\n"
           "  call uplink start|stop\n"
           "  call uplink info\n"
           "  call uplink tone MILLISECONDS\n"
           "  mute uplink on|off\n"
           "  mute downlink on|off\n"
           "  status\n"
           "  help\n"
           "  quit\n");
}

static gboolean
parse_bool(const gchar *value,
           gboolean *result)
{
    if (value == NULL || result == NULL)
        return FALSE;

    if (g_ascii_strcasecmp(value, "1") == 0 ||
        g_ascii_strcasecmp(value, "on") == 0 ||
        g_ascii_strcasecmp(value, "true") == 0 ||
        g_ascii_strcasecmp(value, "yes") == 0) {
        *result = TRUE;
        return TRUE;
    }

    if (g_ascii_strcasecmp(value, "0") == 0 ||
        g_ascii_strcasecmp(value, "off") == 0 ||
        g_ascii_strcasecmp(value, "false") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0) {
        *result = FALSE;
        return TRUE;
    }

    return FALSE;
}

static gint
parse_volume(const gchar *value,
             gdouble *volume)
{
    gchar *end = NULL;
    gdouble parsed;

    if (value == NULL || volume == NULL)
        return -EINVAL;

    errno = 0;
    parsed = g_ascii_strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < 0.0 || parsed > 1.0)
        return -EINVAL;

    *volume = parsed;
    return 0;
}

static gboolean
parse_call_transport(const gchar *value,
                     AudioManagerCallTransport *transport)
{
    if (value == NULL || transport == NULL)
        return FALSE;

    if (g_ascii_strcasecmp(value, "hostless") == 0) {
        *transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "hostful") == 0) {
        *transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL;
        return TRUE;
    }

    return FALSE;
}

static void
print_status(AudioManager *manager)
{
    printf("backend: %s\n",
           audio_manager_get_backend_name(manager));
    printf("output: %s\n", audio_output_device_to_string(audio_manager_get_output_device(manager)));
    printf("input: %s\n", audio_input_device_to_string(audio_manager_get_input_device(manager)));
    printf("call-active: %s\n",
           audio_manager_call_is_active(manager) ? "yes" : "no");
    printf("call-transport: %s\n",
           !audio_manager_call_is_active(manager) ? "none" :
           audio_manager_call_get_transport(manager) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL ?
           "hostful" : "hostless");
    printf("call-state: %s\n",
           audio_call_state_to_string(audio_manager_call_get_state(manager)));
    printf("call-band: %s\n",
           audio_speech_band_to_string(audio_manager_call_get_band(manager)));
    printf("call-volume: %.3f\n",
           audio_manager_call_get_volume(manager));
    printf("uplink-mute: %s\n",
           audio_manager_call_get_uplink_mute(manager) ? "on" : "off");
    printf("downlink-mute: %s\n",
           audio_manager_call_get_downlink_mute(manager) ? "on" : "off");
}

static const gchar *
call_stream_format_name(AudioManagerSampleFormat format)
{
    switch (format) {
    case AUDIO_MANAGER_SAMPLE_S16_LE:
        return "S16_LE";
    case AUDIO_MANAGER_SAMPLE_S24_LE:
        return "S24_LE";
    case AUDIO_MANAGER_SAMPLE_S32_LE:
        return "S32_LE";
    default:
        return "unknown";
    }
}

static gint
print_call_stream_info(AudioManagerCallStream *stream)
{
    AudioManagerCallStreamConfig config = AUDIO_MANAGER_CALL_STREAM_CONFIG_INIT;
    gint ret;

    if (stream == NULL)
        return -ENODEV;

    ret = audio_manager_call_stream_get_config(stream, &config);
    if (ret < 0)
        return ret;

    printf("cellular call PCM stream: direction=%s format=%s rate=%u channels=%u "
           "period=%u count=%u frame-bytes=%zu period-bytes=%zu\n",
           config.direction == AUDIO_MANAGER_CALL_STREAM_DOWNLINK ?
           "downlink" : "uplink",
           call_stream_format_name(config.format),
           config.rate,
           config.channels,
           config.period_size,
           config.period_count,
           config.frame_bytes,
           (gsize)config.period_size * config.frame_bytes);
    return 0;
}

static gint
execute_command(AudioManagerCtl *ctl,
                gint argc,
                gchar **argv)
{
    AudioManagerOutputDevice output;
    AudioManagerInputDevice input;
    gdouble volume;
    gboolean muted;
    gint ret;

    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "output") == 0) {
        if (argc != 2 ||
            !audio_output_device_from_string(argv[1], &output) ||
            output == AUDIO_MANAGER_OUTPUT_NONE)
            return -EINVAL;

        return audio_manager_set_output_device(ctl->manager, output);
    }

    if (strcmp(argv[0], "input") == 0) {
        if (argc != 2 ||
            !audio_input_device_from_string(argv[1], &input) ||
            input == AUDIO_MANAGER_INPUT_NONE)
            return -EINVAL;

        return audio_manager_set_input_device(ctl->manager, input);
    }

    if (strcmp(argv[0], "devices") == 0) {
        if (argc != 3 ||
            !audio_output_device_from_string(argv[1], &output) ||
            output == AUDIO_MANAGER_OUTPUT_NONE ||
            !audio_input_device_from_string(argv[2], &input) ||
            input == AUDIO_MANAGER_INPUT_NONE)
            return -EINVAL;

        return audio_manager_set_devices(ctl->manager, output, input);
    }

    if (strcmp(argv[0], "call") == 0) {
        if (argc == 3 && strcmp(argv[1], "start") == 0) {
            AudioManagerCallTransport transport;

            if (!parse_call_transport(argv[2], &transport))
                return -EINVAL;
            return audio_manager_call_start(ctl->manager, transport);
        }

        if (argc == 5 && strcmp(argv[1], "route") == 0) {
            AudioManagerCallTransport transport;

            if (!audio_output_device_from_string(argv[2], &output) ||
                output == AUDIO_MANAGER_OUTPUT_NONE ||
                !audio_input_device_from_string(argv[3], &input) ||
                input == AUDIO_MANAGER_INPUT_NONE ||
                !parse_call_transport(argv[4], &transport))
                return -EINVAL;
            return audio_manager_call_set_route(ctl->manager, output, input, transport);
        }

        if (argc == 2 && strcmp(argv[1], "stop") == 0)
            return audio_manager_call_stop(ctl->manager);

        if (argc == 4 && strcmp(argv[1], "bluetooth") == 0) {
            AudioManagerBluetoothCallConfig bt = AUDIO_MANAGER_BLUETOOTH_CALL_CONFIG_INIT;

            if (strcmp(argv[2], "wide") == 0 || strcmp(argv[2], "msbc") == 0)
                bt.codec = AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC;
            else if (strcmp(argv[2], "narrow") == 0 || strcmp(argv[2], "cvsd") == 0)
                bt.codec = AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_CVSD;
            else if (strcmp(argv[2], "lc3-swb") == 0)
                bt.codec = AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_LC3_SWB;
            else
                return -EINVAL;

            if (strcmp(argv[3], "nrec") == 0)
                bt.nrec = TRUE;
            else if (strcmp(argv[3], "no-nrec") == 0)
                bt.nrec = FALSE;
            else
                return -EINVAL;

            return audio_manager_call_set_bluetooth_config(ctl->manager, &bt);
        }

        if (argc == 3 && strcmp(argv[1], "volume") == 0) {
            if (parse_volume(argv[2], &volume) < 0)
                return -EINVAL;

            return audio_manager_call_set_volume(ctl->manager, volume);
        }

        if (argc == 3 && strcmp(argv[1], "downlink") == 0 &&
            strcmp(argv[2], "start") == 0) {
            GError *error = NULL;

            if (ctl->call_downlink != NULL)
                return -EALREADY;
            ctl->call_downlink = audio_manager_call_stream_open(ctl->manager,
                                                                AUDIO_MANAGER_CALL_STREAM_DOWNLINK,
                                                                &error);
            if (ctl->call_downlink == NULL) {
                if (error != NULL) {
                    g_warning("failed to open cellular call downlink: %s", error->message);
                    g_error_free(error);
                }
                return -EIO;
            }
            ret = audio_manager_call_stream_start(ctl->call_downlink);
            if (ret < 0) {
                audio_manager_call_stream_close(ctl->call_downlink);
                ctl->call_downlink = NULL;
            }
            return ret;
        }

        if (argc == 3 && strcmp(argv[1], "downlink") == 0 &&
            strcmp(argv[2], "stop") == 0) {
            if (ctl->call_downlink == NULL)
                return 0;
            ret = audio_manager_call_stream_stop(ctl->call_downlink);
            audio_manager_call_stream_close(ctl->call_downlink);
            ctl->call_downlink = NULL;
            return ret;
        }

        if (argc == 3 && strcmp(argv[1], "downlink") == 0 &&
            strcmp(argv[2], "info") == 0)
            return print_call_stream_info(ctl->call_downlink);

        if (argc == 4 && strcmp(argv[1], "downlink") == 0 &&
            strcmp(argv[2], "read") == 0) {
            gchar *end = NULL;
            guint64 bytes;
            gpointer buffer;
            const gint16 *samples;
            gsize sample_count;
            gsize nonzero = 0;
            guint peak = 0;
            gsize i;
            gssize got;

            if (ctl->call_downlink == NULL)
                return -ENODEV;
            errno = 0;
            bytes = g_ascii_strtoull(argv[3], &end, 10);
            if (errno != 0 || end == argv[3] || *end != '\0' ||
                bytes == 0 || bytes > G_MAXSIZE)
                return -EINVAL;
            buffer = g_malloc((gsize)bytes);
            got = audio_manager_call_stream_read(ctl->call_downlink, buffer,
                                                 (gsize)bytes);
            if (got < 0) {
                g_free(buffer);
                return (gint)got;
            }
            samples = buffer;
            sample_count = (gsize)got / sizeof(gint16);
            for (i = 0; i < sample_count; i++) {
                gint sample = samples[i];
                guint magnitude = sample < 0 ? (guint)(-(gint64)sample) : (guint)sample;
                if (sample != 0)
                    nonzero++;
                if (magnitude > peak)
                    peak = magnitude;
            }
            printf("cellular downlink read: %zd bytes, samples=%zu, nonzero=%zu, peak=%u\n",
                   got, sample_count, nonzero, peak);
            g_free(buffer);
            return 0;
        }

        if (argc == 3 && strcmp(argv[1], "uplink") == 0 &&
            strcmp(argv[2], "start") == 0) {
            GError *error = NULL;

            if (ctl->call_uplink != NULL)
                return -EALREADY;
            ctl->call_uplink = audio_manager_call_stream_open(ctl->manager,
                                                              AUDIO_MANAGER_CALL_STREAM_UPLINK,
                                                              &error);
            if (ctl->call_uplink == NULL) {
                if (error != NULL) {
                    g_warning("failed to open cellular call uplink: %s", error->message);
                    g_error_free(error);
                }
                return -EIO;
            }
            ret = audio_manager_call_stream_start(ctl->call_uplink);
            if (ret < 0) {
                audio_manager_call_stream_close(ctl->call_uplink);
                ctl->call_uplink = NULL;
            }
            return ret;
        }

        if (argc == 3 && strcmp(argv[1], "uplink") == 0 &&
            strcmp(argv[2], "stop") == 0) {
            if (ctl->call_uplink == NULL)
                return 0;
            ret = audio_manager_call_stream_stop(ctl->call_uplink);
            audio_manager_call_stream_close(ctl->call_uplink);
            ctl->call_uplink = NULL;
            return ret;
        }

        if (argc == 3 && strcmp(argv[1], "uplink") == 0 &&
            strcmp(argv[2], "info") == 0)
            return print_call_stream_info(ctl->call_uplink);

        if (argc == 4 && strcmp(argv[1], "uplink") == 0 &&
            strcmp(argv[2], "tone") == 0) {
            AudioManagerCallStreamConfig config = AUDIO_MANAGER_CALL_STREAM_CONFIG_INIT;
            gchar *end = NULL;
            guint64 milliseconds;
            gint16 *samples;
            guint64 total_frames;
            guint64 periods;
            guint64 period;
            guint frame;
            gsize period_bytes;

            if (ctl->call_uplink == NULL)
                return -ENODEV;
            if (audio_manager_call_stream_get_config(ctl->call_uplink,
                                                     &config) < 0)
                return -EIO;
            if (config.format != AUDIO_MANAGER_SAMPLE_S16_LE ||
                config.rate == 0 || config.channels == 0 ||
                config.period_size == 0)
                return -ENOTSUP;

            errno = 0;
            milliseconds = g_ascii_strtoull(argv[3], &end, 10);
            if (errno != 0 || end == argv[3] || *end != '\0' ||
                milliseconds == 0 || milliseconds > 60000)
                return -EINVAL;

            total_frames = (milliseconds * config.rate + 999) / 1000;
            periods = (total_frames + config.period_size - 1) /
                      config.period_size;
            period_bytes = (gsize)config.period_size * config.frame_bytes;
            samples = g_malloc(period_bytes);

            for (period = 0; period < periods; period++) {
                gssize written;

                for (frame = 0; frame < config.period_size; frame++) {
                    guint64 absolute_frame = period * config.period_size + frame;
                    gint16 sample = ((absolute_frame * 1000U) % config.rate) <
                                   (config.rate / 2U) ? 6000 : -6000;
                    guint channel;

                    for (channel = 0; channel < config.channels; channel++)
                        samples[frame * config.channels + channel] = sample;
                }

                written = audio_manager_call_stream_write(ctl->call_uplink,
                                                          samples,
                                                          period_bytes);
                if (written < 0) {
                    g_free(samples);
                    return (gint)written;
                }
                if ((gsize)written != period_bytes) {
                    g_free(samples);
                    return -EIO;
                }
            }

            g_free(samples);
            printf("cellular uplink wrote %" G_GUINT64_FORMAT
                   " periods of 1 kHz test tone (%u frames each)\n",
                   periods, config.period_size);
            return 0;
        }

        return -EINVAL;
    }

    if (strcmp(argv[0], "mute") == 0) {
        if (argc != 3 || !parse_bool(argv[2], &muted))
            return -EINVAL;

        if (strcmp(argv[1], "uplink") == 0)
            return audio_manager_call_set_uplink_mute(ctl->manager, muted);

        if (strcmp(argv[1], "downlink") == 0)
            return audio_manager_call_set_downlink_mute(ctl->manager, muted);

        return -EINVAL;
    }

    if (strcmp(argv[0], "status") == 0 && argc == 1) {
        print_status(ctl->manager);
        return 0;
    }

    if (strcmp(argv[0], "help") == 0 && argc == 1) {
        interactive_help();
        return 0;
    }

    if ((strcmp(argv[0], "quit") == 0 ||
         strcmp(argv[0], "exit") == 0) && argc == 1) {
        if (ctl->loop != NULL)
            g_main_loop_quit(ctl->loop);
        return 0;
    }

    ret = -EINVAL;
    return ret;
}

static void
print_command_error(gint ret)
{
    fprintf(stderr,
            "command failed: %s (%d)\n",
            g_strerror(-ret),
            ret);
}

static void
print_prompt(void)
{
    printf("audio-manager> ");
    fflush(stdout);
}

static gboolean
stdin_ready(GIOChannel *channel,
            GIOCondition condition,
            gpointer user_data)
{
    AudioManagerCtl *ctl = user_data;
    GError *error = NULL;
    GIOStatus status;
    gchar *line = NULL;
    gchar **argv = NULL;
    gint argc = 0;
    gint ret;

    if ((condition & (G_IO_ERR | G_IO_NVAL)) != 0) {
        g_main_loop_quit(ctl->loop);
        return G_SOURCE_REMOVE;
    }

    status = g_io_channel_read_line(channel,
                                    &line,
                                    NULL,
                                    NULL,
                                    &error);
    if (status == G_IO_STATUS_EOF) {
        g_main_loop_quit(ctl->loop);
        return G_SOURCE_REMOVE;
    }

    if (status == G_IO_STATUS_AGAIN) {
        print_prompt();
        return G_SOURCE_CONTINUE;
    }

    if (status != G_IO_STATUS_NORMAL) {
        if (error != NULL) {
            g_warning("failed to read stdin: %s", error->message);
            g_error_free(error);
        }
        g_free(line);
        g_main_loop_quit(ctl->loop);
        return G_SOURCE_REMOVE;
    }

    g_strstrip(line);
    if (*line == '\0') {
        g_free(line);
        print_prompt();
        return G_SOURCE_CONTINUE;
    }

    if (!g_shell_parse_argv(line, &argc, &argv, &error)) {
        fprintf(stderr, "parse error: %s\n", error->message);
        g_error_free(error);
        g_free(line);
        print_prompt();
        return G_SOURCE_CONTINUE;
    }

    ret = execute_command(ctl, argc, argv);
    if (ret < 0)
        print_command_error(ret);

    g_strfreev(argv);
    g_free(line);

    if (g_main_loop_is_running(ctl->loop))
        print_prompt();

    return G_SOURCE_CONTINUE;
}

gint
main(gint argc,
     gchar **argv)
{
    AudioManagerConfig config = AUDIO_MANAGER_CONFIG_INIT;
    AudioManagerCtl ctl = { 0 };
    AudioManager *manager;
    GMainContext *context;
    GError *error = NULL;
    GIOChannel *stdin_channel = NULL;
    GSource *stdin_source = NULL;
    const gchar *config_path = NULL;
    gint argi = 1;
    gint ret = 0;

    if (argi < argc &&
        (strcmp(argv[argi], "-h") == 0 ||
         strcmp(argv[argi], "--help") == 0)) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (argi < argc &&
        (strcmp(argv[argi], "-c") == 0 ||
         strcmp(argv[argi], "--config") == 0)) {
        if (argi + 1 >= argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }

        config_path = argv[argi + 1];
        argi += 2;
    }

    if (config_path == NULL)
        config_path = default_config_path();

    context = g_main_context_new();
    config.config_path = config_path;
    config.main_context = context;

    manager = audio_manager_new(&config, &error);
    if (manager == NULL) {
        fprintf(stderr, "audio_manager_new: %s\n", error->message);
        g_error_free(error);
        g_main_context_unref(context);
        return EXIT_FAILURE;
    }

    ctl.manager = manager;

    if (argi < argc) {
        ret = execute_command(&ctl, argc - argi, &argv[argi]);
        if (ret < 0)
            print_command_error(ret);

        while (g_main_context_pending(context))
            g_main_context_iteration(context, FALSE);
    } else {
        ctl.loop = g_main_loop_new(context, FALSE);

        stdin_channel = g_io_channel_unix_new(fileno(stdin));
        g_io_channel_set_close_on_unref(stdin_channel, FALSE);

        stdin_source = g_io_create_watch(stdin_channel,
                                         G_IO_IN |
                                         G_IO_HUP |
                                         G_IO_ERR |
                                         G_IO_NVAL);
        g_source_set_callback(stdin_source,
                              G_SOURCE_FUNC(stdin_ready),
                              &ctl,
                              NULL);
        g_source_attach(stdin_source, context);

        printf("audio-manager-ctl\n");
        printf("backend: %s\n", audio_manager_get_backend_name(manager));
        printf("type 'help' for commands\n");
        print_prompt();

        g_main_loop_run(ctl.loop);
    }

    if (stdin_source != NULL) {
        g_source_destroy(stdin_source);
        g_source_unref(stdin_source);
    }
    if (stdin_channel != NULL)
        g_io_channel_unref(stdin_channel);
    if (ctl.loop != NULL)
        g_main_loop_unref(ctl.loop);

    if (ctl.call_uplink != NULL) {
        audio_manager_call_stream_stop(ctl.call_uplink);
        audio_manager_call_stream_close(ctl.call_uplink);
        ctl.call_uplink = NULL;
    }
    if (ctl.call_downlink != NULL) {
        audio_manager_call_stream_stop(ctl.call_downlink);
        audio_manager_call_stream_close(ctl.call_downlink);
        ctl.call_downlink = NULL;
    }

    audio_manager_free(manager);
    g_main_context_unref(context);

    return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
