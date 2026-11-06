// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_MTK_H
#define FM_MTK_H

#include "fm_rds.h"
#include "fm_tuner.h"

/*
 * The MediaTek FM receiver behind /dev/fm (fmradio_drv, the connectivity
 * driver of the MT66xx combo chips): fm_mtk_ops.
 *
 * Only the ioctls this driver generation still has: power up and down, tune,
 * RSSI, and the soft-mute tune Android's FM JNI searches with. FM_IOCTL_SEEK
 * and FM_IOCTL_SCAN are declared in the driver's header but have no case in
 * its ioctl handler and fail with EPERM, so seeking and scanning are done
 * here, a soft-mute tune per channel between PRE_SEARCH and RESTORE_SEARCH.
 */

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

/*
 * The driver's RDS record (fm_rds.h) for the current station. Does not
 * block: the driver hands over what it has. -ENODATA if it gave none;
 * -ENODEV with the receiver down.
 */
int fm_mtk_rds_read(struct fm_tuner *t, struct fm_rds_raw *raw);

#endif
