// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_TUNER_H
#define FM_TUNER_H

#include <stdbool.h>
#include <stddef.h>

#include "fm_band.h"
#include "fm_rds.h"

/*
 * The MediaTek FM receiver behind /dev/fm (fmradio_drv, the connectivity
 * driver of the MT66xx combo chips).
 *
 * Only the ioctls this driver generation still has: power up and down, tune,
 * RSSI, and the soft-mute tune Android's FM JNI searches with. FM_IOCTL_SEEK
 * and FM_IOCTL_SCAN are declared in the driver's header but have no case in
 * its ioctl handler and fail with EPERM, so seeking and scanning are done
 * here, a soft-mute tune per channel between PRE_SEARCH and RESTORE_SEARCH.
 *
 * Every ioctl goes through ioctl_fn, so the tests can stand in for the chip.
 * All calls block - a scan takes about ten seconds - and none is safe to run
 * from two threads at once: the service keeps the tuner on one worker.
 */

/* Returns 0 or -errno, like a system call wrapper. */
typedef int (*fm_ioctl_fn)(int fd, unsigned long request, void *arg);

/* Like read(2), but returns the byte count or -errno. */
typedef long (*fm_read_fn)(int fd, void *buf, size_t len);

/* Asked before each channel of a scan or seek: true stops it. */
typedef bool (*fm_cancel_fn)(void *data);

/* Waits ms milliseconds. */
typedef void (*fm_sleep_fn)(unsigned int ms);

struct fm_tuner
{
	int fd;
	fm_ioctl_fn ioctl_fn;
	fm_read_fn read_fn;     /* read(2) unless a test sets its own */
	fm_sleep_fn sleep_fn;   /* nanosleep(2) unless a test sets its own */
	bool powered;
	int khz;        /* tuned to, while powered */
};

/* The ioctl numbers, as the driver's fm_ioctl.h declares them. */
#define FM_NR_POWERUP         0
#define FM_NR_POWERDOWN       1
#define FM_NR_TUNE            2
#define FM_NR_GETRSSI         7
#define FM_NR_RDS_ONOFF       18
#define FM_NR_PRE_SEARCH      45
#define FM_NR_RESTORE_SEARCH  46
#define FM_NR_SOFT_MUTE_TUNE  63

/* The ioctl request for nr, encoded the way the driver's header does it. */
unsigned long fm_tuner_request(unsigned int nr);

/* The number back out of a request, or -1 if it is not one of ours. */
int fm_tuner_request_nr(unsigned long request);

/*
 * What the driver reads and writes. Frequencies are in its own unit, 10 kHz
 * (9500 = 95.00 MHz).
 */
struct fm_tune_parm
{
	unsigned char err;
	unsigned char band;
	unsigned char space;
	unsigned char hilo;
	unsigned char deemphasis;
	unsigned short freq;
};

struct fm_softmute_tune_t
{
	int rssi;
	unsigned short freq;
	int valid;
};

/* ioctl_fn NULL: the real ioctl(2). */
void fm_tuner_init(struct fm_tuner *t, fm_ioctl_fn ioctl_fn);

int fm_tuner_open(struct fm_tuner *t, const char *path);

/* Powers the receiver down first if it is up. */
void fm_tuner_close(struct fm_tuner *t);

int fm_tuner_power_up(struct fm_tuner *t, int khz);
int fm_tuner_power_down(struct fm_tuner *t);
int fm_tuner_tune(struct fm_tuner *t, int khz);

/* The current channel's RSSI in dBm. */
int fm_tuner_rssi(struct fm_tuner *t, int *dbm);

/*
 * RDS on or off. Powering down switches it off: it has to be switched on
 * again after each power up.
 */
int fm_tuner_rds_enable(struct fm_tuner *t, bool on);

/*
 * The driver's RDS record (fm_rds.h) for the current station. Does not
 * block: the driver hands over what it has. -ENODATA if it gave none.
 */
int fm_tuner_rds_read(struct fm_tuner *t, struct fm_rds_raw *raw);

/*
 * Every station in the band, by frequency, into out (FM_BAND_CHANNELS fit).
 * Stops early, with -ECANCELED, once cancelled() says so. Leaves the
 * receiver tuned where it was.
 */
int fm_tuner_scan(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
                  struct fm_station *out, size_t *n);

/*
 * The next station up (dir > 0) or down from the current channel, wrapping
 * round the band once, and tunes to it. -ENOENT if there is none, -ECANCELED
 * once cancelled() says so; either way the receiver goes back where it was.
 */
int fm_tuner_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data);

/*
 * How long a station gets to say its name. The driver confirms one once
 * three receptions agree: 2 - 3 s on a good signal. A station with no RDS
 * group at all after FM_NAME_SILENT_MS sends none, and is not waited for.
 */
#define FM_NAME_POLL_MS    250
#define FM_NAME_WAIT_MS    5000
#define FM_NAME_SILENT_MS  1500

/*
 * Tunes to khz and waits there for the station's confirmed RDS name, into
 * out (name "" if it gave none in time). 0 once tuned, name or not;
 * -ENODEV with the receiver off, -ECANCELED once cancelled() says so, or
 * the tune's error. Stays tuned there.
 */
int fm_tuner_find_name(struct fm_tuner *t, int khz, fm_cancel_fn cancelled, void *data,
                       struct fm_rds_info *out);

#endif
