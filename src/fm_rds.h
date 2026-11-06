// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_RDS_H
#define FM_RDS_H

#include <stddef.h>
#include <stdint.h>

/*
 * RDS as the MediaTek driver hands it over: read(2) on /dev/fm gives one
 * struct rds_t (its inc/fm_rds.h), the driver's whole parser state. The
 * layout is copied field for field; it only has fixed-width members, so it is
 * the same on every word size, and its size is checked below.
 *
 * Of it, two fields are what the driver's parser confirmed:
 *
 * - PS_Data.PS[3]: the station's name (programme service, 8 characters),
 *   copied there once three receptions of it agree. PS[0..2] are its working
 *   copies and hold whatever arrived, errors and all. PS_ON is not this
 *   station's name: it is other networks' (group 14, EON).
 * - RT_Data.TextData[3], TextLength long: the last complete RadioText.
 *
 * Tuning clears the whole record, so neither outlives the station it came
 * from; powering down switches RDS off, and it has to be switched on again
 * after every power up.
 */

struct fm_rds_flag
{
	uint8_t tp, ta, music, stereo, artificial_head, compressed, dynamic_pty, text_ab;
	uint32_t flag_status;
};

struct fm_rds_ct
{
	uint16_t month, day, year, hour, minute;
	uint8_t local_time_offset_signbit, local_time_offset_half_hour;
};

struct fm_rds_af
{
	int16_t af_num;
	int16_t af[2][25];
	uint8_t addr_cnt, is_method_a, is_af_num_get;
};

struct fm_rds_ps
{
	uint8_t ps[4][8];
	uint8_t addr_cnt;
};

struct fm_rds_rt
{
	uint8_t text_data[4][64];
	uint8_t get_length, is_rt_display, text_length, is_type_a, buf_cnt;
	uint16_t addr_cnt;
};

struct fm_rds_group_cnt
{
	uint32_t total;
	uint32_t group_a[16];
	uint32_t group_b[16];
};

struct fm_rds_raw
{
	struct fm_rds_ct ct;
	struct fm_rds_flag flag;
	uint16_t pi;
	uint8_t switch_tp;
	uint8_t pty;
	struct fm_rds_af af_data;
	struct fm_rds_af afon_data;
	uint8_t radio_page_code;
	uint16_t program_item_number_code;
	uint8_t extend_country_code;
	uint16_t language_code;
	struct fm_rds_ps ps_data;
	uint8_t ps_on[8];
	struct fm_rds_rt rt_data;
	uint16_t event_status;
	struct fm_rds_group_cnt gc;
};

/* sizeof(struct rds_t) as the armv7 kernel of mindphone was built with it. */
#define FM_RDS_RAW_SIZE 688

/* Each RDS character is at most 3 bytes of UTF-8. */
#define FM_RDS_NAME_MAX (8 * 3 + 1)
#define FM_RDS_TEXT_MAX (64 * 3 + 1)

struct fm_rds_info
{
	char name[FM_RDS_NAME_MAX];     /* "" when none yet */
	char text[FM_RDS_TEXT_MAX];
};

/*
 * n bytes in the RDS character set (EN 50067 Annex E) as UTF-8, into out
 * (size bytes): stops at a carriage return (the end of a RadioText), and
 * leaves out the spaces at either end. Always terminated. Each character
 * needs room for four bytes while it is written, so whole characters stop
 * up to three bytes short of size; FM_RDS_NAME_MAX and FM_RDS_TEXT_MAX
 * always hold all 8 and 64.
 */
void fm_rds_to_utf8(const uint8_t *in, size_t n, char *out, size_t size);

/* What the driver confirmed of the station's name and RadioText. */
void fm_rds_decode(const struct fm_rds_raw *raw, struct fm_rds_info *out);

#endif
