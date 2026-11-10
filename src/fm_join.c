// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_join.h"

void fm_join_dispatching(GThread *thread, gint *done)
{
	/* A wakeup that comes before the poll is not lost: it makes the next
	 * one return at once. */
	while (!g_atomic_int_get(done))
	{
		(void)g_main_context_iteration(NULL, TRUE);
	}

	(void)g_thread_join(thread);
}

void fm_join_thread_done(gint *done)
{
	g_atomic_int_set(done, 1);
	g_main_context_wakeup(NULL);
}
