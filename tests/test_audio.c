// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fm_audio's connection to PulseAudio, with the real libpulse against a
 * socket that stands in for the server: it refuses, or takes connections and
 * says nothing, which leaves each client waiting for the handshake and so
 * shows how many connections fmradiod holds. Built with short retry delays
 * (FM_AUDIO_RETRY_MS, FM_AUDIO_RETRY_MAX_MS).
 *
 * The case the KEY2 hit: fmradiod starts before PulseAudio, the socket
 * refuses, and libpulse reports that from inside pa_context_connect().
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/fm_audio.h"

#define MAX_CONNS 256
/* More warnings than this: the retries ran away, the test stops turning. */
#define RUNAWAY 1000

static int failures;
static char *sock_path;
static char *server;
static unsigned int warnings;

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
			return; \
		} \
	} while (0)

static void count_log(const gchar *domain, GLogLevelFlags level, const gchar *message,
                      gpointer data)
{
	(void)domain;
	(void)data;

	if (level & G_LOG_LEVEL_WARNING)
	{
		warnings++;
	}

	if (g_getenv("TEST_VERBOSE"))
	{
		printf("  log: %s\n", message);
	}
}

/* Bound, so connecting is refused; listen() later makes it a server. */
static int server_socket(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	g_assert(fd >= 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	g_assert(strlen(sock_path) < sizeof(sa.sun_path));
	strcpy(sa.sun_path, sock_path);
	(void)unlink(sock_path);
	g_assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	return fd;
}

struct conns
{
	int fd[MAX_CONNS];
	unsigned int open;      /* in fd[] */
	unsigned int total;     /* ever accepted */
};

static void accept_all(int lfd, struct conns *c)
{
	int fd;

	while (lfd >= 0 && (fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC)) >= 0)
	{
		c->total++;

		if (c->open < MAX_CONNS)
		{
			c->fd[c->open++] = fd;
		}
		else
		{
			close(fd);
		}
	}
}

static void close_all(struct conns *c)
{
	for (unsigned int i = 0; i < c->open; i++)
	{
		close(c->fd[i]);
	}

	c->open = 0;
}

/* Turns the main loop for ms, taking every connection that comes. */
static void run_for(unsigned int ms, int lfd, struct conns *c)
{
	gint64 until = g_get_monotonic_time() + (gint64)ms * G_TIME_SPAN_MILLISECOND;

	while (g_get_monotonic_time() < until)
	{
		while (g_main_context_iteration(NULL, FALSE))
		{
			/* Retries that multiply never let the loop go idle. */
			if (warnings > RUNAWAY)
			{
				return;
			}
		}

		if (c)
		{
			accept_all(lfd, c);
		}

		g_usleep(1000);
	}
}

/*
 * PulseAudio not up yet: the socket refuses for a while (several retry
 * periods), then listens. One connection, and one warning for the outage.
 */
static void test_refused_then_up(void)
{
	struct conns c = { .open = 0 };
	struct fm_audio *a;
	int lfd = server_socket();

	warnings = 0;
	a = fm_audio_new(server, "mode=parameters", NULL, NULL);
	run_for(8 * FM_AUDIO_RETRY_MS, -1, NULL);
	CHECK(warnings <= 1);

	g_assert(listen(lfd, MAX_CONNS) == 0);
	run_for(4 * FM_AUDIO_RETRY_MAX_MS, lfd, &c);
	fm_audio_free(a);
	close_all(&c);
	close(lfd);
	CHECK(c.total == 1);
}

/* No socket at all: the same, and nothing left when freed. */
static void test_missing_socket(void)
{
	struct fm_audio *a;

	(void)unlink(sock_path);
	warnings = 0;
	a = fm_audio_new(server, NULL, NULL, NULL);
	run_for(8 * FM_AUDIO_RETRY_MS, -1, NULL);
	fm_audio_free(a);
	CHECK(warnings <= 1);
}

/*
 * PulseAudio goes away while connected (here: the server drops the
 * connection) and comes back: one connection again each time.
 */
static void test_dropped_reconnects_once(void)
{
	struct conns c = { .open = 0 };
	struct fm_audio *a;
	int lfd = server_socket();

	g_assert(listen(lfd, MAX_CONNS) == 0);
	a = fm_audio_new(server, NULL, NULL, NULL);

	for (unsigned int round = 1; round <= 3; round++)
	{
		run_for(4 * FM_AUDIO_RETRY_MAX_MS, lfd, &c);

		if (c.total != round || c.open != 1)
		{
			printf("  round %u: %u accepted, %u open\n", round, c.total, c.open);
		}

		CHECK(c.total == round);
		CHECK(c.open == 1);
		close_all(&c);
	}

	fm_audio_free(a);
	run_for(10, lfd, &c);
	close_all(&c);
	close(lfd);
}

/* Freed while waiting to retry: the timer goes with it. */
static void test_free_while_waiting(void)
{
	struct fm_audio *a;

	(void)unlink(sock_path);
	a = fm_audio_new(server, NULL, NULL, NULL);
	run_for(FM_AUDIO_RETRY_MS / 2, -1, NULL);
	fm_audio_free(a);
	/* A timer left behind would run on the freed fm_audio now. */
	run_for(4 * FM_AUDIO_RETRY_MAX_MS, -1, NULL);
	CHECK(true);
}

#define RUN(t) \
	do { \
		int before = failures; \
		t(); \
		printf("%s %s\n", failures == before ? "ok" : "not ok", #t); \
	} while (0)

int main(void)
{
	char *cwd = g_get_current_dir();

	sock_path = g_strdup_printf("%s/fm-audio-test-%d.sock", cwd, (int)getpid());
	server = g_strdup_printf("unix:%s", sock_path);
	g_free(cwd);
	g_log_set_default_handler(count_log, NULL);

	RUN(test_refused_then_up);
	RUN(test_missing_socket);
	RUN(test_dropped_reconnects_once);
	RUN(test_free_while_waiting);

	(void)unlink(sock_path);
	g_free(sock_path);
	g_free(server);
	return failures ? 1 : 0;
}
