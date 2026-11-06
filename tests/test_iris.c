// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fm_iris (and fm_tuner over it) against a stand-in for Qualcomm's
 * radio-iris: the SMD link that has to be opened first, a tune the chip only
 * reports a moment later, its own seek, and RDS as the byte streams the
 * driver queues.
 */

#include <errno.h>
#include <glib.h>
#include <linux/videodev2.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "../src/fm_band.h"
#include "../src/fm_iris.h"
#include "../src/fm_rds.h"
#include "../src/fm_tuner.h"

/* ---- the stand-in chip --------------------------------------------------- */

#define MAX_STATIONS 32

struct station
{
	int khz;
	int rssi;
	const char *ps;         /* NULL: no RDS at all */
	const char *rt;
	unsigned int ps_after_ms;
};

static struct
{
	/* The SMD link: what fmsmd_set was last written, and the writes. */
	bool link;
	char writes[16];
	size_t nwrites;
	int write_err;          /* fmsmd_set writes fail with this */
	int state_fails;        /* this many STATE=1 fail ... */
	int state_err;          /* ... with this */

	int state;
	int freq;               /* kHz, as G_FREQUENCY reports it */
	int target;             /* kHz the chip is going to */
	int lag;                /* G_FREQUENCY reads before it gets there */
	int lag_left;
	unsigned int rangelow, rangehigh, audmode;
	int emphasis, spacing, antenna, rdson, grp_proc, srchmode;

	bool search_on;
	int search_polls;       /* SRCHON reads 1 this many times */
	int search_polls_left;
	int search_to;
	bool search_never_ends;
	int srchon_writes;
	bool counts_start;      /* a seek from a station stops on it at once */
	int stops_at;           /* a seek from here stops at once, no station */
	bool list_from_bottom;  /* lists from 87.5 wherever it is */
	int srch_cnt;
	uint8_t list[FM_IRIS_BUF_SIZE]; /* the driver's list fifo: kept */
	size_t list_len;
	uint8_t pending[FM_IRIS_BUF_SIZE]; /* the list of the search on */
	size_t pending_len;
	int list_polls_left;    /* LIST reads before the search reports */
	bool listing;
	bool list_endless;      /* the fifo never runs empty */
	int calls_list;

	struct station st[MAX_STATIONS];
	size_t nst;

	unsigned int clock_ms;
	unsigned int tuned_at_ms;
	bool ps_sent, rt_sent;
	uint8_t ps_fifo[FM_IRIS_BUF_SIZE];
	size_t ps_len;
	uint8_t rt_fifo[FM_IRIS_BUF_SIZE];
	size_t rt_len;
	unsigned int groups_base;
	bool no_counters;
	size_t cntrs_short;     /* the counters come back this much short */

	unsigned long fail_request;     /* this request fails ... */
	unsigned int fail_id;           /* ... only for this control (S_CTRL) or
	                                 * buffer index + 1 (DQBUF); 0: any */
	int fail_err;
	int over_report;        /* DQBUF claims this many bytes more */
	int calls_g_freq, calls_s_freq, calls_seek, calls_state;
} chip;

static void chip_reset(void)
{
	memset(&chip, 0, sizeof(chip));
	chip.freq = 88000;
	chip.antenna = 1;       /* left by someone else: must be set back */
	/* Long enough to be a search, not a seek over at once (2 s). */
	chip.search_polls = 100;
}

static void chip_station(int khz, int rssi, const char *ps, const char *rt, unsigned int after)
{
	g_assert_cmpuint(chip.nst, <, MAX_STATIONS);
	chip.st[chip.nst++] = (struct station){ khz, rssi, ps, rt, after };
}

static const struct station *chip_on(int khz)
{
	for (size_t i = 0; i < chip.nst; i++)
	{
		if (chip.st[i].khz == khz)
		{
			return &chip.st[i];
		}
	}

	return NULL;
}

static void chip_retune(int khz)
{
	chip.target = khz;
	chip.lag_left = chip.lag;
	chip.tuned_at_ms = chip.clock_ms;
	chip.ps_sent = chip.rt_sent = false;
	chip.ps_len = chip.rt_len = 0;
	chip.groups_base = 0;

	if (chip.lag == 0)
	{
		chip.freq = khz;
	}
}

static int fake_write(const char *path, const char *value)
{
	g_assert_cmpstr(path, ==, FM_IRIS_SMD_PARAM);
	g_assert_cmpuint(chip.nwrites, <, sizeof(chip.writes) - 1);
	chip.writes[chip.nwrites++] = value[0];

	if (chip.write_err)
	{
		return chip.write_err;
	}

	chip.link = value[0] == '1';
	return 0;
}

static void fake_sleep(unsigned int ms)
{
	chip.clock_ms += ms;
}

static bool fails(unsigned long request, unsigned int id)
{
	return chip.fail_request == request && (chip.fail_id == 0 || chip.fail_id == id);
}

/* The next station from khz (dir up or down), round the band; khz if none. */
static int next_station(int khz, int dir)
{
	int f = khz;

	for (int i = 0; i < FM_BAND_CHANNELS; i++)
	{
		f = fm_band_next(f, dir);

		if (chip_on(f))
		{
			return f;
		}
	}

	return khz;
}

static unsigned int groups_now(void)
{
	const struct station *s = chip_on(chip.freq);

	return s && s->ps ? (chip.clock_ms - chip.tuned_at_ms) / 10 : 0;
}

/* What the driver queues once the chip has decoded it. */
static void chip_rds(void)
{
	const struct station *s = chip_on(chip.freq);

	if (!s || !s->ps || chip.clock_ms - chip.tuned_at_ms < s->ps_after_ms)
	{
		return;
	}

	if (!chip.ps_sent)
	{
		uint8_t *r = chip.ps_fifo + chip.ps_len;

		r[0] = 1;
		r[1] = 10;
		r[2] = 0x34;
		r[3] = 0x12;
		r[4] = 0;
		memcpy(r + FM_IRIS_RDS_HEADER, s->ps, FM_IRIS_PS_LEN);
		chip.ps_len += FM_IRIS_RDS_HEADER + FM_IRIS_PS_LEN;
		chip.ps_sent = true;
	}

	if (!chip.rt_sent && s->rt)
	{
		size_t len = strlen(s->rt);
		uint8_t *r = chip.rt_fifo + chip.rt_len;

		r[0] = (uint8_t)len;
		r[1] = 10;
		r[2] = 0x34;
		r[3] = 0x12;
		r[4] = 0;
		memcpy(r + FM_IRIS_RDS_HEADER, s->rt, len);
		chip.rt_len += FM_IRIS_RDS_HEADER + len;
		chip.rt_sent = true;
	}
}

static int take(uint8_t *fifo, size_t *len, struct v4l2_buffer *b)
{
	size_t n = *len < b->length ? *len : b->length;

	memcpy((void *)b->m.userptr, fifo, n);
	memmove(fifo, fifo + n, *len - n);
	*len -= n;
	b->bytesused = (unsigned int)n + (unsigned int)chip.over_report;
	return 0;
}

/* The search reports: its list joins whatever the fifo still holds. */
static void chip_list_done(void)
{
	g_assert_cmpuint(chip.list_len + chip.pending_len, <=, sizeof(chip.list));
	memcpy(chip.list + chip.list_len, chip.pending, chip.pending_len);
	chip.list_len += chip.pending_len;
	chip.pending_len = 0;
	chip.listing = false;
}

/* A list in the fifo from before, of one station at khz. */
static void chip_stale_list(int khz)
{
	int rel = (khz - FM_BAND_MIN_KHZ) / 50;
	uint8_t *l = chip.list + chip.list_len;

	l[0] = 1;
	l[1] = (uint8_t)(rel >> 8);
	l[2] = (uint8_t)rel;
	chip.list_len += 3;
}

static int dqbuf(struct v4l2_buffer *b)
{
	g_assert_cmpuint(b->type, ==, V4L2_BUF_TYPE_PRIVATE);
	g_assert_cmpuint(b->length, <=, FM_IRIS_BUF_SIZE);
	chip_rds();

	switch (b->index)
	{
		case FM_IRIS_BUF_LIST:
			if (chip.listing && chip.list_polls_left-- <= 0)
			{
				chip_list_done();
			}

			if (chip.list_endless)
			{
				uint8_t one[] = { 0 };
				size_t n = sizeof(one);

				return take(one, &n, b);
			}

			return take(chip.list, &chip.list_len, b);

		case FM_IRIS_BUF_PS:
			return take(chip.ps_fifo, &chip.ps_len, b);

		case FM_IRIS_BUF_RT:
			return take(chip.rt_fifo, &chip.rt_len, b);

		case FM_IRIS_BUF_CNTRS:
		{
			/* What the last counter read put there. */
			uint8_t c[FM_IRIS_CNTRS_LEN] = { 0 };
			unsigned int g = groups_now() - chip.groups_base;
			size_t n = sizeof(c) - chip.cntrs_short;

			c[FM_IRIS_CNTRS_GROUPS] = (uint8_t)g;
			c[FM_IRIS_CNTRS_GROUPS + 1] = (uint8_t)(g >> 8);
			return take(c, &n, b);
		}

		default:
			return -EINVAL;
	}
}

/*
 * The strong-station list: the first srch_cnt stations from the current
 * channel up, listed strongest first (the order the backend may not rely
 * on), as offsets in 50 kHz from the bottom, high byte first.
 */
static void chip_list(bool up)
{
	const struct station *pick[MAX_STATIONS];
	size_t n = 0;

	g_assert_true(up);

	for (int f = chip.list_from_bottom ? FM_BAND_MIN_KHZ : chip.freq; f <= FM_BAND_MAX_KHZ && (int)n < chip.srch_cnt; f += FM_BAND_STEP_KHZ)
	{
		const struct station *st = chip_on(f);

		if (st)
		{
			pick[n++] = st;
		}
	}

	for (size_t i = 1; i < n; i++)
	{
		for (size_t j = i; j > 0 && pick[j]->rssi > pick[j - 1]->rssi; j--)
		{
			const struct station *x = pick[j];

			pick[j] = pick[j - 1];
			pick[j - 1] = x;
		}
	}

	chip.pending[0] = (uint8_t)n;

	for (size_t i = 0; i < n; i++)
	{
		int rel = (pick[i]->khz - FM_BAND_MIN_KHZ) / 50;

		chip.pending[1 + 2 * i] = (uint8_t)(rel >> 8);
		chip.pending[2 + 2 * i] = (uint8_t)rel;
	}

	chip.pending_len = 1 + 2 * n;
	chip.list_polls_left = chip.search_polls / 5;
	chip.listing = true;
	/* The chip ends up somewhere in the band. */
	chip_retune(n > 0 ? pick[0]->khz : chip.freq);
}

static int s_ctrl(struct v4l2_control *c)
{
	if (c->id == FM_IRIS_CID_STATE)
	{
		chip.calls_state++;

		if (c->value == FM_IRIS_RECV)
		{
			if (!chip.link)
			{
				return -ENODEV;
			}

			if (chip.state_fails > 0)
			{
				chip.state_fails--;
				return chip.state_err;
			}

			if (chip.state != 0)
			{
				return -EINVAL;
			}
		}

		chip.state = c->value;
		return 0;
	}

	/* Everything else only with the receiver on. */
	if (chip.state != FM_IRIS_RECV)
	{
		return -EINVAL;
	}

	switch (c->id)
	{
		case FM_IRIS_CID_EMPHASIS: chip.emphasis = c->value; return 0;
		case FM_IRIS_CID_SPACING: chip.spacing = c->value; return 0;
		case FM_IRIS_CID_ANTENNA: chip.antenna = c->value; return 0;
		case FM_IRIS_CID_RDSON: chip.rdson = c->value; return 0;
		/* The driver ORs it in. */
		case FM_IRIS_CID_RDSGROUP_PROC: chip.grp_proc |= c->value; return 0;
		case FM_IRIS_CID_SRCHMODE: chip.srchmode = c->value; return 0;
		case FM_IRIS_CID_SRCH_CNT: chip.srch_cnt = c->value; return 0;

		case FM_IRIS_CID_SRCHON:
			chip.srchon_writes++;
			chip.search_on = c->value != 0;

			/* Stopped, a list search still reports what it had. */
			if (!chip.search_on && chip.listing)
			{
				chip_list_done();
			}

			return 0;

		case FM_IRIS_CID_RDS_GRP_CNTRS:
			if (chip.no_counters)
			{
				return -EINVAL;
			}

			if (c->value)
			{
				chip.groups_base = groups_now();
			}

			return 0;

		default:
			return -EINVAL;
	}
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
	g_assert_cmpint(fd, ==, 1000);

	unsigned int id = request == VIDIOC_S_CTRL ? ((struct v4l2_control *)arg)->id
	                  : request == VIDIOC_DQBUF ? ((struct v4l2_buffer *)arg)->index + 1 : 0;

	if (fails(request, id))
	{
		return chip.fail_err;
	}

	switch (request)
	{
		case VIDIOC_S_CTRL:
			return s_ctrl(arg);

		case VIDIOC_G_CTRL:
		{
			struct v4l2_control *c = arg;

			if (c->id != FM_IRIS_CID_SRCHON)
			{
				return -EINVAL;
			}

			/* The driver never clears it after a list. */
			if (chip.search_on && chip.srchmode == FM_IRIS_SEEK && !chip.search_never_ends &&
			        chip.search_polls_left-- <= 0)
			{
				chip.search_on = false;
				chip_retune(chip.search_to);
			}

			c->value = chip.search_on;
			return 0;
		}

		case VIDIOC_S_TUNER:
		{
			const struct v4l2_tuner *t = arg;

			if (chip.state != FM_IRIS_RECV)
			{
				return -EINVAL;
			}

			chip.rangelow = t->rangelow;
			chip.rangehigh = t->rangehigh;
			chip.audmode = t->audmode;
			return 0;
		}

		case VIDIOC_G_TUNER:
		{
			struct v4l2_tuner *t = arg;
			const struct station *s = chip_on(chip.freq);

			/* The driver's char: unsigned on ARM, so -98 comes as 158. */
			t->signal = (s ? s->rssi : -100) & 0xff;
			return 0;
		}

		case VIDIOC_S_FREQUENCY:
		{
			const struct v4l2_frequency *f = arg;

			chip.calls_s_freq++;
			g_assert_cmpuint(f->type, ==, V4L2_TUNER_RADIO);

			if (chip.state != FM_IRIS_RECV)
			{
				return -EINVAL;
			}

			chip_retune((int)(f->frequency / FM_IRIS_TUNE_PARAM));
			return 0;
		}

		case VIDIOC_G_FREQUENCY:
		{
			struct v4l2_frequency *f = arg;

			chip.calls_g_freq++;

			if (chip.lag_left > 0 && --chip.lag_left == 0)
			{
				chip.freq = chip.target;
			}

			f->frequency = (unsigned int)chip.freq * FM_IRIS_TUNE_PARAM;
			return 0;
		}

		case VIDIOC_S_HW_FREQ_SEEK:
		{
			const struct v4l2_hw_freq_seek *s = arg;

			g_assert_true(s->wrap_around);
			chip.search_on = true;

			if (chip.srchmode == FM_IRIS_LIST_STRONG)
			{
				chip.calls_list++;
				chip_list(s->seek_upward != 0);
				return 0;
			}

			g_assert_cmpuint(chip.srchmode, ==, FM_IRIS_SEEK);
			chip.calls_seek++;
			chip.search_polls_left = chip.search_polls;

			/* Up from the top of the band it does not move; from a station
			 * it may stop on that one; both at once. */
			if ((s->seek_upward && chip.freq == FM_BAND_MAX_KHZ) ||
			        (chip.counts_start && chip_on(chip.freq)) || chip.freq == chip.stops_at)
			{
				chip.search_to = chip.freq;
				chip.search_polls_left = 2;
			}
			else
			{
				chip.search_to = next_station(chip.freq, s->seek_upward ? 1 : -1);
			}

			return 0;
		}

		case VIDIOC_DQBUF:
			return dqbuf(arg);

		default:
			return -ENOTTY;
	}
}

static bool cancel_now(void *data)
{
	(void)data;
	return true;
}

static bool cancel_after_two(void *data)
{
	int *asked = data;

	return ++*asked > 2;
}

/*
 * Opened the way the service opens it - on /dev/null, which takes no
 * ioctl - then handed the stand-in's descriptor.
 */
static void tuner_init(struct fm_tuner *t)
{
	fm_tuner_init(t, &fm_iris_ops, fake_ioctl);
	t->write_fn = fake_write;
	t->sleep_fn = fake_sleep;
	g_assert_cmpint(fm_tuner_open(t, "/dev/null"), ==, 0);
	(void)close(t->fd);
	t->fd = 1000;
}

static void tuner_up(struct fm_tuner *t, int khz)
{
	tuner_init(t);
	g_assert_cmpint(fm_tuner_power_up(t, khz), ==, 0);
}

/* ---- power ---------------------------------------------------------------- */

static void test_ops(void)
{
	g_assert_cmpstr(fm_iris_ops.path, ==, "/dev/radio0");
	g_assert_cmpstr(fm_mtk_ops.path, ==, "/dev/fm");
	g_assert_cmpstr(fm_iris_ops.name, ==, "iris");

	/* Both looked for, MediaTek first. */
	g_assert_true(fm_tuner_backends[0] == &fm_mtk_ops);
	g_assert_true(fm_tuner_backends[1] == &fm_iris_ops);
	g_assert_null(fm_tuner_backends[2]);
}

static void test_open_opens_the_link_first(void)
{
	struct fm_tuner t;

	chip_reset();
	fm_tuner_init(&t, &fm_iris_ops, fake_ioctl);
	t.write_fn = fake_write;
	t.sleep_fn = fake_sleep;
	g_assert_cmpint(fm_tuner_open(&t, "/dev/null"), ==, 0);
	g_assert_cmpstr(chip.writes, ==, "1");
	/* With time for it to come up. */
	g_assert_cmpuint(chip.clock_ms, ==, FM_IRIS_LINK_OPEN_MS);
	/* Open already: neither again. */
	g_assert_cmpint(fm_tuner_open(&t, "/dev/null"), ==, 0);
	g_assert_cmpstr(chip.writes, ==, "1");
	fm_tuner_close(&t);
	g_assert_cmpint(t.fd, <, 0);

	/* No such parameter (another transport): nothing to wait for. */
	chip_reset();
	chip.write_err = -ENOENT;
	fm_tuner_init(&t, &fm_iris_ops, fake_ioctl);
	t.write_fn = fake_write;
	t.sleep_fn = fake_sleep;
	g_assert_cmpint(fm_tuner_open(&t, "/dev/null"), ==, 0);
	g_assert_cmpuint(chip.clock_ms, ==, 0);
	fm_tuner_close(&t);

	/* MediaTek has no link to open. */
	chip_reset();
	fm_tuner_init(&t, &fm_mtk_ops, fake_ioctl);
	t.write_fn = fake_write;
	g_assert_cmpint(fm_tuner_open(&t, "/dev/null"), ==, 0);
	g_assert_cmpuint(chip.nwrites, ==, 0);
	fm_tuner_close(&t);
}

static void test_open_default_path(void)
{
	struct fm_tuner t;

	fm_tuner_init(&t, &fm_iris_ops, fake_ioctl);
	/* No such node on a build host; either way, the backend's own path. */
	if (!g_file_test(fm_iris_ops.path, G_FILE_TEST_EXISTS))
	{
		g_assert_cmpint(fm_tuner_open(&t, NULL), ==, -ENOENT);
	}

	g_assert_cmpint(t.fd, <, 0);
}

static void test_power_up_configures(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	g_assert_true(t.powered);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.state, ==, FM_IRIS_RECV);
	g_assert_cmpint(chip.freq, ==, 95000);
	/* The link is opened before the receiver is switched on. */
	g_assert_cmpstr(chip.writes, ==, "1");
	g_assert_cmpuint(chip.rangelow, ==, 87500 * 16);
	g_assert_cmpuint(chip.rangehigh, ==, 108000 * 16);
	g_assert_cmpuint(chip.audmode, ==, V4L2_TUNER_MODE_STEREO);
	g_assert_cmpint(chip.emphasis, ==, FM_IRIS_EMP50);
	g_assert_cmpint(chip.spacing, ==, FM_IRIS_SPACE_100K);
	g_assert_cmpint(chip.antenna, ==, FM_IRIS_ANT_WIRED);

	/* Up already: a power up only tunes. */
	g_assert_cmpint(fm_tuner_power_up(&t, 101200), ==, 0);
	g_assert_cmpint(chip.calls_state, ==, 1);
	g_assert_cmpint(chip.freq, ==, 101200);

	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_false(t.powered);
	g_assert_cmpint(chip.state, ==, FM_IRIS_OFF);
	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_cmpint(chip.calls_state, ==, 2);
}

static void test_power_up_reopens_a_slow_link(void)
{
	struct fm_tuner t;

	chip_reset();
	chip.state_fails = 1;
	chip.state_err = -ETIMEDOUT;
	tuner_up(&t, 95000);
	g_assert_cmpstr(chip.writes, ==, "101");
	g_assert_cmpint(chip.calls_state, ==, 2);
	g_assert_cmpuint(chip.clock_ms, >=, FM_IRIS_LINK_DOWN_MS + FM_IRIS_LINK_UP_MS);

	chip_reset();
	chip.state_fails = 1;
	chip.state_err = -EIO;
	tuner_up(&t, 95000);
	g_assert_cmpstr(chip.writes, ==, "101");
}

static void test_power_up_link_stays_down(void)
{
	struct fm_tuner t;

	chip_reset();
	chip.state_fails = 2;
	chip.state_err = -ENODEV;
	tuner_init(&t);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -ENODEV);
	g_assert_false(t.powered);
	g_assert_cmpint(chip.calls_state, ==, 2);
}

static void test_power_up_other_refusal_not_retried(void)
{
	struct fm_tuner t;

	chip_reset();
	chip.state = FM_IRIS_RECV;      /* somebody else has it on */
	chip.link = true;
	tuner_init(&t);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EINVAL);
	g_assert_false(t.powered);
	g_assert_cmpint(chip.calls_state, ==, 1);
	g_assert_cmpstr(chip.writes, ==, "1");
}

static void test_power_up_setting_fails(void)
{
	static const unsigned int ids[] = { FM_IRIS_CID_EMPHASIS, FM_IRIS_CID_SPACING,
	                                    FM_IRIS_CID_ANTENNA };
	struct fm_tuner t;

	for (size_t i = 0; i < G_N_ELEMENTS(ids); i++)
	{
		chip_reset();
		chip.fail_request = VIDIOC_S_CTRL;
		chip.fail_id = ids[i];
		chip.fail_err = -EIO;
		tuner_init(&t);
		g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
		g_assert_false(t.powered);
		/* Switched off again, not left on half set up. */
		g_assert_cmpint(chip.state, ==, FM_IRIS_OFF);
	}

	chip_reset();
	chip.fail_request = VIDIOC_S_TUNER;
	chip.fail_err = -EINVAL;
	tuner_init(&t);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EINVAL);
	g_assert_cmpint(chip.state, ==, FM_IRIS_OFF);

	chip_reset();
	chip.fail_request = VIDIOC_S_FREQUENCY;
	chip.fail_err = -EIO;
	tuner_init(&t);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
	g_assert_cmpint(chip.state, ==, FM_IRIS_OFF);
}

static void test_power_down_failure_is_down(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	chip.fail_request = VIDIOC_S_CTRL;
	chip.fail_id = FM_IRIS_CID_STATE;
	chip.fail_err = -EIO;
	g_assert_cmpint(fm_tuner_power_down(&t), ==, -EIO);
	g_assert_false(t.powered);
}

/* ---- tuning ---------------------------------------------------------------- */

static void test_tune_waits_for_the_chip(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	chip.lag = 4;
	chip.calls_g_freq = 0;
	g_assert_cmpint(fm_tuner_tune(&t, 101200), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);
	g_assert_cmpint(chip.calls_g_freq, ==, 4);
}

static void test_tune_never_lands(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	chip.lag = 1000;
	chip.clock_ms = 0;
	g_assert_cmpint(fm_tuner_tune(&t, 101200), ==, -ETIMEDOUT);
	/* Still where it was, as far as anyone knows. */
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpuint(chip.clock_ms, >=, FM_IRIS_TUNE_WAIT_MS);
	g_assert_cmpuint(chip.clock_ms, <, FM_IRIS_TUNE_WAIT_MS + 100);
}

static void test_tune_errors(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_tune(&t, 95050), ==, -EINVAL);
	chip.fail_request = VIDIOC_G_FREQUENCY;
	chip.fail_err = -EIO;
	g_assert_cmpint(fm_tuner_tune(&t, 96000), ==, -EIO);
	chip.fail_request = 0;
	fm_tuner_power_down(&t);
	g_assert_cmpint(fm_tuner_tune(&t, 96000), ==, -ENODEV);
}

static void test_rssi(void)
{
	struct fm_tuner t;
	int dbm = 0;

	chip_reset();
	chip_station(95000, -98, NULL, NULL, 0);
	chip_station(96000, 5, NULL, NULL, 0);
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, -98);
	fm_tuner_tune(&t, 96000);
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, 5);

	chip.fail_request = VIDIOC_G_TUNER;
	chip.fail_err = -EIO;
	dbm = 1;
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, -EIO);
	g_assert_cmpint(dbm, ==, 1);
}

/* ---- RDS -------------------------------------------------------------------- */

static void test_rds_enable(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, 0);
	g_assert_cmpint(chip.rdson, ==, 1);
	g_assert_cmpint(chip.grp_proc, ==, FM_IRIS_RDS_RT_PS);
	g_assert_cmpint(fm_tuner_rds_enable(&t, false), ==, 0);
	g_assert_cmpint(chip.rdson, ==, 0);

	chip.fail_request = VIDIOC_S_CTRL;
	chip.fail_id = FM_IRIS_CID_RDSON;
	chip.fail_err = -EIO;
	chip.grp_proc = 0;
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, -EIO);
	g_assert_cmpint(chip.grp_proc, ==, 0);
}

static void ps_record(uint8_t *r, size_t *n, uint8_t count, const char *names)
{
	r[*n] = count;
	memset(r + *n + 1, 0, FM_IRIS_RDS_HEADER - 1);
	memcpy(r + *n + FM_IRIS_RDS_HEADER, names, (size_t)count * FM_IRIS_PS_LEN);
	*n += FM_IRIS_RDS_HEADER + (size_t)count * FM_IRIS_PS_LEN;
}

static void test_parse_ps(void)
{
	uint8_t b[256] = { 0 };
	char name[FM_RDS_NAME_MAX] = "old";
	size_t n = 0;

	/* Nothing: nothing changes. */
	g_assert_false(fm_iris_parse_ps(b, 0, name, sizeof(name)));
	g_assert_cmpstr(name, ==, "old");

	/* Several records in one read: the last wins, its first name. */
	ps_record(b, &n, 1, "RADIO 1 ");
	ps_record(b, &n, 2, "SKYRADIOSECOND  ");
	g_assert_true(fm_iris_parse_ps(b, n, name, sizeof(name)));
	g_assert_cmpstr(name, ==, "SKYRADIO");

	/* A blank name, or none at all, does not wipe one out. */
	n = 0;
	ps_record(b, &n, 1, "        ");
	ps_record(b, &n, 0, "");
	g_assert_false(fm_iris_parse_ps(b, n, name, sizeof(name)));
	g_assert_cmpstr(name, ==, "SKYRADIO");

	/* A record cut short ends the read; what came before it counts. */
	n = 0;
	ps_record(b, &n, 1, "VERONICA");
	ps_record(b, &n, 1, "TRUNCATE");
	g_assert_true(fm_iris_parse_ps(b, n - 1, name, sizeof(name)));
	g_assert_cmpstr(name, ==, "VERONICA");
	g_assert_false(fm_iris_parse_ps(b, FM_IRIS_RDS_HEADER - 1, name, sizeof(name)));

	/* A count no record can have is not trusted, nor anything after it. */
	n = 0;
	b[0] = 15;
	n = FM_IRIS_RDS_HEADER + 15 * FM_IRIS_PS_LEN;
	memset(b + 1, 'X', n - 1);
	g_assert_false(fm_iris_parse_ps(b, n, name, sizeof(name)));
	n = 0;
	ps_record(b, &n, 14, "ABCDEFGH......................................................."
	          "........................................................");
	g_assert_true(fm_iris_parse_ps(b, n, name, sizeof(name)));
	g_assert_cmpstr(name, ==, "ABCDEFGH");

	/* Cut to the caller's room as the decoder cuts: room for 4 per character. */
	{
		char small[7];

		n = 0;
		ps_record(b, &n, 1, "RADIO 2 ");
		g_assert_true(fm_iris_parse_ps(b, n, small, sizeof(small)));
		g_assert_cmpstr(small, ==, "RADI");
	}
}

static void rt_record(uint8_t *r, size_t *n, const char *text)
{
	size_t len = strlen(text);

	r[*n] = (uint8_t)len;
	memset(r + *n + 1, 0, FM_IRIS_RDS_HEADER - 1);
	memcpy(r + *n + FM_IRIS_RDS_HEADER, text, len);
	*n += FM_IRIS_RDS_HEADER + len;
}

static void test_parse_rt(void)
{
	uint8_t b[256] = { 0 };
	char text[FM_RDS_TEXT_MAX] = "";
	size_t n = 0;

	rt_record(b, &n, "Now: a song");
	rt_record(b, &n, "Next: the news  ");
	g_assert_true(fm_iris_parse_rt(b, n, text, sizeof(text)));
	g_assert_cmpstr(text, ==, "Next: the news");

	n = 0;
	rt_record(b, &n, "   ");
	g_assert_false(fm_iris_parse_rt(b, n, text, sizeof(text)));
	g_assert_cmpstr(text, ==, "Next: the news");

	n = 0;
	rt_record(b, &n, "Cut");
	g_assert_false(fm_iris_parse_rt(b, n - 1, text, sizeof(text)));

	/* Longer than RadioText can be: not trusted. */
	n = 0;
	b[0] = FM_IRIS_RT_MAX + 1;
	memset(b + 1, 'X', sizeof(b) - 1);
	g_assert_false(fm_iris_parse_rt(b, sizeof(b), text, sizeof(text)));

	/* Exactly the longest. */
	{
		char longest[FM_IRIS_RT_MAX + 1];

		memset(longest, 'L', FM_IRIS_RT_MAX);
		longest[FM_IRIS_RT_MAX] = '\0';
		n = 0;
		rt_record(b, &n, longest);
		g_assert_true(fm_iris_parse_rt(b, n, text, sizeof(text)));
		g_assert_cmpstr(text, ==, longest);
	}
}

static void test_rds_get(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;
	long groups = -2;

	chip_reset();
	chip_station(95000, -60, "100% NL ", "Het beste van nu", 300);
	chip_station(97500, -65, "VERONICA", NULL, 300);
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, 0);

	/* Before the chip has it: nothing, but groups are coming in. */
	fake_sleep(100);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpint(groups, ==, 10);

	fake_sleep(250);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "100% NL");
	g_assert_cmpstr(info.text, ==, "Het beste van nu");

	/* Kept: the driver hands each record over once. */
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, NULL), ==, 0);
	g_assert_cmpstr(info.name, ==, "100% NL");

	/* Another station: none of the last one's, counted from zero. */
	g_assert_cmpint(fm_tuner_tune(&t, 97500), ==, 0);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpstr(info.text, ==, "");
	g_assert_cmpint(groups, ==, 0);

	/* No counters on this driver: -1, the rest as before. */
	chip.no_counters = true;
	fake_sleep(400);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "VERONICA");
	g_assert_cmpint(groups, ==, -1);
	chip.no_counters = false;

	/* A buffer that cannot be read is an error, not "nothing". */
	chip.fail_request = VIDIOC_DQBUF;
	chip.fail_err = -EINVAL;
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, -EINVAL);
	chip.fail_request = 0;

	/* A driver claiming more than it was given room for. */
	chip.over_report = FM_IRIS_BUF_SIZE + 1;
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, -EIO);
	chip.over_report = 0;

	/* Down: forgotten. */
	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_cmpstr(t.rds.name, ==, "");
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, NULL), ==, -ENODEV);
}

static void test_rds_text_buffer_fails(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	tuner_up(&t, 95000);
	/* Only the RadioText read fails: an error, even with a name in. */
	chip.fail_request = VIDIOC_DQBUF;
	chip.fail_id = FM_IRIS_BUF_RT + 1;
	chip.fail_err = -EINVAL;
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, NULL), ==, -EINVAL);

	/* Only the counters: the rest stands, the count is unknown. */
	{
		long groups = 5;

		chip.fail_id = FM_IRIS_BUF_CNTRS + 1;
		g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
		g_assert_cmpint(groups, ==, -1);

		/* Read short: not the count either (not a zero to give up on). */
		chip.fail_request = 0;
		chip.cntrs_short = 1;
		groups = 5;
		g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
		g_assert_cmpint(groups, ==, -1);
	}
}

static void test_find_name(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	chip_reset();
	chip_station(95000, -60, "100% NL ", NULL, 2000);
	chip_station(101200, -70, NULL, NULL, 0);
	tuner_up(&t, 88000);

	g_assert_cmpint(fm_tuner_find_name(&t, 95000, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "100% NL");
	g_assert_cmpint(chip.rdson, ==, 1);

	/* No RDS: given up once nothing was counted. */
	chip.clock_ms = 0;
	chip.tuned_at_ms = 0;
	g_assert_cmpint(fm_tuner_find_name(&t, 101200, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, <, FM_NAME_SILENT_MS + 300);

	/* A driver that cannot count: the whole wait. */
	chip.no_counters = true;
	chip.clock_ms = 0;
	g_assert_cmpint(fm_tuner_find_name(&t, 101200, NULL, NULL, &info), ==, 0);
	g_assert_cmpuint(chip.clock_ms, >=, FM_NAME_WAIT_MS);
}

/* ---- seek and scan ---------------------------------------------------------- */

static void test_seek(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(95000, -60, NULL, NULL, 0);
	chip_station(101200, -70, NULL, NULL, 0);
	chip.search_polls = 3;
	tuner_up(&t, 96000);

	/* What was heard on the last channel is not this one's. */
	(void)g_strlcpy(t.rds.name, "OLD", sizeof(t.rds.name));
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);
	g_assert_cmpstr(t.rds.name, ==, "");
	g_assert_cmpint(chip.srchmode, ==, FM_IRIS_SEEK);
	/* Up from the top station: round to the bottom one. */
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);
	g_assert_cmpint(chip.srchon_writes, ==, 0);
}

static void test_seek_finds_nothing(void)
{
	struct fm_tuner t;

	chip_reset();
	tuner_up(&t, 96000);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 96000);

	/* The only station is the one it is on. */
	chip_station(96000, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 96000);

	/* Up from the top, nothing but the bottom of the band: none either. */
	chip.nst = 0;
	chip_station(87500, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_tune(&t, 108000), ==, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 108000);
	g_assert_cmpint(chip.freq, ==, 108000);
}

static void test_seek_up_from_the_top(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(95000, -60, NULL, NULL, 0);
	tuner_up(&t, 108000);
	/* The chip would stop at once: round from the bottom instead. */
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 95000);

	/* Down from the bottom it wraps by itself. */
	g_assert_cmpint(fm_tuner_tune(&t, 87500), ==, 0);
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 95000);

	/* Retuning to the bottom fails: back on the top, with the error. */
	g_assert_cmpint(fm_tuner_tune(&t, 108000), ==, 0);
	chip.fail_request = VIDIOC_S_FREQUENCY;
	chip.fail_err = -EIO;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -EIO);
	g_assert_cmpint(chip.calls_seek, ==, 2);
}

static void test_seek_cancelled_and_timed_out(void)
{
	struct fm_tuner t;
	int asked = 0;

	chip_reset();
	chip_station(101200, -70, NULL, NULL, 0);
	chip.search_polls = 10;
	tuner_up(&t, 96000);

	g_assert_cmpint(fm_tuner_seek(&t, 1, cancel_after_two, &asked), ==, -ECANCELED);
	g_assert_cmpint(chip.srchon_writes, ==, 1);
	g_assert_false(chip.search_on);
	g_assert_cmpint(t.khz, ==, 96000);
	g_assert_cmpint(chip.freq, ==, 96000);

	chip.search_never_ends = true;
	chip.clock_ms = 0;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ETIMEDOUT);
	g_assert_cmpint(chip.srchon_writes, ==, 2);
	g_assert_cmpuint(chip.clock_ms, >=, FM_IRIS_SEEK_WAIT_MS);
	g_assert_cmpint(t.khz, ==, 96000);
}

static void test_seek_errors(void)
{
	struct fm_tuner t;

	chip_reset();
	chip_station(101200, -70, NULL, NULL, 0);
	tuner_up(&t, 96000);

	chip.fail_request = VIDIOC_S_HW_FREQ_SEEK;
	chip.fail_err = -EBUSY;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -EBUSY);
	g_assert_cmpint(t.khz, ==, 96000);

	chip.fail_request = VIDIOC_S_CTRL;
	chip.fail_id = FM_IRIS_CID_SRCHMODE;
	chip.fail_err = -EINVAL;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -EINVAL);
	g_assert_cmpint(chip.calls_seek, ==, 0);

	chip.fail_request = VIDIOC_G_CTRL;
	chip.fail_id = 0;
	chip.fail_err = -EIO;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -EIO);
	chip.fail_request = 0;

	fm_tuner_power_down(&t);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENODEV);
}

static void test_parse_list(void)
{
	/* 89.6, 87.5 and 108.0 as the chip lists them; 89.65 to the channel. */
	static const uint8_t b[] = { 5, 0, 42, 0, 0, 1, 154, 0, 43, 0, 42 };
	int khz[FM_IRIS_LIST_MAX];

	g_assert_cmpuint(fm_iris_parse_list(b, sizeof(b), khz, G_N_ELEMENTS(khz)), ==, 4);
	g_assert_cmpint(khz[0], ==, 87500);
	g_assert_cmpint(khz[1], ==, 89600);
	g_assert_cmpint(khz[2], ==, 89700);
	g_assert_cmpint(khz[3], ==, 108000);

	/* Fewer bytes than the count says: what is there. */
	g_assert_cmpuint(fm_iris_parse_list(b, 4, khz, G_N_ELEMENTS(khz)), ==, 1);
	g_assert_cmpint(khz[0], ==, 89600);
	g_assert_cmpuint(fm_iris_parse_list(b, 0, khz, G_N_ELEMENTS(khz)), ==, 0);

	/* Out of the band: left out. Full: no more. */
	{
		static const uint8_t off[] = { 2, 1, 155, 0, 1 };
		static const uint8_t two[] = { 3, 0, 42, 0, 0, 1, 154 };

		/* 108.05 rounds past the band; 87.55 to 87.6. */
		g_assert_cmpuint(fm_iris_parse_list(off, sizeof(off), khz, 4), ==, 1);
		g_assert_cmpint(khz[0], ==, 87600);
		g_assert_cmpuint(fm_iris_parse_list(two, sizeof(two), khz, 2), ==, 2);
		g_assert_cmpint(khz[0], ==, 87500);
		g_assert_cmpint(khz[1], ==, 89600);
	}
}

static void test_scan(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	chip_station(87500, -50, NULL, NULL, 0);
	chip_station(95000, -60, NULL, NULL, 0);
	chip_station(101200, -40, NULL, NULL, 0);
	chip_station(108000, -70, NULL, NULL, 0);
	tuner_up(&t, 96000);

	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 4);
	/* In frequency order, whatever the chip's, each with its strength. */
	g_assert_cmpint(out[0].khz, ==, 87500);
	g_assert_cmpint(out[0].strength, ==, -50);
	g_assert_cmpint(out[1].khz, ==, 95000);
	g_assert_cmpint(out[2].khz, ==, 101200);
	g_assert_cmpint(out[2].strength, ==, -40);
	g_assert_cmpint(out[3].khz, ==, 108000);
	g_assert_cmpint(chip.srch_cnt, ==, FM_IRIS_LIST_MAX);
	/* The driver leaves the search on: switched off. */
	g_assert_false(chip.search_on);
	/* Back where it was. */
	g_assert_cmpint(t.khz, ==, 96000);
	g_assert_cmpint(chip.freq, ==, 96000);
	g_assert_cmpint(chip.calls_list, ==, 1);
}

static void test_scan_empty_and_merged(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;

	chip_reset();
	tuner_up(&t, 96000);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(t.khz, ==, 96000);

	/* Neighbours merge into their strongest, as on MediaTek. */
	chip_station(95000, -70, NULL, NULL, 0);
	chip_station(95100, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpint(out[0].khz, ==, 95100);
}

static void test_scan_left_over_list(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 0;
	int asked = 0;

	/* Two lists nobody read: neither is this search's. */
	chip_reset();
	chip_station(95000, -60, NULL, NULL, 0);
	tuner_up(&t, 96000);
	chip_stale_list(90000);
	chip_stale_list(104000);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpint(out[0].khz, ==, 95000);
	g_assert_cmpuint(chip.list_len, ==, 0);

	/* A scan stopped part way leaves its list: the next is its own. */
	g_assert_cmpint(fm_tuner_scan(&t, cancel_after_two, &asked, out, &n), ==, -ECANCELED);
	g_assert_cmpuint(chip.list_len, >, 0);
	chip.nst = 0;
	chip_station(101200, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpint(out[0].khz, ==, 101200);

	/* A fifo that never empties: an error, not a list. */
	chip.list_endless = true;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EIO);
	g_assert_cmpint(t.khz, ==, 96000);
}

static void test_scan_passes(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 0;

	/* 25 stations, a MHz apart: two lists, 20 and 5. */
	chip_reset();

	for (int i = 0; i < 25; i++)
	{
		chip_station(88000 + i * 800, -60 - i, NULL, NULL, 0);
	}

	tuner_up(&t, 96000);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 25);
	g_assert_cmpint(chip.calls_list, ==, 2);

	for (size_t i = 0; i < n; i++)
	{
		g_assert_cmpint(out[i].khz, ==, 88000 + (int)i * 800);
		g_assert_cmpint(out[i].strength, ==, -60 - (int)i);
	}

	/* Exactly 20: a second list finds nothing new and ends it. */
	chip.nst = 20;
	chip.calls_list = 0;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 20);
	g_assert_cmpint(chip.calls_list, ==, 2);

	/*
	 * A chip that lists from the bottom wherever it starts: the second list
	 * repeats the first, nothing in it lies above the last, and that ends it.
	 */
	chip.nst = 25;
	chip.list_from_bottom = true;
	chip.calls_list = 0;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 20);
	g_assert_cmpint(chip.calls_list, ==, 2);
}

static void test_scan_cancelled_and_errors(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 99;
	int asked = 0;

	chip_reset();
	chip_station(95000, -60, NULL, NULL, 0);
	chip_station(101200, -70, NULL, NULL, 0);
	tuner_up(&t, 96000);

	/* While the chip lists. */
	g_assert_cmpint(fm_tuner_scan(&t, cancel_now, NULL, out, &n), ==, -ECANCELED);
	g_assert_cmpuint(n, ==, 0);
	g_assert_false(chip.search_on);
	g_assert_cmpint(t.khz, ==, 96000);
	g_assert_cmpint(chip.freq, ==, 96000);

	/* While measuring what it listed. */
	chip.search_polls = 0;
	g_assert_cmpint(fm_tuner_scan(&t, cancel_after_two, &asked, out, &n), ==, -ECANCELED);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(chip.freq, ==, 96000);

	/* A list that never comes. */
	chip.search_polls = 100000;
	chip.clock_ms = 0;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -ETIMEDOUT);
	g_assert_cmpuint(chip.clock_ms, >=, FM_IRIS_LIST_WAIT_MS);
	g_assert_false(chip.search_on);
	chip.search_polls = 100;

	chip.fail_request = VIDIOC_S_HW_FREQ_SEEK;
	chip.fail_err = -EBUSY;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EBUSY);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(t.khz, ==, 96000);

	chip.fail_request = VIDIOC_S_CTRL;
	chip.fail_id = FM_IRIS_CID_SRCH_CNT;
	chip.fail_err = -EINVAL;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EINVAL);

	chip.fail_request = VIDIOC_DQBUF;
	chip.fail_id = FM_IRIS_BUF_LIST + 1;
	chip.fail_err = -EIO;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EIO);
	g_assert_false(chip.search_on);
	chip.fail_request = 0;

	fm_tuner_power_down(&t);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -ENODEV);
}

static void test_seek_counts_the_start(void)
{
	struct fm_tuner t;

	/* From a station, the chip stops on it at once: from the next one on. */
	chip_reset();
	chip.counts_start = true;
	chip_station(95000, -60, NULL, NULL, 0);
	chip_station(101200, -70, NULL, NULL, 0);
	tuner_up(&t, 95000);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);
	g_assert_cmpint(chip.calls_seek, ==, 2);

	/* And the next channel is a station itself: that one. */
	chip_station(101300, -50, NULL, NULL, 0);
	chip.calls_seek = 0;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 101300);
	g_assert_cmpint(chip.calls_seek, ==, 2);

	/* Down the same way. */
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);

	/* The only station: round from the next and back, none. */
	chip.nst = 1;
	g_assert_cmpint(fm_tuner_tune(&t, 95000), ==, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 95000);

	/*
	 * Noise it stops on only when starting there, and nothing else: the
	 * second seek goes round and back to where it began. None, not that.
	 */
	chip.nst = 0;
	chip.stops_at = 96000;
	g_assert_cmpint(fm_tuner_tune(&t, 96000), ==, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 96000);
	g_assert_cmpint(chip.freq, ==, 96000);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/iris/ops", test_ops);
	g_test_add_func("/iris/open-opens-the-link-first", test_open_opens_the_link_first);
	g_test_add_func("/iris/open-default-path", test_open_default_path);
	g_test_add_func("/iris/power-up-configures", test_power_up_configures);
	g_test_add_func("/iris/power-up-reopens-a-slow-link", test_power_up_reopens_a_slow_link);
	g_test_add_func("/iris/power-up-link-stays-down", test_power_up_link_stays_down);
	g_test_add_func("/iris/power-up-other-refusal-not-retried",
	                test_power_up_other_refusal_not_retried);
	g_test_add_func("/iris/power-up-setting-fails", test_power_up_setting_fails);
	g_test_add_func("/iris/power-down-failure-is-down", test_power_down_failure_is_down);
	g_test_add_func("/iris/tune-waits-for-the-chip", test_tune_waits_for_the_chip);
	g_test_add_func("/iris/tune-never-lands", test_tune_never_lands);
	g_test_add_func("/iris/tune-errors", test_tune_errors);
	g_test_add_func("/iris/rssi", test_rssi);
	g_test_add_func("/iris/rds-enable", test_rds_enable);
	g_test_add_func("/iris/parse-ps", test_parse_ps);
	g_test_add_func("/iris/parse-rt", test_parse_rt);
	g_test_add_func("/iris/rds-get", test_rds_get);
	g_test_add_func("/iris/rds-text-buffer-fails", test_rds_text_buffer_fails);
	g_test_add_func("/iris/find-name", test_find_name);
	g_test_add_func("/iris/seek", test_seek);
	g_test_add_func("/iris/seek-finds-nothing", test_seek_finds_nothing);
	g_test_add_func("/iris/seek-up-from-the-top", test_seek_up_from_the_top);
	g_test_add_func("/iris/seek-cancelled-and-timed-out", test_seek_cancelled_and_timed_out);
	g_test_add_func("/iris/seek-errors", test_seek_errors);
	g_test_add_func("/iris/parse-list", test_parse_list);
	g_test_add_func("/iris/scan", test_scan);
	g_test_add_func("/iris/scan-empty-and-merged", test_scan_empty_and_merged);
	g_test_add_func("/iris/scan-passes", test_scan_passes);
	g_test_add_func("/iris/scan-left-over-list", test_scan_left_over_list);
	g_test_add_func("/iris/seek-counts-the-start", test_seek_counts_the_start);
	g_test_add_func("/iris/scan-cancelled-and-errors", test_scan_cancelled_and_errors);

	return g_test_run();
}
