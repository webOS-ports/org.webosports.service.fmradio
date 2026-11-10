// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fm_join_dispatching() against a thread that, as it winds down, waits for
 * something only the main thread delivers: the shape of the tuner worker
 * powering the helium receiver down at exit, with libgbinder handing the
 * chip's answers to the main context.
 *
 * The answers are idle sources, as libgbinder attaches them: they run only
 * when the main context is turned. (g_main_context_invoke() would not do: on
 * a context no thread owns it runs the function at once, in the caller.)
 */

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

#include "../src/fm_join.h"

/* How long the thread waits for its answer before it gives up. */
#define ANSWER_WAIT_MS 2000

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
			return; \
		} \
	} while (0)

struct winding_down
{
	GMutex lock;
	GCond cond;
	bool answered;          /* by the main thread */
	bool got_answer;        /* the thread saw it in time */
	unsigned int asks;      /* main-context calls to make first */
	gint done;
};

static gboolean answer(gpointer data)
{
	struct winding_down *w = data;

	g_mutex_lock(&w->lock);
	w->answered = true;
	g_cond_broadcast(&w->cond);
	g_mutex_unlock(&w->lock);
	return G_SOURCE_REMOVE;
}

static gpointer thread_main(gpointer data)
{
	struct winding_down *w = data;
	gint64 until;

	/* Calls queued on the way, before the one waited for. */
	for (unsigned int i = 1; i < w->asks; i++)
	{
		(void)g_idle_add(answer, w);
	}

	(void)g_idle_add(answer, w);
	until = g_get_monotonic_time() + (gint64)ANSWER_WAIT_MS * G_TIME_SPAN_MILLISECOND;

	g_mutex_lock(&w->lock);

	while (!w->answered && g_cond_wait_until(&w->cond, &w->lock, until))
	{
	}

	w->got_answer = w->answered;
	g_mutex_unlock(&w->lock);

	fm_join_thread_done(&w->done);
	return NULL;
}

static void run(unsigned int asks, bool *got_answer, gint64 *took_ms)
{
	struct winding_down w = { .asks = asks };
	gint64 start = g_get_monotonic_time();
	GThread *t;

	g_mutex_init(&w.lock);
	g_cond_init(&w.cond);
	t = g_thread_new("winding-down", thread_main, &w);
	fm_join_dispatching(t, &w.done);
	*took_ms = (g_get_monotonic_time() - start) / G_TIME_SPAN_MILLISECOND;
	*got_answer = w.got_answer;

	/* What is still queued runs before w goes, as main.c drains after the
	 * join. */
	while (g_main_context_pending(NULL))
	{
		(void)g_main_context_iteration(NULL, FALSE);
	}

	g_mutex_clear(&w.lock);
	g_cond_clear(&w.cond);
}

/* The answer the thread waits for reaches it, and the join does not sit
 * out the thread's timeout. */
static void test_answer_delivered(void)
{
	bool got;
	gint64 ms;

	run(1, &got, &ms);
	CHECK(got);
	CHECK(ms < ANSWER_WAIT_MS / 2);
}

/* Several calls queued before the one it waits for. */
static void test_queued_results_first(void)
{
	bool got;
	gint64 ms;

	run(5, &got, &ms);
	CHECK(got);
	CHECK(ms < ANSWER_WAIT_MS / 2);
}

/* A thread that needs nothing: the join returns, many times over (a lost
 * wakeup would hang one of them). */
static gpointer quick_main(gpointer data)
{
	fm_join_thread_done(data);
	return NULL;
}

static void test_quick_threads(void)
{
	for (unsigned int i = 0; i < 2000; i++)
	{
		gint done = 0;
		GThread *t = g_thread_new("quick", quick_main, &done);

		fm_join_dispatching(t, &done);
		CHECK(g_atomic_int_get(&done) == 1);
	}
}

#define RUN(t) \
	do { \
		int before = failures; \
		t(); \
		printf("%s %s\n", failures == before ? "ok" : "not ok", #t); \
	} while (0)

int main(void)
{
	RUN(test_answer_delivered);
	RUN(test_queued_results_first);
	RUN(test_quick_threads);
	return failures ? 1 : 0;
}
