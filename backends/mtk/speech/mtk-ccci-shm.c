/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "backends/mtk/speech/mtk-ccci-shm.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <audio-manager/audio-manager-error.h>

#include "common/alsa/alsa-control.h"

#define MTK_CCCI_IOC_MAGIC 'C'
#define MTK_CCCI_IOC_SMEM_BASE _IOR(MTK_CCCI_IOC_MAGIC, 48, guint)
#define MTK_CCCI_IOC_SMEM_LEN _IOR(MTK_CCCI_IOC_MAGIC, 49, guint)

#define MTK_CCCI_SHM_GUARD_SIZE 32U
#define MTK_CCCI_SHM_SPEECH_PARAM_SIZE (12U * 1024U)
#define MTK_CCCI_SHM_AP_DATA_SIZE (8U * 1024U)
#define MTK_CCCI_SHM_MD_DATA_SIZE ((32U * 1024U) - 160U)
#define MTK_CCCI_SHM_FRAME_RESERVE 16U

#define MTK_CCCI_AP_PAYLOAD_SYNC 0xA2A2U
#define MTK_CCCI_MD_PAYLOAD_SYNC 0x1234U
#define MTK_CCCI_MD_HEADER_SIZE 10U

#define MTK_CCCI_SHM_AP_FLAG_READY (1U << 0)
#define MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE (1U << 1)
#define MTK_CCCI_SHM_MD_FLAG_SPH_PARAM_READ (1U << 1)

typedef struct {
    guint32 offset;
    guint32 size;
    guint32 read_idx;
    guint32 write_idx;
} MtkShmRegionInfo;

typedef struct {
    MtkShmRegionInfo speech_param;
    MtkShmRegionInfo ap_data;
    MtkShmRegionInfo md_data;
    MtkShmRegionInfo reserved_1;
    MtkShmRegionInfo reserved_2;
} MtkCcciShmRegions;

typedef struct {
    guint8 guard_pre[MTK_CCCI_SHM_GUARD_SIZE];
    guint32 ap_flag;
    guint32 md_flag;
    MtkCcciShmRegions regions;
    guint16 md_version;
    guint16 reserved;
    guint32 struct_checksum;
    guint8 speech_param[MTK_CCCI_SHM_SPEECH_PARAM_SIZE];
    guint8 ap_data[MTK_CCCI_SHM_AP_DATA_SIZE];
    guint8 md_data[MTK_CCCI_SHM_MD_DATA_SIZE];
    guint8 guard_post[MTK_CCCI_SHM_GUARD_SIZE];
} MtkCcciShmLayout;

struct MtkCcciShm {
    gint fd;
    gpointer mapping;
    gsize mapping_size;
    MtkCcciShmLayout *layout;
    AudioAlsaCard *card;
    gchar *init_control;
    GMutex lock;
};

static inline void
ccci_shm_memory_barrier(void)
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("dsb ish" : : : "memory");
#else
    __sync_synchronize();
#endif
}

static void
ccci_shm_memset(gpointer destination, guint8 value, gsize size)
{
    volatile guint8 *dest = destination;
    gsize i;

    for (i = 0; i < size; i++) {
        dest[i] = value;
        __asm__ __volatile__("" : : : "memory");
    }

    ccci_shm_memory_barrier();
}

static void
ccci_shm_copy_to_mapping(gpointer destination, gconstpointer source, gsize size)
{
    volatile guint8 *dest = destination;
    const guint8 *src = source;
    gsize i;

    for (i = 0; i < size; i++) {
        dest[i] = src[i];
        __asm__ __volatile__("" : : : "memory");
    }

    ccci_shm_memory_barrier();
}

static void
ccci_shm_copy_from_mapping(gpointer destination, gconstpointer source, gsize size)
{
    guint8 *dest = destination;
    const volatile guint8 *src = source;
    gsize i;

    for (i = 0; i < size; i++) {
        dest[i] = src[i];
        __asm__ __volatile__("" : : : "memory");
    }

    ccci_shm_memory_barrier();
}

static gint
set_control(AudioAlsaCard *card, const gchar *name, guint value)
{
    gchar string[32];
    gint ret;

    if (card == NULL || name == NULL || *name == '\0')
        return 0;
    g_snprintf(string, sizeof(string), "%u", value);
    g_debug("CCCI SHM mixer TX %s=%u", name, value);
    ret = audio_alsa_control_set_from_string(card, name, string);
    g_debug("CCCI SHM mixer answer %s ret=%d", name, ret);
    return ret;
}

static void
region_init(MtkShmRegionInfo *region, gconstpointer base, gconstpointer data, gsize size)
{
    region->offset = (guint32)((const guint8 *)data - (const guint8 *)base);
    region->size = (guint32)size;
    region->read_idx = 0;
    region->write_idx = 0;
}

static guint32
region_data_count(MtkShmRegionInfo *region)
{
    if (region == NULL || region->size == 0)
        return 0;
    if (region->read_idx >= region->size)
        region->read_idx %= region->size;
    if (region->write_idx >= region->size)
        region->write_idx %= region->size;
    return region->write_idx >= region->read_idx ?
           region->write_idx - region->read_idx :
           region->size - (region->read_idx - region->write_idx);
}

static guint32
region_free_space(MtkShmRegionInfo *region)
{
    guint32 count;
    if (region == NULL)
        return 0;
    count = region->size - region_data_count(region);
    return count >= MTK_CCCI_SHM_FRAME_RESERVE ?
           count - MTK_CCCI_SHM_FRAME_RESERVE : 0;
}

static gint
region_write(MtkCcciShm *shm, MtkShmRegionInfo *region,
             gconstpointer data, guint32 count)
{
    guint8 *buffer;
    guint32 to_end;

    if (shm == NULL || region == NULL || data == NULL || region->size == 0)
        return -EINVAL;
    if (count > region_free_space(region))
        return -ENOMEM;
    buffer = (guint8 *)shm->layout + region->offset;
    g_debug("CCCI SHM TX region offset=%u size=%u read=%u write=%u bytes=%u",
            region->offset, region->size, region->read_idx, region->write_idx, count);
    to_end = region->size - region->write_idx;
    if (count <= to_end) {
        ccci_shm_copy_to_mapping(buffer + region->write_idx, data, count);
        region->write_idx += count;
        if (region->write_idx == region->size)
            region->write_idx = 0;
    } else {
        ccci_shm_copy_to_mapping(buffer + region->write_idx, data, to_end);
        ccci_shm_copy_to_mapping(buffer, (const guint8 *)data + to_end, count - to_end);
        region->write_idx = count - to_end;
    }
    g_debug("CCCI SHM TX answer new_write=%u", region->write_idx);
    return 0;
}

static gint
region_read(MtkCcciShm *shm, MtkShmRegionInfo *region,
            gpointer data, guint32 count)
{
    guint8 *buffer;
    guint32 to_end;

    if (shm == NULL || region == NULL || data == NULL || region->size == 0)
        return -EINVAL;
    if (count > region_data_count(region))
        return -ENOMEM;
    buffer = (guint8 *)shm->layout + region->offset;
    g_debug("CCCI SHM RX region offset=%u size=%u read=%u write=%u bytes=%u",
            region->offset, region->size, region->read_idx, region->write_idx, count);
    to_end = region->size - region->read_idx;
    if (count <= to_end) {
        ccci_shm_copy_from_mapping(data, buffer + region->read_idx, count);
        region->read_idx += count;
        if (region->read_idx == region->size)
            region->read_idx = 0;
    } else {
        ccci_shm_copy_from_mapping(data, buffer + region->read_idx, to_end);
        ccci_shm_copy_from_mapping((guint8 *)data + to_end, buffer, count - to_end);
        region->read_idx = count - to_end;
    }
    g_debug("CCCI SHM RX answer new_read=%u", region->read_idx);
    return 0;
}

static gint
ccci_shm_format(MtkCcciShm *shm)
{
    MtkCcciShmLayout *layout;
    if (shm == NULL || shm->layout == NULL)
        return -EINVAL;
    layout = shm->layout;
    ccci_shm_memset(layout->guard_pre, 0x0a, sizeof(layout->guard_pre));
    ccci_shm_memset(layout->guard_post, 0x0a, sizeof(layout->guard_post));
    layout->ap_flag = 0;
    layout->md_flag = 0;
    region_init(&layout->regions.speech_param, layout, layout->speech_param,
                sizeof(layout->speech_param));
    region_init(&layout->regions.ap_data, layout, layout->ap_data,
                sizeof(layout->ap_data));
    region_init(&layout->regions.md_data, layout, layout->md_data,
                sizeof(layout->md_data));
    ccci_shm_memset(&layout->regions.reserved_1, 0, sizeof(layout->regions.reserved_1));
    ccci_shm_memset(&layout->regions.reserved_2, 0, sizeof(layout->regions.reserved_2));
    layout->reserved = 0;
    layout->struct_checksum = (guint32)G_STRUCT_OFFSET(MtkCcciShmLayout, struct_checksum);
    ccci_shm_memset(layout->speech_param, 0, sizeof(layout->speech_param));
    ccci_shm_memset(layout->ap_data, 0, sizeof(layout->ap_data));
    ccci_shm_memset(layout->md_data, 0, sizeof(layout->md_data));
    layout->ap_flag |= MTK_CCCI_SHM_AP_FLAG_READY;
    g_debug("CCCI SHM formatted speech=%u ap=%u md=%u checksum=%u",
            layout->regions.speech_param.size, layout->regions.ap_data.size,
            layout->regions.md_data.size, layout->struct_checksum);
    return set_control(shm->card, shm->init_control, 1);
}

MtkCcciShm *
mtk_ccci_shm_new(const gchar *device, AudioAlsaCard *card,
                 const gchar *init_control, GError **error)
{
    MtkCcciShm *shm;
    guint address = 0;
    guint length = 0;
    gint ret;

    shm = g_new0(MtkCcciShm, 1);
    shm->fd = -1;
    shm->card = card;
    shm->init_control = g_strdup(init_control);
    g_mutex_init(&shm->lock);
    shm->fd = open(device, O_RDWR | O_CLOEXEC);
    if (shm->fd < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to open MediaTek CCCI shared memory device '%s': %s",
                    device, g_strerror(errno));
        mtk_ccci_shm_free(shm);
        return NULL;
    }

    g_debug("CCCI SHM ioctl SMEM_BASE fd=%d request=0x%lx", shm->fd,
            (gulong)MTK_CCCI_IOC_SMEM_BASE);
    ret = ioctl(shm->fd, MTK_CCCI_IOC_SMEM_BASE, &address);
    g_debug("CCCI SHM ioctl SMEM_BASE answer=%d address=0x%x errno=%d (%s)",
            ret, address, ret < 0 ? errno : 0, ret < 0 ? g_strerror(errno) : "ok");
    if (ret < 0)
        goto ioctl_fail;

    g_debug("CCCI SHM ioctl SMEM_LEN fd=%d request=0x%lx", shm->fd,
            (gulong)MTK_CCCI_IOC_SMEM_LEN);
    ret = ioctl(shm->fd, MTK_CCCI_IOC_SMEM_LEN, &length);
    g_debug("CCCI SHM ioctl SMEM_LEN answer=%d length=%u errno=%d (%s)",
            ret, length, ret < 0 ? errno : 0, ret < 0 ? g_strerror(errno) : "ok");
    if (ret < 0)
        goto ioctl_fail;

    if (length < sizeof(MtkCcciShmLayout)) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "MediaTek CCCI shared memory is too small (%u bytes)", length);
        mtk_ccci_shm_free(shm);
        return NULL;
    }
    shm->mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        shm->fd, 0);
    if (shm->mapping == MAP_FAILED) {
        shm->mapping = NULL;
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to map MediaTek CCCI shared memory '%s': %s",
                    device, g_strerror(errno));
        mtk_ccci_shm_free(shm);
        return NULL;
    }
    shm->mapping_size = length;
    shm->layout = shm->mapping;
    g_debug("CCCI SHM mmap answer=%p length=%zu kernel_base=0x%x",
            shm->mapping, shm->mapping_size, address);
    if (ccci_shm_format(shm) < 0) {
        g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                    "Unable to initialize MediaTek CCCI speech shared memory");
        mtk_ccci_shm_free(shm);
        return NULL;
    }
    return shm;

ioctl_fail:
    g_set_error(error, AUDIO_MANAGER_ERROR, AUDIO_MANAGER_ERROR_FAILED,
                "Unable to query MediaTek CCCI shared memory '%s': %s",
                device, g_strerror(errno));
    mtk_ccci_shm_free(shm);
    return NULL;
}

void
mtk_ccci_shm_free(MtkCcciShm *shm)
{
    if (shm == NULL)
        return;
    if (shm->mapping != NULL)
        munmap(shm->mapping, shm->mapping_size);
    if (shm->fd >= 0)
        close(shm->fd);
    g_free(shm->init_control);
    g_mutex_clear(&shm->lock);
    g_free(shm);
}

gint
mtk_ccci_shm_reset(MtkCcciShm *shm)
{
    MtkCcciShmLayout *layout;
    if (shm == NULL || shm->layout == NULL)
        return -EINVAL;
    g_mutex_lock(&shm->lock);
    layout = shm->layout;
    layout->ap_flag |= MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
    if ((layout->md_flag & MTK_CCCI_SHM_MD_FLAG_SPH_PARAM_READ) != 0) {
        layout->ap_flag &= ~MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
        g_mutex_unlock(&shm->lock);
        return -EBUSY;
    }
    layout->regions.speech_param.read_idx = 0;
    layout->regions.speech_param.write_idx = 0;
    layout->regions.ap_data.read_idx = 0;
    layout->regions.ap_data.write_idx = 0;
    layout->regions.md_data.read_idx = 0;
    layout->regions.md_data.write_idx = 0;
    layout->ap_flag &= ~MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
    g_debug("CCCI SHM reset all ring indexes");
    g_mutex_unlock(&shm->lock);
    return 0;
}

gint
mtk_ccci_shm_write_speech_params(MtkCcciShm *shm,
                                 gconstpointer data,
                                 gsize size,
                                 guint32 *write_index)
{
    MtkShmRegionInfo *region;
    gint ret;
    if (shm == NULL || data == NULL || write_index == NULL)
        return -EINVAL;
    if (size > G_MAXUINT32)
        return -EMSGSIZE;
    g_mutex_lock(&shm->lock);
    shm->layout->ap_flag |= MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
    if ((shm->layout->md_flag & MTK_CCCI_SHM_MD_FLAG_SPH_PARAM_READ) != 0) {
        shm->layout->ap_flag &= ~MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
        g_mutex_unlock(&shm->lock);
        return -EBUSY;
    }
    region = &shm->layout->regions.speech_param;
    if (size > region_free_space(region)) {
        shm->layout->ap_flag &= ~MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
        g_mutex_unlock(&shm->lock);
        return -ENOMEM;
    }
    *write_index = region->write_idx;
    ret = region_write(shm, region, data, (guint32)size);
    shm->layout->ap_flag &= ~MTK_CCCI_SHM_AP_FLAG_SPH_PARAM_WRITE;
    g_debug("CCCI SHM TX speech param bytes=%zu start=%u ret=%d",
            size, *write_index, ret);
    g_mutex_unlock(&shm->lock);
    return ret;
}

gint
mtk_ccci_shm_read_md_data(MtkCcciShm *shm,
                          gpointer data,
                          guint16 *data_type,
                          guint16 *data_size,
                          guint16 payload_length,
                          guint32 read_index)
{
    MtkShmRegionInfo *region;
    guint16 header[5] = { 0 };
    guint16 expected_data;
    gint ret;
    if (shm == NULL || data == NULL || data_type == NULL || data_size == NULL)
        return -EINVAL;
    if (payload_length < MTK_CCCI_MD_HEADER_SIZE)
        return -EINVAL;
    expected_data = (guint16)(payload_length - MTK_CCCI_MD_HEADER_SIZE);
    if (expected_data > *data_size) {
        *data_size = 0;
        return -ENOMEM;
    }
    g_mutex_lock(&shm->lock);
    region = &shm->layout->regions.md_data;
    if (payload_length > region_data_count(region)) {
        *data_size = 0;
        g_mutex_unlock(&shm->lock);
        return -ENOMEM;
    }
    if (read_index != region->read_idx) {
        g_debug("CCCI SHM RX resync read index kernel=%u local=%u",
                read_index, region->read_idx);
        region->read_idx = region->size != 0 ? read_index % region->size : 0;
    }
    ret = region_read(shm, region, header, sizeof(header));
    if (ret < 0)
        goto out;
    if (header[0] != MTK_CCCI_MD_PAYLOAD_SYNC || header[2] != expected_data ||
        header[3] != header[4]) {
        g_warning("CCCI SHM RX invalid MD header sync=0x%04x type=%u size=%u index=%u total=%u",
                  header[0], header[1], header[2], header[3], header[4]);
        *data_size = 0;
        ret = -EINVAL;
        goto out;
    }
    *data_type = header[1];
    ret = region_read(shm, region, data, expected_data);
    if (ret == 0)
        *data_size = expected_data;
    else
        *data_size = 0;
    g_debug("CCCI SHM RX MD data type=%u data=%u payload=%u index=%u ret=%d",
            *data_type, *data_size, payload_length, read_index, ret);
out:
    g_mutex_unlock(&shm->lock);
    return ret;
}
