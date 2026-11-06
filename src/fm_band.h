// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_BAND_H
#define FM_BAND_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The FM band as the service offers it: 87.5 - 108 MHz in 100 kHz steps (the
 * European/worldwide plan, which the MediaTek driver is powered up with).
 * Every frequency on the bus is an integer number of kHz: 95000 = 95.0 MHz.
 */
#define FM_BAND_MIN_KHZ  87500
#define FM_BAND_MAX_KHZ  108000
#define FM_BAND_STEP_KHZ 100
#define FM_BAND_CHANNELS ((FM_BAND_MAX_KHZ - FM_BAND_MIN_KHZ) / FM_BAND_STEP_KHZ + 1)

struct fm_station
{
	int khz;
	int strength;   /* the driver's own measure, only for comparing stations */
};

/* In the band and on its grid. */
bool fm_band_valid(int khz);

/* The next channel up (dir > 0) or down (dir < 0), wrapping at the ends. */
int fm_band_next(int khz, int dir);

/*
 * A station is heard on the channel next to it as well (91.0 and 91.1 MHz
 * both report valid). Merges each run of neighbouring channels in s - sorted
 * by frequency - into its strongest one, in place. Returns the new count.
 */
size_t fm_band_merge(struct fm_station *s, size_t n);

#endif
