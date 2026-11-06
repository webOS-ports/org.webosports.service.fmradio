// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_band.h"

bool fm_band_valid(int khz)
{
	return khz >= FM_BAND_MIN_KHZ && khz <= FM_BAND_MAX_KHZ &&
	       (khz - FM_BAND_MIN_KHZ) % FM_BAND_STEP_KHZ == 0;
}

int fm_band_next(int khz, int dir)
{
	int next = khz + (dir < 0 ? -FM_BAND_STEP_KHZ : FM_BAND_STEP_KHZ);

	if (next > FM_BAND_MAX_KHZ)
	{
		return FM_BAND_MIN_KHZ;
	}

	if (next < FM_BAND_MIN_KHZ)
	{
		return FM_BAND_MAX_KHZ;
	}

	return next;
}

size_t fm_band_merge(struct fm_station *s, size_t n)
{
	size_t out = 0;
	size_t i = 0;

	while (i < n)
	{
		/*
		 * A run is channels each one step from the one before it, so
		 * 91.0/91.1/91.2 is one station wherever in it the peak is. On a
		 * tie the lower channel stays, which is where scanning up met it.
		 */
		size_t best = i;
		size_t j = i + 1;

		while (j < n && s[j].khz - s[j - 1].khz <= FM_BAND_STEP_KHZ)
		{
			if (s[j].strength > s[best].strength)
			{
				best = j;
			}

			j++;
		}

		s[out++] = s[best];
		i = j;
	}

	return out;
}
