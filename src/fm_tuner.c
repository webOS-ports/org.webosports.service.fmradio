// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_tuner.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FM_IOC_MAGIC 0xf5

/* What the driver is powered up with: the 87.5 - 108 MHz band, 100 kHz
 * channels, no automatic high/low side injection, 50 us de-emphasis. */
#define FM_DRV_BAND_UE     1
#define FM_DRV_SPACE_100K  1
#define FM_DRV_HILO_OFF    0
#define FM_DRV_DEEMPH_50US 0

/* 0 asks to power the receiver down; the driver also has a transmitter. */
#define FM_DRV_RX 0

_Static_assert(sizeof(struct fm_tune_parm) == 8, "fm_tune_parm differs from the driver's");
_Static_assert(sizeof(struct fm_softmute_tune_t) == 12,
               "fm_softmute_tune_t differs from the driver's");

/*
 * The driver's fm_ioctl.h declares each ioctl as _IOWR(0xf5, nr, <type> *):
 * the size field is that of a pointer, whatever the argument really is. The
 * kernel was built with the same header, so a userland of the kernel's word
 * size encodes exactly what it compares against.
 */
unsigned long fm_tuner_request(unsigned int nr)
{
	return _IOC(_IOC_READ | _IOC_WRITE, FM_IOC_MAGIC, nr, sizeof(void *));
}

int fm_tuner_request_nr(unsigned long request)
{
	if (_IOC_TYPE(request) != FM_IOC_MAGIC || _IOC_SIZE(request) != sizeof(void *) ||
	        _IOC_DIR(request) != (_IOC_READ | _IOC_WRITE))
	{
		return -1;
	}

	return (int)_IOC_NR(request);
}

static int sys_ioctl(int fd, unsigned long request, void *arg)
{
	return ioctl(fd, request, arg) < 0 ? -errno : 0;
}

static long sys_read(int fd, void *buf, size_t len)
{
	/* Into the caller's record, len its size: the driver writes no more. */
	ssize_t n = read(fd, buf, len); /* Flawfinder: ignore */

	return n < 0 ? -errno : (long)n;
}

static void sys_sleep(unsigned int ms)
{
	struct timespec ts = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
	{
	}
}

static int call(struct fm_tuner *t, unsigned int nr, void *arg)
{
	if (t->fd < 0)
	{
		return -EBADF;
	}

	return t->ioctl_fn(t->fd, fm_tuner_request(nr), arg);
}

static unsigned short to_driver(int khz)
{
	return (unsigned short)(khz / 10);
}

static void tune_parm(struct fm_tune_parm *p, int khz)
{
	p->err = 0;
	p->band = FM_DRV_BAND_UE;
	p->space = FM_DRV_SPACE_100K;
	p->hilo = FM_DRV_HILO_OFF;
	p->deemphasis = FM_DRV_DEEMPH_50US;
	p->freq = to_driver(khz);
}

void fm_tuner_init(struct fm_tuner *t, fm_ioctl_fn ioctl_fn)
{
	t->fd = -1;
	t->ioctl_fn = ioctl_fn ? ioctl_fn : sys_ioctl;
	t->read_fn = sys_read;
	t->sleep_fn = sys_sleep;
	t->powered = false;
	t->khz = 0;
}

int fm_tuner_open(struct fm_tuner *t, const char *path)
{
	int fd;

	if (t->fd >= 0)
	{
		return 0;
	}

	/* The driver's node, a fixed path: nothing to redirect. */
	fd = open(path, O_RDWR | O_CLOEXEC); /* Flawfinder: ignore */

	if (fd < 0)
	{
		return -errno;
	}

	t->fd = fd;
	return 0;
}

void fm_tuner_close(struct fm_tuner *t)
{
	if (t->fd < 0)
	{
		return;
	}

	if (t->powered)
	{
		(void)fm_tuner_power_down(t);
	}

	(void)close(t->fd);
	t->fd = -1;
}

int fm_tuner_power_up(struct fm_tuner *t, int khz)
{
	struct fm_tune_parm p;
	int ret;

	if (!fm_band_valid(khz))
	{
		return -EINVAL;
	}

	if (t->powered)
	{
		return fm_tuner_tune(t, khz);
	}

	tune_parm(&p, khz);
	ret = call(t, FM_NR_POWERUP, &p);

	if (ret < 0)
	{
		return ret;
	}

	/* err set with a 0 return is the chip refusing the frequency. */
	if (p.err)
	{
		return -EIO;
	}

	t->powered = true;
	t->khz = khz;
	return 0;
}

int fm_tuner_power_down(struct fm_tuner *t)
{
	int32_t type = FM_DRV_RX;
	int ret;

	if (!t->powered)
	{
		return 0;
	}

	ret = call(t, FM_NR_POWERDOWN, &type);
	/* Down either way: a receiver that failed to power down is not one to
	 * tune again, and the next power up starts it over. */
	t->powered = false;
	return ret;
}

int fm_tuner_tune(struct fm_tuner *t, int khz)
{
	struct fm_tune_parm p;
	int ret;

	if (!fm_band_valid(khz))
	{
		return -EINVAL;
	}

	if (!t->powered)
	{
		return -ENODEV;
	}

	tune_parm(&p, khz);
	ret = call(t, FM_NR_TUNE, &p);

	if (ret < 0)
	{
		return ret;
	}

	if (p.err)
	{
		return -EIO;
	}

	t->khz = khz;
	return 0;
}

int fm_tuner_rssi(struct fm_tuner *t, int *dbm)
{
	int32_t rssi = 0;
	int ret;

	if (!t->powered)
	{
		return -ENODEV;
	}

	ret = call(t, FM_NR_GETRSSI, &rssi);

	if (ret == 0)
	{
		*dbm = (int)rssi;
	}

	return ret;
}

int fm_tuner_rds_enable(struct fm_tuner *t, bool on)
{
	/* The driver reads an unsigned short: 1 on, 0 off. */
	uint16_t value = on ? 1 : 0;

	if (!t->powered)
	{
		return -ENODEV;
	}

	return call(t, FM_NR_RDS_ONOFF, &value);
}

int fm_tuner_rds_read(struct fm_tuner *t, struct fm_rds_raw *raw)
{
	long n;

	if (!t->powered)
	{
		return -ENODEV;
	}

	if (t->fd < 0)
	{
		return -EBADF;
	}

	/* The driver gives the whole record or nothing (0: a smaller buffer, or
	 * the chip resetting). */
	n = t->read_fn(t->fd, raw, sizeof(*raw));

	if (n < 0)
	{
		return (int)n;
	}

	return n == (long)sizeof(*raw) ? 0 : -ENODATA;
}

/* Valid as a station, with the driver's strength for it. */
static int probe(struct fm_tuner *t, int khz, bool *valid, int *strength)
{
	struct fm_softmute_tune_t p = { 0 };
	int ret;

	p.freq = to_driver(khz);
	ret = call(t, FM_NR_SOFT_MUTE_TUNE, &p);

	if (ret < 0)
	{
		return ret;
	}

	*valid = p.valid != 0;
	*strength = p.rssi;
	return 0;
}

/*
 * PRE_SEARCH mutes the receiver and sets its search thresholds; RESTORE_SEARCH
 * undoes that. Both only fail on a receiver that is down, which is checked
 * before, so their results are only passed on, not acted on.
 */
static int search_begin(struct fm_tuner *t)
{
	int32_t unused = 0;

	return call(t, FM_NR_PRE_SEARCH, &unused);
}

static void search_end(struct fm_tuner *t, int back_to)
{
	int32_t unused = 0;

	(void)call(t, FM_NR_RESTORE_SEARCH, &unused);
	/* A soft-mute tune leaves the receiver on the channel it probed. */
	(void)fm_tuner_tune(t, back_to);
}

int fm_tuner_scan(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
                  struct fm_station *out, size_t *n)
{
	int home = t->khz;
	size_t found = 0;
	int ret;

	*n = 0;

	if (!t->powered)
	{
		return -ENODEV;
	}

	ret = search_begin(t);

	if (ret < 0)
	{
		return ret;
	}

	for (int khz = FM_BAND_MIN_KHZ; khz <= FM_BAND_MAX_KHZ; khz += FM_BAND_STEP_KHZ)
	{
		bool valid = false;
		int strength = 0;

		if (cancelled && cancelled(data))
		{
			ret = -ECANCELED;
			break;
		}

		/* One channel that fails to answer does not end the scan. */
		if (probe(t, khz, &valid, &strength) < 0 || !valid)
		{
			continue;
		}

		out[found].khz = khz;
		out[found].strength = strength;
		found++;
	}

	search_end(t, home);

	if (ret < 0)
	{
		return ret;
	}

	*n = fm_band_merge(out, found);
	return 0;
}

int fm_tuner_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data)
{
	int home = t->khz;
	int khz = home;
	int found = 0;
	int found_strength = 0;
	bool leaving = false;
	int ret;

	if (!t->powered)
	{
		return -ENODEV;
	}

	ret = search_begin(t);

	if (ret < 0)
	{
		return ret;
	}

	/*
	 * A station is heard on the channels next to it too, so from one that
	 * is on a station the first step would find the same station again
	 * (95.0 -> 95.1). Leave the current station's run first.
	 */
	{
		int strength;

		(void)probe(t, home, &leaving, &strength);
	}

	/* Round the band once, coming back to home last. */
	for (int i = 0; i < FM_BAND_CHANNELS; i++)
	{
		bool valid = false;
		int strength = 0;

		if (cancelled && cancelled(data))
		{
			found = 0;
			ret = -ECANCELED;
			break;
		}

		khz = fm_band_next(khz, dir);

		if (probe(t, khz, &valid, &strength) < 0)
		{
			valid = false;
		}

		if (leaving)
		{
			if (valid && khz != home)
			{
				continue;
			}

			leaving = false;
		}

		if (!valid)
		{
			/* Past the station's run: the best of it is the one. */
			if (found)
			{
				break;
			}

			continue;
		}

		/*
		 * Keep climbing while the next channel is valid and stronger, so the
		 * seek lands on the peak rather than on the edge it met first.
		 */
		if (found && strength <= found_strength)
		{
			break;
		}

		found = khz;
		found_strength = strength;

		if (khz == home)
		{
			break;
		}
	}

	/* Round the band and back on the station it started from: none other. */
	if (ret == 0 && (found == 0 || found == home))
	{
		found = 0;
		ret = -ENOENT;
	}

	search_end(t, found ? found : home);
	return ret;
}

int fm_tuner_find_name(struct fm_tuner *t, int khz, fm_cancel_fn cancelled, void *data,
                       struct fm_rds_info *out)
{
	int ret;

	*out = (struct fm_rds_info){ .name = "" };

	if (!t->powered)
	{
		return -ENODEV;
	}

	ret = fm_tuner_tune(t, khz);

	if (ret < 0)
	{
		return ret;
	}

	/* A tune may leave RDS as it was; on is on either way. */
	ret = fm_tuner_rds_enable(t, true);

	if (ret < 0)
	{
		return ret;
	}

	for (unsigned int waited = FM_NAME_POLL_MS; waited <= FM_NAME_WAIT_MS; waited += FM_NAME_POLL_MS)
	{
		struct fm_rds_raw raw;

		if (cancelled && cancelled(data))
		{
			return -ECANCELED;
		}

		t->sleep_fn(FM_NAME_POLL_MS);

		if (fm_tuner_rds_read(t, &raw) != 0)
		{
			continue;
		}

		fm_rds_decode(&raw, out);

		if (out->name[0])
		{
			return 0;
		}

		/* The tune cleared the record: nothing counted since is no RDS. */
		if (waited >= FM_NAME_SILENT_MS && raw.gc.total == 0)
		{
			return 0;
		}
	}

	return 0;
}
