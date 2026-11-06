// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_IRIS_H
#define FM_IRIS_H

#include <stddef.h>
#include <stdint.h>

#include "fm_tuner.h"

/*
 * Qualcomm's FM receiver behind /dev/radio0 (radio-iris, the FM part of the
 * WCNSS Wi-Fi/Bluetooth chips, e.g. msm8953's WCN3660/3680): fm_iris_ops.
 *
 * V4L2 with the driver's private controls. Numbers and layouts are from the
 * kernel's include/uapi/media/radio-iris.h and radio-iris-commands.h and
 * drivers/media/radio/radio-iris.c (msm-4.9).
 *
 * - The driver talks to the chip over an SMD channel to WCNSS, which only
 *   the fmsmd_set module parameter opens (Android's init does it on
 *   hw.fm.init), and which takes a moment to come up. Closing the node closes
 *   it again, so it is opened every time the node is.
 * - Frequencies are kHz * 16 (TUNE_PARAM). A tune returns before the chip
 *   has retuned; G_FREQUENCY reads back what the chip reported last.
 * - Seeking is the chip's own (VIDIOC_S_HW_FREQ_SEEK), and returns at once;
 *   the SRCHON control reads 1 until it is done. The chip counts the channel
 *   it starts on, and up from 108.0 it does not move at all.
 * - A scan is the chip's list of strong stations (SRCHMODE 2), at most 20 a
 *   time, which the driver queues as a buffer and leaves SRCHON at 1 for.
 * - RDS comes as the chip decoded it: station names and RadioText, each a
 *   byte stream of records read with VIDIOC_DQBUF, which does not block for
 *   these. A tune empties both.
 */

#define FM_IRIS_SMD_PARAM "/sys/module/radio_iris_transport/parameters/fmsmd_set"

#define FM_IRIS_CID_BASE           0x08000000
#define FM_IRIS_CID_SRCHMODE       (FM_IRIS_CID_BASE + 1)
#define FM_IRIS_CID_SRCHON         (FM_IRIS_CID_BASE + 3)
#define FM_IRIS_CID_STATE          (FM_IRIS_CID_BASE + 4)
#define FM_IRIS_CID_SRCH_CNT       (FM_IRIS_CID_BASE + 11)
#define FM_IRIS_CID_EMPHASIS       (FM_IRIS_CID_BASE + 12)
#define FM_IRIS_CID_SPACING        (FM_IRIS_CID_BASE + 14)
#define FM_IRIS_CID_RDSON          (FM_IRIS_CID_BASE + 15)
#define FM_IRIS_CID_RDSGROUP_PROC  (FM_IRIS_CID_BASE + 16)
#define FM_IRIS_CID_ANTENNA        (FM_IRIS_CID_BASE + 18)
#define FM_IRIS_CID_RDS_GRP_CNTRS  (FM_IRIS_CID_BASE + 39)

/* STATE */
#define FM_IRIS_OFF   0
#define FM_IRIS_RECV  1

/* SRCHMODE: seek to the next station; list the strong ones (at most 20). */
#define FM_IRIS_SEEK         0
#define FM_IRIS_LIST_STRONG  2
#define FM_IRIS_LIST_MAX     20
/* Reads of lists left over before a search, at most (a list is 41 bytes). */
#define FM_IRIS_LIST_DRAIN   16

/* EMPHASIS 1: 50 us; SPACING 1: 100 kHz; ANTENNA 0: the wired one. */
#define FM_IRIS_EMP50       1
#define FM_IRIS_SPACE_100K  1
#define FM_IRIS_ANT_WIRED   0

/* RDSGROUP_PROC: the chip decodes RadioText (1) and station names (2). */
#define FM_IRIS_RDS_RT_PS   0x3

/* The driver's buffers, by VIDIOC_DQBUF index. */
#define FM_IRIS_BUF_LIST     0
#define FM_IRIS_BUF_RT       2
#define FM_IRIS_BUF_PS       3
#define FM_IRIS_BUF_CNTRS    8
#define FM_IRIS_BUF_SIZE     256

/* Frequency unit: 1/16 kHz. */
#define FM_IRIS_TUNE_PARAM   16

/* Each RDS record starts with 5 bytes: count or length, PTY, PI, flag. */
#define FM_IRIS_RDS_HEADER   5
#define FM_IRIS_PS_LEN       8
#define FM_IRIS_RT_MAX       64

/* RDS group counters: 9 little-endian 32-bit counts; total groups second. */
#define FM_IRIS_CNTRS_LEN     36
#define FM_IRIS_CNTRS_GROUPS  4

/* How long the chip gets. */
#define FM_IRIS_POLL_MS       20
#define FM_IRIS_TUNE_WAIT_MS  1000
#define FM_IRIS_SEEK_WAIT_MS  20000
#define FM_IRIS_LIST_POLL_MS  100
#define FM_IRIS_LIST_WAIT_MS  30000
/* A seek over by then stopped where it began, without searching (tissot:
 * 0.05 - 0.4 s; one round the band takes 7 - 9 s). */
#define FM_IRIS_SEEK_AT_ONCE_MS 1000
/* Passes of the 20-station list it takes to cover a full band. */
#define FM_IRIS_LIST_PASSES   ((FM_BAND_CHANNELS + FM_IRIS_LIST_MAX - 1) / FM_IRIS_LIST_MAX)
#define FM_IRIS_LINK_OPEN_MS  200
#define FM_IRIS_LINK_DOWN_MS  1000
#define FM_IRIS_LINK_UP_MS    2000

/*
 * The last station name in n bytes of the PS stream, as UTF-8 into name
 * (size bytes). Each record: header, then count names of 8 characters; the
 * first is taken. Stops at the first record that does not fit. Returns
 * whether it found one that is not blank; name is left as it was if not.
 * Cut to size as fm_rds_to_utf8 cuts (whole characters only).
 */
bool fm_iris_parse_ps(const uint8_t *buf, size_t n, char *name, size_t size);

/* The same for the RadioText stream: header (length first), then the text. */
bool fm_iris_parse_rt(const uint8_t *buf, size_t n, char *text, size_t size);

/*
 * The chip's station list in n bytes - a count, then each station's offset
 * from the bottom of the band in 50 kHz, high byte first - into khz (room
 * for max), as channels of the band, in frequency order, each once.
 * Returns how many.
 */
size_t fm_iris_parse_list(const uint8_t *buf, size_t n, int *khz, size_t max);

#endif
