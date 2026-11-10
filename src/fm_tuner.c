// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_tuner.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

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

static int sys_write(const char *path, const char *value)
{
	/* A terminated constant of the backend ("0", "1"). */
	size_t len = strlen(value); /* Flawfinder: ignore */
	/* A fixed sysfs parameter of the driver: nothing to redirect. */
	int fd = open(path, O_WRONLY | O_CLOEXEC); /* Flawfinder: ignore */
	ssize_t n;
	int err;

	if (fd < 0)
	{
		return -errno;
	}

	n = write(fd, value, len);
	err = n < 0 ? -errno : (size_t)n == len ? 0 : -EIO;
	(void)close(fd);
	return err;
}

static void sys_sleep(unsigned int ms)
{
	struct timespec ts = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
	{
	}
}

const struct fm_tuner_ops *const fm_tuner_backends[] = { &fm_mtk_ops, &fm_iris_ops, &fm_helium_ops,
                                                         NULL };

const struct fm_tuner_ops *fm_tuner_detect(void)
{
	for (size_t i = 0; fm_tuner_backends[i]; i++)
	{
		/* Only whether the node is there: opening it is the worker's. */
		if (access(fm_tuner_backends[i]->path, F_OK) == 0) /* Flawfinder: ignore */
		{
			return fm_tuner_backends[i];
		}
	}

	return NULL;
}

void fm_tuner_init(struct fm_tuner *t, const struct fm_tuner_ops *ops, fm_ioctl_fn ioctl_fn)
{
	t->ops = ops;
	t->fd = -1;
	t->ioctl_fn = ioctl_fn ? ioctl_fn : sys_ioctl;
	t->read_fn = sys_read;
	t->write_fn = sys_write;
	t->sleep_fn = sys_sleep;
	t->powered = false;
	t->khz = 0;
	t->rds = (struct fm_rds_info){ .name = "" };
	t->hci_ops = NULL;
	t->hci = NULL;
	t->linked = false;
}

static bool is_open(const struct fm_tuner *t)
{
	return t->fd >= 0 || t->linked;
}

int fm_tuner_open(struct fm_tuner *t, const char *path)
{
	int fd;

	if (is_open(t))
	{
		return 0;
	}

	if (t->ops->before_open)
	{
		t->ops->before_open(t);
	}

	if (t->ops->open)
	{
		/* The backend's own hook, not open(2). */
		int ret = t->ops->open(t); /* Flawfinder: ignore */

		t->linked = ret == 0;
		return ret;
	}

	/* The driver's node, a fixed path: nothing to redirect. */
	fd = open(path ? path : t->ops->path, t->ops->open_flags | O_CLOEXEC); /* Flawfinder: ignore */

	if (fd < 0)
	{
		return -errno;
	}

	t->fd = fd;
	return 0;
}

void fm_tuner_close(struct fm_tuner *t)
{
	if (!is_open(t))
	{
		return;
	}

	if (t->powered)
	{
		(void)fm_tuner_power_down(t);
	}

	if (t->linked)
	{
		t->ops->close(t);
		t->linked = false;
		return;
	}

	(void)close(t->fd);
	t->fd = -1;
}

int fm_tuner_power_up(struct fm_tuner *t, int khz)
{
	if (!fm_band_valid(khz))
	{
		return -EINVAL;
	}

	if (t->powered)
	{
		return fm_tuner_tune(t, khz);
	}

	if (!is_open(t))
	{
		return -EBADF;
	}

	return t->ops->power_up(t, khz);
}

int fm_tuner_power_down(struct fm_tuner *t)
{
	if (!t->powered)
	{
		return 0;
	}

	return t->ops->power_down(t);
}

int fm_tuner_tune(struct fm_tuner *t, int khz)
{
	if (!fm_band_valid(khz))
	{
		return -EINVAL;
	}

	if (!t->powered)
	{
		return -ENODEV;
	}

	return t->ops->tune(t, khz);
}

int fm_tuner_rssi(struct fm_tuner *t, int *dbm)
{
	return t->powered ? t->ops->rssi(t, dbm) : -ENODEV;
}

int fm_tuner_rds_enable(struct fm_tuner *t, bool on)
{
	return t->powered ? t->ops->rds_enable(t, on) : -ENODEV;
}

int fm_tuner_rds_get(struct fm_tuner *t, struct fm_rds_info *out, long *groups)
{
	long unused;

	return t->powered ? t->ops->rds_get(t, out, groups ? groups : &unused) : -ENODEV;
}

int fm_tuner_scan(struct fm_tuner *t, fm_cancel_fn cancelled, void *data,
                  struct fm_station *out, size_t *n)
{
	*n = 0;
	return t->powered ? t->ops->scan(t, cancelled, data, out, n) : -ENODEV;
}

int fm_tuner_seek(struct fm_tuner *t, int dir, fm_cancel_fn cancelled, void *data)
{
	return t->powered ? t->ops->seek(t, dir, cancelled, data) : -ENODEV;
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
		long groups = -1;

		if (cancelled && cancelled(data))
		{
			return -ECANCELED;
		}

		t->sleep_fn(FM_NAME_POLL_MS);

		if (fm_tuner_rds_get(t, out, &groups) != 0)
		{
			continue;
		}

		if (out->name[0])
		{
			return 0;
		}

		/* Counted from the tune: nothing yet is no RDS. */
		if (waited >= FM_NAME_SILENT_MS && groups == 0)
		{
			return 0;
		}
	}

	return 0;
}
