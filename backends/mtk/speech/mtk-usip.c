/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-usip.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <audio-manager/audio-manager-error.h>

#include "common/alsa/alsa-control.h"

#define MTK_USIP_IOC_MAGIC 'D'
#define MTK_USIP_IOC_GET_EMI_SIZE _IOWR(MTK_USIP_IOC_MAGIC, 0xf0, guint64)

#define MTK_USIP_CACHE_RESERVED_SIZE 3072U
#define MTK_USIP_GUARD_SIZE 32U
#define MTK_USIP_SPEECH_PARAM_SIZE 180192U
#define MTK_USIP_MD_PRIVATE_SIZE 13272U
#define MTK_USIP_FRAME_RESERVE 16U

typedef struct {
    guint8 cache_reserved[MTK_USIP_CACHE_RESERVED_SIZE];
    guint8 guard_pre[MTK_USIP_GUARD_SIZE];

    guint32 md_private_range_offset;
    guint32 speech_param_offset;

    guint8 md_private_range[MTK_USIP_MD_PRIVATE_SIZE];
    guint8 speech_param[MTK_USIP_SPEECH_PARAM_SIZE];

    guint8 guard_post[MTK_USIP_GUARD_SIZE];
} MtkUsipLayout;

struct MtkUsip {
    gint fd;
    gpointer mapping;
    gsize mapping_size;
    MtkUsipLayout *layout;

    AudioAlsaCard *card;
    gchar *init_control;
    gchar *write_index_control;

    guint32 write_index;
    GMutex lock;
};

static gint
set_control(AudioAlsaCard *card,
            const gchar *name,
            guint32 value)
{
    gchar string[32];

    if (card == NULL || name == NULL || *name == '\0')
        return 0;

    g_snprintf(string, sizeof(string), "%u", value);
    return audio_alsa_control_set_from_string(card, name, string);
}

static gint
usip_format(MtkUsip *usip)
{
    MtkUsipLayout *layout;
    gint ret;

    if (usip == NULL || usip->layout == NULL)
        return -EINVAL;

    layout = usip->layout;

    memset(layout->guard_pre, 0x0a, sizeof(layout->guard_pre));
    memset(layout->guard_post, 0x0a, sizeof(layout->guard_post));

    layout->md_private_range_offset = (guint32)G_STRUCT_OFFSET(MtkUsipLayout, md_private_range);
    layout->speech_param_offset = (guint32)G_STRUCT_OFFSET(MtkUsipLayout, speech_param);

    usip->write_index = 0;

    ret = set_control(usip->card, usip->write_index_control, 0);
    if (ret < 0)
        return ret;

    return set_control(usip->card, usip->init_control, 1);
}

MtkUsip *
mtk_usip_new(const gchar *device,
             AudioAlsaCard *card,
             const gchar *init_control,
             const gchar *write_index_control,
             GError **error)
{
    MtkUsip *usip;
    guint64 length = 0;
    gint ioctl_ret;

    usip = g_new0(MtkUsip, 1);
    usip->fd = -1;
    usip->card = card;
    usip->init_control = g_strdup(init_control);
    usip->write_index_control = g_strdup(write_index_control);
    g_mutex_init(&usip->lock);

    usip->fd = open(device, O_RDWR | O_CLOEXEC);
    if (usip->fd < 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to open MediaTek USIP device '%s': %s",
                    device,
                    g_strerror(errno));
        mtk_usip_free(usip);
        return NULL;
    }

    g_debug("USIP ioctl GET_EMI_SIZE fd=%d request=0x%lx", usip->fd,
            (gulong)MTK_USIP_IOC_GET_EMI_SIZE);
    errno = 0;
    ioctl_ret = ioctl(usip->fd, MTK_USIP_IOC_GET_EMI_SIZE, &length);
    g_debug("USIP ioctl GET_EMI_SIZE answer=%d length=%" G_GUINT64_FORMAT " errno=%d (%s)",
            ioctl_ret, length, ioctl_ret < 0 ? errno : 0,
            ioctl_ret < 0 ? g_strerror(errno) : "ok");
    if (ioctl_ret < 0 || length == 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to query MediaTek USIP memory size '%s': %s",
                    device,
                    g_strerror(errno));
        mtk_usip_free(usip);
        return NULL;
    }

    if (length < sizeof(MtkUsipLayout)) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "MediaTek USIP memory is too small (%" G_GUINT64_FORMAT " bytes)",
                    length);
        mtk_usip_free(usip);
        return NULL;
    }

    usip->mapping = mmap(NULL,
                         (gsize)length,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         usip->fd,
                         0);
    if (usip->mapping == MAP_FAILED) {
        usip->mapping = NULL;
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to map MediaTek USIP memory '%s': %s",
                    device,
                    g_strerror(errno));
        mtk_usip_free(usip);
        return NULL;
    }

    usip->mapping_size = (gsize)length;
    usip->layout = usip->mapping;
    g_debug("USIP mmap answer=%p length=%zu", usip->mapping, usip->mapping_size);

    if (usip_format(usip) < 0) {
        g_set_error(error,
                    AUDIO_MANAGER_ERROR,
                    AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to initialize MediaTek USIP speech memory");
        mtk_usip_free(usip);
        return NULL;
    }

    return usip;
}

void
mtk_usip_free(MtkUsip *usip)
{
    if (usip == NULL)
        return;

    if (usip->mapping != NULL)
        munmap(usip->mapping, usip->mapping_size);
    if (usip->fd >= 0)
        close(usip->fd);

    g_free(usip->init_control);
    g_free(usip->write_index_control);
    g_mutex_clear(&usip->lock);
    g_free(usip);
}

gint
mtk_usip_reset(MtkUsip *usip)
{
    gint ret;

    if (usip == NULL)
        return -EINVAL;

    g_mutex_lock(&usip->lock);
    usip->write_index = 0;
    ret = set_control(usip->card, usip->write_index_control, 0);
    g_mutex_unlock(&usip->lock);

    return ret;
}

gint
mtk_usip_write_speech_params(MtkUsip *usip,
                             gconstpointer data,
                             gsize size,
                             guint32 *write_index)
{
    guint32 start;
    gint ret;

    if (usip == NULL || data == NULL || write_index == NULL)
        return -EINVAL;
    if (size == 0)
        return 0;
    if (size > MTK_USIP_SPEECH_PARAM_SIZE - MTK_USIP_FRAME_RESERVE)
        return -EMSGSIZE;

    g_mutex_lock(&usip->lock);

    if ((gsize)usip->write_index + size > MTK_USIP_SPEECH_PARAM_SIZE)
        usip->write_index = 0;

    start = usip->write_index;
    memcpy(usip->layout->speech_param + start, data, size);
    usip->write_index = (guint32)(start + size);
    if (usip->write_index == MTK_USIP_SPEECH_PARAM_SIZE)
        usip->write_index = 0;

    g_debug("USIP TX speech param bytes=%zu start=%u next=%u",
            size, start, usip->write_index);
    ret = set_control(usip->card,
                      usip->write_index_control,
                      usip->write_index);
    if (ret == 0)
        *write_index = start;

    g_mutex_unlock(&usip->lock);
    return ret;
}
