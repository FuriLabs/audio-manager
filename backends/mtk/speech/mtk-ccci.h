/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MTK_CCCI_H
#define MTK_CCCI_H

#include <glib.h>

#include "backends/mtk/speech/mtk-speech-protocol.h"

typedef struct MtkCcci MtkCcci;

/**
 * Parsed MediaTek AP/modem speech mailbox message.
 */
typedef struct {
    guint16 msg_id;         /**< MediaTek speech message ID */
    guint16 param_16bit;    /**< 16 bit mailbox parameter */
    guint32 param_32bit;    /**< 32 bit mailbox parameter */

    gboolean has_payload;   /**< TRUE when payload data accompanies the mailbox */
    guint16 payload_type;   /**< Shared buffer data type */
    guint16 payload_size;   /**< Payload bytes in this message */
    guint16 payload_index;  /**< Shared memory or fragmented payload index */
    guint16 payload_total;  /**< Total fragmented payload size when applicable */
    const guint8 *payload;  /**< Borrowed payload bytes valid during callback */
} MtkCcciMessage;

/**
 * Callback for decoded modem speech messages.
 */
typedef void (*MtkCcciMessageFunc)(MtkCcci *ccci,
                                   const MtkCcciMessage *message,
                                   gpointer user_data);

/**
 * Callback for CCCI HUP/ERR conditions that require recovery.
 */
typedef void (*MtkCcciFaultFunc)(MtkCcci *ccci,
                                 GIOCondition condition,
                                 gpointer user_data);

/**
 * Open the MediaTek CCCI speech control device and attach it to a main context.
 */
MtkCcci *
mtk_ccci_new(const gchar *device,
             GMainContext *main_context,
             MtkCcciMessageFunc callback,
             MtkCcciFaultFunc fault_callback,
             gpointer user_data,
             GError **error);

/**
 * Close and free the CCCI speech control channel.
 */
void
mtk_ccci_free(MtkCcci *ccci);

/**
 * Return TRUE when the CCCI control device is currently open.
 */
gboolean
mtk_ccci_is_connected(MtkCcci *ccci);

/**
 * Reopen a failed CCCI control device after modem recovery.
 */
gint
mtk_ccci_reopen(MtkCcci *ccci,
                GError **error);

/**
 * Query the CCCI driver's modem ready state.
 */
gboolean
mtk_ccci_modem_ready(MtkCcci *ccci);

/**
 * Send a mailbox only AP to modem speech command.
 */
gint
mtk_ccci_send_mailbox(MtkCcci *ccci,
                      guint16 msg_id,
                      guint16 param_16bit,
                      guint32 param_32bit);

/**
 * Send a speech command carrying an inline payload.
 */
gint
mtk_ccci_send_payload(MtkCcci *ccci,
                      guint16 msg_id,
                      guint16 payload_type,
                      gconstpointer payload,
                      guint16 payload_size);

#endif /* MTK_CCCI_H */
