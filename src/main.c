// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fmradiod - org.webosports.service.fmradio
 *
 * Puts the FM radio on the bus: the receiver for tuning (fm_tuner.c - the
 * MediaTek one behind /dev/fm, Qualcomm's behind /dev/radio0, or Qualcomm's
 * reached through the Bluetooth HAL, whichever this device has), and
 * module-droid-fm in PulseAudio for the sound
 * (fm_audio.c), which plays the radio through the audio HAL as a stream on
 * the "fm" sink - ducked, muted and turned up and down by the policy and the
 * volume keys like any other.
 *
 * The tuner only ever runs on one worker thread: its calls block, a scan for
 * about ten seconds, and the bus must answer meanwhile. Requests go to the
 * worker in order; each comes back to the main loop with its result, which is
 * where all state lives and every reply is sent. A request that changes what
 * the radio does (power, tune, seek, scan) stops a seek or scan still running.
 *
 * Frequencies are integers in kHz: 95000 is 95.0 MHz.
 */

#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>
#include <stdbool.h>
#include <string.h>

#include "fm_audio.h"
#include "fm_band.h"
#include "fm_helium.h"
#include "fm_join.h"
#include "fm_rds.h"
#include "fm_tuner.h"

#define FMRADIO_SERVICE "org.webosports.service.fmradio"

#ifndef FM_PULSE_SERVER
#define FM_PULSE_SERVER "unix:/var/run/pulse/native"
#endif

#ifndef FM_SIGNAL_INTERVAL_MS
#define FM_SIGNAL_INTERVAL_MS 2000
#endif

#define FM_DEFAULT_KHZ FM_BAND_MIN_KHZ

/* Stations as one value, copied by assignment. */
struct station_list
{
	size_t n;
	struct fm_station s[FM_BAND_CHANNELS];
};

enum job_kind
{
	JOB_POWER_ON,
	JOB_POWER_OFF,
	JOB_TUNE,
	JOB_SEEK,
	JOB_SCAN,
	JOB_NAMES,
	JOB_SIGNAL,
	JOB_QUIT,
};

struct job
{
	enum job_kind kind;
	int gen;                /* preempt_gen when it was queued */
	int khz;                /* POWER_ON, TUNE */
	int dir;                /* SEEK */
	struct station_list want;       /* NAMES: the stations to name */
	LSHandle *sh;
	LSMessage *msg;         /* to answer, or NULL */

	/* Filled in by the worker. */
	int ret;
	bool powered;
	int tuned_khz;
	bool have_rssi;
	int rssi;
	bool have_rds;
	struct fm_rds_info rds;
	struct station_list found;      /* SCAN */
};

static struct
{
	bool powered;
	int khz;                /* tuned to, or to power up on */
	bool have_rssi;
	int rssi;
	struct fm_rds_info rds; /* of the station tuned to; "" when none */
	int scans;              /* queued or running */
	int seeks;
	int namings;
	/* Every station's name heard so far, while on or looked for. */
	size_t nnames;
	struct
	{
		int khz;
		char name[FM_RDS_NAME_MAX];
	} names[FM_BAND_CHANNELS];
	bool signal_queued;
	struct station_list stations;   /* the last scan's */
} st = { .khz = FM_DEFAULT_KHZ };

static LSHandle *service_handle = NULL;
static GMainLoop *main_loop = NULL;
static int exit_status = 0;
static struct fm_audio *audio = NULL;

static GAsyncQueue *jobs = NULL;
static GThread *worker = NULL;
/* Set by the worker as it finishes; see fm_join_dispatching(). */
static gint worker_done = 0;

/*
 * Bumped for every request that changes what the radio does. A seek or scan
 * checks it between channels and stops when it moved past its own: it was
 * asked before something newer.
 */
static gint preempt_gen = 0;

static void post_status(void);

/* ---- worker ------------------------------------------------------------ */

/* fm_cancel_fn's signature: data cannot be const. */
// cppcheck-suppress constParameterCallback
static bool job_cancelled(void *data)
{
	const struct job *j = data;

	return g_atomic_int_get(&preempt_gen) != j->gen;
}

static gboolean job_done(gpointer data);

/* A station's name, found by a NAMES job and handed to the main loop. */
struct name_found
{
	int khz;
	char name[FM_RDS_NAME_MAX];
};

static gboolean name_done(gpointer data);

/*
 * RDS on, after anything that may have left it off: a power up always does,
 * and a search's PRE_SEARCH/RESTORE_SEARCH is the driver's to change. Switching
 * it on when it is on already only sets the same register again.
 */
static void rds_on(struct fm_tuner *t)
{
	int ret;

	if (!t->powered)
	{
		return;
	}

	ret = fm_tuner_rds_enable(t, true);

	if (ret < 0)
	{
		g_warning("switching RDS on failed: %s", g_strerror(-ret));
	}
}

static void run_job(struct fm_tuner *t, struct job *j)
{
	int ret = 0;

	switch (j->kind)
	{
		case JOB_POWER_ON:
			ret = fm_tuner_open(t, NULL);

			if (ret == 0)
			{
				ret = fm_tuner_power_up(t, j->khz);
			}

			if (ret < 0)
			{
				fm_tuner_close(t);
			}

			rds_on(t);
			break;

		case JOB_POWER_OFF:
			/* Closing releases the driver; it powers down first. */
			fm_tuner_close(t);
			break;

		case JOB_TUNE:
			ret = fm_tuner_tune(t, j->khz);
			break;

		case JOB_SEEK:
			ret = fm_tuner_seek(t, j->dir, job_cancelled, j);
			rds_on(t);
			break;

		case JOB_SCAN:
			/*
			 * With the radio off, the receiver is powered up for the scan
			 * alone and down again after it: the stations can be found
			 * before anything plays, and nothing is heard meanwhile - the
			 * sound only follows a powerOn.
			 */
			if (!t->powered)
			{
				ret = fm_tuner_open(t, NULL);

				if (ret == 0)
				{
					ret = fm_tuner_power_up(t, j->khz);
				}

				if (ret == 0)
				{
					ret = fm_tuner_scan(t, job_cancelled, j, j->found.s, &j->found.n);
				}

				fm_tuner_close(t);
				break;
			}

			ret = fm_tuner_scan(t, job_cancelled, j, j->found.s, &j->found.n);
			rds_on(t);
			break;

		case JOB_NAMES:
			/*
			 * Each station tuned to for a few seconds, with no sound, for its
			 * name: only with the radio off, since it would take the station
			 * listened to away. A newer request - a powerOn first of all -
			 * ends it within a poll.
			 */
			if (t->powered)
			{
				ret = -EBUSY;
				break;
			}

			ret = j->want.n > 0 ? fm_tuner_open(t, NULL) : 0;

			if (ret == 0 && j->want.n > 0)
			{
				ret = fm_tuner_power_up(t, j->want.s[0].khz);
			}

			for (size_t i = 0; ret == 0 && i < j->want.n; i++)
			{
				struct fm_rds_info info;
				int got = fm_tuner_find_name(t, j->want.s[i].khz, job_cancelled, j, &info);

				if (got == -ECANCELED)
				{
					ret = got;
				}
				else if (got < 0)
				{
					g_warning("no name for %d kHz: %s", j->want.s[i].khz, g_strerror(-got));
				}
				else if (info.name[0])
				{
					struct name_found *f = g_new0(struct name_found, 1);

					f->khz = j->want.s[i].khz;
					g_strlcpy(f->name, info.name, sizeof(f->name));
					g_main_context_invoke(NULL, name_done, f);
				}
			}

			fm_tuner_close(t);
			break;

		case JOB_SIGNAL:
		case JOB_QUIT:
			break;
	}

	j->ret = ret;
	j->powered = t->powered;
	j->tuned_khz = t->khz;
	j->have_rssi = t->powered && fm_tuner_rssi(t, &j->rssi) == 0;

	j->have_rds = t->powered && fm_tuner_rds_get(t, &j->rds, NULL) == 0;
}

static gpointer worker_main(gpointer data)
{
	struct fm_tuner tuner;
	const struct fm_tuner_ops *ops = fm_tuner_detect();

	(void)data;

	/* None found: the MediaTek one, whose open then says what is missing. */
	if (!ops)
	{
		g_warning("no FM receiver: none of %s, %s, %s", fm_mtk_ops.path, fm_iris_ops.path,
		          fm_helium_ops.path);
		ops = &fm_mtk_ops;
	}
	else
	{
		g_message("FM receiver: %s on %s", ops->name, ops->path);
	}

	fm_tuner_init(&tuner, ops, NULL);
	tuner.hci_ops = &fm_hci_hidl_ops;

	for (;;)
	{
		struct job *j = g_async_queue_pop(jobs);

		if (j->kind == JOB_QUIT)
		{
			g_free(j);
			break;
		}

		run_job(&tuner, j);
		g_main_context_invoke(NULL, job_done, j);
	}

	/* Nothing may leave the receiver running behind the service. */
	fm_tuner_close(&tuner);
	fm_join_thread_done(&worker_done);
	return NULL;
}

static struct job *queue_job(enum job_kind kind, LSHandle *sh, LSMessage *msg)
{
	struct job *j = g_new0(struct job, 1);

	j->kind = kind;
	j->sh = sh;

	if (msg)
	{
		LSMessageRef(msg);
		j->msg = msg;
	}

	j->gen = kind == JOB_SIGNAL ? g_atomic_int_get(&preempt_gen)
	                            : g_atomic_int_add(&preempt_gen, 1) + 1;
	return j;
}

static void push_job(struct job *j)
{
	g_async_queue_push(jobs, j);
}

/* ---- replies ----------------------------------------------------------- */

static void send_reply(LSHandle *sh, LSMessage *msg, jvalue_ref reply)
{
	LSError lserror;

	LSErrorInit(&lserror);

	if (!LSMessageReply(sh, msg, jvalue_tostring_simple(reply), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

static bool reply_error(LSHandle *sh, LSMessage *msg, int code, const char *text)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(code));
	jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(text));
	send_reply(sh, msg, reply);
	j_release(&reply);
	return true;
}

static jvalue_ref stations_json(const struct fm_station *s, size_t n)
{
	jvalue_ref arr = jarray_create(NULL);

	for (size_t i = 0; i < n; i++)
	{
		jarray_append(arr, jnumber_create_i32(s[i].khz));
	}

	return arr;
}

static const char *busy_text(void)
{
	if (st.scans > 0)
	{
		return "scanning";
	}

	if (st.seeks > 0)
	{
		return "seeking";
	}

	return st.namings > 0 ? "naming" : "idle";
}

static jvalue_ref names_json(void)
{
	jvalue_ref obj = jobject_create();

	for (size_t i = 0; i < st.nnames; i++)
	{
		char key[16];

		g_snprintf(key, sizeof(key), "%d", st.names[i].khz);
		jobject_put(obj, jstring_create(key), jstring_create(st.names[i].name));
	}

	return obj;
}

/* Kept for khz: true if that is news. */
static bool remember_name(int khz, const char *name)
{
	size_t i;

	for (i = 0; i < st.nnames && st.names[i].khz != khz; i++)
	{
	}

	if (i < st.nnames && strcmp(st.names[i].name, name) == 0)
	{
		return false;
	}

	/* One per channel of the band: there is always room. */
	if (i == st.nnames)
	{
		if (st.nnames == G_N_ELEMENTS(st.names))
		{
			return false;
		}

		st.nnames++;
	}

	st.names[i].khz = khz;
	g_strlcpy(st.names[i].name, name, sizeof(st.names[i].name));
	return true;
}

static jvalue_ref build_status(void)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref band = jobject_create();
	const char *audio_error = audio ? fm_audio_error(audio) : NULL;

	jobject_put(band, J_CSTR_TO_JVAL("min"), jnumber_create_i32(FM_BAND_MIN_KHZ));
	jobject_put(band, J_CSTR_TO_JVAL("max"), jnumber_create_i32(FM_BAND_MAX_KHZ));
	jobject_put(band, J_CSTR_TO_JVAL("step"), jnumber_create_i32(FM_BAND_STEP_KHZ));

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"),
	            jboolean_create(fm_tuner_detect() != NULL));
	jobject_put(reply, J_CSTR_TO_JVAL("powered"), jboolean_create(st.powered));
	jobject_put(reply, J_CSTR_TO_JVAL("frequency"), jnumber_create_i32(st.khz));
	jobject_put(reply, J_CSTR_TO_JVAL("busy"), jstring_create(busy_text()));
	jobject_put(reply, J_CSTR_TO_JVAL("audio"),
	            jboolean_create(audio && fm_audio_active(audio)));
	jobject_put(reply, J_CSTR_TO_JVAL("band"), band);
	jobject_put(reply, J_CSTR_TO_JVAL("stations"), stations_json(st.stations.s, st.stations.n));
	jobject_put(reply, J_CSTR_TO_JVAL("names"), names_json());

	if (st.powered && st.have_rssi)
	{
		jobject_put(reply, J_CSTR_TO_JVAL("rssi"), jnumber_create_i32(st.rssi));
	}

	/* What the station says of itself over RDS, once it has said it. */
	if (st.powered && (st.rds.name[0] || st.rds.text[0]))
	{
		jvalue_ref rds = jobject_create();

		if (st.rds.name[0])
		{
			jobject_put(rds, J_CSTR_TO_JVAL("name"), jstring_create(st.rds.name));
		}

		if (st.rds.text[0])
		{
			jobject_put(rds, J_CSTR_TO_JVAL("text"), jstring_create(st.rds.text));
		}

		jobject_put(reply, J_CSTR_TO_JVAL("rds"), rds);
	}

	if (audio_error)
	{
		jobject_put(reply, J_CSTR_TO_JVAL("audioError"), jstring_create(audio_error));
	}

	return reply;
}

static void post_status(void)
{
	LSError lserror;
	jvalue_ref status = build_status();

	LSErrorInit(&lserror);

	if (!LSSubscriptionReply(service_handle, "/getStatus", jvalue_tostring_simple(status),
	                         &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&status);
}

static void audio_changed(void *data)
{
	(void)data;
	post_status();
}

static gboolean name_done(gpointer data)
{
	struct name_found *f = data;

	if (remember_name(f->khz, f->name))
	{
		post_status();
	}

	g_free(f);
	return G_SOURCE_REMOVE;
}

static struct job *queue_job(enum job_kind kind, LSHandle *sh, LSMessage *msg);
static void push_job(struct job *j);

/* A NAMES job, its stations still to fill in before push_job(). */
static struct job *names_job(LSHandle *sh, LSMessage *msg)
{
	struct job *j = queue_job(JOB_NAMES, sh, msg);

	st.namings++;
	return j;
}

/* A scan's stations kept, and named next when the radio is off. */
static void scan_done(struct job *j)
{
	st.stations = j->found;

	/*
	 * Found with the radio off and nothing asked since: their names next,
	 * the same quiet way, so the list has them from the start.
	 */
	if (!st.powered && !job_cancelled(j) && st.stations.n > 0)
	{
		struct job *names = names_job(NULL, NULL);

		names->want = st.stations;
		push_job(names);
	}
}

/* A job's result, back on the main loop. */
static gboolean job_done(gpointer data)
{
	struct job *j = data;
	bool was_powered = st.powered;
	int was_khz = st.khz;
	bool had_rssi = st.have_rssi;
	int was_rssi = st.rssi;
	struct fm_rds_info was_rds = st.rds;

	if (j->kind == JOB_SCAN)
	{
		st.scans--;
	}
	else if (j->kind == JOB_SEEK)
	{
		st.seeks--;
	}
	else if (j->kind == JOB_NAMES)
	{
		st.namings--;
	}
	else if (j->kind == JOB_SIGNAL)
	{
		st.signal_queued = false;
	}

	st.powered = j->powered;

	if (j->powered)
	{
		st.khz = j->tuned_khz;
	}

	st.have_rssi = j->have_rssi;
	st.rssi = j->rssi;

	/* Another station, or none: what the last one said is not this one's.
	 * The driver clears its record on a tune as well. */
	if (!st.powered || st.khz != was_khz)
	{
		static const struct fm_rds_info none;

		st.rds = none;
	}

	if (j->have_rds)
	{
		st.rds = j->rds;
	}

	if (st.powered && st.rds.name[0])
	{
		(void)remember_name(st.khz, st.rds.name);
	}

	if (j->kind == JOB_SCAN && j->ret == 0)
	{
		scan_done(j);
	}

	/*
	 * The sound follows the receiver: on once it is up, and off already when
	 * the power off was asked for (see cb_power_off). A receiver that went
	 * down by itself - a failed power up - takes the sound with it too.
	 */
	if (audio && (st.powered != was_powered || j->kind == JOB_POWER_ON))
	{
		fm_audio_set_enabled(audio, st.powered);
	}

	if (j->msg)
	{
		if (j->ret < 0)
		{
			const char *text = j->ret == -ENOENT ? "no station found" :
			                   j->ret == -ECANCELED ? "cancelled by a newer request" :
			                   j->ret == -ENODEV ? "the radio is off" :
			                   g_strerror(-j->ret);

			reply_error(j->sh, j->msg, -j->ret, text);
		}
		else
		{
			jvalue_ref reply = jobject_create();

			jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
			jobject_put(reply, J_CSTR_TO_JVAL("frequency"), jnumber_create_i32(st.khz));

			if (j->kind == JOB_SCAN)
			{
				jobject_put(reply, J_CSTR_TO_JVAL("stations"),
				            stations_json(st.stations.s, st.stations.n));
			}

			if (j->kind == JOB_NAMES)
			{
				jobject_put(reply, J_CSTR_TO_JVAL("names"), names_json());
			}

			send_reply(j->sh, j->msg, reply);
			j_release(&reply);
		}

		LSMessageUnref(j->msg);
	}

	/* The signal is polled: told about only when something changed. */
	if (j->kind != JOB_SIGNAL || st.powered != was_powered || st.have_rssi != had_rssi ||
	        st.rssi != was_rssi || strcmp(st.rds.name, was_rds.name) != 0 ||
	        strcmp(st.rds.text, was_rds.text) != 0)
	{
		post_status();
	}

	g_free(j);
	return G_SOURCE_REMOVE;
}

/* ---- methods ----------------------------------------------------------- */

static jvalue_ref parse(LSMessage *msg)
{
	JSchemaInfo schema;

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	return jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);
}

/*
 * The "frequency" of the request: 0 if it has none, -1 if it has one that is
 * not a channel of the band.
 */
static int requested_khz(jvalue_ref parsed)
{
	jvalue_ref value;
	int32_t khz = 0;

	if (!jobject_get_exists(parsed, J_CSTR_TO_BUF("frequency"), &value))
	{
		return 0;
	}

	if (!jis_number(value) || jnumber_get_i32(value, &khz) != CONV_OK ||
	        !fm_band_valid(khz))
	{
		return -1;
	}

	return khz;
}

static bool cb_get_status(LSHandle *sh, LSMessage *msg, void *ctx)
{
	LSError lserror;
	jvalue_ref status;

	(void)ctx;
	LSErrorInit(&lserror);

	if (LSMessageIsSubscription(msg) && !LSSubscriptionProcess(sh, msg, &(bool){false}, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	status = build_status();
	send_reply(sh, msg, status);
	j_release(&status);
	return true;
}

static bool bad_frequency(LSHandle *sh, LSMessage *msg)
{
	return reply_error(sh, msg, EINVAL,
	                   "frequency must be kHz on the band's grid, 87500 - 108000 in steps of 100");
}

static bool cb_power_on(LSHandle *sh, LSMessage *msg, void *ctx)
{
	jvalue_ref parsed = parse(msg);
	int khz;
	struct job *j;

	(void)ctx;

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, msg, EINVAL, "malformed json");
	}

	khz = requested_khz(parsed);
	j_release(&parsed);

	if (khz < 0)
	{
		return bad_frequency(sh, msg);
	}

	j = queue_job(JOB_POWER_ON, sh, msg);
	j->khz = khz ? khz : st.khz;
	push_job(j);
	return true;
}

static bool cb_power_off(LSHandle *sh, LSMessage *msg, void *ctx)
{
	(void)ctx;

	/* Silent at once, not once the worker comes to it. */
	if (audio)
	{
		fm_audio_set_enabled(audio, false);
	}

	push_job(queue_job(JOB_POWER_OFF, sh, msg));
	return true;
}

static bool cb_tune(LSHandle *sh, LSMessage *msg, void *ctx)
{
	jvalue_ref parsed = parse(msg);
	int khz;
	struct job *j;

	(void)ctx;

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, msg, EINVAL, "malformed json");
	}

	khz = requested_khz(parsed);
	j_release(&parsed);

	if (khz <= 0)
	{
		return bad_frequency(sh, msg);
	}

	if (!st.powered)
	{
		/* Taken as where to start next time, so a tune before powerOn is
		 * not lost. */
		st.khz = khz;
		post_status();
		return reply_error(sh, msg, ENODEV, "the radio is off");
	}

	j = queue_job(JOB_TUNE, sh, msg);
	j->khz = khz;
	push_job(j);
	return true;
}

static bool cb_seek(LSHandle *sh, LSMessage *msg, void *ctx)
{
	jvalue_ref parsed = parse(msg);
	jvalue_ref value;
	int dir = 0;
	struct job *j;

	(void)ctx;

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, msg, EINVAL, "malformed json");
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("direction"), &value) && jis_string(value))
	{
		raw_buffer b = jstring_get_fast(value);

		if (b.m_len == 2 && strncmp(b.m_str, "up", 2) == 0)
		{
			dir = 1;
		}
		else if (b.m_len == 4 && strncmp(b.m_str, "down", 4) == 0)
		{
			dir = -1;
		}
	}

	j_release(&parsed);

	if (dir == 0)
	{
		return reply_error(sh, msg, EINVAL, "direction must be \"up\" or \"down\"");
	}

	/*
	 * Not refused here when the radio is off: a powerOn sent just before
	 * is queued ahead of it and not done yet. The worker finds out, in
	 * order (ENODEV).
	 */
	j = queue_job(JOB_SEEK, sh, msg);
	j->dir = dir;
	st.seeks++;
	push_job(j);
	post_status();
	return true;
}

static bool cb_scan(LSHandle *sh, LSMessage *msg, void *ctx)
{
	struct job *j;

	(void)ctx;
	j = queue_job(JOB_SCAN, sh, msg);
	/* Where to power up, if the radio is off when the worker comes to it. */
	j->khz = st.khz;
	st.scans++;
	push_job(j);
	post_status();
	return true;
}

/*
 * The names of stations (frequency in kHz each; the last scan's when none
 * are given), looked for with the radio off and no sound. Answered once all
 * were tried, with every name known; each name found is in the status at
 * once. Refused while the radio is on.
 */
static bool cb_find_names(LSHandle *sh, LSMessage *msg, void *ctx)
{
	jvalue_ref parsed = parse(msg);
	jvalue_ref list;
	struct station_list want = { 0 };
	struct job *j;

	(void)ctx;

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, msg, EINVAL, "malformed json");
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("stations"), &list))
	{
		ssize_t len = jis_array(list) ? jarray_size(list) : -1;

		if (len < 0 || len > FM_BAND_CHANNELS)
		{
			j_release(&parsed);
			return reply_error(sh, msg, EINVAL, "stations must be an array of frequencies");
		}

		for (want.n = 0; want.n < (size_t)len; want.n++)
		{
			int32_t k = 0;
			jvalue_ref v = jarray_get(list, (ssize_t)want.n);

			if (!jis_number(v) || jnumber_get_i32(v, &k) != CONV_OK || !fm_band_valid(k))
			{
				j_release(&parsed);
				return bad_frequency(sh, msg);
			}

			want.s[want.n] = (struct fm_station){ .khz = k };
		}
	}
	else
	{
		want = st.stations;
	}

	j_release(&parsed);

	if (st.powered)
	{
		return reply_error(sh, msg, EBUSY, "the radio is on");
	}

	j = names_job(sh, msg);
	j->want = want;
	push_job(j);
	post_status();
	return true;
}

static LSMethod methods[] =
{
	{ "getStatus", cb_get_status, LUNA_METHOD_FLAGS_NONE },
	{ "powerOn",   cb_power_on,   LUNA_METHOD_FLAGS_NONE },
	{ "powerOff",  cb_power_off,  LUNA_METHOD_FLAGS_NONE },
	{ "tune",      cb_tune,       LUNA_METHOD_FLAGS_NONE },
	{ "seek",      cb_seek,       LUNA_METHOD_FLAGS_NONE },
	{ "scan",      cb_scan,       LUNA_METHOD_FLAGS_NONE },
	{ "findNames", cb_find_names, LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

/* ---- main -------------------------------------------------------------- */

/* The signal strength, while the radio is on and nothing else is queued. */
static gboolean poll_signal(gpointer data)
{
	(void)data;

	if (st.powered && !st.signal_queued && st.scans == 0 && st.seeks == 0)
	{
		st.signal_queued = true;
		push_job(queue_job(JOB_SIGNAL, NULL, NULL));
	}

	return G_SOURCE_CONTINUE;
}

/*
 * The hub went away. Its registration cannot be made again from here, and a
 * process that answers nothing is worse than none: exit non-zero for systemd
 * to start a fresh one - through the cleanup in main(), which switches the
 * radio off (see torchd).
 */
static void hub_disconnected(LSHandle *sh, void *ctx)
{
	(void)sh;
	(void)ctx;
	g_warning("lost the luna-service2 hub; exiting so systemd restarts us");
	exit_status = 1;
	g_main_loop_quit(main_loop);
}

/* Kept, not removed: main() removes it once the loop is done. */
static gboolean on_quit_signal(gpointer data)
{
	(void)data;
	g_main_loop_quit(main_loop);
	return G_SOURCE_CONTINUE;
}

/* Jobs whose results were never taken: their messages go unanswered. */
static gboolean drop_done(gpointer data)
{
	struct job *j = data;

	if (j->msg)
	{
		LSMessageUnref(j->msg);
	}

	g_free(j);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	LSError lserror;
	guint poll_id, term_id, int_id;
	struct job *quit;

	(void)argc;
	(void)argv;

	main_loop = g_main_loop_new(NULL, FALSE);
	LSErrorInit(&lserror);

	if (!LSRegister(FMRADIO_SERVICE, &service_handle, &lserror) ||
	        !LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror) ||
	        !LSSetDisconnectHandler(service_handle, hub_disconnected, NULL, &lserror) ||
	        !LSGmainAttach(service_handle, main_loop, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		g_main_loop_unref(main_loop);
		return 1;
	}

	jobs = g_async_queue_new();
	worker = g_thread_new("fm-tuner", worker_main, NULL);
	{
		const struct fm_tuner_ops *ops = fm_tuner_detect();

		audio = fm_audio_new(FM_PULSE_SERVER, ops ? ops->audio_args : NULL, audio_changed, NULL);
	}

	poll_id = g_timeout_add(FM_SIGNAL_INTERVAL_MS, poll_signal, NULL);
	term_id = g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
	int_id = g_unix_signal_add(SIGINT, on_quit_signal, NULL);

	g_main_loop_run(main_loop);

	g_source_remove(poll_id);
	g_source_remove(term_id);
	g_source_remove(int_id);

	/* The sound first, then the receiver: a patch left on a powered-down
	 * tuner is the HAL's FM path held open on silence. */
	fm_audio_free(audio);
	audio = NULL;

	quit = g_new0(struct job, 1);
	quit->kind = JOB_QUIT;
	g_atomic_int_inc(&preempt_gen);
	g_async_queue_push(jobs, quit);
	/* Not a plain join: powering the receiver down waits for the chip's
	 * answers, which the helium link delivers on this thread. */
	fm_join_dispatching(worker, &worker_done);

	/* Results the worker sent after the loop stopped. */
	while (g_main_context_pending(NULL))
	{
		(void)g_main_context_iteration(NULL, FALSE);
	}

	/* Still queued behind the quit: nothing will run them now. */
	for (struct job *j; (j = g_async_queue_try_pop(jobs)) != NULL;)
	{
		drop_done(j);
	}

	g_async_queue_unref(jobs);
	LSUnregister(service_handle, &lserror);
	g_main_loop_unref(main_loop);
	return exit_status;
}
