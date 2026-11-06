// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_iris.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>

/* At most this many names in one PS record (the event carries 119 bytes). */
#define PS_MAX_NAMES 14

/*
 * The last record in buf (n bytes) whose text is not blank, decoded into got
 * (size bytes): its offset, or n for none. A record is the header, then
 * its text: count names of 8 for PS, count bytes for RadioText.
 */
static size_t last_text(const uint8_t *buf, size_t n, bool ps, char *got, size_t size)
{
	size_t found = n;
	size_t off = 0;

	while (off + FM_IRIS_RDS_HEADER <= n)
	{
		size_t count = buf[off];
		size_t text = ps ? count * FM_IRIS_PS_LEN : count;
		size_t max = ps ? (size_t)PS_MAX_NAMES * FM_IRIS_PS_LEN : FM_IRIS_RT_MAX;

		/* A record cut short (the buffer was full) ends what can be read. */
		if (text > max || off + FM_IRIS_RDS_HEADER + text > n)
		{
			break;
		}

		if (text > 0)
		{
			/* Of a PS record the first name: the one now on air. */
			fm_rds_to_utf8(buf + off + FM_IRIS_RDS_HEADER, ps ? FM_IRIS_PS_LEN : text, got, size);

			if (got[0])
			{
				found = off;
			}
		}

		off += FM_IRIS_RDS_HEADER + text;
	}

	return found;
}

/*
 * The last record's text that is not blank into out (size bytes), cut as
 * fm_rds_to_utf8 cuts; out is left as it was when there is none.
 */
static bool take_text(const uint8_t *buf, size_t n, bool ps, char *out, size_t size)
{
	char got[FM_RDS_TEXT_MAX];
	size_t off = last_text(buf, n, ps, got, sizeof(got));

	if (off == n)
	{
		return false;
	}

	/* Again, into out: got holds the last record's, blank or not. */
	fm_rds_to_utf8(buf + off + FM_IRIS_RDS_HEADER, ps ? FM_IRIS_PS_LEN : buf[off], out, size);
	return true;
}

bool fm_iris_parse_ps(const uint8_t *buf, size_t n, char *name, size_t size)
{
	return take_text(buf, n, true, name, size);
}

bool fm_iris_parse_rt(const uint8_t *buf, size_t n, char *text, size_t size)
{
	return take_text(buf, n, false, text, size);
}

size_t fm_iris_parse_list(const uint8_t *buf, size_t n, int *khz, size_t max)
{
	size_t count = n > 0 ? buf[0] : 0;
	size_t have = 0;

	for (size_t i = 0; i < count && 3 + 2 * i <= n; i++)
	{
		int rel = buf[1 + 2 * i] << 8 | buf[2 + 2 * i];
		/* To the nearest channel: the chip measures in 50 kHz. */
		int f = FM_BAND_MIN_KHZ + (rel * 50 + FM_BAND_STEP_KHZ / 2) / FM_BAND_STEP_KHZ *
		        FM_BAND_STEP_KHZ;
		size_t at = have;

		if (!fm_band_valid(f))
		{
			continue;
		}

		while (at > 0 && khz[at - 1] > f)
		{
			at--;
		}

		if ((at > 0 && khz[at - 1] == f) || have == max)
		{
			continue;
		}

		for (size_t j = have; j > at; j--)
		{
			khz[j] = khz[j - 1];
		}

		khz[at] = f;
		have++;
	}

	return have;
}

static int xioctl(struct fm_tuner *t, unsigned long request, void *arg)
{
	return t->ioctl_fn(t->fd, request, arg);
}

static int set_ctrl(struct fm_tuner *t, unsigned int id, int value)
{
	struct v4l2_control c = { .id = id, .value = value };

	return xioctl(t, VIDIOC_S_CTRL, &c);
}

static int get_ctrl(struct fm_tuner *t, unsigned int id, int *value)
{
	struct v4l2_control c = { .id = id };
	int ret = xioctl(t, VIDIOC_G_CTRL, &c);

	if (ret == 0)
	{
		*value = c.value;
	}

	return ret;
}

/* One of the driver's buffers, up to len bytes: the byte count or -errno. */
/* The driver writes buf through the user pointer: it cannot be const. */
// cppcheck-suppress constParameterPointer
static int dqbuf(struct fm_tuner *t, unsigned int index, uint8_t *buf, unsigned int len)
{
	/* The driver copies into buf: written, though only through the ioctl. */
	struct v4l2_buffer b = {
		.index = index,
		.type = V4L2_BUF_TYPE_PRIVATE,
		.memory = V4L2_MEMORY_USERPTR,
		.m.userptr = (unsigned long)buf,
		.length = len,
	};
	int ret = xioctl(t, VIDIOC_DQBUF, &b);

	if (ret < 0)
	{
		return ret;
	}

	return b.bytesused <= len ? (int)b.bytesused : -EIO;
}

static int freq_now(struct fm_tuner *t, int *khz)
{
	struct v4l2_frequency f = { .type = V4L2_TUNER_RADIO };
	int ret = xioctl(t, VIDIOC_G_FREQUENCY, &f);

	if (ret == 0)
	{
		*khz = (int)(f.frequency / FM_IRIS_TUNE_PARAM);
	}

	return ret;
}

/* RDS from the tune on: nothing heard yet, and the chip counting anew. */
static void rds_restart(struct fm_tuner *t)
{
	t->rds = (struct fm_rds_info){ .name = "" };

	/* 1 reads the counters and zeroes them; the read goes to the buffer. */
	if (set_ctrl(t, FM_IRIS_CID_RDS_GRP_CNTRS, 1) == 0)
	{
		uint8_t buf[FM_IRIS_CNTRS_LEN] = { 0 };

		(void)dqbuf(t, FM_IRIS_BUF_CNTRS, buf, sizeof(buf));
	}
}

static int iris_tune(struct fm_tuner *t, int khz)
{
	struct v4l2_frequency f = {
		.type = V4L2_TUNER_RADIO,
		.frequency = (unsigned int)khz * FM_IRIS_TUNE_PARAM,
	};
	int now = 0;
	int ret = xioctl(t, VIDIOC_S_FREQUENCY, &f);

	if (ret < 0)
	{
		return ret;
	}

	/* Retuned once the chip reports it; what it measures before is not
	 * this channel's. */
	for (unsigned int waited = 0;; waited += FM_IRIS_POLL_MS)
	{
		ret = freq_now(t, &now);

		if (ret < 0)
		{
			return ret;
		}

		if (now == khz)
		{
			break;
		}

		if (waited >= FM_IRIS_TUNE_WAIT_MS)
		{
			return -ETIMEDOUT;
		}

		t->sleep_fn(FM_IRIS_POLL_MS);
	}

	t->khz = khz;
	rds_restart(t);
	return 0;
}

/*
 * The SMD link to the chip. It opens in the background: a command sent
 * straight after mostly goes unanswered (tissot: 5 times in 6, and the
 * driver then waits ten seconds), one sent 50 ms later was answered every
 * time. Closing the node closes the link again.
 */
static void iris_before_open(struct fm_tuner *t)
{
	if (t->write_fn(FM_IRIS_SMD_PARAM, "1") == 0)
	{
		t->sleep_fn(FM_IRIS_LINK_OPEN_MS);
	}
}

/*
 * The receiver on. A link that does not answer (ENODEV, or ETIMEDOUT after
 * the driver's ten seconds) is closed and opened again, with time for each,
 * and it is tried once more.
 */
static int receiver_on(struct fm_tuner *t)
{
	int ret = set_ctrl(t, FM_IRIS_CID_STATE, FM_IRIS_RECV);

	if (ret == -ENODEV || ret == -ETIMEDOUT || ret == -EIO)
	{
		(void)t->write_fn(FM_IRIS_SMD_PARAM, "0");
		t->sleep_fn(FM_IRIS_LINK_DOWN_MS);
		(void)t->write_fn(FM_IRIS_SMD_PARAM, "1");
		t->sleep_fn(FM_IRIS_LINK_UP_MS);
		ret = set_ctrl(t, FM_IRIS_CID_STATE, FM_IRIS_RECV);
	}

	return ret;
}

static int iris_power_down(struct fm_tuner *t)
{
	int ret = set_ctrl(t, FM_IRIS_CID_STATE, FM_IRIS_OFF);

	t->powered = false;
	t->rds = (struct fm_rds_info){ .name = "" };
	return ret;
}

static int iris_power_up(struct fm_tuner *t, int khz)
{
	/* The band, stereo, 50 us, 100 kHz channels; the antenna choice stays
	 * in the chip across power ups, so it is set every time. */
	struct v4l2_tuner tuner = {
		.rangelow = (unsigned int)FM_BAND_MIN_KHZ * FM_IRIS_TUNE_PARAM,
		.rangehigh = (unsigned int)FM_BAND_MAX_KHZ * FM_IRIS_TUNE_PARAM,
		.audmode = V4L2_TUNER_MODE_STEREO,
	};
	int ret = receiver_on(t);

	if (ret < 0)
	{
		return ret;
	}

	t->powered = true;
	ret = xioctl(t, VIDIOC_S_TUNER, &tuner);

	if (ret == 0)
	{
		ret = set_ctrl(t, FM_IRIS_CID_EMPHASIS, FM_IRIS_EMP50);
	}

	if (ret == 0)
	{
		ret = set_ctrl(t, FM_IRIS_CID_SPACING, FM_IRIS_SPACE_100K);
	}

	if (ret == 0)
	{
		ret = set_ctrl(t, FM_IRIS_CID_ANTENNA, FM_IRIS_ANT_WIRED);
	}

	if (ret == 0)
	{
		ret = iris_tune(t, khz);
	}

	if (ret < 0)
	{
		(void)iris_power_down(t);
	}

	return ret;
}

static int iris_rssi(struct fm_tuner *t, int *dbm)
{
	struct v4l2_tuner tuner = { .index = 0 };
	int ret = xioctl(t, VIDIOC_G_TUNER, &tuner);

	if (ret == 0)
	{
		/* The chip's signed byte, through a plain char: unsigned on ARM. */
		int v = tuner.signal & 0xff;

		*dbm = v >= 0x80 ? v - 0x100 : v;
	}

	return ret;
}

static int iris_rds_enable(struct fm_tuner *t, bool on)
{
	int ret = set_ctrl(t, FM_IRIS_CID_RDSON, on ? 1 : 0);

	if (ret == 0 && on)
	{
		ret = set_ctrl(t, FM_IRIS_CID_RDSGROUP_PROC, FM_IRIS_RDS_RT_PS);
	}

	return ret;
}

static long groups_since_tune(struct fm_tuner *t)
{
	uint8_t buf[FM_IRIS_CNTRS_LEN] = { 0 };
	const uint8_t *g = buf + FM_IRIS_CNTRS_GROUPS;

	if (set_ctrl(t, FM_IRIS_CID_RDS_GRP_CNTRS, 0) < 0 ||
	        dqbuf(t, FM_IRIS_BUF_CNTRS, buf, sizeof(buf)) != (int)sizeof(buf))
	{
		return -1;
	}

	return (long)((uint32_t)g[0] | (uint32_t)g[1] << 8 | (uint32_t)g[2] << 16 |
	              (uint32_t)g[3] << 24);
}

static int iris_rds_get(struct fm_tuner *t, struct fm_rds_info *out, long *groups)
{
	uint8_t buf[FM_IRIS_BUF_SIZE] = { 0 };
	int n = dqbuf(t, FM_IRIS_BUF_PS, buf, sizeof(buf));

	if (n < 0)
	{
		return n;
	}

	(void)fm_iris_parse_ps(buf, (size_t)n, t->rds.name, sizeof(t->rds.name));
	n = dqbuf(t, FM_IRIS_BUF_RT, buf, sizeof(buf));

	if (n < 0)
	{
		return n;
	}

	(void)fm_iris_parse_rt(buf, (size_t)n, t->rds.text, sizeof(t->rds.text));
	*out = t->rds;
	*groups = groups_since_tune(t);
	return 0;
}

/*
 * Starts the chip's search in mode, up or down from the current channel,
 * round the band.
 */
static int search_start(struct fm_tuner *t, int mode, int dir)
{
	struct v4l2_hw_freq_seek seek = {
		.type = V4L2_TUNER_RADIO,
		.seek_upward = dir > 0,
		.wrap_around = 1,
	};
	int ret = set_ctrl(t, FM_IRIS_CID_SRCHMODE, mode);

	if (ret < 0)
	{
		return ret;
	}

	return xioctl(t, VIDIOC_S_HW_FREQ_SEEK, &seek);
}

/*
 * The chip seeks from the current channel, up or down, round the band once,
 * and stops on the next station or back where it began: into landed, and
 * whether it was over at once, without searching.
 */
static int hw_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data, int *landed,
                   bool *at_once)
{
	unsigned int waited = 0;
	int ret = search_start(t, FM_IRIS_SEEK, dir);

	if (ret < 0)
	{
		return ret;
	}

	for (;; waited += FM_IRIS_POLL_MS)
	{
		int on = 1;

		if (cancelled && cancelled(data))
		{
			(void)set_ctrl(t, FM_IRIS_CID_SRCHON, 0);
			return -ECANCELED;
		}

		if (waited >= FM_IRIS_SEEK_WAIT_MS)
		{
			(void)set_ctrl(t, FM_IRIS_CID_SRCHON, 0);
			return -ETIMEDOUT;
		}

		t->sleep_fn(FM_IRIS_POLL_MS);
		ret = get_ctrl(t, FM_IRIS_CID_SRCHON, &on);

		if (ret < 0)
		{
			return ret;
		}

		if (!on)
		{
			break;
		}
	}

	*at_once = waited < FM_IRIS_SEEK_AT_ONCE_MS;
	return freq_now(t, landed);
}

/*
 * Lists still queued: the driver keeps each until it is read, and a tune
 * does not clear them. One a search left when it was stopped, or another
 * program's, would otherwise be taken for the next one - and that one then
 * left for the search after it. 0 once there are none, or -errno.
 */
static int drain_list(struct fm_tuner *t, uint8_t *buf, unsigned int len)
{
	for (int i = 0; i < FM_IRIS_LIST_DRAIN; i++)
	{
		int n = dqbuf(t, FM_IRIS_BUF_LIST, buf, len);

		if (n <= 0)
		{
			return n;
		}
	}

	return -EIO;
}

/*
 * The chip's list of the strong stations from the current channel up, as
 * channels in frequency order. The driver leaves the search on after it, so
 * it is switched off here.
 */
static int search_list(struct fm_tuner *t, fm_cancel_fn cancelled, void *data, int *khz,
                       size_t *count)
{
	uint8_t buf[FM_IRIS_BUF_SIZE] = { 0 };
	int n = drain_list(t, buf, sizeof(buf));

	if (n == 0)
	{
		n = set_ctrl(t, FM_IRIS_CID_SRCH_CNT, FM_IRIS_LIST_MAX);
	}

	if (n == 0)
	{
		n = search_start(t, FM_IRIS_LIST_STRONG, 1);
	}

	for (unsigned int waited = 0; n == 0; waited += FM_IRIS_LIST_POLL_MS)
	{
		if (cancelled && cancelled(data))
		{
			n = -ECANCELED;
		}
		else if (waited >= FM_IRIS_LIST_WAIT_MS)
		{
			n = -ETIMEDOUT;
		}
		else
		{
			t->sleep_fn(FM_IRIS_LIST_POLL_MS);
			n = dqbuf(t, FM_IRIS_BUF_LIST, buf, sizeof(buf));
		}
	}

	(void)set_ctrl(t, FM_IRIS_CID_SRCHON, 0);

	if (n < 0)
	{
		return n;
	}

	*count = fm_iris_parse_list(buf, (size_t)n, khz, FM_IRIS_LIST_MAX);
	return 0;
}

static int iris_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data)
{
	int home = t->khz;
	int from = home;
	int landed = home;
	bool at_once = false;
	int ret = 0;

	/* Up from the top the chip does not move: round from the bottom. */
	if (dir > 0 && home == FM_BAND_MAX_KHZ)
	{
		from = FM_BAND_MIN_KHZ;
		ret = iris_tune(t, from);
	}

	if (ret == 0)
	{
		ret = hw_seek(t, dir, cancelled, data, &landed, &at_once);
	}

	/*
	 * Over at once where it began: the chip counted the channel it started
	 * on. Once more from the next one - where stopping at once again means
	 * that channel is the station.
	 */
	if (ret == 0 && at_once && landed == from && fm_band_next(from, dir) != home)
	{
		from = fm_band_next(from, dir);
		ret = iris_tune(t, from);

		if (ret == 0)
		{
			ret = hw_seek(t, dir, cancelled, data, &landed, &at_once);
		}

		if (ret == 0 && landed == from && !at_once)
		{
			/* Round the band and back: none. */
			landed = home;
		}
	}
	else if (ret == 0 && landed == from)
	{
		landed = home;
	}

	if (ret == 0 && landed != home && fm_band_valid(landed))
	{
		t->khz = landed;
		rds_restart(t);
		return 0;
	}

	/* Nothing else in the band, or stopped: back where it was. */
	(void)iris_tune(t, home);
	return ret < 0 ? ret : -ENOENT;
}

static int iris_scan(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
                     struct fm_station *out, size_t *n)
{
	int home = t->khz;
	int from = FM_BAND_MIN_KHZ;
	int last = 0;
	size_t found = 0;
	int ret = 0;

	/*
	 * The chip lists 20 stations at most: while it lists that many, once more
	 * from past the last. Only what lies above the last counts, whatever the
	 * chip makes of where it starts.
	 */
	for (int pass = 0; ret == 0 && pass < FM_IRIS_LIST_PASSES; pass++)
	{
		int list[FM_IRIS_LIST_MAX];
		size_t count = 0;
		size_t added = 0;

		ret = iris_tune(t, from);

		if (ret == 0)
		{
			ret = search_list(t, cancelled, data, list, &count);
		}

		for (size_t i = 0; ret == 0 && i < count && found < FM_BAND_CHANNELS; i++)
		{
			if (list[i] > last)
			{
				out[found].khz = list[i];
				out[found].strength = 0;
				found++;
				added++;
				last = list[i];
			}
		}

		if (count < FM_IRIS_LIST_MAX || added == 0 || last == FM_BAND_MAX_KHZ)
		{
			break;
		}

		from = fm_band_next(last, 1);
	}

	/* How strong each is: the list does not say. */
	for (size_t i = 0; ret == 0 && i < found; i++)
	{
		if (cancelled && cancelled(data))
		{
			ret = -ECANCELED;
		}
		else if (iris_tune(t, out[i].khz) == 0)
		{
			(void)iris_rssi(t, &out[i].strength);
		}
	}

	(void)iris_tune(t, home);

	if (ret < 0)
	{
		return ret;
	}

	*n = fm_band_merge(out, found);
	return 0;
}

const struct fm_tuner_ops fm_iris_ops = {
	.name = "iris",
	.path = "/dev/radio0",
	.open_flags = O_RDONLY,
	.before_open = iris_before_open,
	.power_up = iris_power_up,
	.power_down = iris_power_down,
	.tune = iris_tune,
	.rssi = iris_rssi,
	.rds_enable = iris_rds_enable,
	.rds_get = iris_rds_get,
	.scan = iris_scan,
	.seek = iris_seek,
};
