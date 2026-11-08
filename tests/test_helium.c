// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/*
 * fm_helium (and fm_tuner over it) against a stand-in for Qualcomm's
 * WCN3990-class FM: HCI commands answered by Command Complete or Command
 * Status, a tune and a search reported by their own events a moment later,
 * and RDS as the chip's Program Service and Radio Text events.
 */

#include <errno.h>
#include <glib.h>
#include <stddef.h>
#include <string.h>

#include "../src/fm_band.h"
#include "../src/fm_helium.h"
#include "../src/fm_rds.h"
#include "../src/fm_tuner.h"

/* ---- the stand-in chip --------------------------------------------------- */

#define MAX_STATIONS 32
#define MAX_LOG      512
#define MAX_LATER    64

struct station
{
	int khz;
	int rssi;
	const char *ps;         /* NULL: no RDS at all */
	const char *rt;
	unsigned int rds_after_ms;
};

struct sent
{
	uint16_t op;
	uint8_t p[FM_HCI_PARAM_MAX];
	size_t len;
};

struct later
{
	unsigned int due_ms;
	int retune_to;          /* the chip gets there then (0: stays) */
	uint8_t ev[FM_HCI_EV_HEADER + FM_HCI_PARAM_MAX];
	size_t n;
};

static struct
{
	struct fm_hci *h;
	int opens, closes;
	int open_err;
	int send_err;

	bool enabled;
	int freq;
	int tune_lag_ms;        /* Tune Status this long after the tune */
	bool tune_lands_elsewhere;
	uint8_t rds_std;
	uint32_t grp_proc;

	/* Seeking. */
	unsigned int search_ms;         /* a search that finds something takes this */
	bool counts_start;              /* a seek from a station stops on it at once */
	bool search_never_ends;
	bool cancel_silent;             /* a cancelled search sends no Search Complete */
	unsigned int cancel_lag_ms;     /* ... or sends it this much later */

	/* RDS. */
	unsigned int clock_ms;
	unsigned int tuned_at_ms;
	bool ps_sent, rt_sent;
	unsigned int groups_base_ms;

	uint16_t fail_op;       /* this command fails (status 1) ... */
	int fail_after;         /* ... after this many good ones */
	uint16_t mute_op;       /* this command is never answered */
	bool answer_other;      /* answer each command first as some other one */

	struct station st[MAX_STATIONS];
	size_t nst;

	struct sent log[MAX_LOG];
	size_t nlog;

	struct later later[MAX_LATER];
	size_t nlater;
} chip;

static void chip_reset(void)
{
	memset(&chip, 0, sizeof(chip));
	chip.freq = 88000;
	chip.search_ms = 2000;
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

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void emit(uint8_t code, const uint8_t *p, size_t len)
{
	uint8_t ev[FM_HCI_EV_HEADER + FM_HCI_PARAM_MAX];

	ev[0] = code;
	ev[1] = (uint8_t)len;
	memcpy(ev + FM_HCI_EV_HEADER, p, len);
	fm_hci_event(chip.h, ev, FM_HCI_EV_HEADER + len);
}

static void emit_later(unsigned int ms, uint8_t code, const uint8_t *p, size_t len)
{
	struct later *l;

	g_assert_cmpuint(chip.nlater, <, MAX_LATER);
	l = &chip.later[chip.nlater++];
	l->due_ms = chip.clock_ms + ms;
	l->retune_to = 0;
	l->ev[0] = code;
	l->ev[1] = (uint8_t)len;
	memcpy(l->ev + FM_HCI_EV_HEADER, p, len);
	l->n = FM_HCI_EV_HEADER + len;
}

static void complete(uint16_t op, uint8_t status, const uint8_t *ret, size_t len)
{
	uint8_t p[FM_HCI_PARAM_MAX];

	p[0] = 1;       /* credits */
	p[1] = (uint8_t)op;
	p[2] = (uint8_t)(op >> 8);
	p[3] = status;

	if (len > 0)
	{
		memcpy(p + 4, ret, len);
	}

	emit(FM_HCI_EV_CMD_COMPLETE, p, 4 + len);
}

static void cmd_status(uint16_t op, uint8_t status)
{
	const uint8_t p[] = { status, 1, (uint8_t)op, (uint8_t)(op >> 8) };

	emit(FM_HCI_EV_CMD_STATUS, p, sizeof(p));
}

/* Tune Status for khz: sub-event, frequency, service, RSSI, ... */
static void tune_status(unsigned int ms, int khz)
{
	const struct station *s = chip_on(khz);
	uint8_t p[12] = { 0 };

	put32(p + 1, (uint32_t)khz);
	p[5] = s ? 1 : 0;
	p[6] = (uint8_t)(s ? s->rssi : -110);
	emit_later(ms, FM_HCI_EV_TUNE_STATUS, p, sizeof(p));
}

static void retune(int khz)
{
	chip.freq = khz;
	chip.tuned_at_ms = chip.clock_ms;
	chip.ps_sent = chip.rt_sent = false;
	chip.groups_base_ms = chip.clock_ms;
}

/* What RDS the station has sent by now, as events. */
static void chip_rds(void)
{
	const struct station *s = chip_on(chip.freq);

	if (!s || !s->ps || chip.rds_std != FM_HCI_RDS_ON ||
	        chip.clock_ms - chip.tuned_at_ms < s->rds_after_ms)
	{
		return;
	}

	if (!chip.ps_sent && (chip.grp_proc & 2))
	{
		uint8_t p[FM_HCI_PS_NAMES + FM_HCI_PS_LEN] = { 0x12, 0x34, 10 };

		p[FM_HCI_PS_COUNT] = 1;
		memset(p + FM_HCI_PS_NAMES, ' ', FM_HCI_PS_LEN);
		memcpy(p + FM_HCI_PS_NAMES, s->ps, strlen(s->ps));
		emit(FM_HCI_EV_PROGRAM_SERVICE, p, sizeof(p));
		chip.ps_sent = true;
	}

	if (!chip.rt_sent && s->rt && (chip.grp_proc & 1))
	{
		uint8_t p[FM_HCI_RT_TEXT + FM_HCI_RT_MAX] = { 0x12, 0x34, 10, 0, 0 };
		size_t len = strlen(s->rt);

		memcpy(p + FM_HCI_RT_TEXT, s->rt, len);
		p[FM_HCI_RT_TEXT + len] = 0x0d;
		emit(FM_HCI_EV_RADIO_TEXT, p, FM_HCI_RT_TEXT + len + 1);
		chip.rt_sent = true;
	}
}

/* Where a seek from the current channel stops, and how long it takes. */
static int seek_target(int dir, unsigned int *ms)
{
	int khz = chip.freq;

	if (chip.counts_start && chip_on(khz))
	{
		*ms = 40;
		return khz;
	}

	for (int i = 0; i < FM_BAND_CHANNELS; i++)
	{
		khz = fm_band_next(khz, dir);

		if (khz == chip.freq)
		{
			break;
		}

		if (chip_on(khz))
		{
			*ms = chip.search_ms;
			return khz;
		}
	}

	*ms = chip.search_ms;
	return chip.freq;
}

static int fake_open(struct fm_hci *h)
{
	chip.opens++;

	if (chip.open_err)
	{
		return chip.open_err;
	}

	chip.h = h;
	return 0;
}

static void fake_close(struct fm_hci *h)
{
	g_assert_true(h == chip.h);
	chip.closes++;
	chip.h = NULL;
}

static int fake_send(struct fm_hci *h, const uint8_t *pkt, size_t len)
{
	uint16_t op;
	const uint8_t *p = pkt + FM_HCI_CMD_HEADER;
	uint8_t r[64] = { 0 };

	g_assert_true(h == chip.h);
	g_assert_cmpuint(len, >=, FM_HCI_CMD_HEADER);
	g_assert_cmpuint(pkt[2], ==, len - FM_HCI_CMD_HEADER);

	if (chip.send_err)
	{
		return chip.send_err;
	}

	op = (uint16_t)(pkt[0] | pkt[1] << 8);
	g_assert_cmpuint(chip.nlog, <, MAX_LOG);
	chip.log[chip.nlog].op = op;
	chip.log[chip.nlog].len = len - FM_HCI_CMD_HEADER;
	memcpy(chip.log[chip.nlog].p, p, len - FM_HCI_CMD_HEADER);
	chip.nlog++;

	if (op == chip.mute_op)
	{
		return 0;
	}

	if (chip.answer_other)
	{
		complete((uint16_t)(op + 1), 0, NULL, 0);
		cmd_status((uint16_t)(op + 1), 0);
	}

	if (op == chip.fail_op)
	{
		if (chip.fail_after == 0)
		{
			if (op == FM_HCI_TUNE || op == FM_HCI_SEARCH)
			{
				cmd_status(op, 1);
			}
			else
			{
				complete(op, 1, NULL, 0);
			}

			return 0;
		}

		chip.fail_after--;
	}

	switch (op)
	{
	case FM_HCI_ENABLE_RECV:
		chip.enabled = true;
		complete(op, 0, NULL, 0);
		break;

	case FM_HCI_DISABLE_RECV:
		chip.enabled = false;
		complete(op, 0, NULL, 0);
		break;

	case FM_HCI_SET_RECV_CONF:
		g_assert_cmpuint(len - FM_HCI_CMD_HEADER, ==, FM_HCI_RECV_CONF_LEN);
		chip.rds_std = p[2];
		complete(op, 0, NULL, 0);
		break;

	case FM_HCI_RDS_GRP_PROCESS:
		chip.grp_proc = get32(p);
		complete(op, 0, NULL, 0);
		break;

	case FM_HCI_TUNE:
	{
		int khz = (int)get32(p);

		cmd_status(op, 0);
		retune(khz);
		tune_status((unsigned int)chip.tune_lag_ms, chip.tune_lands_elsewhere ? khz + 100 : khz);
		break;
	}

	case FM_HCI_GET_STATION:
	{
		const struct station *s = chip_on(chip.freq);

		put32(r, (uint32_t)chip.freq);
		r[4] = s ? 1 : 0;
		r[5] = (uint8_t)(s ? s->rssi : -110);
		complete(op, 0, r, 11);
		break;
	}

	case FM_HCI_SEARCH:
	{
		unsigned int ms = 0;
		int to = seek_target(p[2] == FM_HCI_DIR_UP ? 1 : -1, &ms);

		cmd_status(op, 0);

		if (!chip.search_never_ends)
		{
			/* There once the search is over, not before. */
			tune_status(ms, to);
			emit_later(ms, FM_HCI_EV_SEARCH_COMPLETE, r, 1);
			chip.later[chip.nlater - 1].retune_to = to;
		}

		break;
	}

	case FM_HCI_CANCEL_SEARCH:
		/* The search on goes, and ends with its Search Complete. */
		for (size_t i = 0; i < chip.nlater;)
		{
			if (chip.later[i].ev[0] == FM_HCI_EV_SEARCH_COMPLETE ||
			        chip.later[i].ev[0] == FM_HCI_EV_TUNE_STATUS)
			{
				chip.later[i] = chip.later[--chip.nlater];
			}
			else
			{
				i++;
			}
		}

		complete(op, 0, NULL, 0);

		if (chip.cancel_lag_ms > 0)
		{
			emit_later(chip.cancel_lag_ms, FM_HCI_EV_SEARCH_COMPLETE, r, 1);
		}
		else if (!chip.cancel_silent)
		{
			emit(FM_HCI_EV_SEARCH_COMPLETE, r, 1);
		}

		break;

	case FM_HCI_READ_GRP_CNTRS:
	{
		const struct station *s = chip_on(chip.freq);
		/* A group each 100 ms from a station with RDS. */
		uint32_t groups = s && s->ps ? (chip.clock_ms - chip.groups_base_ms) / 100 : 0;

		if (get32(p) == 1)
		{
			chip.groups_base_ms = chip.clock_ms;
		}

		put32(r + 4, groups);
		complete(op, 0, r, 36);
		break;
	}

	default:
		complete(op, 0, NULL, 0);
		break;
	}

	return 0;
}

static const struct fm_hci_ops fake_ops = {
	.open = fake_open,
	.close = fake_close,
	.send = fake_send,
};

static void fake_sleep(unsigned int ms)
{
	chip.clock_ms += ms;

	for (size_t i = 0; i < chip.nlater;)
	{
		if (chip.later[i].due_ms <= chip.clock_ms)
		{
			if (chip.later[i].retune_to)
			{
				retune(chip.later[i].retune_to);
			}

			fm_hci_event(chip.h, chip.later[i].ev, chip.later[i].n);
			chip.later[i] = chip.later[--chip.nlater];
		}
		else
		{
			i++;
		}
	}

	chip_rds();
}

static size_t count_op(uint16_t op)
{
	size_t n = 0;

	for (size_t i = 0; i < chip.nlog; i++)
	{
		n += chip.log[i].op == op;
	}

	return n;
}

static const struct sent *last_op(uint16_t op)
{
	for (size_t i = chip.nlog; i > 0; i--)
	{
		if (chip.log[i - 1].op == op)
		{
			return &chip.log[i - 1];
		}
	}

	return NULL;
}

static void setup(struct fm_tuner *t)
{
	chip_reset();
	fm_tuner_init(t, &fm_helium_ops, NULL);
	t->hci_ops = &fake_ops;
	t->sleep_fn = fake_sleep;
}

static void setup_open(struct fm_tuner *t)
{
	setup(t);
	g_assert_cmpint(fm_tuner_open(t, NULL), ==, 0);
}

static void setup_up(struct fm_tuner *t, int khz)
{
	setup_open(t);
	g_assert_cmpint(fm_tuner_power_up(t, khz), ==, 0);
	chip.nlog = 0;
}

/* ---- tests ----------------------------------------------------------------- */

static void test_ops(void)
{
	size_t i;

	g_assert_cmpstr(fm_helium_ops.name, ==, "helium");
	g_assert_cmpstr(fm_helium_ops.path, ==, FM_HELIUM_IMPL);
	g_assert_cmpstr(fm_helium_ops.audio_args, ==, "mode=parameters");
	g_assert_nonnull(fm_helium_ops.open);
	g_assert_nonnull(fm_helium_ops.close);

	/* Looked for last: a node of the others wins. */
	for (i = 0; fm_tuner_backends[i]; i++)
	{
	}

	g_assert_true(fm_tuner_backends[i - 1] == &fm_helium_ops);
	g_assert_true(fm_tuner_backends[0] == &fm_mtk_ops);
}

static void test_opcodes(void)
{
	/* Spelled out as radio-iris' and helium's headers give them. */
	g_assert_cmphex(FM_HCI_ENABLE_RECV, ==, 0x4c01);
	g_assert_cmphex(FM_HCI_SET_RECV_CONF, ==, 0x4c04);
	g_assert_cmphex(FM_HCI_SEARCH, ==, 0x4c0e);
	g_assert_cmphex(FM_HCI_TUNE, ==, 0x5401);
	g_assert_cmphex(FM_HCI_READ_GRP_CNTRS, ==, 0x5801);
	g_assert_cmphex(FM_HCI_ENABLE_SLIMBUS, ==, 0xfc0e);
}

static void test_open_and_close(void)
{
	struct fm_tuner t;

	setup(&t);
	g_assert_cmpint(fm_tuner_open(&t, NULL), ==, 0);
	g_assert_true(t.linked);
	g_assert_cmpint(t.fd, <, 0);
	g_assert_nonnull(t.hci);
	/* Open already: not again. */
	g_assert_cmpint(fm_tuner_open(&t, NULL), ==, 0);
	g_assert_cmpint(chip.opens, ==, 1);

	fm_tuner_close(&t);
	g_assert_false(t.linked);
	g_assert_null(t.hci);
	g_assert_cmpint(chip.closes, ==, 1);
	/* Closed already: nothing. */
	fm_tuner_close(&t);
	g_assert_cmpint(chip.closes, ==, 1);
}

static void test_open_fails(void)
{
	struct fm_tuner t;

	setup(&t);
	chip.open_err = -ENODEV;
	g_assert_cmpint(fm_tuner_open(&t, NULL), ==, -ENODEV);
	g_assert_false(t.linked);
	g_assert_null(t.hci);
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EBADF);

	/* No transport given: nothing to reach the chip with. */
	setup(&t);
	t.hci_ops = NULL;
	g_assert_cmpint(fm_tuner_open(&t, NULL), ==, -ENODEV);
	g_assert_false(t.linked);
}

static void test_power_up_configures(void)
{
	static const uint16_t order[] = {
		FM_HCI_ENABLE_RECV, FM_HCI_SET_MUTE_MODE, FM_HCI_SET_STEREO_MODE, FM_HCI_SET_EVENT_MASK,
		FM_HCI_SET_RECV_CONF, FM_HCI_SET_ANTENNA, FM_HCI_TUNE, FM_HCI_READ_GRP_CNTRS,
		FM_HCI_ENABLE_SLIMBUS,
	};
	struct fm_tuner t;
	const struct sent *s;

	setup_open(&t);
	chip.tune_lag_ms = 60;
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, 0);
	g_assert_true(t.powered);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_true(chip.enabled);

	g_assert_cmpuint(chip.nlog, ==, G_N_ELEMENTS(order));

	for (size_t i = 0; i < G_N_ELEMENTS(order); i++)
	{
		g_assert_cmphex(chip.log[i].op, ==, order[i]);
	}

	s = last_op(FM_HCI_SET_MUTE_MODE);
	g_assert_cmpuint(s->len, ==, 2);
	g_assert_cmpuint(s->p[0], ==, 0);
	g_assert_cmpuint(s->p[1], ==, 1);
	s = last_op(FM_HCI_SET_STEREO_MODE);
	g_assert_cmpuint(s->len, ==, 4);
	g_assert_cmpmem(s->p, 4, ((const uint8_t[]){ 0, 1, 1, 1 }), 4);
	s = last_op(FM_HCI_SET_EVENT_MASK);
	g_assert_cmpuint(s->len, ==, 1);
	g_assert_cmpuint(s->p[0], ==, 0x07);
	s = last_op(FM_HCI_SET_RECV_CONF);
	g_assert_cmpuint(s->p[0], ==, FM_HCI_EMP50);
	g_assert_cmpuint(s->p[1], ==, FM_HCI_SPACE_100K);
	g_assert_cmpuint(s->p[2], ==, FM_HCI_RDS_OFF);
	g_assert_cmpuint(s->p[3], ==, 0);
	g_assert_cmpuint(get32(s->p + 4), ==, FM_BAND_MIN_KHZ);
	g_assert_cmpuint(get32(s->p + 8), ==, FM_BAND_MAX_KHZ);
	s = last_op(FM_HCI_SET_ANTENNA);
	g_assert_cmpuint(s->p[0], ==, FM_HCI_ANT_WIRED);
	s = last_op(FM_HCI_TUNE);
	g_assert_cmpuint(s->len, ==, 4);
	g_assert_cmpuint(get32(s->p), ==, 95000);
	s = last_op(FM_HCI_ENABLE_SLIMBUS);
	g_assert_cmpuint(s->len, ==, 1);
	g_assert_cmpuint(s->p[0], ==, 1);

	fm_tuner_close(&t);
}

static void test_power_up_refused(void)
{
	struct fm_tuner t;

	setup_open(&t);
	chip.fail_op = FM_HCI_ENABLE_RECV;
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
	g_assert_false(t.powered);
	/* Never on: nothing to switch off. */
	g_assert_cmpuint(count_op(FM_HCI_DISABLE_RECV), ==, 0);
	fm_tuner_close(&t);
}

static void test_power_up_setting_fails(void)
{
	static const uint16_t steps[] = {
		FM_HCI_SET_MUTE_MODE, FM_HCI_SET_STEREO_MODE, FM_HCI_SET_EVENT_MASK, FM_HCI_SET_RECV_CONF,
		FM_HCI_SET_ANTENNA, FM_HCI_TUNE, FM_HCI_ENABLE_SLIMBUS,
	};

	for (size_t i = 0; i < G_N_ELEMENTS(steps); i++)
	{
		struct fm_tuner t;

		setup_open(&t);
		chip.fail_op = steps[i];
		g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, -EIO);
		g_assert_false(t.powered);
		g_assert_false(chip.enabled);
		g_assert_cmpuint(count_op(FM_HCI_DISABLE_RECV), ==, 1);
		fm_tuner_close(&t);
	}
}

static void test_power_down(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	g_assert_cmpint(fm_tuner_power_down(&t), ==, 0);
	g_assert_false(t.powered);
	g_assert_false(chip.enabled);

	/* A refusal is down all the same. */
	g_assert_cmpint(fm_tuner_power_up(&t, 95000), ==, 0);
	chip.fail_op = FM_HCI_DISABLE_RECV;
	g_assert_cmpint(fm_tuner_power_down(&t), ==, -EIO);
	g_assert_false(t.powered);
	fm_tuner_close(&t);

	/* Closing a receiver that is on switches it off first. */
	setup_up(&t, 95000);
	fm_tuner_close(&t);
	g_assert_cmpuint(count_op(FM_HCI_DISABLE_RECV), ==, 1);
	g_assert_cmpint(chip.closes, ==, 1);
}

static void test_tune_waits_for_the_chip(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	chip.tune_lag_ms = 300;
	g_assert_cmpint(fm_tuner_tune(&t, 101200), ==, 0);
	g_assert_cmpint(t.khz, ==, 101200);
	g_assert_cmpuint(chip.clock_ms, >=, 300);
	fm_tuner_close(&t);
}

static void test_tune_never_lands(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	chip.tune_lands_elsewhere = true;
	g_assert_cmpint(fm_tuner_tune(&t, 101200), ==, -ETIMEDOUT);
	g_assert_cmpint(t.khz, ==, 95000);

	/* The tune refused. */
	chip.tune_lands_elsewhere = false;
	chip.fail_op = FM_HCI_TUNE;
	g_assert_cmpint(fm_tuner_tune(&t, 101200), ==, -EIO);
	fm_tuner_close(&t);
}

static void test_command_unanswered(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	chip.mute_op = FM_HCI_GET_STATION;
	chip.clock_ms = 0;
	{
		int dbm = 0;

		g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, -ETIMEDOUT);
	}
	g_assert_cmpuint(chip.clock_ms, >=, FM_HELIUM_CMD_WAIT_MS);
	fm_tuner_close(&t);
}

static void test_command_answers_matched(void)
{
	struct fm_tuner t;
	int dbm = 0;

	setup_up(&t, 95000);
	chip_station(95000, -60, NULL, NULL, 0);
	/* Each answer first comes for some other command: not this one's. */
	chip.answer_other = true;
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, -60);
	fm_tuner_close(&t);
}

static void test_transport_fails(void)
{
	struct fm_tuner t;
	int dbm = 0;

	setup_up(&t, 95000);
	chip.send_err = -EIO;
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, -EIO);
	chip.send_err = 0;
	fm_tuner_close(&t);
}

static void test_events_malformed(void)
{
	struct fm_tuner t;
	int dbm = 0;
	/* Short of a header; a Command Complete short of its opcode; a Tune
	 * Status short of its RSSI; a length past the end. */
	static const uint8_t junk1[] = { FM_HCI_EV_CMD_COMPLETE };
	static const uint8_t junk2[] = { FM_HCI_EV_CMD_COMPLETE, 2, 1, 0x0a };
	static const uint8_t junk3[] = { FM_HCI_EV_TUNE_STATUS, 3, 0, 0x18, 0x73 };
	static const uint8_t junk4[] = { FM_HCI_EV_PROGRAM_SERVICE, 200, 0, 0 };

	setup_up(&t, 95000);
	chip_station(95000, -70, NULL, NULL, 0);
	fm_hci_event(t.hci, junk1, sizeof(junk1));
	fm_hci_event(t.hci, junk2, sizeof(junk2));
	fm_hci_event(t.hci, junk3, sizeof(junk3));
	fm_hci_event(t.hci, junk4, sizeof(junk4));
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, -70);
	g_assert_cmpstr(t.rds.name, ==, "");
	fm_tuner_close(&t);
}

static void test_rssi(void)
{
	struct fm_tuner t;
	int dbm = 0;

	setup_up(&t, 95000);
	chip_station(95000, -67, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, -67);

	chip.st[0].rssi = 5;
	g_assert_cmpint(fm_tuner_rssi(&t, &dbm), ==, 0);
	g_assert_cmpint(dbm, ==, 5);
	fm_tuner_close(&t);
}

static void test_rds_enable(void)
{
	struct fm_tuner t;
	const struct sent *s;

	setup_up(&t, 95000);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, 0);
	g_assert_cmpuint(chip.rds_std, ==, FM_HCI_RDS_ON);
	s = last_op(FM_HCI_RDS_GRP_PROCESS);
	g_assert_nonnull(s);
	g_assert_cmpuint(s->len, ==, 4);
	g_assert_cmpuint(get32(s->p), ==, FM_HCI_RDS_RT_PS);

	chip.nlog = 0;
	g_assert_cmpint(fm_tuner_rds_enable(&t, false), ==, 0);
	g_assert_cmpuint(chip.rds_std, ==, FM_HCI_RDS_OFF);
	g_assert_cmpuint(count_op(FM_HCI_RDS_GRP_PROCESS), ==, 0);

	chip.fail_op = FM_HCI_RDS_GRP_PROCESS;
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, -EIO);
	fm_tuner_close(&t);
}

static void test_parse_ps(void)
{
	uint8_t p[FM_HCI_PS_NAMES + 2 * FM_HCI_PS_LEN] = { 0 };
	char name[FM_RDS_NAME_MAX] = "old";

	/* Too short for a name. */
	g_assert_false(fm_helium_parse_ps(p, FM_HCI_PS_NAMES + 7, name, sizeof(name)));
	/* No names. */
	g_assert_false(fm_helium_parse_ps(p, sizeof(p), name, sizeof(name)));
	/* A blank one. */
	p[FM_HCI_PS_COUNT] = 2;
	memset(p + FM_HCI_PS_NAMES, ' ', 2 * FM_HCI_PS_LEN);
	g_assert_false(fm_helium_parse_ps(p, sizeof(p), name, sizeof(name)));
	g_assert_cmpstr(name, ==, "old");
	/* The first of two. */
	memcpy(p + FM_HCI_PS_NAMES, "  NPO 2 ", 8);
	memcpy(p + FM_HCI_PS_NAMES + 8, "OTHER   ", 8);
	g_assert_true(fm_helium_parse_ps(p, sizeof(p), name, sizeof(name)));
	g_assert_cmpstr(name, ==, "NPO 2");
	/* The character set: 0x91 is a-umlaut. */
	memcpy(p + FM_HCI_PS_NAMES, "R\x91" "DIO   ", 8);
	g_assert_true(fm_helium_parse_ps(p, sizeof(p), name, sizeof(name)));
	g_assert_cmpstr(name, ==, "R\xc3\xa4" "DIO");
}

static void test_parse_rt(void)
{
	uint8_t p[FM_HCI_RT_TEXT + FM_HCI_RT_MAX + 8];
	char text[FM_RDS_TEXT_MAX] = "old";

	memset(p, 0, sizeof(p));
	/* Nothing after the header. */
	g_assert_false(fm_helium_parse_rt(p, FM_HCI_RT_TEXT, text, sizeof(text)));
	/* At once a CR. */
	p[FM_HCI_RT_TEXT] = 0x0d;
	g_assert_false(fm_helium_parse_rt(p, sizeof(p), text, sizeof(text)));
	/* Blank. */
	memset(p + FM_HCI_RT_TEXT, ' ', 10);
	p[FM_HCI_RT_TEXT + 10] = 0x0d;
	g_assert_false(fm_helium_parse_rt(p, sizeof(p), text, sizeof(text)));
	g_assert_cmpstr(text, ==, "old");
	/* Up to the CR. */
	memcpy(p + FM_HCI_RT_TEXT, "Now: song", 9);
	g_assert_true(fm_helium_parse_rt(p, sizeof(p), text, sizeof(text)));
	g_assert_cmpstr(text, ==, "Now: song");
	/* No CR: the event's end, or 64 characters. */
	memset(p + FM_HCI_RT_TEXT, 'x', sizeof(p) - FM_HCI_RT_TEXT);
	g_assert_true(fm_helium_parse_rt(p, FM_HCI_RT_TEXT + 3, text, sizeof(text)));
	g_assert_cmpstr(text, ==, "xxx");
	g_assert_true(fm_helium_parse_rt(p, sizeof(p), text, sizeof(text)));
	g_assert_cmpuint(strlen(text), ==, FM_HCI_RT_MAX);
}

static void test_rds_get(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;
	long groups = -1;

	setup_up(&t, 95000);
	chip_station(95000, -60, "RADIO 1", "Hello there", 500);
	chip_station(97500, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_rds_enable(&t, true), ==, 0);

	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "");

	fake_sleep(600);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "RADIO 1");
	g_assert_cmpstr(info.text, ==, "Hello there");
	g_assert_cmpint(groups, >, 0);

	/* A tune forgets it, and counts anew. */
	g_assert_cmpint(fm_tuner_tune(&t, 97500), ==, 0);
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpstr(info.text, ==, "");
	g_assert_cmpint(groups, ==, 0);

	/* No counters: the groups are not known. */
	chip.fail_op = FM_HCI_READ_GRP_CNTRS;
	g_assert_cmpint(fm_tuner_rds_get(&t, &info, &groups), ==, 0);
	g_assert_cmpint(groups, ==, -1);
	fm_tuner_close(&t);
}

static void test_find_name(void)
{
	struct fm_tuner t;
	struct fm_rds_info info;

	setup_up(&t, 95000);
	chip_station(101200, -60, "SKY", NULL, 2000);
	chip_station(97500, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_find_name(&t, 101200, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "SKY");

	/* No RDS at all: not waited for to the end. */
	chip.clock_ms = 0;
	g_assert_cmpint(fm_tuner_find_name(&t, 97500, NULL, NULL, &info), ==, 0);
	g_assert_cmpstr(info.name, ==, "");
	g_assert_cmpuint(chip.clock_ms, <, FM_NAME_WAIT_MS);
	fm_tuner_close(&t);
}

static void test_seek(void)
{
	struct fm_tuner t;
	const struct sent *s;

	setup_up(&t, 95000);
	chip_station(97500, -60, NULL, NULL, 0);
	chip_station(91000, -60, NULL, NULL, 0);

	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 97500);
	s = last_op(FM_HCI_SEARCH);
	g_assert_cmpuint(s->len, ==, 3);
	g_assert_cmpuint(s->p[0], ==, FM_HCI_SEEK);
	g_assert_cmpuint(s->p[2], ==, FM_HCI_DIR_UP);

	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 91000);
	s = last_op(FM_HCI_SEARCH);
	g_assert_cmpuint(s->p[2], ==, FM_HCI_DIR_DOWN);

	/* Round the band. */
	g_assert_cmpint(fm_tuner_seek(&t, -1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 97500);
	fm_tuner_close(&t);
}

static void test_seek_finds_nothing(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 95000);

	/* Only the station it is on. */
	chip_station(95000, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ENOENT);
	g_assert_cmpint(chip.freq, ==, 95000);
	fm_tuner_close(&t);
}

static void test_seek_counts_the_start(void)
{
	struct fm_tuner t;

	/* The chip stops at once on the station it starts on. */
	setup_up(&t, 95000);
	chip.counts_start = true;
	chip_station(95000, -60, NULL, NULL, 0);
	chip_station(99000, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 99000);

	/* The next channel is a station itself. */
	chip_station(99100, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 99100);

	/* Up from the top: from the bottom. */
	g_assert_cmpint(fm_tuner_tune(&t, FM_BAND_MAX_KHZ), ==, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 95000);
	fm_tuner_close(&t);
}

static bool cancel_now(void *data)
{
	(void)data;
	return true;
}

static void test_seek_cancelled_and_timed_out(void)
{
	struct fm_tuner t;

	setup_up(&t, 95000);
	chip_station(97500, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_seek(&t, 1, cancel_now, NULL), ==, -ECANCELED);
	g_assert_cmpuint(count_op(FM_HCI_CANCEL_SEARCH), ==, 1);
	g_assert_cmpint(t.khz, ==, 95000);

	chip.nlog = 0;
	chip.search_never_ends = true;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -ETIMEDOUT);
	g_assert_cmpuint(count_op(FM_HCI_CANCEL_SEARCH), ==, 1);
	g_assert_cmpint(t.khz, ==, 95000);

	/* A chip that ends a cancelled search late: not the next one's end. */
	chip.search_never_ends = false;
	chip.cancel_lag_ms = 200;
	g_assert_cmpint(fm_tuner_seek(&t, 1, cancel_now, NULL), ==, -ECANCELED);
	chip.nlog = 0;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 97500);
	/* One search: the old end did not cut it short (which only a second
	 * one, from the next channel, would have made up for). */
	g_assert_cmpuint(count_op(FM_HCI_SEARCH), ==, 1);
	g_assert_cmpint(fm_tuner_tune(&t, 95000), ==, 0);
	chip.cancel_lag_ms = 0;

	/* A chip that says nothing on a cancel: the next seek still works. */
	chip.search_never_ends = false;
	chip.cancel_silent = true;
	g_assert_cmpint(fm_tuner_seek(&t, 1, cancel_now, NULL), ==, -ECANCELED);
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, 0);
	g_assert_cmpint(t.khz, ==, 97500);
	g_assert_cmpint(fm_tuner_tune(&t, 95000), ==, 0);

	/* The search refused. */
	chip.fail_op = FM_HCI_SEARCH;
	g_assert_cmpint(fm_tuner_seek(&t, 1, NULL, NULL), ==, -EIO);
	g_assert_cmpint(t.khz, ==, 95000);
	fm_tuner_close(&t);
}

static void test_scan(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 0;

	setup_up(&t, 95000);
	chip_station(107900, -70, NULL, NULL, 0);
	chip_station(91000, -60, NULL, NULL, 0);
	chip_station(91100, -50, NULL, NULL, 0);
	chip_station(101200, -80, NULL, NULL, 0);

	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	/* 91.0 and 91.1 are one station, at its strongest. */
	g_assert_cmpuint(n, ==, 3);
	g_assert_cmpint(out[0].khz, ==, 91100);
	g_assert_cmpint(out[0].strength, ==, -50);
	g_assert_cmpint(out[1].khz, ==, 101200);
	g_assert_cmpint(out[1].strength, ==, -80);
	g_assert_cmpint(out[2].khz, ==, 107900);
	/* Back where it was. */
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 95000);

	/* The same where the chip counts the channel it starts on. */
	chip.counts_start = true;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 3);
	g_assert_cmpint(out[0].khz, ==, 91100);
	g_assert_cmpint(out[2].khz, ==, 107900);
	fm_tuner_close(&t);
}

static void test_scan_empty_and_top(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 7;

	setup_up(&t, 95000);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 0);

	/* A station at the very top ends it. */
	chip_station(FM_BAND_MAX_KHZ, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, 0);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpint(out[0].khz, ==, FM_BAND_MAX_KHZ);
	g_assert_cmpint(t.khz, ==, 95000);
	fm_tuner_close(&t);
}

static void test_scan_cancelled_and_errors(void)
{
	struct fm_tuner t;
	struct fm_station out[FM_BAND_CHANNELS];
	size_t n = 0;

	setup_up(&t, 95000);
	chip_station(97500, -60, NULL, NULL, 0);
	g_assert_cmpint(fm_tuner_scan(&t, cancel_now, NULL, out, &n), ==, -ECANCELED);
	g_assert_cmpint(t.khz, ==, 95000);

	/* The second search refused: the error, and back home. */
	chip.fail_op = FM_HCI_SEARCH;
	chip.fail_after = 1;
	g_assert_cmpint(fm_tuner_scan(&t, NULL, NULL, out, &n), ==, -EIO);
	g_assert_cmpint(t.khz, ==, 95000);
	g_assert_cmpint(chip.freq, ==, 95000);
	fm_tuner_close(&t);
}

static void test_queue_freed_with_events(void)
{
	/* Events left queued go with the link. */
	struct fm_hci *h = fm_hci_new(&fake_ops, NULL);
	static const uint8_t ev[] = { FM_HCI_EV_SEARCH_COMPLETE, 1, 0 };

	fm_hci_event(h, ev, sizeof(ev));
	fm_hci_event(h, ev, sizeof(ev));
	fm_hci_free(h);
	fm_hci_free(NULL);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/helium/ops", test_ops);
	g_test_add_func("/helium/opcodes", test_opcodes);
	g_test_add_func("/helium/open-and-close", test_open_and_close);
	g_test_add_func("/helium/open-fails", test_open_fails);
	g_test_add_func("/helium/power-up-configures", test_power_up_configures);
	g_test_add_func("/helium/power-up-refused", test_power_up_refused);
	g_test_add_func("/helium/power-up-setting-fails", test_power_up_setting_fails);
	g_test_add_func("/helium/power-down", test_power_down);
	g_test_add_func("/helium/tune-waits-for-the-chip", test_tune_waits_for_the_chip);
	g_test_add_func("/helium/tune-never-lands", test_tune_never_lands);
	g_test_add_func("/helium/command-unanswered", test_command_unanswered);
	g_test_add_func("/helium/command-answers-matched", test_command_answers_matched);
	g_test_add_func("/helium/transport-fails", test_transport_fails);
	g_test_add_func("/helium/events-malformed", test_events_malformed);
	g_test_add_func("/helium/rssi", test_rssi);
	g_test_add_func("/helium/rds-enable", test_rds_enable);
	g_test_add_func("/helium/parse-ps", test_parse_ps);
	g_test_add_func("/helium/parse-rt", test_parse_rt);
	g_test_add_func("/helium/rds-get", test_rds_get);
	g_test_add_func("/helium/find-name", test_find_name);
	g_test_add_func("/helium/seek", test_seek);
	g_test_add_func("/helium/seek-finds-nothing", test_seek_finds_nothing);
	g_test_add_func("/helium/seek-counts-the-start", test_seek_counts_the_start);
	g_test_add_func("/helium/seek-cancelled-and-timed-out", test_seek_cancelled_and_timed_out);
	g_test_add_func("/helium/scan", test_scan);
	g_test_add_func("/helium/scan-empty-and-top", test_scan_empty_and_top);
	g_test_add_func("/helium/scan-cancelled-and-errors", test_scan_cancelled_and_errors);
	g_test_add_func("/helium/queue-freed-with-events", test_queue_freed_with_events);

	return g_test_run();
}
