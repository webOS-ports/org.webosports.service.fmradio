// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_helium.h"

#include <errno.h>
#include <gbinder.h>

/*
 * The HCI link to the FM chip through vendor.qti.hardware.fm@1.0::IFmHci,
 * which the vendor's Bluetooth HAL process registers next to IBluetoothHci
 * (KEY2: android.hardware.bluetooth@1.0-service-qti, "Registering FM
 * Service"). From the interface (vendor/qcom/opensource/interfaces,
 * fm/1.0):
 *
 *   IFmHci:          initialize(IFmHciCallbacks), sendHciCommand(vec<uint8_t>),
 *                    close()
 *   IFmHciCallbacks: initializationComplete(Status), hciEventReceived(vec<uint8_t>)
 *
 * Transaction codes are the methods in that order, from 1. Status SUCCESS
 * is 0.
 *
 * libgbinder hands the callbacks to the main thread (the service's GLib
 * loop), while the tuner calls from its worker: the callbacks go through
 * one lock, and to the open link's event queue.
 */

#define FM_HIDL_IFACE    "vendor.qti.hardware.fm@1.0::IFmHci"
#define FM_HIDL_CB_IFACE "vendor.qti.hardware.fm@1.0::IFmHciCallbacks"
#define FM_HIDL_FQNAME   FM_HIDL_IFACE "/default"

enum
{
	FM_HIDL_INITIALIZE = 1,
	FM_HIDL_SEND_HCI_COMMAND,
	FM_HIDL_CLOSE,
};

enum
{
	FM_HIDL_CB_INIT_COMPLETE = 1,
	FM_HIDL_CB_HCI_EVENT,
};

#define FM_HIDL_STATUS_SUCCESS 0

/* fm_hci waits HCI_TIMEOUT, 3 s, for initializationComplete. */
#define FM_HIDL_INIT_WAIT_MS 3000

struct hidl_link
{
	GBinderServiceManager *sm;
	GBinderRemoteObject *remote;
	GBinderClient *client;
	GBinderLocalObject *callbacks;
};

/*
 * What the callbacks reach. It outlives every link: a callback already on
 * its way to the main thread when a link closes finds h NULL, not freed.
 */
static struct
{
	GMutex lock;
	GCond cond;
	struct fm_hci *h;
	bool init_done;
	gint32 init_status;
} cb;

static GBinderLocalReply *on_callback(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                      guint code, guint flags, int *status, void *user_data)
{
	(void)flags;
	(void)user_data;

	if (g_strcmp0(gbinder_remote_request_interface(req), FM_HIDL_CB_IFACE) != 0)
	{
		*status = GBINDER_STATUS_FAILED;
		return NULL;
	}

	if (code == FM_HIDL_CB_INIT_COMPLETE)
	{
		gint32 result = -1;

		(void)gbinder_remote_request_read_int32(req, &result);
		g_mutex_lock(&cb.lock);
		cb.init_done = true;
		cb.init_status = result;
		g_cond_broadcast(&cb.cond);
		g_mutex_unlock(&cb.lock);
	}
	else if (code == FM_HIDL_CB_HCI_EVENT)
	{
		GBinderReader reader;
		gsize count = 0;
		gsize elemsize = 0;
		const void *vec;

		gbinder_remote_request_init_reader(req, &reader);
		vec = gbinder_reader_read_hidl_vec(&reader, &count, &elemsize);

		g_mutex_lock(&cb.lock);

		if (vec && elemsize == 1 && cb.h)
		{
			fm_hci_event(cb.h, vec, count);
		}

		g_mutex_unlock(&cb.lock);
	}
	else
	{
		*status = GBINDER_STATUS_FAILED;
		return NULL;
	}

	*status = GBINDER_STATUS_OK;
	return gbinder_local_reply_append_int32(gbinder_local_object_new_reply(obj), 0);
}

static void link_free(struct hidl_link *l)
{
	g_mutex_lock(&cb.lock);
	cb.h = NULL;
	g_mutex_unlock(&cb.lock);

	if (l->callbacks)
	{
		gbinder_local_object_drop(l->callbacks);
	}

	gbinder_client_unref(l->client);
	gbinder_remote_object_unref(l->remote);
	gbinder_servicemanager_unref(l->sm);
	g_free(l);
}

/* initializationComplete, or -ETIMEDOUT; -EIO if it says it failed. */
static int wait_initialized(void)
{
	gint64 until = g_get_monotonic_time() + (gint64)FM_HIDL_INIT_WAIT_MS * G_TIME_SPAN_MILLISECOND;
	int ret = 0;

	g_mutex_lock(&cb.lock);

	while (!cb.init_done && ret == 0)
	{
		if (!g_cond_wait_until(&cb.cond, &cb.lock, until))
		{
			ret = -ETIMEDOUT;
		}
	}

	if (ret == 0 && cb.init_status != FM_HIDL_STATUS_SUCCESS)
	{
		ret = -EIO;
	}

	g_mutex_unlock(&cb.lock);
	return ret;
}

static int hidl_open(struct fm_hci *h)
{
	struct hidl_link *l = g_new0(struct hidl_link, 1);
	GBinderLocalRequest *req;
	GBinderRemoteReply *reply;
	int status = 0;
	int ret;

	l->sm = gbinder_servicemanager_new(GBINDER_DEFAULT_HWBINDER);

	if (l->sm)
	{
		/* Not ours until referenced. */
		l->remote = gbinder_remote_object_ref(
		                gbinder_servicemanager_get_service_sync(l->sm, FM_HIDL_FQNAME, &status));
	}

	if (!l->remote)
	{
		link_free(l);
		return -ENODEV;
	}

	l->client = gbinder_client_new(l->remote, FM_HIDL_IFACE);
	l->callbacks = gbinder_servicemanager_new_local_object(l->sm, FM_HIDL_CB_IFACE, on_callback,
	               NULL);

	if (!l->client || !l->callbacks)
	{
		link_free(l);
		return -ENOMEM;
	}

	g_mutex_lock(&cb.lock);
	cb.h = h;
	cb.init_done = false;
	cb.init_status = -1;
	g_mutex_unlock(&cb.lock);

	req = gbinder_client_new_request(l->client);
	gbinder_local_request_append_local_object(req, l->callbacks);
	reply = gbinder_client_transact_sync_reply(l->client, FM_HIDL_INITIALIZE, req, &status);
	gbinder_local_request_unref(req);
	gbinder_remote_reply_unref(reply);

	ret = status == GBINDER_STATUS_OK ? wait_initialized() : -EIO;

	if (ret < 0)
	{
		link_free(l);
		return ret;
	}

	h->link = l;
	return 0;
}

static void hidl_close(struct fm_hci *h)
{
	struct hidl_link *l = h->link;
	int status = 0;

	if (!l)
	{
		return;
	}

	gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(l->client, FM_HIDL_CLOSE, NULL,
	                           &status));
	link_free(l);
	h->link = NULL;
}

static int hidl_send(struct fm_hci *h, const uint8_t *pkt, size_t len)
{
	struct hidl_link *l = h->link;
	GBinderLocalRequest *req;
	GBinderWriter writer;
	int status = 0;

	if (!l)
	{
		return -ENOTCONN;
	}

	req = gbinder_client_new_request(l->client);
	gbinder_local_request_init_writer(req, &writer);
	/* Refers to pkt, which outlives the call: it is synchronous. */
	gbinder_writer_append_hidl_vec(&writer, pkt, (guint)len, 1);
	gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(l->client,
	                           FM_HIDL_SEND_HCI_COMMAND, req, &status));
	gbinder_local_request_unref(req);
	return status == GBINDER_STATUS_OK ? 0 : -EIO;
}

const struct fm_hci_ops fm_hci_hidl_ops = {
	.open = hidl_open,
	.close = hidl_close,
	.send = hidl_send,
};
