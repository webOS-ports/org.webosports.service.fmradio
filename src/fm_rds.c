// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fm_rds.h"


_Static_assert(sizeof(struct fm_rds_raw) == FM_RDS_RAW_SIZE, "fm_rds_raw differs from the driver's rds_t");
_Static_assert(offsetof(struct fm_rds_raw, ps_data) == 248, "rds_t.PS_Data moved");
_Static_assert(offsetof(struct fm_rds_raw, rt_data) == 290, "rds_t.RT_Data moved");
_Static_assert(offsetof(struct fm_rds_raw, event_status) == 554, "rds_t.event_status moved");

/*
 * The RDS basic character set, EN 50067:1998 Annex E, as UTF-8. Taken from
 * redsea (src/text/rdsstring.cc, MIT licence, (c) Oona Raisanen), with the
 * control codes 0x00 - 0x1f as spaces and the soft hyphen (0x1f) as nothing.
 */
/* Each character's UTF-8: its length and its bytes. */
struct rds_char
{
	uint8_t len;
	char utf8[4];
};

static const struct rds_char rds_g0[256] =
{
	/* 0x00 */ { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " },
	/* 0x08 */ { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " },
	/* 0x10 */ { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " },
	/* 0x18 */ { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 1, " " }, { 0, "" },
	/* 0x20 */ { 1, " " }, { 1, "!" }, { 1, "\x22" }, { 1, "#" }, { 2, "\xc2\xa4" }, { 1, "%" }, { 1, "&" }, { 1, "'" },
	/* 0x28 */ { 1, "(" }, { 1, ")" }, { 1, "*" }, { 1, "+" }, { 1, "," }, { 1, "-" }, { 1, "." }, { 1, "/" },
	/* 0x30 */ { 1, "0" }, { 1, "1" }, { 1, "2" }, { 1, "3" }, { 1, "4" }, { 1, "5" }, { 1, "6" }, { 1, "7" },
	/* 0x38 */ { 1, "8" }, { 1, "9" }, { 1, ":" }, { 1, ";" }, { 1, "<" }, { 1, "=" }, { 1, ">" }, { 1, "\x3f" },
	/* 0x40 */ { 1, "@" }, { 1, "A" }, { 1, "B" }, { 1, "C" }, { 1, "D" }, { 1, "E" }, { 1, "F" }, { 1, "G" },
	/* 0x48 */ { 1, "H" }, { 1, "I" }, { 1, "J" }, { 1, "K" }, { 1, "L" }, { 1, "M" }, { 1, "N" }, { 1, "O" },
	/* 0x50 */ { 1, "P" }, { 1, "Q" }, { 1, "R" }, { 1, "S" }, { 1, "T" }, { 1, "U" }, { 1, "V" }, { 1, "W" },
	/* 0x58 */ { 1, "X" }, { 1, "Y" }, { 1, "Z" }, { 1, "[" }, { 1, "\x5c" }, { 1, "]" }, { 3, "\xe2\x80\x95" }, { 1, "_" },
	/* 0x60 */ { 3, "\xe2\x80\x96" }, { 1, "a" }, { 1, "b" }, { 1, "c" }, { 1, "d" }, { 1, "e" }, { 1, "f" }, { 1, "g" },
	/* 0x68 */ { 1, "h" }, { 1, "i" }, { 1, "j" }, { 1, "k" }, { 1, "l" }, { 1, "m" }, { 1, "n" }, { 1, "o" },
	/* 0x70 */ { 1, "p" }, { 1, "q" }, { 1, "r" }, { 1, "s" }, { 1, "t" }, { 1, "u" }, { 1, "v" }, { 1, "w" },
	/* 0x78 */ { 1, "x" }, { 1, "y" }, { 1, "z" }, { 1, "{" }, { 1, "|" }, { 1, "}" }, { 2, "\xc2\xaf" }, { 1, " " },
	/* 0x80 */ { 2, "\xc3\xa1" }, { 2, "\xc3\xa0" }, { 2, "\xc3\xa9" }, { 2, "\xc3\xa8" }, { 2, "\xc3\xad" }, { 2, "\xc3\xac" }, { 2, "\xc3\xb3" }, { 2, "\xc3\xb2" },
	/* 0x88 */ { 2, "\xc3\xba" }, { 2, "\xc3\xb9" }, { 2, "\xc3\x91" }, { 2, "\xc3\x87" }, { 2, "\xc5\x9e" }, { 2, "\xce\xb2" }, { 2, "\xc2\xa1" }, { 2, "\xc4\xb2" },
	/* 0x90 */ { 2, "\xc3\xa2" }, { 2, "\xc3\xa4" }, { 2, "\xc3\xaa" }, { 2, "\xc3\xab" }, { 2, "\xc3\xae" }, { 2, "\xc3\xaf" }, { 2, "\xc3\xb4" }, { 2, "\xc3\xb6" },
	/* 0x98 */ { 2, "\xc3\xbb" }, { 2, "\xc3\xbc" }, { 2, "\xc3\xb1" }, { 2, "\xc3\xa7" }, { 2, "\xc5\x9f" }, { 2, "\xc7\xa7" }, { 2, "\xc4\xb1" }, { 2, "\xc4\xb3" },
	/* 0xa0 */ { 2, "\xc2\xaa" }, { 2, "\xce\xb1" }, { 2, "\xc2\xa9" }, { 3, "\xe2\x80\xb0" }, { 2, "\xc7\xa6" }, { 2, "\xc4\x9b" }, { 2, "\xc5\x88" }, { 2, "\xc5\x91" },
	/* 0xa8 */ { 2, "\xcf\x80" }, { 3, "\xe2\x82\xac" }, { 2, "\xc2\xa3" }, { 1, "$" }, { 3, "\xe2\x86\x90" }, { 3, "\xe2\x86\x91" }, { 3, "\xe2\x86\x92" }, { 3, "\xe2\x86\x93" },
	/* 0xb0 */ { 2, "\xc2\xba" }, { 2, "\xc2\xb9" }, { 2, "\xc2\xb2" }, { 2, "\xc2\xb3" }, { 2, "\xc2\xb1" }, { 2, "\xc4\xb0" }, { 2, "\xc5\x84" }, { 2, "\xc5\xb1" },
	/* 0xb8 */ { 2, "\xc2\xb5" }, { 2, "\xc2\xbf" }, { 2, "\xc3\xb7" }, { 2, "\xc2\xb0" }, { 2, "\xc2\xbc" }, { 2, "\xc2\xbd" }, { 2, "\xc2\xbe" }, { 2, "\xc2\xa7" },
	/* 0xc0 */ { 2, "\xc3\x81" }, { 2, "\xc3\x80" }, { 2, "\xc3\x89" }, { 2, "\xc3\x88" }, { 2, "\xc3\x8d" }, { 2, "\xc3\x8c" }, { 2, "\xc3\x93" }, { 2, "\xc3\x92" },
	/* 0xc8 */ { 2, "\xc3\x9a" }, { 2, "\xc3\x99" }, { 2, "\xc5\x98" }, { 2, "\xc4\x8c" }, { 2, "\xc5\xa0" }, { 2, "\xc5\xbd" }, { 2, "\xc3\x90" }, { 2, "\xc4\xbf" },
	/* 0xd0 */ { 2, "\xc3\x82" }, { 2, "\xc3\x84" }, { 2, "\xc3\x8a" }, { 2, "\xc3\x8b" }, { 2, "\xc3\x8e" }, { 2, "\xc3\x8f" }, { 2, "\xc3\x94" }, { 2, "\xc3\x96" },
	/* 0xd8 */ { 2, "\xc3\x9b" }, { 2, "\xc3\x9c" }, { 2, "\xc5\x99" }, { 2, "\xc4\x8d" }, { 2, "\xc5\xa1" }, { 2, "\xc5\xbe" }, { 2, "\xc4\x91" }, { 2, "\xc5\x80" },
	/* 0xe0 */ { 2, "\xc3\x83" }, { 2, "\xc3\x85" }, { 2, "\xc3\x86" }, { 2, "\xc5\x92" }, { 2, "\xc5\xb7" }, { 2, "\xc3\x9d" }, { 2, "\xc3\x95" }, { 2, "\xc3\x98" },
	/* 0xe8 */ { 2, "\xc3\x9e" }, { 2, "\xc5\x8a" }, { 2, "\xc5\x94" }, { 2, "\xc4\x86" }, { 2, "\xc5\x9a" }, { 2, "\xc5\xb9" }, { 2, "\xc5\xa6" }, { 2, "\xc3\xb0" },
	/* 0xf0 */ { 2, "\xc3\xa3" }, { 2, "\xc3\xa5" }, { 2, "\xc3\xa6" }, { 2, "\xc5\x93" }, { 2, "\xc5\xb5" }, { 2, "\xc3\xbd" }, { 2, "\xc3\xb5" }, { 2, "\xc3\xb8" },
	/* 0xf8 */ { 2, "\xc3\xbe" }, { 2, "\xc5\x8b" }, { 2, "\xc5\x95" }, { 2, "\xc4\x87" }, { 2, "\xc5\x9b" }, { 2, "\xc5\xba" }, { 2, "\xc5\xa7" }, { 1, " " },
};

#define RDS_END_OF_TEXT 0x0d

/* Shown as a space (or as nothing): not kept at either end. */
static int blank(uint8_t c)
{
	return rds_g0[c].len == 0 || (rds_g0[c].len == 1 && rds_g0[c].utf8[0] == ' ');
}

void fm_rds_to_utf8(const uint8_t *in, size_t n, char *out, size_t size)
{
	size_t first = 0;
	size_t last = 0;
	size_t len = 0;

	if (size == 0)
	{
		return;
	}

	/* The text: up to a carriage return, without the blanks round it. */
	while (last < n && in[last] != RDS_END_OF_TEXT)
	{
		last++;
	}

	while (first < last && blank(in[first]))
	{
		first++;
	}

	while (last > first && blank(in[last - 1]))
	{
		last--;
	}

	/*
	 * Each character writes all four of its bytes (zero after its own) and
	 * moves on by its length, so there must be room for four: whole
	 * characters only, and the terminator always fits. FM_RDS_NAME_MAX and
	 * FM_RDS_TEXT_MAX (3 per character, plus 1) hold every one.
	 */
	for (size_t i = first; i < last && size - len >= sizeof(rds_g0[0].utf8); i++)
	{
		const struct rds_char *ch = &rds_g0[in[i]];

		out[len] = ch->utf8[0];
		out[len + 1] = ch->utf8[1];
		out[len + 2] = ch->utf8[2];
		out[len + 3] = ch->utf8[3];
		len += ch->len;
	}

	out[len] = '\0';
}

void fm_rds_decode(const struct fm_rds_raw *raw, struct fm_rds_info *out)
{
	size_t text_len = raw->rt_data.text_length;

	if (text_len > sizeof(raw->rt_data.text_data[3]))
	{
		text_len = sizeof(raw->rt_data.text_data[3]);
	}

	fm_rds_to_utf8(raw->ps_data.ps[3], sizeof(raw->ps_data.ps[3]), out->name, sizeof(out->name));
	fm_rds_to_utf8(raw->rt_data.text_data[3], text_len, out->text, sizeof(out->text));
}
