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
/*
 * Connecting again after a failure: after FM_AUDIO_RETRY_MS, doubling up to
 * FM_AUDIO_RETRY_MAX_MS while PulseAudio stays away. (The tests build with
 * shorter ones.)
 */
#ifndef FM_AUDIO_RETRY_MS
#define FM_AUDIO_RETRY_MS 1000
#endif
#ifndef FM_AUDIO_RETRY_MAX_MS
#define FM_AUDIO_RETRY_MAX_MS 16000
#endif
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
	guint retry_ms;         /* the next wait before connecting again */
	bool down;              /* not connected since the last failure was logged */
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

/*
 * The one place a retry is set up: never a second timer while one waits,
 * else each failure that is reported twice doubles them.
 */
static void schedule_retry(struct fm_audio *a)
{
	if (a->retry_id)
	{
		return;
	}

	a->retry_id = g_timeout_add(a->retry_ms, retry, a);
	a->retry_ms = MIN(a->retry_ms * 2, FM_AUDIO_RETRY_MAX_MS);
}

/* Said once per outage, not on every attempt. */
static void log_down(struct fm_audio *a, const char *what, int error)
{
	if (!a->down)
	{
		g_warning("%s PulseAudio at %s: %s; trying again until it is there", what, a->server,
		          pa_strerror(error));
		a->down = true;
	}
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
			if (a->down)
			{
				g_message("connected to PulseAudio at %s", a->server);
				a->down = false;
			}

			a->ready = true;
			a->retry_ms = FM_AUDIO_RETRY_MS;
			/* The module index is only good for this PulseAudio: list what
			 * it has rather than trust one from before. */
			a->index = PA_INVALID_INDEX;
			track(a, pa_context_get_module_info_list(c, module_listed, a));
			break;

		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			log_down(a, a->ready ? "lost" : "cannot connect to", pa_context_errno(c));
			drop_context(a);
			a->index = PA_INVALID_INDEX;
			notify(a);
			schedule_retry(a);
			break;

		default:
			break;
	}
}

static void connect_pulse(struct fm_audio *a)
{
	pa_mainloop_api *api = pa_glib_mainloop_get_api(a->mainloop);
	pa_context *c;

	if (a->ctx)
	{
		return;
	}

	c = pa_context_new(api, "org.webosports.service.fmradio");

	if (!c)
	{
		schedule_retry(a);
		return;
	}

	a->ctx = c;
	pa_context_set_state_callback(c, context_state, a);

	/*
	 * A socket that refuses (PulseAudio not up yet) fails inside
	 * pa_context_connect(): context_state() has then seen FAILED, dropped
	 * the context and set up the retry before this returns. Held here so its
	 * error can still be read either way.
	 */
	pa_context_ref(c);

	if (pa_context_connect(c, a->server, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
	{
		log_down(a, "cannot connect to", pa_context_errno(c));

		if (a->ctx == c)
		{
			drop_context(a);
		}

		schedule_retry(a);
	}

	pa_context_unref(c);
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
	a->retry_ms = FM_AUDIO_RETRY_MS;
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
