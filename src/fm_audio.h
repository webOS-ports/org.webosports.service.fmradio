// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_AUDIO_H
#define FM_AUDIO_H

#include <stdbool.h>

/*
 * The radio's sound: module-droid-fm (pulseaudio-modules-droid) loaded into
 * PulseAudio while the radio is on. The module asks the audio HAL for the
 * FM tuner -> output device patch and stands for it as a stream on the "fm"
 * sink, so the policy routes, ducks and sets the volume of the radio like
 * any other stream. Nothing here touches samples.
 *
 * Asynchronous, on the GLib main context: fm_audio_set_enabled() says what
 * is wanted, and the module is loaded or unloaded as soon as PulseAudio is
 * there. A PulseAudio that goes away is connected to again, and the module
 * loaded again if it is still wanted. On connecting, a module-droid-fm left
 * behind by an earlier run is taken over if wanted, else unloaded.
 */

struct fm_audio;

typedef void (*fm_audio_changed_fn)(void *data);

/*
 * server: the PulseAudio server, e.g. "unix:/var/run/pulse/native". Named,
 * never found through PULSE_RUNTIME_PATH: a root client given that path
 * makes libpulse take the directory over (root, 0700), and every other
 * client of the system daemon is locked out.
 */
struct fm_audio *fm_audio_new(const char *server, const char *module_args,
                              fm_audio_changed_fn changed, void *data);

/* Unloads the module, waiting up to a second for it, and disconnects. */
void fm_audio_free(struct fm_audio *a);

void fm_audio_set_enabled(struct fm_audio *a, bool on);

/* The module is loaded. */
bool fm_audio_active(const struct fm_audio *a);

/* Why the last load failed, or NULL. Cleared by fm_audio_set_enabled(). */
const char *fm_audio_error(const struct fm_audio *a);

#endif
