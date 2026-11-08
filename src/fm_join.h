// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_JOIN_H
#define FM_JOIN_H

#include <glib.h>

/*
 * Joins thread, turning the default main context until it has finished.
 *
 * For a thread that may still need the main thread while it winds down: the
 * tuner worker powers the receiver down at exit, and libgbinder hands the
 * Bluetooth HAL's answers (and its calls back to us) to the main context. A
 * plain g_thread_join() there blocks the one thread that could deliver them.
 *
 * The thread sets *done (g_atomic_int_set) and then calls
 * fm_join_thread_done(), as the last thing it does.
 */
void fm_join_dispatching(GThread *thread, gint *done);

/* From the thread: *done set, the main context woken to see it. */
void fm_join_thread_done(gint *done);

#endif
