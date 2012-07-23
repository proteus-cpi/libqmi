/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libqmi-glib -- GLib/GIO based library to control QMI devices
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2012 Aleksander Morgado <aleksander@lanedo.com>
 */

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <gio/gio.h>

#include "qmi-device.h"
#include "qmi-message.h"
#include "qmi-ctl.h"
#include "qmi-dms.h"
#include "qmi-wds.h"
#include "qmi-utils.h"
#include "qmi-error-types.h"
#include "qmi-enum-types.h"

static void async_initable_iface_init (GAsyncInitableIface *iface);

G_DEFINE_TYPE_EXTENDED (QmiDevice, qmi_device, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init))

enum {
    PROP_0,
    PROP_FILE,
    PROP_CLIENT_CTL,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _QmiDevicePrivate {
    /* File */
    GFile *file;
    gchar *path;
    gchar *path_display;

    /* Implicit CTL client */
    QmiClientCtl *client_ctl;

    /* Supported services */
    GArray *supported_services;

    /* I/O channel, set when the file is open */
    GIOChannel *iochannel;
    guint watch_id;
    GByteArray *response;

    /* HT to keep track of ongoing transactions */
    GHashTable *transactions;

    /* HT of clients that want to get indications */
    GHashTable *registered_clients;
};

#define BUFFER_SIZE 2048

/*****************************************************************************/
/* Message transactions (private) */

typedef struct {
    QmiMessage *message;
    GSimpleAsyncResult *result;
    guint timeout_id;
} Transaction;

static Transaction *
transaction_new (QmiDevice *self,
                 QmiMessage *message,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    Transaction *tr;

    tr = g_slice_new0 (Transaction);
    tr->message = qmi_message_ref (message);
    tr->result = g_simple_async_result_new (G_OBJECT (self),
                                            callback,
                                            user_data,
                                            transaction_new);

    return tr;
}

static void
transaction_complete_and_free (Transaction *tr,
                               QmiMessage *reply,
                               const GError *error)
{
    g_assert (reply != NULL || error != NULL);

    if (tr->timeout_id)
        g_source_remove (tr->timeout_id);

    if (reply)
        g_simple_async_result_set_op_res_gpointer (tr->result,
                                                   qmi_message_ref (reply),
                                                   (GDestroyNotify)qmi_message_unref);
    else
        g_simple_async_result_set_from_error (tr->result, error);

    g_simple_async_result_complete_in_idle (tr->result);
    g_object_unref (tr->result);
    qmi_message_unref (tr->message);
    g_slice_free (Transaction, tr);
}

static inline gpointer
build_transaction_key (QmiMessage *message)
{
    gpointer key;
    guint8 service;
    guint8 client_id;
    guint16 transaction_id;

    service = (guint8)qmi_message_get_service (message);
    client_id = qmi_message_get_client_id (message);
    transaction_id = qmi_message_get_transaction_id (message);

    /* We're putting a 32 bit value into a gpointer */
    key = GUINT_TO_POINTER ((((service << 8) | client_id) << 16) | transaction_id);

#ifdef MESSAGE_ENABLE_TRACE
    {
        gchar *hex;

        hex = qmi_utils_str_hex (&key, sizeof (key), ':');
        g_debug ("KEY: %s", hex);
        g_free (hex);

        hex = qmi_utils_str_hex (&service, sizeof (service), ':');
        g_debug ("  Service: %s", hex);
        g_free (hex);

        hex = qmi_utils_str_hex (&client_id, sizeof (client_id), ':');
        g_debug ("  Client ID: %s", hex);
        g_free (hex);

        hex = qmi_utils_str_hex (&transaction_id, sizeof (transaction_id), ':');
        g_debug ("  Transaction ID: %s", hex);
        g_free (hex);
    }
#endif /* MESSAGE_ENABLE_TRACE */

    return key;
}

static Transaction *
device_release_transaction (QmiDevice *self,
                            gpointer key)
{
    Transaction *tr = NULL;

    if (self->priv->transactions) {
        tr = g_hash_table_lookup (self->priv->transactions, key);
        if (tr)
            /* If found, remove it from the HT */
            g_hash_table_remove (self->priv->transactions, key);
    }

    return tr;
}

typedef struct {
    QmiDevice *self;
    gpointer key;
} TransactionTimeoutContext;

static void
transaction_timeout_context_free (TransactionTimeoutContext *ctx)
{
    g_slice_free (TransactionTimeoutContext, ctx);
}

static gboolean
transaction_timed_out (TransactionTimeoutContext *ctx)
{
    Transaction *tr;
    GError *error = NULL;

    tr = device_release_transaction (ctx->self, ctx->key);
    tr->timeout_id = 0;

    /* Complete transaction with a timeout error */
    error = g_error_new (QMI_CORE_ERROR,
                         QMI_CORE_ERROR_TIMEOUT,
                         "Transaction timed out");
    transaction_complete_and_free (tr, NULL, error);
    g_error_free (error);

    return FALSE;
}

static void
device_store_transaction (QmiDevice *self,
                          Transaction *tr,
                          guint timeout)
{
    TransactionTimeoutContext *timeout_ctx;
    gpointer key;

    if (G_UNLIKELY (!self->priv->transactions))
        self->priv->transactions = g_hash_table_new (g_direct_hash,
                                                     g_direct_equal);

    key = build_transaction_key (tr->message);
    g_hash_table_insert (self->priv->transactions, key, tr);

    /* Once it gets into the HT, setup the timeout */
    timeout_ctx = g_slice_new (TransactionTimeoutContext);
    timeout_ctx->self = self;
    timeout_ctx->key = key; /* valid as long as the transaction is in the HT */
    tr->timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                 timeout,
                                                 (GSourceFunc)transaction_timed_out,
                                                 timeout_ctx,
                                                 (GDestroyNotify)transaction_timeout_context_free);
}

static Transaction *
device_match_transaction (QmiDevice *self,
                          QmiMessage *message)
{
    /* msg can be either the original message or the response */
    return device_release_transaction (self, build_transaction_key (message));
}

/*****************************************************************************/
/* Version info checks (private) */

static const QmiMessageCtlGetVersionInfoOutputServiceListService *
find_service_version_info (QmiDevice *self,
                           QmiService service)
{
    guint i;

    for (i = 0; i < self->priv->supported_services->len; i++) {
        const QmiMessageCtlGetVersionInfoOutputServiceListService *info;

        info = &g_array_index (self->priv->supported_services,
                               QmiMessageCtlGetVersionInfoOutputServiceListService,
                               i);

        if (service == info->service)
            return info;
    }

    return NULL;
}

static gboolean
check_service_supported (QmiDevice *self,
                         QmiService service)
{
    /* If we didn't check supported services, just assume it is supported */
    if (!self->priv->supported_services) {
        g_debug ("[%s] Assuming service '%s' is supported...",
                 self->priv->path_display,
                 qmi_service_get_string (service));
        return TRUE;
    }

    return !!find_service_version_info (self, service);
}

static gboolean
check_message_supported (QmiDevice *self,
                         QmiMessage *message,
                         GError **error)
{
    const QmiMessageCtlGetVersionInfoOutputServiceListService *info;
    guint major = 0;
    guint minor = 0;

    /* If we didn't check supported services, just assume it is supported */
    if (!self->priv->supported_services)
        return TRUE;

    /* For CTL, we assume all are supported */
    if (qmi_message_get_service (message) == QMI_SERVICE_CTL)
        return TRUE;

    /* If we cannot get in which version this message was introduced, we'll just
     * assume it's supported */
    if (!qmi_message_get_version_introduced (message, &major, &minor))
        return TRUE;

    /* Get version info. It MUST exist because we allowed creating a client
     * of this service type */
    info = find_service_version_info (self, qmi_message_get_service (message));
    g_assert (info != NULL);
    g_assert (info->service == qmi_message_get_service (message));

    /* If the version of the message is greater than the version of the service,
     * report unsupported */
    if (major > info->major_version ||
        (major == info->major_version &&
         minor > info->minor_version)) {
        g_set_error (error,
                     QMI_CORE_ERROR,
                     QMI_CORE_ERROR_UNSUPPORTED,
                     "QMI service '%s' version '%u.%u' required, got version '%u.%u'",
                     qmi_service_get_string (qmi_message_get_service (message)),
                     major, minor,
                     info->major_version,
                     info->minor_version);
        return FALSE;
    }

    /* Supported! */
    return TRUE;
}

/*****************************************************************************/

/**
 * qmi_device_get_file:
 * @self: a #QmiDevice.
 *
 * Get the #GFile associated with this #QmiDevice.
 *
 * Returns: a #GFile that must be freed with g_object_unref().
 */
GFile *
qmi_device_get_file (QmiDevice *self)
{
    GFile *file = NULL;

    g_return_val_if_fail (QMI_IS_DEVICE (self), NULL);

    g_object_get (G_OBJECT (self),
                  QMI_DEVICE_FILE, &file,
                  NULL);
    return file;
}

/**
 * qmi_device_peek_file:
 * @self: a #QmiDevice.
 *
 * Get the #GFile associated with this #QmiDevice, without increasing the reference count
 * on the returned object.
 *
 * Returns: a #GFile. Do not free the returned object, it is owned by @self.
 */
GFile *
qmi_device_peek_file (QmiDevice *self)
{
    g_return_val_if_fail (QMI_IS_DEVICE (self), NULL);

    return self->priv->file;
}

/**
 * qmi_device_get_path:
 * @self: a #QmiDevice.
 *
 * Get the system path of the underlying QMI device.
 *
 * Returns: the system path of the device.
 */
const gchar *
qmi_device_get_path (QmiDevice *self)
{
    g_return_val_if_fail (QMI_IS_DEVICE (self), NULL);

    return self->priv->path;
}

/**
 * qmi_device_get_path_display:
 * @self: a #QmiDevice.
 *
 * Get the system path of the underlying QMI device in UTF-8.
 *
 * Returns: UTF-8 encoded system path of the device.
 */
const gchar *
qmi_device_get_path_display (QmiDevice *self)
{
    g_return_val_if_fail (QMI_IS_DEVICE (self), NULL);

    return self->priv->path_display;
}

/**
 * qmi_device_is_open:
 * @self: a #QmiDevice.
 *
 * Checks whether the #QmiDevice is open for I/O.
 *
 * Returns: #TRUE if @self is open, #FALSE otherwise.
 */
gboolean
qmi_device_is_open (QmiDevice *self)
{
    g_return_val_if_fail (QMI_IS_DEVICE (self), FALSE);

    return !!self->priv->iochannel;
}

/*****************************************************************************/
/* Register/Unregister clients that want to receive indications */

static gpointer
build_registered_client_key (guint8 cid,
                             QmiService service)
{
    return GUINT_TO_POINTER (((guint8)service << 8) | cid);
}

static gboolean
register_client (QmiDevice *self,
                 QmiClient *client,
                 GError **error)
{
    gpointer key;

    key = build_registered_client_key (qmi_client_get_cid (client),
                                       qmi_client_get_service (client));
    /* Only add the new client if not already registered one with the same CID
     * for the same service */
    if (g_hash_table_lookup (self->priv->registered_clients, key)) {
        g_set_error (error,
                     QMI_CORE_ERROR,
                     QMI_CORE_ERROR_FAILED,
                     "A client with CID '%u' and service '%s' is already registered",
                     qmi_client_get_cid (client),
                     qmi_service_get_string (qmi_client_get_service (client)));
        return FALSE;
    }

    g_hash_table_insert (self->priv->registered_clients,
                         key,
                         g_object_ref (client));
    return TRUE;
}

static void
unregister_client (QmiDevice *self,
                   QmiClient *client)
{
    g_hash_table_remove (self->priv->registered_clients,
                         build_registered_client_key (qmi_client_get_cid (client),
                                                      qmi_client_get_service (client)));
}

/*****************************************************************************/
/* Allocate new client */

typedef struct {
    QmiDevice *self;
    GSimpleAsyncResult *result;
    QmiService service;
    GType client_type;
    guint8 cid;
} AllocateClientContext;

static void
allocate_client_context_complete_and_free (AllocateClientContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_slice_free (AllocateClientContext, ctx);
}

/**
 * qmi_device_allocate_client_finish:
 * @self: a #QmiDevice.
 * @res: a #GAsyncResult.
 * @error: a #GError.
 *
 * Finishes an operation started with qmi_device_allocate_client().
 *
 * Returns: a newly allocated #QmiClient, or #NULL if @error is set.
 */
QmiClient *
qmi_device_allocate_client_finish (QmiDevice *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return QMI_CLIENT (g_object_ref (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res))));
}

static void
build_client_object (AllocateClientContext *ctx)
{
    QmiClient *client;
    GError *error = NULL;

    /* We now have a proper CID for the client, we should be able to create it
     * right away */
    client = g_object_new (ctx->client_type,
                           QMI_CLIENT_DEVICE,  ctx->self,
                           QMI_CLIENT_SERVICE, ctx->service,
                           QMI_CLIENT_CID,     ctx->cid,
                           NULL);

    /* Register the client to get indications */
    if (!register_client (ctx->self, client, &error)) {
        g_prefix_error (&error,
                        "Cannot register new client with CID '%u' and service '%s'",
                        ctx->cid,
                        qmi_service_get_string (ctx->service));
        g_simple_async_result_take_error (ctx->result, error);
        allocate_client_context_complete_and_free (ctx);
        g_object_unref (client);
        return;
    }

    g_debug ("[%s] Registered '%s' client with ID '%u'",
             ctx->self->priv->path_display,
             qmi_service_get_string (ctx->service),
             ctx->cid);

    /* Client created and registered, complete successfully */
    g_simple_async_result_set_op_res_gpointer (ctx->result,
                                               client,
                                               (GDestroyNotify)g_object_unref);
    allocate_client_context_complete_and_free (ctx);
}

static void
allocate_cid_ready (QmiClientCtl *client_ctl,
                    GAsyncResult *res,
                    AllocateClientContext *ctx)
{
    QmiMessageCtlAllocateCidOutput *output;
    QmiService service;
    guint8 cid;
    GError *error = NULL;

    /* Check result of the async operation */
    output = qmi_client_ctl_allocate_cid_finish (client_ctl, res, &error);
    if (!output) {
        g_prefix_error (&error, "CID allocation failed in the CTL client: ");
        g_simple_async_result_take_error (ctx->result, error);
        allocate_client_context_complete_and_free (ctx);
        return;
    }

    /* Check result of the QMI operation */
    if (!qmi_message_ctl_allocate_cid_output_get_result (output, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        allocate_client_context_complete_and_free (ctx);
        qmi_message_ctl_allocate_cid_output_unref (output);
        return;
    }

    /* Allocation info is mandatory when result is success */
    g_assert (qmi_message_ctl_allocate_cid_output_get_allocation_info (output, &service, &cid, NULL));

    if (service != ctx->service) {
        g_simple_async_result_set_error (
            ctx->result,
            QMI_CORE_ERROR,
            QMI_CORE_ERROR_FAILED,
            "CID allocation failed in the CTL client: "
            "Service mismatch (requested '%s', got '%s')",
            qmi_service_get_string (ctx->service),
            qmi_service_get_string (service));
        allocate_client_context_complete_and_free (ctx);
        qmi_message_ctl_allocate_cid_output_unref (output);
        return;
    }

    ctx->cid = cid;
    build_client_object (ctx);
    qmi_message_ctl_allocate_cid_output_unref (output);
}

/**
 * qmi_device_allocate_client:
 * @self: a #QmiDevice.
 * @service: a valid #QmiService.
 * @cid: a valid client ID, or #QMI_CID_NONE.
 * @timeout: maximum time to wait.
 * @cancellable: optional #GCancellable object, #NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the operation is finished.
 * @user_data: the data to pass to callback function.
 *
 * Asynchronously allocates a new #QmiClient in @self.
 *
 * If #QMI_CID_NONE is given in @cid, a new client ID will be allocated;
 * otherwise a client with the given @cid will be generated.
 *
 * When the operation is finished @callback will be called. You can then call
 * qmi_device_allocate_client_finish() to get the result of the operation.
 *
 * Note: Clients for the #QMI_SERVICE_CTL cannot be created with this method;
 * instead get/peek the implicit one from @self.
 */
void
qmi_device_allocate_client (QmiDevice *self,
                            QmiService service,
                            guint8 cid,
                            guint timeout,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    AllocateClientContext *ctx;

    g_return_if_fail (QMI_IS_DEVICE (self));
    g_return_if_fail (service != QMI_SERVICE_UNKNOWN);

    ctx = g_slice_new0 (AllocateClientContext);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             qmi_device_allocate_client);
    ctx->service = service;

    /* Check if the requested service is supported by the device */
    if (!check_service_supported (self, service)) {
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_UNSUPPORTED,
                                         "Service '%s' not supported by the device",
                                         qmi_service_get_string (service));
        allocate_client_context_complete_and_free (ctx);
        return;
    }

    switch (service) {
    case QMI_SERVICE_CTL:
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_INVALID_ARGS,
                                         "Cannot create additional clients for the CTL service");
        allocate_client_context_complete_and_free (ctx);
        return;

    case QMI_SERVICE_DMS:
        ctx->client_type = QMI_TYPE_CLIENT_DMS;
        break;

    case QMI_SERVICE_WDS:
        ctx->client_type = QMI_TYPE_CLIENT_WDS;
        break;

    default:
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_INVALID_ARGS,
                                         "Clients for service '%s' not yet supported",
                                         qmi_service_get_string (service));
        allocate_client_context_complete_and_free (ctx);
        return;
    }

    /* Allocate a new CID for the client to be created */
    if (cid == QMI_CID_NONE) {
        QmiMessageCtlAllocateCidInput *input;

        input = qmi_message_ctl_allocate_cid_input_new ();
        qmi_message_ctl_allocate_cid_input_set_service (input, ctx->service, NULL);

        g_debug ("[%s] Allocating new client ID...",
                 ctx->self->priv->path_display);
        qmi_client_ctl_allocate_cid (self->priv->client_ctl,
                                     input,
                                     timeout,
                                     cancellable,
                                     (GAsyncReadyCallback)allocate_cid_ready,
                                     ctx);

        qmi_message_ctl_allocate_cid_input_unref (input);
        return;
    }

    /* Reuse the given CID */
    g_debug ("[%s] Reusing client CID '%u'...",
             ctx->self->priv->path_display,
             cid);
    ctx->cid = cid;
    build_client_object (ctx);
}

/*****************************************************************************/
/* Release client */

typedef struct {
    QmiClient *client;
    GSimpleAsyncResult *result;
} ReleaseClientContext;

static void
release_client_context_complete_and_free (ReleaseClientContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_slice_free (ReleaseClientContext, ctx);
}

/**
 * qmi_device_release_client_finish:
 * @self: a #QmiDevice.
 * @res: a #GAsyncResult.
 * @error: a #GError.
 *
 * Finishes an operation started with qmi_device_release_client().
 *
 * Note that even if the release operation returns an error, the client should
 * anyway be considered released, and shouldn't be used afterwards.
 *
 * Returns: #TRUE if successful, or #NULL if @error is set.
 */
gboolean
qmi_device_release_client_finish (QmiDevice *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
client_ctl_release_cid_ready (QmiClientCtl *client_ctl,
                              GAsyncResult *res,
                              ReleaseClientContext *ctx)
{
    GError *error = NULL;
    QmiMessageCtlReleaseCidOutput *output;

    /* Note: even if we return an error, the client is to be considered
     * released! (so shouldn't be used) */

    /* Check result of the async operation */
    output = qmi_client_ctl_release_cid_finish (client_ctl, res, &error);
    if (!output) {
        g_simple_async_result_take_error (ctx->result, error);
        release_client_context_complete_and_free (ctx);
        return;
    }

    /* Check result of the QMI operation */
    if (!qmi_message_ctl_release_cid_output_get_result (output, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        release_client_context_complete_and_free (ctx);
        qmi_message_ctl_release_cid_output_unref (output);
        return;
    }

    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    release_client_context_complete_and_free (ctx);
    qmi_message_ctl_release_cid_output_unref (output);
}

/**
 * qmi_device_release_client:
 * @self: a #QmiDevice.
 * @client: the #QmiClient to release.
 * @flags: mask of #QmiDeviceReleaseClientFlags specifying how the client should be released.
 * @timeout: maximum time to wait.
 * @cancellable: optional #GCancellable object, #NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the operation is finished.
 * @user_data: the data to pass to callback function.
 *
 * Asynchronously releases the #QmiClient from the #QmiDevice.
 *
 * Once the #QmiClient has been released, it cannot be used any more to
 * perform operations.
 *
 *
 * When the operation is finished @callback will be called. You can then call
 * qmi_device_release_client_finish() to get the result of the operation.
 */
void
qmi_device_release_client (QmiDevice *self,
                           QmiClient *client,
                           QmiDeviceReleaseClientFlags flags,
                           guint timeout,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    ReleaseClientContext *ctx;
    QmiService service;
    guint8 cid;

    g_return_if_fail (QMI_IS_DEVICE (self));
    g_return_if_fail (QMI_IS_CLIENT (client));

    /* The CTL client should not have been created out of the QmiDevice */
    g_assert (qmi_client_get_service (client) != QMI_SERVICE_CTL);

    /* NOTE! The operation must not take a reference to self, or we won't be
     * able to use it implicitly from our dispose() */

    ctx = g_slice_new0 (ReleaseClientContext);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             qmi_device_release_client);

    cid = qmi_client_get_cid (client);
    service = (guint8)qmi_client_get_service (client);

    /* Do not try to release an already released client */
    if (cid == QMI_CID_NONE) {
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_INVALID_ARGS,
                                         "Client is already released");
        release_client_context_complete_and_free (ctx);
        return;
    }

    /* Unregister from device */
    unregister_client (self, client);

    g_debug ("[%s] Unregistered '%s' client with ID '%u'",
             self->priv->path_display,
             qmi_service_get_string (service),
             cid);

    /* Reset the contents of the client object, making it unusable */
    g_object_set (client,
                  QMI_CLIENT_CID,     QMI_CID_NONE,
                  QMI_CLIENT_SERVICE, QMI_SERVICE_UNKNOWN,
                  QMI_CLIENT_DEVICE,  NULL,
                  NULL);

    if (flags & QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID) {
        QmiMessageCtlReleaseCidInput *input;

        /* And now, really try to release the CID */
        input = qmi_message_ctl_release_cid_input_new ();
        qmi_message_ctl_release_cid_input_set_release_info (input, service,cid, NULL);

        /* And now, really try to release the CID */
        qmi_client_ctl_release_cid (self->priv->client_ctl,
                                    input,
                                    timeout,
                                    cancellable,
                                    (GAsyncReadyCallback)client_ctl_release_cid_ready,
                                    ctx);

        qmi_message_ctl_release_cid_input_unref (input);
        return;
    }

    /* No need to release the CID, so just done */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    release_client_context_complete_and_free (ctx);
    return;
}

/*****************************************************************************/
/* Set instance ID */

/**
 * qmi_device_set_instance_id_finish:
 * @self: a #QmiDevice.
 * @res: a #GAsyncResult.
 * @link_id: a placeholder for the output #guint16, or #NULL if not required.
 * @error: a #GError.
 *
 * Finishes an operation started with qmi_device_set_instance_id().
 *
 * Returns: #TRUE if successful, #FALSE if @error is set.
 */
gboolean
qmi_device_set_instance_id_finish (QmiDevice *self,
                                   GAsyncResult *res,
                                   guint16 *link_id,
                                   GError **error)
{

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    if (link_id)
        *link_id = ((guint16) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res))));
    return TRUE;
}

static void
set_instance_id_ready (QmiClientCtl *client_ctl,
                       GAsyncResult *res,
                       GSimpleAsyncResult *simple)
{
    QmiMessageCtlSetInstanceIdOutput *output;
    GError *error = NULL;

    /* Check result of the async operation */
    output = qmi_client_ctl_set_instance_id_finish (client_ctl, res, &error);
    if (!output)
        g_simple_async_result_take_error (simple, error);
    else {
        /* Check result of the QMI operation */
        if (!qmi_message_ctl_set_instance_id_output_get_result (output, &error))
            g_simple_async_result_take_error (simple, error);
        else {
            guint16 link_id;

            qmi_message_ctl_set_instance_id_output_get_link_id (output, &link_id, NULL);
            g_simple_async_result_set_op_res_gpointer (simple, GUINT_TO_POINTER ((guint)link_id), NULL);
        }
        qmi_message_ctl_set_instance_id_output_unref (output);
    }

    g_simple_async_result_complete (simple);
}

/**
 * qmi_device_set_instance_id:
 * @self: a #QmiDevice.
 * @instance_id: the instance ID.
 * @timeout: maximum time to wait.
 * @cancellable: optional #GCancellable object, #NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the operation is finished.
 * @user_data: the data to pass to callback function.
 *
 * Sets the instance ID of the #QmiDevice.
 *
 * When the operation is finished @callback will be called. You can then call
 * qmi_device_set_instance_id_finish() to get the result of the operation.
 */
void
qmi_device_set_instance_id (QmiDevice *self,
                            guint8 instance_id,
                            guint timeout,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiMessageCtlSetInstanceIdInput *input;


    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        qmi_device_set_instance_id);

    input = qmi_message_ctl_set_instance_id_input_new ();
    qmi_message_ctl_set_instance_id_input_set_id (
        input,
        instance_id,
        NULL);
    qmi_client_ctl_set_instance_id (self->priv->client_ctl,
                                    input,
                                    timeout,
                                    cancellable,
                                    (GAsyncReadyCallback)set_instance_id_ready,
                                    result);
    qmi_message_ctl_set_instance_id_input_unref (input);
}

/*****************************************************************************/
/* Open device */

typedef struct {
    QmiClient *client;
    QmiMessage *message;
} IdleIndicationContext;

static gboolean
process_indication_idle (IdleIndicationContext *ctx)
{
    g_assert (ctx->client != NULL);
    g_assert (ctx->message != NULL);

    qmi_client_process_indication (ctx->client, ctx->message);

    g_object_unref (ctx->client);
    qmi_message_unref (ctx->message);
    g_slice_free (IdleIndicationContext, ctx);
    return FALSE;
}

static void
report_indication (QmiClient *client,
                   QmiMessage *message)
{
    IdleIndicationContext *ctx;

    /* Setup an idle to Pass the indication down to the client */
    ctx = g_slice_new (IdleIndicationContext);
    ctx->client = g_object_ref (client);
    ctx->message = qmi_message_ref (message);
    g_idle_add ((GSourceFunc)process_indication_idle, ctx);
}

static void
process_message (QmiDevice *self,
                 QmiMessage *message)
{
    GError *error = NULL;

    /* Ensure the read message is valid */
    if (!qmi_message_check (message, &error)) {
        g_warning ("[%s] Invalid QMI message received: %s",
                   self->priv->path_display,
                   error->message);
        g_error_free (error);
        return;
    }

#ifdef MESSAGE_ENABLE_TRACE
    {
        gchar *printable;

        printable = qmi_message_get_printable (message, ">>>>>> ");
        g_debug ("[%s] Received message...\n%s",
                 self->priv->path_display,
                 printable);
        g_free (printable);
    }
#endif /* MESSAGE_ENABLE_TRACE */

    if (qmi_message_is_indication (message)) {
        if (qmi_message_get_client_id (message) == QMI_CID_BROADCAST) {
            GHashTableIter iter;
            gpointer key;
            QmiClient *client;

            g_hash_table_iter_init (&iter, self->priv->registered_clients);
            while (g_hash_table_iter_next (&iter, &key, (gpointer *)&client)) {
                /* For broadcast messages, report them just if the service matches */
                if (qmi_message_get_service (message) == qmi_client_get_service (client))
                    report_indication (client, message);
            }
        } else {
            QmiClient *client;

            client = g_hash_table_lookup (self->priv->registered_clients,
                                          build_registered_client_key (qmi_message_get_client_id (message),
                                                                       qmi_message_get_service (message)));
            if (client)
                report_indication (client, message);
        }

        return;
    }

    if (qmi_message_is_response (message)) {
        Transaction *tr;

        tr = device_match_transaction (self, message);
        if (!tr)
            g_debug ("[%s] No transaction matched in received message",
                     self->priv->path_display);
        else
            /* Report the reply message */
            transaction_complete_and_free (tr, message, NULL);

        return;
    }

    g_debug ("[%s] Message received but it is neither an indication nor a response. Skipping it.",
             self->priv->path_display);
}

static void
parse_response (QmiDevice *self)
{
    do {
        QmiMessage *message;

        /* Every message received must start with the QMUX marker.
         * If it doesn't, we broke framing :-/
         * If we broke framing, an error should be reported and the device
         * should get closed */
        if (self->priv->response->len > 0 &&
            self->priv->response->data[0] != QMI_MESSAGE_QMUX_MARKER) {
            /* TODO: Report fatal error */
            g_warning ("[%s] QMI framing error detected",
                       self->priv->path_display);
            return;
        }

        message = qmi_message_new_from_raw (self->priv->response->data,
                                            self->priv->response->len);
        if (!message)
            /* More data we need */
            return;

        /* Remove the read data from the response buffer */
        g_byte_array_remove_range (self->priv->response,
                                   0,
                                   qmi_message_get_length (message));

        /* Play with the received message */
        process_message (self, message);

        qmi_message_unref (message);
    } while (self->priv->response->len > 0);
}

static gboolean
data_available (GIOChannel *source,
                GIOCondition condition,
                QmiDevice *self)
{
    gsize bytes_read;
    GIOStatus status;
    gchar buffer[BUFFER_SIZE + 1];

    if (condition & G_IO_HUP) {
        g_debug ("[%s] unexpected port hangup!",
                 self->priv->path_display);

        if (self->priv->response &&
            self->priv->response->len)
            g_byte_array_remove_range (self->priv->response, 0, self->priv->response->len);

        qmi_device_close (self, NULL);
        return FALSE;
    }

    if (condition & G_IO_ERR) {
        if (self->priv->response &&
            self->priv->response->len)
            g_byte_array_remove_range (self->priv->response, 0, self->priv->response->len);
        return TRUE;
    }

    /* If not ready yet, prepare the response with default initial size. */
    if (G_UNLIKELY (!self->priv->response))
        self->priv->response = g_byte_array_sized_new (500);

    do {
        GError *error = NULL;

        status = g_io_channel_read_chars (source,
                                          buffer,
                                          BUFFER_SIZE,
                                          &bytes_read,
                                          &error);
        if (status == G_IO_STATUS_ERROR) {
            if (error) {
                g_warning ("[%s] error reading from the IOChannel: '%s'",
                           self->priv->path_display,
                           error->message);
                g_error_free (error);
            }

            /* Port is closed; we're done */
            if (self->priv->watch_id == 0)
                break;
        }

        /* If no bytes read, just let g_io_channel wait for more data */
        if (bytes_read == 0)
            break;

        if (bytes_read > 0)
            g_byte_array_append (self->priv->response, (const guint8 *)buffer, bytes_read);

        /* Try to parse what we already got */
        parse_response (self);

        /* And keep on if we were told to keep on */
    } while (bytes_read == BUFFER_SIZE || status == G_IO_STATUS_AGAIN);

    return TRUE;
}

static gboolean
create_iochannel (QmiDevice *self,
                  GError **error)
{
    GError *inner_error = NULL;
    guint fd;

    if (self->priv->iochannel) {
        g_set_error (error,
                     QMI_CORE_ERROR,
                     QMI_CORE_ERROR_WRONG_STATE,
                     "Already open");
        return FALSE;
    }

    g_assert (self->priv->file);
    g_assert (self->priv->path);

    errno = 0;
    fd = open (self->priv->path, O_RDWR | O_EXCL | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        g_set_error (error,
                     QMI_CORE_ERROR,
                     QMI_CORE_ERROR_FAILED,
                     "Cannot open device file '%s': %s",
                     self->priv->path_display,
                     strerror (errno));
        return FALSE;
    }

    /* Create new GIOChannel */
    self->priv->iochannel = g_io_channel_unix_new (fd);

    /* We don't want UTF-8 encoding, we're playing with raw binary data */
    g_io_channel_set_encoding (self->priv->iochannel, NULL, NULL);

    /* We don't want to get the channel buffered */
    g_io_channel_set_buffered (self->priv->iochannel, FALSE);

    /* Let the GIOChannel own the FD */
    g_io_channel_set_close_on_unref (self->priv->iochannel, TRUE);

    /* We don't want to get blocked while writing stuff */
    if (!g_io_channel_set_flags (self->priv->iochannel,
                                 G_IO_FLAG_NONBLOCK,
                                 &inner_error)) {
        g_prefix_error (&inner_error, "Cannot set non-blocking channel: ");
        g_propagate_error (error, inner_error);
        g_io_channel_shutdown (self->priv->iochannel, FALSE, NULL);
        g_io_channel_unref (self->priv->iochannel);
        self->priv->iochannel = NULL;
        return FALSE;
    }

    self->priv->watch_id = g_io_add_watch (self->priv->iochannel,
                                           G_IO_IN | G_IO_ERR | G_IO_HUP,
                                           (GIOFunc)data_available,
                                           self);

    return !!self->priv->iochannel;
}

typedef struct {
    QmiDevice *self;
    GSimpleAsyncResult *result;
    GCancellable *cancellable;
    QmiDeviceOpenFlags flags;
    guint timeout;
    guint version_check_retries;
} DeviceOpenContext;

static void
device_open_context_complete_and_free (DeviceOpenContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    g_object_unref (ctx->self);
    g_slice_free (DeviceOpenContext, ctx);
}

/**
 * qmi_device_open_finish:
 * @self: a #QmiDevice.
 * @res: a #GAsyncResult.
 * @error: a #GError.
 *
 * Finishes an asynchronous open operation started with qmi_device_open_async().
 *
 * Returns: #TRUE if successful, #FALSE if @error is set.
 */
gboolean
qmi_device_open_finish (QmiDevice *self,
                        GAsyncResult *res,
                        GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void process_open_flags (DeviceOpenContext *ctx);

static void
sync_ready (QmiClientCtl *client_ctl,
            GAsyncResult *res,
            DeviceOpenContext *ctx)
{
    GError *error = NULL;
    QmiMessageCtlSyncOutput *output;

    /* Check result of the async operation */
    output = qmi_client_ctl_sync_finish (client_ctl, res, &error);
    if(!output) {
        g_simple_async_result_take_error (ctx->result, error);
        device_open_context_complete_and_free (ctx);
        return;
    }

    /* Check result of the QMI operation */
    if (!qmi_message_ctl_sync_output_get_result (output, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        device_open_context_complete_and_free (ctx);
        qmi_message_ctl_sync_output_unref (output);
        return;
    }

    g_debug ("[%s] Sync operation finished",
             ctx->self->priv->path_display);

    /* Keep on with next flags */
    process_open_flags (ctx);
    qmi_message_ctl_sync_output_unref (output);
}

static void
version_info_ready (QmiClientCtl *client_ctl,
                    GAsyncResult *res,
                    DeviceOpenContext *ctx)
{
    GArray *service_list;
    QmiMessageCtlGetVersionInfoOutput *output;
    GError *error = NULL;
    guint i;

    /* Check result of the async operation */
    output = qmi_client_ctl_get_version_info_finish (client_ctl, res, &error);
    if (!output) {
        if (g_error_matches (error, QMI_CORE_ERROR, QMI_CORE_ERROR_TIMEOUT)) {
            /* Update retries... */
            ctx->version_check_retries--;
            /* If retries left, retry */
            if (ctx->version_check_retries > 0) {
                qmi_client_ctl_get_version_info (ctx->self->priv->client_ctl,
                                                 NULL,
                                                 1,
                                                 ctx->cancellable,
                                                 (GAsyncReadyCallback)version_info_ready,
                                                 ctx);
                return;
            }

            /* Otherwise, propagate the error */
        }

        g_simple_async_result_take_error (ctx->result, error);
        device_open_context_complete_and_free (ctx);
        return;
    }

    /* Check result of the QMI operation */
    if (!qmi_message_ctl_get_version_info_output_get_result (output, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        device_open_context_complete_and_free (ctx);
        qmi_message_ctl_get_version_info_output_unref (output);
        return;
    }

    /* QMI operation succeeded, we can now get the outputs */
    service_list = NULL;
    qmi_message_ctl_get_version_info_output_get_service_list (output,
                                                              &service_list,
                                                              NULL);
    ctx->self->priv->supported_services = g_array_ref (service_list);

    g_debug ("[%s] QMI Device supports %u services:",
             ctx->self->priv->path_display,
             ctx->self->priv->supported_services->len);
    for (i = 0; i < ctx->self->priv->supported_services->len; i++) {
        QmiMessageCtlGetVersionInfoOutputServiceListService *info;

        info = &g_array_index (ctx->self->priv->supported_services,
                               QmiMessageCtlGetVersionInfoOutputServiceListService,
                               i);
        g_debug ("[%s]    %s (%u.%u)",
                 ctx->self->priv->path_display,
                 qmi_service_get_string (info->service),
                 info->major_version,
                 info->minor_version);
    }

    /* Keep on with next flags */
    process_open_flags (ctx);
    qmi_message_ctl_get_version_info_output_unref (output);
}

static void
process_open_flags (DeviceOpenContext *ctx)
{
    /* Query version info? */
    if (ctx->flags & QMI_DEVICE_OPEN_FLAGS_VERSION_INFO) {
        ctx->flags &= ~QMI_DEVICE_OPEN_FLAGS_VERSION_INFO;
        /* Setup how many times to retry... We'll retry once per second */
        ctx->version_check_retries = ctx->timeout > 0 ? ctx->timeout : 1;
        g_debug ("[%s] Checking version info (%u retries)...",
                 ctx->self->priv->path_display,
                 ctx->version_check_retries);
        qmi_client_ctl_get_version_info (ctx->self->priv->client_ctl,
                                         NULL,
                                         1,
                                         ctx->cancellable,
                                         (GAsyncReadyCallback)version_info_ready,
                                         ctx);
        return;
    }

    /* Sync? */
    if (ctx->flags & QMI_DEVICE_OPEN_FLAGS_SYNC) {
        g_debug ("[%s] Running sync...",
                 ctx->self->priv->path_display);
        ctx->flags &= ~QMI_DEVICE_OPEN_FLAGS_SYNC;
        qmi_client_ctl_sync (ctx->self->priv->client_ctl,
                             NULL,
                             ctx->timeout,
                             ctx->cancellable,
                             (GAsyncReadyCallback)sync_ready,
                             ctx);
        return;
    }

    /* No more flags to process, done we are */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    device_open_context_complete_and_free (ctx);
}

/**
 * qmi_device_open:
 * @self: a #QmiDevice.
 * @flags: mask of #QmiDeviceOpenFlags specifying how the device should be opened.
 * @timeout: maximum time, in seconds, to wait for the device to be opened.
 * @cancellable: optional #GCancellable object, #NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the operation is finished.
 * @user_data: the data to pass to callback function.
 *
 * Asynchronously opens a #QmiDevice for I/O.
 *
 * When the operation is finished @callback will be called. You can then call
 * qmi_device_open_finish() to get the result of the operation.
 */
void
qmi_device_open (QmiDevice *self,
                 QmiDeviceOpenFlags flags,
                 guint timeout,
                 GCancellable *cancellable,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    DeviceOpenContext *ctx;
    GError *error = NULL;

    g_return_if_fail (QMI_IS_DEVICE (self));

    ctx = g_slice_new (DeviceOpenContext);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             qmi_device_open);
    ctx->flags = flags;
    ctx->timeout = timeout;
    ctx->cancellable = (cancellable ? g_object_ref (cancellable) : NULL);

    if (!create_iochannel (self, &error)) {
        g_prefix_error (&error,
                        "Cannot open QMI device: ");
        g_simple_async_result_take_error (ctx->result, error);
        device_open_context_complete_and_free (ctx);
        return;
    }

    /* Process all open flags */
    process_open_flags (ctx);
}

/*****************************************************************************/
/* Close channel */

static gboolean
destroy_iochannel (QmiDevice *self,
                   GError **error)
{
    GError *inner_error = NULL;

    /* Already closed? */
    if (!self->priv->iochannel)
        return TRUE;

    g_io_channel_shutdown (self->priv->iochannel, TRUE, &inner_error);

    /* Failures when closing still make the device to get closed */
    g_io_channel_unref (self->priv->iochannel);
    self->priv->iochannel = NULL;

    if (self->priv->watch_id) {
        g_source_remove (self->priv->watch_id);
        self->priv->watch_id = 0;
    }

    if (self->priv->response) {
        g_byte_array_unref (self->priv->response);
        self->priv->response = NULL;
    }

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    return TRUE;
}

/**
 * qmi_device_close:
 * @self: a #QmiDevice
 * @error: a #GError
 *
 * Synchronously closes a #QmiDevice, preventing any further I/O.
 *
 * Closing a #QmiDevice multiple times will not return an error.
 *
 * Returns: #TRUE if successful, #FALSE if @error is set.
 */
gboolean
qmi_device_close (QmiDevice *self,
                  GError **error)
{
    g_return_val_if_fail (QMI_IS_DEVICE (self), FALSE);

    if (!destroy_iochannel (self, error)) {
        g_prefix_error (error,
                        "Cannot close QMI device: ");
        return FALSE;
    }

    return TRUE;
}

/*****************************************************************************/
/* Command */

QmiMessage *
qmi_device_command_finish (QmiDevice *self,
                           GAsyncResult *res,
                           GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return qmi_message_ref (g_simple_async_result_get_op_res_gpointer (
                                G_SIMPLE_ASYNC_RESULT (res)));
}

void
qmi_device_command (QmiDevice *self,
                    QmiMessage *message,
                    guint timeout,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GError *error = NULL;
    Transaction *tr;
    gconstpointer raw_message;
    gsize raw_message_len;
    gsize written;
    GIOStatus write_status;

    g_return_if_fail (QMI_IS_DEVICE (self));
    g_return_if_fail (message != NULL);

    tr = transaction_new (self, message, callback, user_data);

    /* Device must be open */
    if (!self->priv->iochannel) {
        error = g_error_new (QMI_CORE_ERROR,
                             QMI_CORE_ERROR_WRONG_STATE,
                             "Device must be open to send commands");
        transaction_complete_and_free (tr, NULL, error);
        g_error_free (error);
        return;
    }

    /* Non-CTL services should use a proper CID */
    if (qmi_message_get_service (message) != QMI_SERVICE_CTL &&
        qmi_message_get_client_id (message) == 0) {
        error = g_error_new (QMI_CORE_ERROR,
                             QMI_CORE_ERROR_FAILED,
                             "Cannot send message in service '%s' without a CID",
                             qmi_service_get_string (qmi_message_get_service (message)));
        transaction_complete_and_free (tr, NULL, error);
        g_error_free (error);
        return;
    }

    /* Check if the message to be sent is supported by the device
     * (only applicable if we did version info check when opening) */
    if (!check_message_supported (self, message, &error)) {
        g_prefix_error (&error, "Cannot send message: ");
        transaction_complete_and_free (tr, NULL, error);
        g_error_free (error);
        return;
    }

#ifdef MESSAGE_ENABLE_TRACE
    {
        gchar *printable;

        printable = qmi_message_get_printable (message, "<<<<<< ");
        g_debug ("[%s] Sending message...\n%s",
                 self->priv->path_display,
                 printable);
        g_free (printable);
    }
#endif /* MESSAGE_ENABLE_TRACE */

    /* Get raw message */
    raw_message = qmi_message_get_raw (message, &raw_message_len, &error);
    if (!raw_message) {
        g_prefix_error (&error, "Cannot get raw message: ");
        transaction_complete_and_free (tr, NULL, error);
        g_error_free (error);
        return;
    }

    /* Setup context to match response */
    device_store_transaction (self, tr, timeout);

    written = 0;
    write_status = G_IO_STATUS_AGAIN;
    while (write_status == G_IO_STATUS_AGAIN) {
        write_status = g_io_channel_write_chars (self->priv->iochannel,
                                                 raw_message,
                                                 (gssize)raw_message_len,
                                                 &written,
                                                 &error);
        switch (write_status) {
        case G_IO_STATUS_ERROR:
            g_prefix_error (&error, "Cannot write message: ");

            /* Match transaction so that we remove it from our tracking table */
            tr = device_match_transaction (self, message);
            transaction_complete_and_free (tr, NULL, error);
            g_error_free (error);
            return;

        case G_IO_STATUS_EOF:
            /* We shouldn't get EOF when writing */
            g_assert_not_reached ();
            break;

        case G_IO_STATUS_NORMAL:
            /* All good, we'll exit the loop now */
            break;

        case G_IO_STATUS_AGAIN:
            /* We're in a non-blocking channel and therefore we're up to receive
             * EAGAIN; just retry in this case. TODO: in an idle? */
            break;
        }
    }

    /* Just return, we'll get response asynchronously */
}

/*****************************************************************************/
/* New QMI device */

/**
 * qmi_device_new_finish:
 * @res: a #GAsyncResult.
 * @error: a #GError.
 *
 * Finishes an operation started with qmi_device_new().
 *
 * Returns: A newly created #QmiDevice, or #NULL if @error is set.
 */
QmiDevice *
qmi_device_new_finish (GAsyncResult *res,
                       GError **error)
{
  GObject *ret;
  GObject *source_object;

  source_object = g_async_result_get_source_object (res);
  ret = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object), res, error);
  g_object_unref (source_object);

  return (ret ? QMI_DEVICE (ret) : NULL);
}

/**
 * qmi_device_new:
 * @file: a #GFile.
 * @cancellable: optional #GCancellable object, #NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the initialization is finished.
 * @user_data: the data to pass to callback function.
 *
 * Asynchronously creates a #QmiDevice object to manage @file.
 * When the operation is finished, @callback will be invoked. You can then call
 * qmi_device_new_finish() to get the result of the operation.
 */
void
qmi_device_new (GFile *file,
                GCancellable *cancellable,
                GAsyncReadyCallback callback,
                gpointer user_data)
{
    g_async_initable_new_async (QMI_TYPE_DEVICE,
                                G_PRIORITY_DEFAULT,
                                cancellable,
                                callback,
                                user_data,
                                QMI_DEVICE_FILE, file,
                                NULL);
}

/*****************************************************************************/
/* Async init */

typedef struct {
    QmiDevice *self;
    GSimpleAsyncResult *result;
    GCancellable *cancellable;
} InitContext;

static void
init_context_complete_and_free (InitContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_slice_free (InitContext, ctx);
}

static gboolean
initable_init_finish (GAsyncInitable  *initable,
                      GAsyncResult    *result,
                      GError         **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

static void
sync_indication_cb (QmiClientCtl *client_ctl,
                    QmiDevice *self)
{
    /* Just log about it */
    g_debug ("[%s] Sync indication received",
             self->priv->path_display);
}

static void
query_info_async_ready (GFile *file,
                        GAsyncResult *res,
                        InitContext *ctx)
{
    GError *error = NULL;
    GFileInfo *info;

    info = g_file_query_info_finish (file, res, &error);
    if (!info) {
        g_prefix_error (&error,
                        "Couldn't query file info: ");
        g_simple_async_result_take_error (ctx->result, error);
        init_context_complete_and_free (ctx);
        return;
    }

    /* Our QMI device must be of SPECIAL type */
    if (g_file_info_get_file_type (info) != G_FILE_TYPE_SPECIAL) {
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_FAILED,
                                         "Wrong file type");
        init_context_complete_and_free (ctx);
        return;
    }
    g_object_unref (info);

    /* Create the implicit CTL client */
    ctx->self->priv->client_ctl = g_object_new (QMI_TYPE_CLIENT_CTL,
                                                QMI_CLIENT_DEVICE,  ctx->self,
                                                QMI_CLIENT_SERVICE, QMI_SERVICE_CTL,
                                                QMI_CLIENT_CID,     QMI_CID_NONE,
                                                NULL);

    /* Register the CTL client to get indications */
    register_client (ctx->self,
                     QMI_CLIENT (ctx->self->priv->client_ctl),
                     &error);
    g_assert_no_error (error);

    /* Connect to 'Sync' indications */
    g_signal_connect (ctx->self->priv->client_ctl,
                      "sync",
                      G_CALLBACK (sync_indication_cb),
                      ctx->self);

    /* Done we are */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    init_context_complete_and_free (ctx);
}

static void
initable_init_async (GAsyncInitable *initable,
                     int io_priority,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    InitContext *ctx;

    ctx = g_slice_new0 (InitContext);
    ctx->self = g_object_ref (initable);
    if (cancellable)
        ctx->cancellable = g_object_ref (cancellable);
    ctx->result = g_simple_async_result_new (G_OBJECT (initable),
                                             callback,
                                             user_data,
                                             initable_init_async);

    /* We need a proper file to initialize */
    if (!ctx->self->priv->file) {
        g_simple_async_result_set_error (ctx->result,
                                         QMI_CORE_ERROR,
                                         QMI_CORE_ERROR_INVALID_ARGS,
                                         "Cannot initialize QMI device: No file given");
        init_context_complete_and_free (ctx);
        return;
    }

    /* Check the file type. Note that this is just a quick check to avoid
     * creating QmiDevices pointing to a location already known not to be a QMI
     * device. */
    g_file_query_info_async (ctx->self->priv->file,
                             G_FILE_ATTRIBUTE_STANDARD_TYPE,
                             G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT,
                             ctx->cancellable,
                             (GAsyncReadyCallback)query_info_async_ready,
                             ctx);
}

/*****************************************************************************/

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    QmiDevice *self = QMI_DEVICE (object);

    switch (prop_id) {
    case PROP_FILE:
        g_assert (self->priv->file == NULL);
        self->priv->file = g_value_dup_object (value);
        self->priv->path = g_file_get_path (self->priv->file);
        self->priv->path_display = g_filename_display_name (self->priv->path);
        break;
    case PROP_CLIENT_CTL:
        /* Not writable */
        g_assert_not_reached ();
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject *object,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    QmiDevice *self = QMI_DEVICE (object);

    switch (prop_id) {
    case PROP_FILE:
        g_value_set_object (value, self->priv->file);
        break;
    case PROP_CLIENT_CTL:
        g_value_set_object (value, self->priv->client_ctl);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
qmi_device_init (QmiDevice *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              QMI_TYPE_DEVICE,
                                              QmiDevicePrivate);

    self->priv->registered_clients = g_hash_table_new_full (g_direct_hash,
                                                            g_direct_equal,
                                                            NULL,
                                                            g_object_unref);
}

static gboolean
foreach_warning (gpointer key,
                 QmiClient *client,
                 QmiDevice *self)
{
    g_warning ("[%s] QMI client for service '%s' with CID '%u' wasn't released",
               self->priv->path_display,
               qmi_service_get_string (qmi_client_get_service (client)),
               qmi_client_get_cid (client));

    return TRUE;
}

static void
dispose (GObject *object)
{
    QmiDevice *self = QMI_DEVICE (object);

    g_clear_object (&self->priv->file);

    /* unregister our CTL client */
    unregister_client (self, QMI_CLIENT (self->priv->client_ctl));

    /* If clients were left unreleased, we'll just warn about it.
     * There is no point in trying to request CID releases, as the device
     * itself is being disposed. */
    g_hash_table_foreach_remove (self->priv->registered_clients,
                                 (GHRFunc)foreach_warning,
                                 self);

    g_clear_object (&self->priv->client_ctl);

    G_OBJECT_CLASS (qmi_device_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    QmiDevice *self = QMI_DEVICE (object);

    /* Transactions keep refs to the device, so it's actually
     * impossible to have any content in the HT */
    if (self->priv->transactions) {
        g_assert (g_hash_table_size (self->priv->transactions) == 0);
        g_hash_table_unref (self->priv->transactions);
    }

    g_hash_table_unref (self->priv->registered_clients);

    if (self->priv->supported_services)
        g_array_unref (self->priv->supported_services);

    g_free (self->priv->path);
    g_free (self->priv->path_display);
    if (self->priv->response)
        g_byte_array_unref (self->priv->response);
    if (self->priv->iochannel)
        g_io_channel_unref (self->priv->iochannel);

    G_OBJECT_CLASS (qmi_device_parent_class)->finalize (object);
}

static void
async_initable_iface_init (GAsyncInitableIface *iface)
{
    iface->init_async = initable_init_async;
    iface->init_finish = initable_init_finish;
}

static void
qmi_device_class_init (QmiDeviceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (QmiDevicePrivate));

    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->finalize = finalize;
    object_class->dispose = dispose;

    properties[PROP_FILE] =
        g_param_spec_object (QMI_DEVICE_FILE,
                             "Device file",
                             "File to the underlying QMI device",
                             G_TYPE_FILE,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property (object_class, PROP_FILE, properties[PROP_FILE]);

    properties[PROP_CLIENT_CTL] =
        g_param_spec_object (QMI_DEVICE_CLIENT_CTL,
                             "CTL client",
                             "Implicit CTL client",
                             QMI_TYPE_CLIENT_CTL,
                             G_PARAM_READABLE);
    g_object_class_install_property (object_class, PROP_CLIENT_CTL, properties[PROP_CLIENT_CTL]);
}
