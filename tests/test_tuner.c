// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fm_band, fm_tuner and fm_rds against a stand-in for the MediaTek chip: a
 * band with a few stations, each heard on its neighbours too, as the real one
 * does, and the RDS record its driver hands over.
 */

#include <errno.h>
#include <stddef.h>
#include <glib.h>
#include <string.h>

#include "../src/fm_band.h"
#include "../src/fm_rds.h"
#include "../src/fm_tuner.h"

/* ---- the stand-in chip --------------------------------------------------- */

#define MAX_STATIONS 8

static struct
{
	bool powered;
	int freq;                       /* driver units, 10 kHz */
	bool searching;
	int calls[64];                  /* per ioctl number */
	int fail_nr;                    /* this ioctl fails ... */
	int fail_errno;                 /* ... with this */
	int fail_freq;                  /* or soft-mute tunes of this channel do */
	int refuse_freq;                /* power up and tune set err for this one */
	int cancel_after;               /* soft-mute tunes before cancelling, -1 never */
	int probes;
	bool rds_on;
	long read_ret;                  /* what read() returns; 0: sizeof the record */
	struct fm_rds_raw rds;          /* what read() hands over */
	/* Or, when there are any, the record each station's RDS fills in. */
	struct { int khz; const char *name; unsigned int name_after_ms; } senders[MAX_STATIONS];
	size_t nsenders;
	unsigned int clock_ms;          /* advanced by the tuner's sleeps */
	unsigned int tuned_at_ms;
	unsigned int read_fails_until_ms; /* read() fails with EIO before then */
	struct { int khz; int strength; } stations[MAX_STATIONS];
	size_t nstations;
} chip;

static void chip_reset(void)
{
	memset(&chip, 0, sizeof(chip));
	chip.fail_nr = -1;
	chip.cancel_after = -1;
}

static void chip_station(int khz, int strength)
{
	chip.stations[chip.nstations].khz = khz;
	chip.stations[chip.nstations].strength = strength;
	chip.nstations++;
}

/* A station is valid on its own channel and the ones next to it, weaker. */
static bool chip_probe(int khz, int *strength)
{
	bool valid = false;

	*strength = -300;

	for (size_t i = 0; i < chip.nstations; i++)
	{
		int d = abs(khz - chip.stations[i].khz);

		if (d <= FM_BAND_STEP_KHZ)
		{
			int s = chip.stations[i].strength - (d ? 20 : 0);

			if (!valid || s > *strength)
			{
				*strength = s;
			}

			valid = true;
		}
	}

	return valid;
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
	int nr = fm_tuner_request_nr(request);

	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(nr, >=, 0);
	g_assert_cmpint(nr, <, 64);
	chip.calls[nr]++;

	if (nr == chip.fail_nr)
	{
		return -chip.fail_errno;
	}

	switch (nr)
	{
		case FM_NR_POWERUP:
		{
			struct fm_tune_parm *p = arg;

			g_assert_false(chip.powered);

			if (p->freq * 10 == chip.refuse_freq)
			{
				p->err = 1;
				return 0;
			}

			chip.powered = true;
			chip.freq = p->freq;
			return 0;
		}

		case FM_NR_POWERDOWN:
			g_assert_cmpint(*(int *)arg, ==, 0);
			chip.powered = false;
			chip.rds_on = false;
			return 0;

		case FM_NR_RDS_ONOFF:
		{
			const uint16_t *on = arg;

			if (!chip.powered)
			{
				return -EPERM;
			}

			g_assert_true(*on == 0 || *on == 1);
			chip.rds_on = *on == 1;
			return 0;
		}

		case FM_NR_TUNE:
		{
			struct fm_tune_parm *p = arg;

			if (!chip.powered)
			{
				return -EPERM;
			}

			g_assert_cmpint(p->band, ==, 1);
			g_assert_cmpint(p->space, ==, 1);

			if (p->freq * 10 == chip.refuse_freq)
			{
				p->err = 1;
				return 0;
			}

			chip.freq = p->freq;
			/* The driver clears its record on a tune. */
			chip.tuned_at_ms = chip.clock_ms;
			return 0;
		}

		case FM_NR_GETRSSI:
		{
			int s;

			*(int *)arg = chip_probe(chip.freq * 10, &s) ? -70 : -95;
			return 0;
		}

		case FM_NR_PRE_SEARCH:
			g_assert_false(chip.searching);
			chip.searching = true;
			return 0;

		case FM_NR_RESTORE_SEARCH:
			g_assert_true(chip.searching);
			chip.searching = false;
			return 0;

		case FM_NR_SOFT_MUTE_TUNE:
		{
			struct fm_softmute_tune_t *p = arg;
			int s;

			/* Only ever inside PRE_SEARCH/RESTORE_SEARCH, as Android does. */
			g_assert_true(chip.searching);
			chip.probes++;

			if (p->freq * 10 == chip.fail_freq)
			{
				return -EIO;
			}

			chip.freq = p->freq;
			p->valid = chip_probe(p->freq * 10, &s);
			p->rssi = s;
			return 0;
		}

		default:
			g_assert_not_reached();
	}

	return 0;
}

static bool fake_cancelled(void *data)
{
	(void)data;
	return chip.cancel_after >= 0 && chip.probes >= chip.cancel_after;
}

static long fake_read(int fd, void *buf, size_t len)
{
	g_assert_cmpint(fd, >=, 0);

	if (chip.read_ret != 0)
	{
		return chip.read_ret;
	}

	if (chip.clock_ms < chip.read_fails_until_ms)
	{
		return -EIO;
	}

	/* The driver: the whole record, or nothing for a smaller buffer. */
	if (len < sizeof(chip.rds))
	{
		return 0;
	}

	if (chip.nsenders > 0)
	{
		unsigned int since = chip.clock_ms - chip.tuned_at_ms;

		memset(&chip.rds, 0, sizeof(chip.rds));

		for (size_t i = 0; i < chip.nsenders; i++)
		{
			if (chip.senders[i].khz != chip.freq * 10)
			{
				continue;
			}

			/* A group every 100 ms; the name confirmed after a while. */
			chip.rds.gc.total = since / 100;

			if (chip.senders[i].name && since >= chip.senders[i].name_after_ms)
			{
				memcpy(chip.rds.ps_data.ps[3], chip.senders[i].name, 8);
			}
		}
	}

	memcpy(buf, &chip.rds, sizeof(chip.rds));
	return (long)sizeof(chip.rds);
}

static void fake_sleep(unsigned int ms)
{
	chip.clock_ms += ms;
}

/* A station sending RDS; name NULL: groups, but no name confirmed. */
static void chip_sender(int khz, const char *name, unsigned int after_ms)
{
	chip.senders[chip.nsenders].khz = khz;
	chip.senders[chip.nsenders].name = name;
	chip.senders[chip.nsenders].name_after_ms = after_ms;
	chip.nsenders++;
}

/* A tuner on a stand-in descriptor, powered up at khz. */
static void tuner_up(struct fm_tuner *t, int khz)
{
	fm_tuner_init(t, fake_ioctl);
	t->read_fn = fake_read;
	t->sleep_fn = fake_sleep;
	t->fd = 1000;
	g_assert_cmpint(fm_tuner_power_up(t, khz), ==, 0);
}

/* ---- fm_band ------------------------------------------------------------- */

static void test_band_valid(void)
{
	g_assert_true(fm_band_valid(87500));
	g_assert_true(fm_band_valid(108000));
	g_assert_true(fm_band_valid(95000));
	g_assert_false(fm_band_valid(87400));
	g_assert_false(fm_band_valid(108100));
	g_assert_false(fm_band_valid(95050));
	g_assert_false(fm_band_valid(0));
	g_assert_false(fm_band_valid(-95000));
}

static void test_band_next_wraps(void)
{
	g_assert_cmpint(fm_band_next(95000, 1), ==, 95100);
	g_assert_cmpint(fm_band_next(95000, -1), ==, 94900);
	g_assert_cmpint(fm_band_next(108000, 1), ==, 87500);
	g_assert_cmpint(fm_band_next(87500, -1), ==, 108000);
}

static void test_band_merge_keeps_each_runs_peak(void)
{
	struct fm_station s[] =
	{
		{ 89100, -230 },
		{ 91000, -211 }, { 91100, -226 },               /* peak first */
		{ 94900, -240 }, { 95000, -179 }, { 95100, -200 }, /* peak inside */
		{ 98800, -260 }, { 98900, -250 },               /* peak last */
		{ 104000, -225 },
	};
	size_t n = fm_band_merge(s, G_N_ELEMENTS(s));

	g_assert_cmpuint(n, ==, 5);
	g_assert_cmpint(s[0].khz, ==, 89100);
	g_assert_cmpint(s[1].khz, ==, 91000);
	g_assert_cmpint(s[2].khz, ==, 95000);
	g_assert_cmpint(s[3].khz, ==, 98900);
	g_assert_cmpint(s[4].khz, ==, 104000);
}

static void test_band_merge_tie_keeps_lower(void)
{
	struct fm_station s[] = { { 91000, -200 }, { 91100, -200 } };

	g_assert_cmpuint(fm_band_merge(s, 2), ==, 1);
	g_assert_cmpint(s[0].khz, ==, 91000);
}

static void test_band_merge_empty(void)
{
	g_assert_cmpuint(fm_band_merge(NULL, 0), ==, 0);
}

/* ---- fm_tuner ------------------------------------------------------------ */

static void test_request_encoding(void)
{
	/* What the mindphone's fmradio_drv dispatches on (armv7: 4-byte size). */
	if (sizeof(void *) == 4)
	{
		g_assert_cmphex(fm_tuner_request(FM_NR_POWERUP), ==, 0xc004f500);
		g_assert_cmphex(fm_tuner_request(FM_NR_SOFT_MUTE_TUNE), ==, 0xc004f53f);
	}

	g_assert_cmpint(fm_tuner_request_nr(fm_tuner_request(FM_NR_GETRSSI)), ==, FM_NR_GETRSSI);
	g_assert_cmpint(fm_tuner_request_nr(0x5401), ==, -1);
}

static void test_power_up_and_down(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	g_assert_true(t.powered);
	g_assert_cmpint(chip.freq, ==, 9500);

	/* Powered already: a power up is a tune. */
	g_assert_cmpint(fm_tuner_power_up(&t, 96000), ==, 0);
	g_assert_cmpint(chip.calls[FM_NR_POWERUP], ==, 1);
	g_assert_cmpint(chip.freq, ==, 9600);

	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_false(chip.powered);
	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_cmpint(chip.calls[FM_NR_POWERDOWN], ==, 1);
}

static void test_power_up_rejects_off_band(void)
{
	struct fm_tuner t;

	chip_reset();
	fm_tuner_init(&t, fake_ioctl);
	t.fd = 1000;
	g_assert_cmpint(fm_tuner_power_up(&t, 95050), ==, -EINVAL);
	g_assert_cmpint(fm_tuner_power_up(&t, 120000), ==, -EINVAL);
	g_assert_cmpint(chip.calls[FM_NR_POWERUP], ==, 0);
}

static void test_power_up_failure_leaves_it_down(void)
{
	struct fm_tuner t;

	chip_reset();
	chip.fail_nr = FM_NR_POWERUP;
	chip.fail_errno = EIO;
	fm_tuner_init(&t, fake_ioctl);
	t.fd = 1000;
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
	g_assert_false(t.powered);
}

/* The driver says no in the struct, not the return: err set, 0 returned. */
static void test_refused_frequency(void)
{
	struct fm_tuner t;

	chip_reset();
	chip.refuse_freq = 95000;
	fm_tuner_init(&t, fake_ioctl);
	t.fd = 1000;
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
	g_assert_false(t.powered);

	g_assert_cmpint(fm_tuner_power_up(&t, 96000), ==, 0);
	g_assert_cmpint(fm_tuner_tune(&t, 95000), ==, -EIO);
	g_assert_cmpint(t.khz, ==, 96000);
}

static void test_tune_needs_power(void)
{
	struct fm_tuner t;

	chip_reset();
	fm_tuner_init(&t, fake_ioctl);
	t.fd = 1000;
	g_assert_cmpint(fm_tuner_tune(&t, 95000), ==, -ENODEV);
	g_assert_cmpint(chip.calls[FM_NR_TUNE], ==, 0);
}

static void test_no_descriptor(void)
{
	struct fm_tuner t;

	chip_reset();
	fm_tuner_init(&t, fake_ioctl);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EBADF);
	g_assert_cmpint(fm_tuner_open(&t, "/nonexistent/fm"), ==, -ENOENT);
	fm_tuner_close(&t);
}

static void test_rssi(void)
{
	struct fm_tuner t;
	int dbm = 0;

	chip_reset();
	chip_station(95000, -180);
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, -70);
	fm_tuner_power_down(&t);
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, -ENODEV);
}

static void test_scan_finds_merged_stations_and_goes_home(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	chip_station(87500, -200);      /* the band's edges count too */
	chip_station(91000, -211);
	chip_station(95000, -179);
	chip_station(108000, -240);
	tuner_up(&t, 100000);

	g_assert_cmpint(fm_tuner_scan(&t, fake_cancelled, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 4);
	g_assert_cmpint(out[0].khz, ==, 87500);
	g_assert_cmpint(out[1].khz, ==, 91000);
	g_assert_cmpint(out[2].khz, ==, 95000);
	g_assert_cmpint(out[3].khz, ==, 108000);
	g_assert_cmpint(chip.probes, ==, FM_BAND_CHANNELS);
	g_assert_false(chip.searching);
	g_assert_cmpint(chip.freq, ==, 10000);
	g_assert_cmpint(t.khz, ==, 100000);
}

static void test_scan_skips_a_channel_that_fails(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 0;

	chip_reset();
	chip_station(91000, -211);
	chip_station(95000, -179);
	chip.fail_freq = 93000;
	tuner_up(&t, 100000);

	/* The failed channel is skipped and the scan goes on past it. */
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpint(chip.probes, ==, FM_BAND_CHANNELS);
	g_assert_cmpuint(n, ==, 2);
	g_assert_cmpint(out[0].khz, ==, 91000);
	g_assert_cmpint(out[1].khz, ==, 95000);
}

static void test_scan_cancelled(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	chip_station(91000, -211);
	chip.cancel_after = 10;
	tuner_up(&t, 100000);

	g_assert_cmpint(fm_tuner_scan(&t, fake_cancelled, NULL, out, &n), ==, -ECANCELED);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(chip.probes, ==, 10);
	g_assert_false(chip.searching);
	g_assert_cmpint(chip.freq, ==, 10000);
}

static void test_scan_needs_power(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	fm_tuner_init(&t, fake_ioctl);
	t.fd = 1000;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -ENODEV);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(chip.calls[FM_NR_PRE_SEARCH], ==, 0);
}

static void test_scan_pre_search_fails(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	tuner_up(&t, 100000);
	chip.fail_nr = FM_NR_PRE_SEARCH;
	chip.fail_errno = EPERM;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EPERM);
	g_assert_cmpint(chip.probes, ==, 0);
}

static void test_seek_up_lands_on_the_peak(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(95000, -179);
	chip_station(104000, -225);
	tuner_up(&t, 91000);

	/* Meets 94.9 first, climbs to 95.0, stops at the weaker 95.1. */
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 9500);
	g_assert_false(chip.searching);

	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 104000);
}

static void test_seek_down_and_wrap(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(104000, -225);
	tuner_up(&t, 91000);

	/* Down from 91.0 passes the bottom of the band and wraps to the top. */
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 104000);
}

static void test_seek_skips_the_station_it_is_on(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(95000, -179);
	chip_station(98000, -200);
	tuner_up(&t, 95000);

	/* 95.1 still shows 95.0, weaker - but it is the station already on, so
	 * the seek must not stop there. */
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 98000);
}

static void test_seek_finds_nothing(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);

	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	/* The channel it is on, then every channel round to it again. */
	g_assert_cmpint(chip.probes, ==, FM_BAND_CHANNELS + 1);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 9500);
	g_assert_false(chip.searching);
}

static void test_seek_cancelled_goes_home(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(104000, -225);
	chip.cancel_after = 5;
	tuner_up(&t, 95000);

	g_assert_cmpint(fm_tuner_seek(&t, 1, fake_cancelled, NULL), ==, -ECANCELED);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 9500);
}

/* ---- fm_rds -------------------------------------------------------------- */

static void utf8(const char *rds, size_t n, const char *want)
{
	char out[FM_RDS_TEXT_MAX];

	fm_rds_to_utf8((const uint8_t *)rds, n, out, sizeof(out));
	g_assert_cmpstr(out, ==, want);
}

static void test_rds_ascii_and_trimmed(void)
{
	utf8("100% NL ", 8, "100% NL");
	utf8("  RADIO1", 8, "RADIO1");
	utf8("        ", 8, "");
	utf8("\0\0\0\0\0\0\0\0", 8, "");
	utf8("A  B", 4, "A  B");
}

static void test_rds_charset(void)
{
	/* EN 50067 Annex E: 0x91 a-umlaut, 0x82 e-acute, 0xd7 O-umlaut, 0xa9 euro,
	 * 0x24 the currency sign. */
	utf8("K\x91se", 4, "K\xc3\xa4se");
	utf8("Caf\x82", 4, "Caf\xc3\xa9");
	utf8("\xd7", 1, "\xc3\x96");
	utf8("\xa9", 1, "\xe2\x82\xac");
	utf8("$", 1, "\xc2\xa4");
}

static void test_rds_text_ends_at_cr(void)
{
	utf8("Onair: Song\r junk", 18, "Onair: Song");
	utf8("\r", 1, "");
}

static void test_rds_control_codes(void)
{
	/* A line feed shows as a space, a soft hyphen as nothing. */
	utf8("A\nB", 3, "A B");
	utf8("Soft\x1fhyphen", 11, "Softhyphen");
}

static void test_rds_never_overflows(void)
{
	uint8_t in[64];
	char out[10];

	/* 64 two-byte characters into 10 bytes: whole characters, terminated. */
	memset(in, 0x91, sizeof(in));
	fm_rds_to_utf8(in, sizeof(in), out, sizeof(out));
	g_assert_cmpuint(strlen(out), ==, 8);
	g_assert_cmpstr(out, ==, "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4");

	fm_rds_to_utf8(in, sizeof(in), out, 1);
	g_assert_cmpstr(out, ==, "");
}

static void test_rds_decode_takes_the_confirmed_copies(void)
{
	struct fm_rds_raw raw;
	struct fm_rds_info info;

	memset(&raw, 0, sizeof(raw));
	/* Working copies, half received: not the name. */
	memcpy(raw.ps_data.ps[0], "2 0% NL ", 8);
	memcpy(raw.ps_data.ps[2], "      L ", 8);
	memcpy(raw.ps_data.ps[3], "100% NL ", 8);
	/* Other networks' names: not this station's. */
	memcpy(raw.ps_on, "OTHER FM", 8);
	memcpy(raw.rt_data.text_data[0], "garbage", 7);
	memcpy(raw.rt_data.text_data[3], "Onair: Guus Meeuwis\r", 20);
	raw.rt_data.text_length = 20;
	fm_rds_decode(&raw, &info);
	g_assert_cmpstr(info.name, ==, "100% NL");
	g_assert_cmpstr(info.text, ==, "Onair: Guus Meeuwis");

	/* Only as much text as the driver says it has. */
	memcpy(raw.rt_data.text_data[3], "ABCDEFGH", 8);
	raw.rt_data.text_length = 3;
	fm_rds_decode(&raw, &info);
	g_assert_cmpstr(info.text, ==, "ABC");

	/* A length past the buffer is cut to it. */
	memset(raw.rt_data.text_data[3], 'x', 64);
	raw.rt_data.text_length = 200;
	fm_rds_decode(&raw, &info);
	g_assert_cmpuint(strlen(info.text), ==, 64);
}

static void test_rds_layout(void)
{
	/* The driver's rds_t, as the armv7 kernel lays it out. */
	g_assert_cmpuint(sizeof(struct fm_rds_raw), ==, 688);
	g_assert_cmpuint(offsetof(struct fm_rds_raw, ps_on), ==, 281);
	g_assert_cmpuint(offsetof(struct fm_rds_raw, rt_data), ==, 290);
	g_assert_cmpuint(offsetof(struct fm_rds_raw, event_status), ==, 554);
}

static void test_tuner_rds(void)
{
	struct fm_tuner t;
	struct fm_rds_raw raw;

	chip_reset();
	memcpy(chip.rds.ps_data.ps[3], "RADIO 2 ", 8);
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, 0);
	g_assert_true(chip.rds_on);
	g_assert_cmpint(fm_tuner_rds_read(&t, &raw), ==, 0);
	g_assert_cmpmem(raw.ps_data.ps[3], 8, "RADIO 2 ", 8);

	/* A short or failed read is no record. */
	chip.read_ret = 10;
	g_assert_cmpint(fm_tuner_rds_read(&t, &raw), ==, -ENODATA);
	chip.read_ret = -EIO;
	g_assert_cmpint(fm_tuner_rds_read(&t, &raw), ==, -EIO);
	chip.read_ret = 0;

	g_assert_cmpint(fm_tuner_rds_enable(&t, false), ==, 0);
	g_assert_false(chip.rds_on);

	/* Down: neither is asked of the driver. */
	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, -ENODEV);
	g_assert_cmpint(fm_tuner_rds_read(&t, &raw), ==, -ENODEV);
	g_assert_cmpint(chip.calls[FM_NR_RDS_ONOFF], ==, 2);
}

static bool cancel_after_a_second(void *data)
{
	(void)data;
	return chip.clock_ms >= 1000;
}

static void test_find_name(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	chip_sender(95000, "100% NL ", 2500);
	chip_sender(99500, "NOORDZEE", 500);
	tuner_up(&t, 99500);
	/* A read that fails is asked again at the next poll. */
	chip.read_fails_until_ms = 1000;

	g_assert_cmpint(fm_tuner_find_name(&t, 95000, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "100% NL");
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 9500);
	g_assert_true(chip.rds_on);
	/* Asked again every poll until there: no longer than that. */
	g_assert_cmpuint(chip.clock_ms, ==, 2500);

	/* The next station's own name, not the last one's. */
	chip.clock_ms = 0;
	chip.tuned_at_ms = 0;
	chip.read_fails_until_ms = 0;
	g_assert_cmpint(fm_tuner_find_name(&t, 99500, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "NOORDZEE");
	g_assert_cmpuint(chip.clock_ms, ==, 500);
}

static void test_find_name_silent_station(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	chip_sender(95000, "100% NL ", 2500);
	tuner_up(&t, 95000);

	/* No RDS at all: not waited for beyond FM_NAME_SILENT_MS. */
	g_assert_cmpint(fm_tuner_find_name(&t, 101200, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, ==, FM_NAME_SILENT_MS);
	g_assert_cmpint(t.khz, ==, 101200);
}

static void test_find_name_gives_up(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	/* RDS, but too weak for the driver to confirm a name. */
	chip_sender(104000, NULL, 0);
	tuner_up(&t, 95000);

	g_assert_cmpint(fm_tuner_find_name(&t, 104000, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, ==, FM_NAME_WAIT_MS);
}

static void test_find_name_cancelled(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	chip_sender(95000, "100% NL ", 2500);
	tuner_up(&t, 99500);

	g_assert_cmpint(fm_tuner_find_name(&t, 95000, cancel_after_a_second, NULL, &info), ==, -ECANCELED);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, ==, 1000);
}

static void test_find_name_needs_power(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	tuner_up(&t, 95000);
	chip.refuse_freq = 96800;
	g_assert_cmpint(fm_tuner_find_name(&t, 96800, NULL, NULL, &info), <, 0);
	g_assert_cmpuint(chip.clock_ms, ==, 0);
	chip.fail_nr = FM_NR_RDS_ONOFF;
	chip.fail_errno = EIO;
	g_assert_cmpint(fm_tuner_find_name(&t, 95000, NULL, NULL, &info), ==, -EIO);
	chip.fail_nr = -1;

	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_cmpint(fm_tuner_find_name(&t, 95000, NULL, NULL, &info), ==, -ENODEV);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/band/valid", test_band_valid);
	g_test_add_func("/band/next-wraps", test_band_next_wraps);
	g_test_add_func("/band/merge-keeps-each-runs-peak", test_band_merge_keeps_each_runs_peak);
	g_test_add_func("/band/merge-tie-keeps-lower", test_band_merge_tie_keeps_lower);
	g_test_add_func("/band/merge-empty", test_band_merge_empty);

	g_test_add_func("/tuner/request-encoding", test_request_encoding);
	g_test_add_func("/tuner/power-up-and-down", test_power_up_and_down);
	g_test_add_func("/tuner/power-up-rejects-off-band", test_power_up_rejects_off_band);
	g_test_add_func("/tuner/power-up-failure-leaves-it-down", test_power_up_failure_leaves_it_down);
	g_test_add_func("/tuner/refused-frequency", test_refused_frequency);
	g_test_add_func("/tuner/tune-needs-power", test_tune_needs_power);
	g_test_add_func("/tuner/no-descriptor", test_no_descriptor);
	g_test_add_func("/tuner/rssi", test_rssi);
	g_test_add_func("/tuner/scan-finds-merged-stations-and-goes-home",
	                test_scan_finds_merged_stations_and_goes_home);
	g_test_add_func("/tuner/scan-skips-a-channel-that-fails", test_scan_skips_a_channel_that_fails);
	g_test_add_func("/tuner/scan-cancelled", test_scan_cancelled);
	g_test_add_func("/tuner/scan-needs-power", test_scan_needs_power);
	g_test_add_func("/tuner/scan-pre-search-fails", test_scan_pre_search_fails);
	g_test_add_func("/tuner/seek-up-lands-on-the-peak", test_seek_up_lands_on_the_peak);
	g_test_add_func("/tuner/seek-down-and-wrap", test_seek_down_and_wrap);
	g_test_add_func("/tuner/seek-skips-the-station-it-is-on", test_seek_skips_the_station_it_is_on);
	g_test_add_func("/tuner/seek-finds-nothing", test_seek_finds_nothing);
	g_test_add_func("/tuner/seek-cancelled-goes-home", test_seek_cancelled_goes_home);

	g_test_add_func("/rds/ascii-and-trimmed", test_rds_ascii_and_trimmed);
	g_test_add_func("/rds/charset", test_rds_charset);
	g_test_add_func("/rds/text-ends-at-cr", test_rds_text_ends_at_cr);
	g_test_add_func("/rds/control-codes", test_rds_control_codes);
	g_test_add_func("/rds/never-overflows", test_rds_never_overflows);
	g_test_add_func("/rds/decode-takes-the-confirmed-copies", test_rds_decode_takes_the_confirmed_copies);
	g_test_add_func("/rds/layout", test_rds_layout);
	g_test_add_func("/tuner/rds", test_tuner_rds);
	g_test_add_func("/tuner/find-name", test_find_name);
	g_test_add_func("/tuner/find-name-silent-station", test_find_name_silent_station);
	g_test_add_func("/tuner/find-name-gives-up", test_find_name_gives_up);
	g_test_add_func("/tuner/find-name-cancelled", test_find_name_cancelled);
	g_test_add_func("/tuner/find-name-needs-power", test_find_name_needs_power);

	return g_test_run();
}
