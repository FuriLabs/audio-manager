/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-ccci.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <audio-manager/audio-manager-error.h>

#include "common/mainloop/fd-source.h"

#define MTK_CCCI_IOC_MAGIC 'C'
#define MTK_CCCI_IOC_GET_MD_STATE _IOR(MTK_CCCI_IOC_MAGIC, 1, guint)

#define MTK_MODEM_STATUS_READY 2
#define MTK_CCCI_WRITE_RETRY_COUNT 20U
#define MTK_CCCI_WRITE_RETRY_US 2000U

typedef struct __attribute__((packed)) {
    guint32 magic;
    guint16 param_16bit;
    guint16 msg_id;
    guint32 channel;
    guint32 param_32bit;
} CcciMailbox;

typedef struct __attribute__((packed)) {
    guint32 offset;
    guint32 payload_size;
    guint32 channel;
    guint16 param_16bit;
    guint16 msg_id;
    guint16 data_sync;
    guint16 data_type;
    guint16 data_size;
    guint8 data[MTK_CCCI_MAX_AP_PAYLOAD_DATA_SIZE];
} CcciApPayload;

typedef struct __attribute__((packed)) {
    guint32 offset;
    guint32 message_size;
    guint32 channel;
    guint16 param_16bit;
    guint16 msg_id;
    guint16 data_sync;
    guint16 data_type;
    guint16 data_size;
    guint16 index;
    guint16 total_index;
    guint8 data[MTK_CCCI_MAX_MD_PAYLOAD_DATA_SIZE];
} CcciMdPayload;

struct MtkCcci {
    gint fd;
    gchar *device;
    GMainContext *main_context;
    AudioFdSource *source;
    gboolean connected;
    gboolean fault_notified;

    MtkCcciMessageFunc callback;
    MtkCcciFaultFunc fault_callback;
    gpointer user_data;
};

static void
ccci_close(MtkCcci *ccci)
{
    audio_fd_source_free(ccci->source);
    ccci->source = NULL;

    if (ccci->fd >= 0) {
        close(ccci->fd);
        ccci->fd = -1;
    }

    ccci->connected = FALSE;
}

static gint
ccci_write_all(MtkCcci *ccci,
               gconstpointer buffer,
               gsize size)
{
    const guint8 *data = buffer;
    gsize offset = 0;

    if (!mtk_ccci_is_connected(ccci))
        return -ENODEV;

    while (offset < size) {
        guint retry = 0;

        for (;;) {
            gssize ret;
            gint saved_errno = 0;

            g_debug("CCCI TX write fd=%d offset=%zu remaining=%zu",
                    ccci->fd, offset, size - offset);
            ret = write(ccci->fd, data + offset, size - offset);
            if (ret < 0)
                saved_errno = errno;

            if (ret > 0) {
                g_debug("CCCI TX write answer=%zd bytes", ret);
                offset += (gsize)ret;
                break;
            }

            if (ret < 0 && saved_errno == EINTR) {
                g_debug("CCCI TX write interrupted. retrying");
                continue;
            }

            if (ret < 0 &&
                (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) &&
                retry < MTK_CCCI_WRITE_RETRY_COUNT) {
                retry++;
                g_debug("CCCI TX busy answer=%s retry=%u/%u",
                        g_strerror(saved_errno), retry, MTK_CCCI_WRITE_RETRY_COUNT);
                usleep(MTK_CCCI_WRITE_RETRY_US);
                continue;
            }

            g_debug("CCCI TX write answer=%zd errno=%d (%s)",
                    ret, saved_errno,
                    ret < 0 ? g_strerror(saved_errno) : "short write");
            return ret < 0 ? -saved_errno : -EIO;
        }
    }

    return 0;
}

static void
ccci_notify_fault(MtkCcci *ccci,
                  GIOCondition condition)
{
    if (ccci->fault_notified)
        return;

    ccci->fault_notified = TRUE;
    ccci->connected = FALSE;

    if (ccci->fault_callback != NULL)
        ccci->fault_callback(ccci, condition, ccci->user_data);
}

static gboolean
ccci_source_ready(gint fd,
                  GIOCondition condition,
                  gpointer user_data)
{
    MtkCcci *ccci = user_data;
    guint8 buffer[MTK_CCCI_MAX_BUF_SIZE];
    gssize length;

    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
        g_warning("CCCI device '%s' reported condition 0x%x",
                  ccci->device,
                  condition);
        ccci_notify_fault(ccci, condition);
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        MtkCcciMessage message = { 0 };

        gint saved_errno = 0;

        g_debug("CCCI RX read fd=%d max=%zu", fd, sizeof(buffer));
        length = read(fd, buffer, sizeof(buffer));
        if (length < 0)
            saved_errno = errno;

        if (length >= 0)
            g_debug("CCCI RX read answer=%zd bytes", length);
        else
            g_debug("CCCI RX read answer=%zd errno=%d (%s)",
                    length, saved_errno, g_strerror(saved_errno));

        if (length < 0 && saved_errno == EINTR)
            continue;
        if (length < 0 &&
            (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK))
            break;
        if (length < 0) {
            g_warning("CCCI read failed on '%s': %s",
                      ccci->device,
                      g_strerror(saved_errno));
            ccci_notify_fault(ccci, G_IO_ERR);
            return G_SOURCE_REMOVE;
        }
        if (length == 0) {
            g_warning("CCCI device '%s' reached EOF", ccci->device);
            ccci_notify_fault(ccci, G_IO_HUP);
            return G_SOURCE_REMOVE;
        }
        if (length < MTK_CCCI_MAILBOX_SIZE)
            continue;

        if (((CcciMailbox *)buffer)->magic == MTK_CCCI_MAILBOX_MAGIC) {
            CcciMailbox *mailbox = (CcciMailbox *)buffer;

            message.msg_id = mailbox->msg_id;
            message.param_16bit = mailbox->param_16bit;
            message.param_32bit = mailbox->param_32bit;
        } else {
            CcciMdPayload *payload = (CcciMdPayload *)buffer;
            gsize expected;

            if (length < (gssize)(MTK_CCCI_MAILBOX_SIZE +
                                  MTK_CCCI_MAX_MD_PAYLOAD_HEADER_SIZE))
                continue;
            if (payload->data_sync != MTK_CCCI_MD_PAYLOAD_SYNC)
                continue;
            if (payload->data_size > MTK_CCCI_MAX_MD_PAYLOAD_DATA_SIZE)
                continue;

            expected = MTK_CCCI_MAILBOX_SIZE +
                       MTK_CCCI_MAX_MD_PAYLOAD_HEADER_SIZE +
                       payload->data_size;
            if ((gsize)length < expected)
                continue;

            message.msg_id = payload->msg_id;
            message.param_16bit = payload->param_16bit;
            message.has_payload = TRUE;
            message.payload_type = payload->data_type;
            message.payload_size = payload->data_size;
            message.payload_index = payload->index;
            message.payload_total = payload->total_index;
            message.payload = payload->data;
        }

        g_debug("CCCI RX %s id=0x%04x param16=0x%04x param32=0x%08x payload=%s type=%u size=%u index=%u total=%u",
                mtk_speech_message_name(message.msg_id) != NULL ?
                    mtk_speech_message_name(message.msg_id) : "UNKNOWN",
                message.msg_id, message.param_16bit, message.param_32bit,
                message.has_payload ? "yes" : "no", message.payload_type,
                message.payload_size, message.payload_index,
                message.payload_total);

        if (ccci->callback != NULL)
            ccci->callback(ccci, &message, ccci->user_data);
    }

    return G_SOURCE_CONTINUE;
}

static gint
ccci_open(MtkCcci *ccci,
          GError **error)
{
    ccci->fd = open(ccci->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (ccci->fd < 0) {
        gint saved_errno = errno;

        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to open MediaTek CCCI device '%s': %s",
                    ccci->device,
                    g_strerror(saved_errno));
        return -saved_errno;
    }

    ccci->source = audio_fd_source_new(ccci->main_context,
                                       ccci->fd,
                                       G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                       ccci_source_ready,
                                       ccci);
    if (ccci->source == NULL) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to attach MediaTek CCCI device to main context");
        close(ccci->fd);
        ccci->fd = -1;
        return -EIO;
    }

    ccci->connected = TRUE;
    ccci->fault_notified = FALSE;
    return 0;
}

MtkCcci *
mtk_ccci_new(const gchar *device,
             GMainContext *main_context,
             MtkCcciMessageFunc callback,
             MtkCcciFaultFunc fault_callback,
             gpointer user_data,
             GError **error)
{
    MtkCcci *ccci;

    ccci = g_new0(MtkCcci, 1);
    ccci->fd = -1;
    ccci->device = g_strdup(device);
    ccci->main_context = main_context;
    ccci->callback = callback;
    ccci->fault_callback = fault_callback;
    ccci->user_data = user_data;

    if (ccci_open(ccci, error) < 0) {
        mtk_ccci_free(ccci);
        return NULL;
    }

    return ccci;
}

void
mtk_ccci_free(MtkCcci *ccci)
{
    if (ccci == NULL)
        return;

    ccci_close(ccci);
    g_free(ccci->device);
    g_free(ccci);
}

gboolean
mtk_ccci_is_connected(MtkCcci *ccci)
{
    return ccci != NULL && ccci->connected && ccci->fd >= 0;
}

gint
mtk_ccci_reopen(MtkCcci *ccci,
                GError **error)
{
    if (ccci == NULL)
        return -EINVAL;

    ccci_close(ccci);
    return ccci_open(ccci, error);
}

gboolean
mtk_ccci_modem_ready(MtkCcci *ccci)
{
    guint state = 0;

    if (!mtk_ccci_is_connected(ccci))
        return FALSE;

    g_debug("CCCI ioctl GET_MD_STATE fd=%d request=0x%lx",
            ccci->fd, (gulong)MTK_CCCI_IOC_GET_MD_STATE);
    if (ioctl(ccci->fd, MTK_CCCI_IOC_GET_MD_STATE, &state) < 0) {
        g_debug("CCCI ioctl GET_MD_STATE answer=-1 errno=%d (%s)",
                errno, g_strerror(errno));
        return FALSE;
    }

    g_debug("CCCI ioctl GET_MD_STATE answer=0 state=%u ready=%s",
            state, state == MTK_MODEM_STATUS_READY ? "yes" : "no");
    return state == MTK_MODEM_STATUS_READY;
}

gint
mtk_ccci_send_mailbox(MtkCcci *ccci,
                      guint16 msg_id,
                      guint16 param_16bit,
                      guint32 param_32bit)
{
    CcciMailbox mailbox = {
        .magic = MTK_CCCI_MAILBOX_MAGIC,
        .param_16bit = param_16bit,
        .msg_id = msg_id,
        .channel = MTK_CCCI_A2M_CHANNEL,
        .param_32bit = param_32bit,
    };

    if (ccci == NULL)
        return -EINVAL;
    if (!mtk_ccci_modem_ready(ccci))
        return -EPIPE;

    g_debug("CCCI TX mailbox %s id=0x%04x param16=0x%04x param32=0x%08x bytes=%zu",
            mtk_speech_message_name(msg_id) != NULL ?
                mtk_speech_message_name(msg_id) : "UNKNOWN",
            msg_id, param_16bit, param_32bit, sizeof(mailbox));
    return ccci_write_all(ccci, &mailbox, sizeof(mailbox));
}

gint
mtk_ccci_send_payload(MtkCcci *ccci,
                      guint16 msg_id,
                      guint16 payload_type,
                      gconstpointer payload,
                      guint16 payload_size)
{
    CcciApPayload message = { 0 };
    gsize size;

    if (ccci == NULL || payload == NULL)
        return -EINVAL;
    if (payload_size > MTK_CCCI_MAX_AP_PAYLOAD_DATA_SIZE)
        return -EMSGSIZE;
    if (!mtk_ccci_modem_ready(ccci))
        return -EPIPE;

    message.offset = 0;
    message.payload_size = MTK_CCCI_MAX_AP_PAYLOAD_HEADER_SIZE + payload_size;
    message.channel = MTK_CCCI_A2M_CHANNEL;
    message.param_16bit = (guint16)message.payload_size;
    message.msg_id = msg_id;
    message.data_sync = MTK_CCCI_AP_PAYLOAD_SYNC;
    message.data_type = payload_type;
    message.data_size = payload_size;
    memcpy(message.data, payload, payload_size);

    size = MTK_CCCI_MAILBOX_SIZE + message.payload_size;
    g_debug("CCCI TX payload %s id=0x%04x type=%u data_size=%u wire_size=%zu",
            mtk_speech_message_name(msg_id) != NULL ?
                mtk_speech_message_name(msg_id) : "UNKNOWN",
            msg_id, payload_type, payload_size, size);
    return ccci_write_all(ccci, &message, size);
}
