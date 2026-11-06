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
 * The FM receiver, whichever driver it is behind. Two are known:
 *
 * - fm_mtk_ops: MediaTek's fmradio_drv on /dev/fm (fm_mtk.c), the MT66xx
 *   connectivity combo chips.
 * - fm_iris_ops: Qualcomm's radio-iris on /dev/radio0 (fm_iris.c), the FM
 *   part of the WCNSS (WCN36xx) Wi-Fi/Bluetooth chips, over SMD.
 *
 * What is the same for both lives here: the band checks, what the
 * receiver's state allows, and the station name search. A backend only does
 * what its driver needs.
 *
 * Every ioctl goes through ioctl_fn, and every other call to the outside
 * through its own hook, so the tests can stand in for the chip. All calls
 * block - a scan takes ten seconds or more - and none is safe to run from two
 * threads at once: the service keeps the tuner on one worker.
 */

/* Returns 0 or -errno, like a system call wrapper. */
typedef int (*fm_ioctl_fn)(int fd, unsigned long request, void *arg);

/* Like read(2), but returns the byte count or -errno. */
typedef long (*fm_read_fn)(int fd, void *buf, size_t len);

/* Writes value to the file at path (a sysfs parameter): 0 or -errno. */
typedef int (*fm_write_fn)(const char *path, const char *value);

/* Asked before each channel of a scan or seek: true stops it. */
typedef bool (*fm_cancel_fn)(void *data);

/* Waits ms milliseconds. */
typedef void (*fm_sleep_fn)(unsigned int ms);

struct fm_tuner;

/*
 * A backend. Each call is only made in the state it needs - the receiver up
 * for all but power_up, a descriptor open, a frequency in the band - so a
 * backend checks none of that itself.
 */
struct fm_tuner_ops
{
	const char *name;       /* "mtk", "iris" */
	const char *path;       /* the driver's node */
	int open_flags;
	/*
	 * module-droid-fm's arguments for the audio HAL that comes with this
	 * receiver (NULL: none). Qualcomm's HALs take FM with set_parameters,
	 * not as an audio patch.
	 */
	const char *audio_args;

	/* Before the node is opened (NULL: nothing to do). */
	void (*before_open)(struct fm_tuner *t);

	/* From down: up and tuned to khz, powered and khz set; down on failure. */
	int (*power_up)(struct fm_tuner *t, int khz);
	/* Down, powered cleared, whatever the driver says. */
	int (*power_down)(struct fm_tuner *t);
	int (*tune)(struct fm_tuner *t, int khz);
	int (*rssi)(struct fm_tuner *t, int *dbm);
	int (*rds_enable)(struct fm_tuner *t, bool on);
	/*
	 * The current station's name and RadioText so far, and how many RDS
	 * groups have come in since the tune (-1: the driver cannot tell).
	 */
	int (*rds_get)(struct fm_tuner *t, struct fm_rds_info *out, long *groups);
	int (*scan)(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
	            struct fm_station *out, size_t *n);
	int (*seek)(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data);
};

extern const struct fm_tuner_ops fm_mtk_ops;
extern const struct fm_tuner_ops fm_iris_ops;

/* The known backends in the order they are looked for, NULL-terminated. */
extern const struct fm_tuner_ops *const fm_tuner_backends[];

struct fm_tuner
{
	const struct fm_tuner_ops *ops;
	int fd;
	fm_ioctl_fn ioctl_fn;
	fm_read_fn read_fn;     /* read(2) unless a test sets its own */
	fm_write_fn write_fn;   /* a plain file write unless a test sets its own */
	fm_sleep_fn sleep_fn;   /* nanosleep(2) unless a test sets its own */
	bool powered;
	int khz;        /* tuned to, while powered */
	/* IRIS: what the driver handed over of this station's RDS so far. */
	struct fm_rds_info rds;
};

/* The backend whose driver node exists here, or NULL if none does. */
const struct fm_tuner_ops *fm_tuner_detect(void);

/* ioctl_fn NULL: the real ioctl(2). */
void fm_tuner_init(struct fm_tuner *t, const struct fm_tuner_ops *ops, fm_ioctl_fn ioctl_fn);

/* Opens path, or the backend's own node when path is NULL. */
int fm_tuner_open(struct fm_tuner *t, const char *path);

/* Powers the receiver down first if it is up. */
void fm_tuner_close(struct fm_tuner *t);

/* Up and tuned to khz; only tunes when it is up already. */
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
 * What the driver has of the current station's name and RadioText (each ""
 * while there is none). Does not block. groups, if not NULL: the RDS groups
 * received since the tune, or -1 when the driver cannot tell. -ENODATA if the
 * driver handed over nothing this time.
 */
int fm_tuner_rds_get(struct fm_tuner *t, struct fm_rds_info *out, long *groups);

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
 * How long a station gets to say its name. The drivers confirm one once
 * three receptions agree: 2 - 3 s on a good signal. A station with no RDS
 * group at all after FM_NAME_SILENT_MS sends none, and is not waited for -
 * where the driver counts groups.
 */
#define FM_NAME_POLL_MS    250
#define FM_NAME_WAIT_MS    5000
#define FM_NAME_SILENT_MS  1500

/*
 * Tunes to khz and waits there for the station's RDS name, into out (name ""
 * if it gave none in time). 0 once tuned, name or not; -ENODEV with the
 * receiver off, -ECANCELED once cancelled() says so, or the tune's error.
 * Stays tuned there.
 */
int fm_tuner_find_name(struct fm_tuner *t, int khz, fm_cancel_fn cancelled, void *data,
                       struct fm_rds_info *out);

#endif
