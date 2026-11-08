// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#ifndef FM_HELIUM_H
#define FM_HELIUM_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fm_tuner.h"

/*
 * Qualcomm's FM receiver in the WCN3990-class Wi-Fi/Bluetooth chips
 * ("cherokee": SDM660, SDM845, ...): fm_helium_ops.
 *
 * There is no kernel driver for it. The chip takes FM commands as HCI
 * packets on the Bluetooth UART, and the vendor's Bluetooth HAL process
 * passes them on through vendor.qti.hardware.fm@1.0::IFmHci (fm_helium_hidl.c).
 * Qualcomm's own user of it is "helium" (vendor/qcom/opensource/fm-commonsys:
 * helium/radio_helium_hal*.c, fm_hci/fm_hci.cpp).
 *
 * The commands and events are the ones radio-iris sends over SMD to the
 * older WCNSS chips, so the numbers and layouts here are from the kernel's
 * include/uapi/media/radio-iris.h (msm-4.9); where helium differs, that is
 * said where it matters.
 *
 * - A command is opcode (OGF << 10 | OCF, little-endian), parameter length,
 *   parameters; an event is code, length, parameters. No packet type byte.
 * - Each command is answered by Command Complete (credits, opcode, then the
 *   return parameters, status first) or Command Status (status, credits,
 *   opcode). One command at a time: the chip gives one credit.
 * - A tune or a search is accepted with Command Status and reported later:
 *   Tune Status with the new channel, Search Complete at the end.
 * - RDS comes as the chip decoded it: Program Service and Radio Text events,
 *   each a whole name or text.
 * - The audio leaves the chip on a SLIMbus port that is opened with its own
 *   command: Qualcomm's FM app does so before it routes FM audio.
 */

/* OGF */
#define FM_HCI_OGF_RECV    0x13
#define FM_HCI_OGF_COMMON  0x15
#define FM_HCI_OGF_STATUS  0x16
#define FM_HCI_OGF_DIAG    0x3f

#define FM_HCI_OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | ((ocf) & 0x3ff)))

#define FM_HCI_ENABLE_RECV     FM_HCI_OP(FM_HCI_OGF_RECV, 0x01)
#define FM_HCI_DISABLE_RECV    FM_HCI_OP(FM_HCI_OGF_RECV, 0x02)
#define FM_HCI_SET_RECV_CONF   FM_HCI_OP(FM_HCI_OGF_RECV, 0x04)
#define FM_HCI_SET_MUTE_MODE   FM_HCI_OP(FM_HCI_OGF_RECV, 0x05)
#define FM_HCI_SET_STEREO_MODE FM_HCI_OP(FM_HCI_OGF_RECV, 0x06)
#define FM_HCI_SET_ANTENNA     FM_HCI_OP(FM_HCI_OGF_RECV, 0x07)
#define FM_HCI_GET_STATION     FM_HCI_OP(FM_HCI_OGF_RECV, 0x0a)
#define FM_HCI_SEARCH          FM_HCI_OP(FM_HCI_OGF_RECV, 0x0e)
#define FM_HCI_CANCEL_SEARCH   FM_HCI_OP(FM_HCI_OGF_RECV, 0x11)
#define FM_HCI_RDS_GRP_PROCESS FM_HCI_OP(FM_HCI_OGF_RECV, 0x13)
#define FM_HCI_SET_EVENT_MASK  FM_HCI_OP(FM_HCI_OGF_RECV, 0x16)
#define FM_HCI_TUNE            FM_HCI_OP(FM_HCI_OGF_COMMON, 0x01)
#define FM_HCI_READ_GRP_CNTRS  FM_HCI_OP(FM_HCI_OGF_STATUS, 0x01)
#define FM_HCI_ENABLE_SLIMBUS  FM_HCI_OP(FM_HCI_OGF_DIAG, 0x0e)

/* Events */
#define FM_HCI_EV_TUNE_STATUS      0x01
#define FM_HCI_EV_PROGRAM_SERVICE  0x09
#define FM_HCI_EV_RADIO_TEXT       0x0a
#define FM_HCI_EV_CMD_COMPLETE     0x0f
#define FM_HCI_EV_CMD_STATUS       0x10
#define FM_HCI_EV_SEARCH_COMPLETE  0x12
#define FM_HCI_EV_HW_ERR           0x1a

/* Command header: opcode (2), length (1); event header: code, length. */
#define FM_HCI_CMD_HEADER  3
#define FM_HCI_EV_HEADER   2
#define FM_HCI_PARAM_MAX   255

/*
 * Command Complete: credits, opcode (2), then the return parameters;
 * Command Status: status, credits, opcode (2).
 */
#define FM_HCI_CC_RETURN   3
#define FM_HCI_CS_LEN      4

/*
 * Tune Status, and the station parameters (Get Station Parameters' return,
 * which has the status where the event has its sub-event): frequency in kHz
 * (le32) at 1, RSSI (signed dBm) at 6.
 */
#define FM_HCI_STN_FREQ    1
#define FM_HCI_STN_RSSI    6
#define FM_HCI_STN_LEN     7

/*
 * Program Service: PI (2), PTY, TA, TA announcement, M/S, DI, the number of
 * names at 7, the names from 8 (8 characters each). Radio Text: PI (2),
 * PTY, TA, A/B at 4, the text from 5, up to 64 characters or a CR.
 */
#define FM_HCI_PS_COUNT    7
#define FM_HCI_PS_NAMES    8
#define FM_HCI_PS_LEN      8
#define FM_HCI_RT_TEXT     5
#define FM_HCI_RT_MAX      64

/*
 * The RDS group counters' return: status, then 9 le32 counts, the total of
 * groups second.
 */
#define FM_HCI_CNTRS_GROUPS 5
#define FM_HCI_CNTRS_LEN    37

/* Set Receiver Configuration: emphasis, spacing, RDS standard, HLSI,
 * band low and high in kHz (le32): 12 bytes. */
#define FM_HCI_RECV_CONF_LEN 12
#define FM_HCI_EMP50         1
#define FM_HCI_SPACE_100K    1
/* RDS on and off as Qualcomm's own FM library switches it (libfmjni's
 * EnableRDS and DisableRDS: RDSON 1 and 2, the RDS standard in the
 * configuration). */
#define FM_HCI_RDS_ON        1
#define FM_HCI_RDS_OFF       2

/* What radio-iris' initialise_recv sets after the receiver is on. */
#define FM_HCI_SOFT_MUTE_ON  1
#define FM_HCI_EVENT_MASK    0x07       /* signal level, RDS sync, audio */
/* Stereo: auto (0), signal blending, interference blending, MOST switch. */
#define FM_HCI_STEREO_AUTO   0

/* The chip decodes RadioText (1) and station names (2). */
#define FM_HCI_RDS_RT_PS     0x3

/* Search: mode 0 is seek; direction 0 up, 1 down. */
#define FM_HCI_SEEK          0
#define FM_HCI_DIR_UP        0
#define FM_HCI_DIR_DOWN      1

#define FM_HCI_ANT_WIRED     0

/* How long the chip gets. */
#define FM_HELIUM_POLL_MS       20
#define FM_HELIUM_CMD_WAIT_MS   3000    /* fm_hci's HCI_TIMEOUT */
#define FM_HELIUM_TUNE_WAIT_MS  1000
#define FM_HELIUM_SEEK_WAIT_MS  20000
/* A seek over by then stopped where it began, without searching. */
#define FM_HELIUM_SEEK_AT_ONCE_MS 1000

/* The detection mark: the vendor ships the IFmHci implementation. */
#define FM_HELIUM_IMPL "/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so"

struct fm_hci;

/* How the HCI packets reach the chip. */
struct fm_hci_ops
{
	/* Connects; the chip's events then go to fm_hci_event(). 0 or -errno. */
	int (*open)(struct fm_hci *h);
	void (*close)(struct fm_hci *h);
	/* One command packet, header and parameters: 0 or -errno. */
	int (*send)(struct fm_hci *h, const uint8_t *pkt, size_t len);
};

struct fm_hci
{
	const struct fm_hci_ops *ops;
	void *link;             /* the transport's own */
	GAsyncQueue *events;    /* GBytes, each an event as the chip sent it */

	/* What the chip reported. */
	bool tuned;             /* a Tune Status since this was cleared */
	int tuned_khz;
	bool search_done;       /* a Search Complete since this was cleared */
	unsigned int hw_errors; /* Hardware Error events: logged at power down */
};

/* The HIDL transport (fm_helium_hidl.c). */
extern const struct fm_hci_ops fm_hci_hidl_ops;

/* Over ops, nothing queued. Freed with fm_hci_free. */
struct fm_hci *fm_hci_new(const struct fm_hci_ops *ops, void *link);
void fm_hci_free(struct fm_hci *h);

/* Queues an event from the chip, n bytes with its header; from any thread. */
void fm_hci_event(struct fm_hci *h, const uint8_t *ev, size_t n);

/*
 * The station name in a Program Service event's parameters (n bytes), as
 * UTF-8 into name (size bytes): the first of its names. Returns whether
 * there was one that is not blank; name is left as it was if not.
 */
bool fm_helium_parse_ps(const uint8_t *p, size_t n, char *name, size_t size);

/* The same for a Radio Text event: the text up to the CR. */
bool fm_helium_parse_rt(const uint8_t *p, size_t n, char *text, size_t size);

#endif
