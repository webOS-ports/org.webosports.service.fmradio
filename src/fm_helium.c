// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_helium.h"

#include <errno.h>

static void bytes_unref(gpointer b)
{
	g_bytes_unref(b);
}

struct fm_hci *fm_hci_new(const struct fm_hci_ops *ops, void *link)
{
	struct fm_hci *h = g_new0(struct fm_hci, 1);

	h->ops = ops;
	h->link = link;
	h->events = g_async_queue_new_full(bytes_unref);
	return h;
}

void fm_hci_free(struct fm_hci *h)
{
	if (!h)
	{
		return;
	}

	g_async_queue_unref(h->events);
	g_free(h);
}

void fm_hci_event(struct fm_hci *h, const uint8_t *ev, size_t n)
{
	g_async_queue_push(h->events, g_bytes_new(ev, n));
}

bool fm_helium_parse_ps(const uint8_t *p, size_t n, char *name, size_t size)
{
	char got[FM_RDS_NAME_MAX];

	if (n < FM_HCI_PS_NAMES + FM_HCI_PS_LEN || p[FM_HCI_PS_COUNT] == 0)
	{
		return false;
	}

	/* Of the names the first: the one now on air. */
	fm_rds_to_utf8(p + FM_HCI_PS_NAMES, FM_HCI_PS_LEN, got, sizeof(got));

	if (!got[0])
	{
		return false;
	}

	fm_rds_to_utf8(p + FM_HCI_PS_NAMES, FM_HCI_PS_LEN, name, size);
	return true;
}

bool fm_helium_parse_rt(const uint8_t *p, size_t n, char *text, size_t size)
{
	char got[FM_RDS_TEXT_MAX];
	size_t len;

	/* Within the event and 64 characters (helium reads on to 64 regardless);
	 * fm_rds_to_utf8 stops at the CR. */
	if (n <= FM_HCI_RT_TEXT)
	{
		return false;
	}

	len = n - FM_HCI_RT_TEXT < FM_HCI_RT_MAX ? n - FM_HCI_RT_TEXT : FM_HCI_RT_MAX;

	fm_rds_to_utf8(p + FM_HCI_RT_TEXT, len, got, sizeof(got));

	if (!got[0])
	{
		return false;
	}

	fm_rds_to_utf8(p + FM_HCI_RT_TEXT, len, text, size);
	return true;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

/* The command waiting for its answer, and what came back. */
struct pending
{
	uint16_t opcode;
	bool done;
	int err;
	GBytes *ret;            /* Command Complete's return parameters */
};

/*
 * Whether a Command Complete or Command Status (event b, its parameters len
 * bytes at p) answers cmd: then it is done, with what came back.
 */
static void answer(struct pending *cmd, GBytes *b, uint8_t code, const uint8_t *p, size_t len)
{
	if (code == FM_HCI_EV_CMD_COMPLETE && len >= FM_HCI_CC_RETURN &&
	        get_le16(p + 1) == cmd->opcode)
	{
		cmd->done = true;
		cmd->ret = g_bytes_new_from_bytes(b, FM_HCI_EV_HEADER + FM_HCI_CC_RETURN,
		                                  len - FM_HCI_CC_RETURN);
		/* The status first, if there are return parameters at all. */
		cmd->err = len > FM_HCI_CC_RETURN && p[FM_HCI_CC_RETURN] != 0 ? -EIO : 0;
	}
	else if (code == FM_HCI_EV_CMD_STATUS && len >= FM_HCI_CS_LEN &&
	         get_le16(p + 2) == cmd->opcode)
	{
		cmd->done = true;
		cmd->err = p[0] != 0 ? -EIO : 0;
	}
}

/* An event that is no answer: what the chip reports on its own. */
static void notice(struct fm_tuner *t, uint8_t code, const uint8_t *p, size_t len)
{
	switch (code)
	{
	case FM_HCI_EV_TUNE_STATUS:
		if (len >= FM_HCI_STN_LEN)
		{
			t->hci->tuned = true;
			t->hci->tuned_khz = (int)get_le32(p + FM_HCI_STN_FREQ);
		}

		break;

	case FM_HCI_EV_SEARCH_COMPLETE:
		t->hci->search_done = true;
		break;

	case FM_HCI_EV_PROGRAM_SERVICE:
		(void)fm_helium_parse_ps(p, len, t->rds.name, sizeof(t->rds.name));
		break;

	case FM_HCI_EV_RADIO_TEXT:
		(void)fm_helium_parse_rt(p, len, t->rds.text, sizeof(t->rds.text));
		break;

	case FM_HCI_EV_HW_ERR:
		t->hci->hw_errors++;
		break;

	default:
		break;
	}
}

/* One event from the chip: what it says goes where it belongs. */
static void dispatch(struct fm_tuner *t, GBytes *b, struct pending *cmd)
{
	gsize n = 0;
	const uint8_t *ev = g_bytes_get_data(b, &n);
	size_t len;

	if (!ev || n < FM_HCI_EV_HEADER)
	{
		return;
	}

	/* The length it gives, or what arrived if that is less. */
	len = ev[1] < n - FM_HCI_EV_HEADER ? ev[1] : n - FM_HCI_EV_HEADER;

	if (cmd && !cmd->done &&
	        (ev[0] == FM_HCI_EV_CMD_COMPLETE || ev[0] == FM_HCI_EV_CMD_STATUS))
	{
		answer(cmd, b, ev[0], ev + FM_HCI_EV_HEADER, len);
	}
	else
	{
		notice(t, ev[0], ev + FM_HCI_EV_HEADER, len);
	}
}

/* Whatever the chip has sent so far, without waiting. */
static void drain(struct fm_tuner *t, struct pending *cmd)
{
	GBytes *b;

	while ((b = g_async_queue_try_pop(t->hci->events)) != NULL)
	{
		dispatch(t, b, cmd);
		g_bytes_unref(b);
	}
}

/*
 * Sends a command and waits for its Command Complete or Command Status.
 * ret, if not NULL: the return parameters (status first; empty for a
 * Command Status), for the caller to unref. 0, -EIO when the chip says it
 * failed, -ETIMEDOUT, or the transport's error.
 */
static int command(struct fm_tuner *t, uint16_t opcode, const uint8_t *params, size_t len,
                   GBytes **ret)
{
	const uint8_t header[FM_HCI_CMD_HEADER] = { (uint8_t)opcode, (uint8_t)(opcode >> 8),
	                                            (uint8_t)len };
	struct pending cmd = { .opcode = opcode };
	GByteArray *pkt;
	int err;

	if (len > FM_HCI_PARAM_MAX)
	{
		return -EINVAL;
	}

	pkt = g_byte_array_sized_new((guint)(FM_HCI_CMD_HEADER + len));
	g_byte_array_append(pkt, header, FM_HCI_CMD_HEADER);

	if (len > 0)
	{
		g_byte_array_append(pkt, params, (guint)len);
	}

	/* What came before is no answer to this one. */
	drain(t, NULL);
	err = t->hci->ops->send(t->hci, pkt->data, pkt->len);
	g_byte_array_unref(pkt);

	for (unsigned int waited = 0; err == 0; waited += FM_HELIUM_POLL_MS)
	{
		drain(t, &cmd);

		if (cmd.done)
		{
			err = cmd.err;
			break;
		}

		if (waited >= FM_HELIUM_CMD_WAIT_MS)
		{
			err = -ETIMEDOUT;
			break;
		}

		t->sleep_fn(FM_HELIUM_POLL_MS);
	}

	if (err == 0 && ret)
	{
		*ret = cmd.ret ? cmd.ret : g_bytes_new(NULL, 0);
	}
	else if (cmd.ret)
	{
		g_bytes_unref(cmd.ret);
	}

	return err;
}

static int command1(struct fm_tuner *t, uint16_t opcode, uint8_t value)
{
	return command(t, opcode, &value, 1, NULL);
}

static int command32(struct fm_tuner *t, uint16_t opcode, uint32_t value)
{
	uint8_t p[4];

	put_le32(p, value);
	return command(t, opcode, p, sizeof(p), NULL);
}

/* The station parameters: frequency and RSSI of the current channel. */
static int station(struct fm_tuner *t, int *khz, int *dbm)
{
	GBytes *r = NULL;
	gsize n = 0;
	const uint8_t *p;
	int ret = command(t, FM_HCI_GET_STATION, NULL, 0, &r);

	if (ret < 0)
	{
		return ret;
	}

	p = g_bytes_get_data(r, &n);

	if (!p || n < FM_HCI_STN_LEN)
	{
		ret = -EIO;
	}
	else
	{
		if (khz)
		{
			*khz = (int)get_le32(p + FM_HCI_STN_FREQ);
		}

		if (dbm)
		{
			int v = p[FM_HCI_STN_RSSI];

			*dbm = v >= 0x80 ? v - 0x100 : v;
		}
	}

	g_bytes_unref(r);
	return ret;
}

/* The receiver configuration, RDS on or off: the band, 50 us, 100 kHz. */
static int set_conf(struct fm_tuner *t, uint8_t rds)
{
	uint8_t p[FM_HCI_RECV_CONF_LEN] = { FM_HCI_EMP50, FM_HCI_SPACE_100K, rds, 0 };

	put_le32(p + 4, FM_BAND_MIN_KHZ);
	put_le32(p + 8, FM_BAND_MAX_KHZ);
	return command(t, FM_HCI_SET_RECV_CONF, p, sizeof(p), NULL);
}

/* RDS from the tune on: nothing heard yet, and the chip counting anew. */
static void rds_restart(struct fm_tuner *t)
{
	t->rds = (struct fm_rds_info){ .name = "" };
	/* 1 reads the counters and zeroes them. */
	(void)command32(t, FM_HCI_READ_GRP_CNTRS, 1);
}

static int helium_tune(struct fm_tuner *t, int khz)
{
	int ret;

	drain(t, NULL);
	t->hci->tuned = false;
	ret = command32(t, FM_HCI_TUNE, (uint32_t)khz);

	if (ret < 0)
	{
		return ret;
	}

	/* Retuned once the chip reports it; what it measures before is not
	 * this channel's. */
	for (unsigned int waited = 0;; waited += FM_HELIUM_POLL_MS)
	{
		drain(t, NULL);

		if (t->hci->tuned && t->hci->tuned_khz == khz)
		{
			break;
		}

		if (waited >= FM_HELIUM_TUNE_WAIT_MS)
		{
			return -ETIMEDOUT;
		}

		t->sleep_fn(FM_HELIUM_POLL_MS);
	}

	t->khz = khz;
	rds_restart(t);
	return 0;
}

static int helium_open(struct fm_tuner *t)
{
	int ret;

	if (!t->hci_ops)
	{
		return -ENODEV;
	}

	t->hci = fm_hci_new(t->hci_ops, NULL);
	/* The transport's hook, not open(2). */
	ret = t->hci->ops->open(t->hci); /* Flawfinder: ignore */

	if (ret < 0)
	{
		fm_hci_free(t->hci);
		t->hci = NULL;
	}

	return ret;
}

static void helium_close(struct fm_tuner *t)
{
	t->hci->ops->close(t->hci);
	fm_hci_free(t->hci);
	t->hci = NULL;
}

static int helium_power_down(struct fm_tuner *t)
{
	int ret = command(t, FM_HCI_DISABLE_RECV, NULL, 0, NULL);

	/* Said once a session rather than at each, from the worker. */
	if (t->hci->hw_errors > 0)
	{
		g_warning("FM chip reported %u hardware errors", t->hci->hw_errors);
		t->hci->hw_errors = 0;
	}

	t->powered = false;
	t->rds = (struct fm_rds_info){ .name = "" };
	return ret;
}

/*
 * As radio-iris' initialise_recv: soft mute, stereo with blending, the
 * events; then the band, the antenna, the channel, and the SLIMbus port the
 * audio leaves the chip on.
 */
static int helium_power_up(struct fm_tuner *t, int khz)
{
	static const uint8_t mute[] = { 0, FM_HCI_SOFT_MUTE_ON };          /* hard, soft */
	static const uint8_t stereo[] = { FM_HCI_STEREO_AUTO, 1, 1, 1 };
	int ret = command(t, FM_HCI_ENABLE_RECV, NULL, 0, NULL);

	if (ret < 0)
	{
		return ret;
	}

	t->powered = true;
	ret = command(t, FM_HCI_SET_MUTE_MODE, mute, sizeof(mute), NULL);

	if (ret == 0)
	{
		ret = command(t, FM_HCI_SET_STEREO_MODE, stereo, sizeof(stereo), NULL);
	}

	if (ret == 0)
	{
		ret = command1(t, FM_HCI_SET_EVENT_MASK, FM_HCI_EVENT_MASK);
	}

	if (ret == 0)
	{
		ret = set_conf(t, FM_HCI_RDS_OFF);
	}

	if (ret == 0)
	{
		ret = command1(t, FM_HCI_SET_ANTENNA, FM_HCI_ANT_WIRED);
	}

	if (ret == 0)
	{
		ret = helium_tune(t, khz);
	}

	if (ret == 0)
	{
		ret = command1(t, FM_HCI_ENABLE_SLIMBUS, 1);
	}

	if (ret < 0)
	{
		(void)helium_power_down(t);
	}

	return ret;
}

static int helium_rssi(struct fm_tuner *t, int *dbm)
{
	return station(t, NULL, dbm);
}

static int helium_rds_enable(struct fm_tuner *t, bool on)
{
	int ret = set_conf(t, on ? FM_HCI_RDS_ON : FM_HCI_RDS_OFF);

	if (ret == 0 && on)
	{
		ret = command32(t, FM_HCI_RDS_GRP_PROCESS, FM_HCI_RDS_RT_PS);
	}

	return ret;
}

static long groups_since_tune(struct fm_tuner *t)
{
	static const uint8_t keep[4] = { 0 };
	GBytes *r = NULL;
	gsize n = 0;
	const uint8_t *p;
	long groups = -1;

	if (command(t, FM_HCI_READ_GRP_CNTRS, keep, sizeof(keep), &r) < 0)
	{
		return -1;
	}

	p = g_bytes_get_data(r, &n);

	if (p && n >= FM_HCI_CNTRS_LEN)
	{
		groups = (long)get_le32(p + FM_HCI_CNTRS_GROUPS);
	}

	g_bytes_unref(r);
	return groups;
}

static int helium_rds_get(struct fm_tuner *t, struct fm_rds_info *out, long *groups)
{
	drain(t, NULL);
	*out = t->rds;
	*groups = groups_since_tune(t);
	return 0;
}

/*
 * Stops the search on, and waits a moment for the Search Complete it ends
 * with: left in the queue, it would end the next search before it began.
 */
static void stop_search(struct fm_tuner *t)
{
	(void)command(t, FM_HCI_CANCEL_SEARCH, NULL, 0, NULL);

	for (unsigned int waited = 0; waited < FM_HELIUM_TUNE_WAIT_MS; waited += FM_HELIUM_POLL_MS)
	{
		drain(t, NULL);

		if (t->hci->search_done)
		{
			break;
		}

		t->sleep_fn(FM_HELIUM_POLL_MS);
	}
}

/*
 * The chip seeks from the current channel, up or down, round the band once,
 * and stops on the next station or back where it began: into landed, and
 * whether it was over at once, without searching.
 */
static int hw_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data, int *landed,
                   bool *at_once)
{
	const uint8_t p[] = { FM_HCI_SEEK, 0, dir > 0 ? FM_HCI_DIR_UP : FM_HCI_DIR_DOWN };
	unsigned int waited = 0;
	int ret;

	drain(t, NULL);
	t->hci->search_done = false;
	ret = command(t, FM_HCI_SEARCH, p, sizeof(p), NULL);

	if (ret < 0)
	{
		return ret;
	}

	for (;; waited += FM_HELIUM_POLL_MS)
	{
		drain(t, NULL);

		if (t->hci->search_done)
		{
			break;
		}

		if (cancelled && cancelled(data))
		{
			stop_search(t);
			return -ECANCELED;
		}

		if (waited >= FM_HELIUM_SEEK_WAIT_MS)
		{
			stop_search(t);
			return -ETIMEDOUT;
		}

		t->sleep_fn(FM_HELIUM_POLL_MS);
	}

	*at_once = waited < FM_HELIUM_SEEK_AT_ONCE_MS;
	return station(t, landed, NULL);
}

/*
 * The next station up or down from start, other than start: into landed,
 * the chip left there. -ENOENT if there is none.
 */
static int seek_from(struct fm_tuner *t, int start, int dir, fm_cancel_fn cancelled, void *data,
                     int *landed)
{
	int from = start;
	int got = start;
	bool at_once = false;
	int ret = helium_tune(t, start);

	if (ret == 0)
	{
		ret = hw_seek(t, dir, cancelled, data, &got, &at_once);
	}

	/*
	 * Over at once where it began: radio-iris' WCNSS counts the channel it
	 * starts on, and this chip may as well. Once more from the next one -
	 * where stopping at once again means that channel is the station.
	 */
	if (ret == 0 && at_once && got == from)
	{
		from = fm_band_next(start, dir);
		ret = helium_tune(t, from);

		if (ret == 0)
		{
			ret = hw_seek(t, dir, cancelled, data, &got, &at_once);
		}

		if (ret == 0 && got == from && !at_once)
		{
			/* Round the band and back: none. */
			return -ENOENT;
		}
	}
	else if (ret == 0 && got == from)
	{
		return -ENOENT;
	}

	if (ret < 0)
	{
		return ret;
	}

	if (got == start || !fm_band_valid(got))
	{
		return -ENOENT;
	}

	*landed = got;
	return 0;
}

static int helium_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data)
{
	int home = t->khz;
	int start = home;
	int landed = home;
	int ret;

	/* Up from the top radio-iris' chip does not move: round from the bottom. */
	if (dir > 0 && home == FM_BAND_MAX_KHZ)
	{
		start = FM_BAND_MIN_KHZ;
	}

	ret = seek_from(t, start, dir, cancelled, data, &landed);

	if (ret == 0 && landed != home)
	{
		t->khz = landed;
		rds_restart(t);
		return 0;
	}

	/* Nothing else in the band, or stopped: back where it was. */
	(void)helium_tune(t, home);
	return ret < 0 ? ret : -ENOENT;
}

/*
 * Seek after seek up the band, from the bottom, until the chip comes round
 * again. The chip's own list of strong stations is not used: radio-iris
 * sends its request packed, helium with the padding of an arm64 struct, and
 * which of the two this chip reads is not known.
 */
static int helium_scan(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
                       struct fm_station *out, size_t *n)
{
	int home = t->khz;
	int from = FM_BAND_MIN_KHZ;
	int last = 0;
	size_t found = 0;
	int ret = 0;

	while (found < FM_BAND_CHANNELS)
	{
		int landed = 0;

		ret = seek_from(t, from, 1, cancelled, data, &landed);

		if (ret == -ENOENT)
		{
			ret = 0;
			break;
		}

		/* Round the band: every station is in. */
		if (ret < 0 || landed <= last)
		{
			break;
		}

		out[found].khz = landed;

		if (helium_rssi(t, &out[found].strength) < 0)
		{
			out[found].strength = 0;
		}

		found++;
		last = landed;

		if (landed == FM_BAND_MAX_KHZ)
		{
			break;
		}

		from = landed;
	}

	(void)helium_tune(t, home);

	if (ret < 0)
	{
		return ret;
	}

	*n = fm_band_merge(out, found);
	return 0;
}

const struct fm_tuner_ops fm_helium_ops = {
	.name = "helium",
	.path = FM_HELIUM_IMPL,
	.open_flags = 0,
	.audio_args = "mode=parameters",
	/* A hook, not open(2). */
	.open = helium_open, /* Flawfinder: ignore */
	.close = helium_close,
	.power_up = helium_power_up,
	.power_down = helium_power_down,
	.tune = helium_tune,
	.rssi = helium_rssi,
	.rds_enable = helium_rds_enable,
	.rds_get = helium_rds_get,
	.scan = helium_scan,
	.seek = helium_seek,
};
