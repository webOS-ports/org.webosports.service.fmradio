// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_audio.h"

#include <glib.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/glib-mainloop.h>
#include <pulse/introspect.h>
#include <string.h>

#define MODULE_NAME "module-droid-fm"
#define RETRY_MS 2000
#define FREE_WAIT_US G_USEC_PER_SEC

struct fm_audio
{
	pa_glib_mainloop *mainloop;
	pa_context *ctx;
	char *server;
	char *module_args;
	fm_audio_changed_fn changed;
	void *data;

	bool ready;
	bool want;
	bool failed;            /* the last load failed: not tried again until asked */
	int pending;            /* operations in flight */
	uint32_t index;         /* of the module, PA_INVALID_INDEX if none */
	char *error;
	guint retry_id;
};

static void connect_pulse(struct fm_audio *a);

static void notify(struct fm_audio *a)
{
	if (a->changed)
	{
		a->changed(a->data);
	}
}

static void set_error(struct fm_audio *a, const char *what)
{
	g_free(a->error);
	a->error = what ? g_strdup(what) : NULL;
}

static void track(struct fm_audio *a, pa_operation *op)
{
	if (op)
	{
		a->pending++;
		pa_operation_unref(op);
	}
}

static void sync_module(struct fm_audio *a);

static void loaded(pa_context *c, uint32_t idx, void *userdata)
{
	struct fm_audio *a = userdata;

	a->pending--;

	if (idx == PA_INVALID_INDEX)
	{
		const char *why = pa_strerror(pa_context_errno(c));

		g_warning("loading %s failed: %s", MODULE_NAME, why);
		a->failed = true;
		set_error(a, why);
	}
	else
	{
		a->index = idx;
	}

	notify(a);
	sync_module(a);
}

static void unloaded(pa_context *c, int success, void *userdata)
{
	struct fm_audio *a = userdata;

	(void)c;
	a->pending--;

	if (!success)
	{
		/* Gone already, most likely; the next connect finds out for sure. */
		g_warning("unloading %s (#%u) failed", MODULE_NAME, a->index);
	}

	a->index = PA_INVALID_INDEX;
	notify(a);
	sync_module(a);
}

static void sync_module(struct fm_audio *a)
{
	if (!a->ready || a->pending > 0)
	{
		return;
	}

	if (a->want && !a->failed && a->index == PA_INVALID_INDEX)
	{
		track(a, pa_context_load_module(a->ctx, MODULE_NAME, a->module_args, loaded, a));
	}
	else if (!a->want && a->index != PA_INVALID_INDEX)
	{
		track(a, pa_context_unload_module(a->ctx, a->index, unloaded, a));
	}
}

static void stale_unloaded(pa_context *c, int success, void *userdata)
{
	struct fm_audio *a = userdata;

	(void)c;
	(void)success;
	a->pending--;
	sync_module(a);
}

static void module_listed(pa_context *c, const pa_module_info *info, int eol, void *userdata)
{
	struct fm_audio *a = userdata;

	if (eol)
	{
		a->pending--;
		notify(a);
		sync_module(a);
		return;
	}

	if (!info || !info->name || strcmp(info->name, MODULE_NAME) != 0)
	{
		return;
	}

	if (a->want && a->index == PA_INVALID_INDEX)
	{
		g_message("taking over %s #%u", MODULE_NAME, info->index);
		a->index = info->index;
		return;
	}

	g_message("unloading %s #%u left behind", MODULE_NAME, info->index);
	track(a, pa_context_unload_module(c, info->index, stale_unloaded, a));
}

static gboolean retry(gpointer userdata)
{
	struct fm_audio *a = userdata;

	a->retry_id = 0;
	connect_pulse(a);
	return G_SOURCE_REMOVE;
}

static void drop_context(struct fm_audio *a)
{
	if (!a->ctx)
	{
		return;
	}

	pa_context_set_state_callback(a->ctx, NULL, NULL);
	pa_context_disconnect(a->ctx);
	pa_context_unref(a->ctx);
	a->ctx = NULL;
	a->ready = false;
	a->pending = 0;
}

static void context_state(pa_context *c, void *userdata)
{
	struct fm_audio *a = userdata;

	switch (pa_context_get_state(c))
	{
		case PA_CONTEXT_READY:
			a->ready = true;
			/* The module index is only good for this PulseAudio: list what
			 * it has rather than trust one from before. */
			a->index = PA_INVALID_INDEX;
			track(a, pa_context_get_module_info_list(c, module_listed, a));
			break;

		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			g_warning("lost PulseAudio: %s", pa_strerror(pa_context_errno(c)));
			drop_context(a);
			a->index = PA_INVALID_INDEX;
			notify(a);

			if (!a->retry_id)
			{
				a->retry_id = g_timeout_add(RETRY_MS, retry, a);
			}

			break;

		default:
			break;
	}
}

static void connect_pulse(struct fm_audio *a)
{
	pa_mainloop_api *api = pa_glib_mainloop_get_api(a->mainloop);

	a->ctx = pa_context_new(api, "org.webosports.service.fmradio");

	if (!a->ctx)
	{
		a->retry_id = g_timeout_add(RETRY_MS, retry, a);
		return;
	}

	pa_context_set_state_callback(a->ctx, context_state, a);

	if (pa_context_connect(a->ctx, a->server, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
	{
		g_warning("cannot connect to PulseAudio at %s: %s", a->server,
		          pa_strerror(pa_context_errno(a->ctx)));
		drop_context(a);
		a->retry_id = g_timeout_add(RETRY_MS, retry, a);
	}
}

struct fm_audio *fm_audio_new(const char *server, const char *module_args,
                              fm_audio_changed_fn changed, void *data)
{
	struct fm_audio *a = g_new0(struct fm_audio, 1);

	a->mainloop = pa_glib_mainloop_new(NULL);
	a->server = g_strdup(server);
	a->module_args = g_strdup(module_args ? module_args : "");
	a->changed = changed;
	a->data = data;
	a->index = PA_INVALID_INDEX;
	connect_pulse(a);
	return a;
}

void fm_audio_free(struct fm_audio *a)
{
	if (!a)
	{
		return;
	}

	a->want = false;
	a->changed = NULL;
	sync_module(a);

	/*
	 * The main loop has stopped by now, so turn the context by hand until
	 * the unload is answered. A radio left playing after the service is gone
	 * could not be switched off by anything.
	 */
	if (a->ctx)
	{
		gint64 until = g_get_monotonic_time() + FREE_WAIT_US;

		while ((a->pending > 0 || a->index != PA_INVALID_INDEX) && a->ready &&
		        g_get_monotonic_time() < until)
		{
			(void)g_main_context_iteration(NULL, FALSE);
			sync_module(a);
			g_usleep(1000);
		}
	}

	if (a->retry_id)
	{
		g_source_remove(a->retry_id);
	}

	drop_context(a);
	pa_glib_mainloop_free(a->mainloop);
	g_free(a->server);
	g_free(a->module_args);
	g_free(a->error);
	g_free(a);
}

void fm_audio_set_enabled(struct fm_audio *a, bool on)
{
	a->want = on;
	a->failed = false;
	set_error(a, NULL);
	sync_module(a);
}

bool fm_audio_active(const struct fm_audio *a)
{
	return a->index != PA_INVALID_INDEX;
}

const char *fm_audio_error(const struct fm_audio *a)
{
	return a->error;
}
