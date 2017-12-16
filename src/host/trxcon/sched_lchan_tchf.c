/*
 * OsmocomBB <-> SDR connection bridge
 * TDMA scheduler: handlers for DL / UL bursts on logical channels
 *
 * (C) 2017 by Vadim Yanitskiy <axilirator@gmail.com>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <errno.h>
#include <string.h>
#include <talloc.h>
#include <stdint.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/bits.h>

#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/protocol/gsm_08_58.h>
#include <osmocom/gsm/gsm_utils.h>

#include <osmocom/coding/gsm0503_coding.h>
#include <osmocom/codec/codec.h>

#include "l1ctl_proto.h"
#include "scheduler.h"
#include "sched_trx.h"
#include "logging.h"
#include "trx_if.h"
#include "trxcon.h"
#include "l1ctl.h"

int rx_tchf_fn(struct trx_instance *trx, struct trx_ts *ts,
	struct trx_lchan_state *lchan, uint32_t fn, uint8_t bid,
	sbit_t *bits, int8_t rssi, float toa)
{
	const struct trx_lchan_desc *lchan_desc;
	uint8_t rsl_cmode, tch_mode, mode;
	int n_errors, n_bits_total, rc;
	sbit_t *buffer, *offset;
	uint8_t l2[128], *mask;
	uint32_t *first_fn;
	size_t l2_len;

	/* Set up pointers */
	lchan_desc = &trx_lchan_desc[lchan->type];
	first_fn = &lchan->rx_first_fn;
	mask = &lchan->rx_burst_mask;
	buffer = lchan->rx_bursts;

	LOGP(DSCHD, LOGL_DEBUG, "Traffic received on %s: fn=%u ts=%u bid=%u\n",
		lchan_desc->name, fn, ts->index, bid);

	/* Reset internal state */
	if (bid == 0) {
		/* Clean up old measurements */
		memset(&lchan->meas, 0x00, sizeof(lchan->meas));

		*first_fn = fn;
		*mask = 0x00;
	}

	/* Update mask */
	*mask |= (1 << bid);

	/* Update mask and RSSI */
	lchan->meas.rssi_sum += rssi;
	lchan->meas.toa_sum += toa;
	lchan->meas.rssi_num++;
	lchan->meas.toa_num++;

	/* Copy burst to end of buffer of 8 bursts */
	offset = buffer + bid * 116 + 464;
	memcpy(offset, bits + 3, 58);
	memcpy(offset + 58, bits + 87, 58);

	/* Wait until complete set of bursts */
	if (bid != 3)
		return 0;

	/* Check for complete set of bursts */
	if ((*mask & 0xf) != 0xf) {
		LOGP(DSCHD, LOGL_DEBUG, "Received incomplete traffic frame at "
			"fn=%u (%u/%u) for %s\n", *first_fn,
			(*first_fn) % ts->mf_layout->period,
			ts->mf_layout->period,
			lchan_desc->name);
		return -EINVAL;
	}

	/**
	 * Get current RSL / TCH modes
	 *
	 * FIXME: we do support speech only, and
	 * CSD support may be implemented latter.
	 */
	rsl_cmode = RSL_CMOD_SPD_SPEECH;
	tch_mode = lchan->tch_mode;

	mode = rsl_cmode != RSL_CMOD_SPD_SPEECH ?
		GSM48_CMODE_SPEECH_V1 : tch_mode;

	switch (mode) {
	case GSM48_CMODE_SIGN:
	case GSM48_CMODE_SPEECH_V1: /* FR */
		rc = gsm0503_tch_fr_decode(l2, buffer,
			1, 0, &n_errors, &n_bits_total);
		break;
	case GSM48_CMODE_SPEECH_EFR: /* EFR */
		rc = gsm0503_tch_fr_decode(l2, buffer,
			1, 1, &n_errors, &n_bits_total);
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/**
		 * TODO: AMR requires a dedicated loop,
		 * which will be implemented later...
		 */
		LOGP(DSCHD, LOGL_ERROR, "AMR isn't supported yet\n");
		return -ENOTSUP;
	default:
		LOGP(DSCHD, LOGL_ERROR, "Invalid TCH mode: %u\n", tch_mode);
		return -EINVAL;
	}

	/* Shift buffer by 4 bursts for interleaving */
	memcpy(buffer, buffer + 464, 464);

	/* Check decoding result */
	if (rc < 4) {
		LOGP(DSCHD, LOGL_DEBUG, "Received bad TCH frame ending at "
			"fn=%u for %s\n", fn, lchan_desc->name);

		l2_len = sched_bad_frame_ind(l2, rsl_cmode, tch_mode);
	} else if (rc == GSM_MACBLOCK_LEN) {
		/* FACCH received, forward it to the higher layers */
		sched_send_data_ind(trx, ts, lchan,
			l2, GSM_MACBLOCK_LEN, false, n_errors);

		/* Send BFI instead of stolen TCH frame */
		l2_len = sched_bad_frame_ind(l2, rsl_cmode, tch_mode);
	} else {
		/* A good TCH frame received */
		l2_len = rc;
	}

	/* Send a traffic frame to the higher layers */
	if (l2_len > 0)
		sched_send_data_ind(trx, ts, lchan,
			l2, l2_len, false, n_errors);

	return 0;
}

int tx_tchf_fn(struct trx_instance *trx, struct trx_ts *ts,
	struct trx_lchan_state *lchan, uint32_t fn, uint8_t bid)
{
	const struct trx_lchan_desc *lchan_desc;
	struct trx_ts_prim *prim;
	ubit_t burst[GSM_BURST_LEN];
	ubit_t *buffer, *offset;
	uint8_t *mask, *l2;
	const uint8_t *tsc;
	size_t l2_len;
	int rc;

	/* Set up pointers */
	lchan_desc = &trx_lchan_desc[lchan->type];
	mask = &lchan->tx_burst_mask;
	buffer = lchan->tx_bursts;

	/* If we have encoded bursts */
	if (*mask)
		goto send_burst;

	/* Wait until a first burst in period */
	if (bid > 0)
		return 0;

	/* Check the current TCH mode */
	switch (lchan->tch_mode) {
	case GSM48_CMODE_SIGN:
	case GSM48_CMODE_SPEECH_V1: /* FR */
		l2_len = GSM_FR_BYTES;
		break;
	case GSM48_CMODE_SPEECH_EFR: /* EFR */
		l2_len = GSM_EFR_BYTES;
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/**
		 * TODO: AMR requires a dedicated loop,
		 * which will be implemented later...
		 */
		/* TODO: drop prim here */
		LOGP(DSCHD, LOGL_ERROR, "AMR isn't supported yet\n");
		return -ENOTSUP;
	default:
		/* TODO: drop prim here */
		LOGP(DSCHD, LOGL_ERROR, "Invalid TCH mode: %u\n",
			lchan->tch_mode);
		return -EINVAL;
	}

	/* Get a message from TX queue */
	prim = sched_dequeue_tch_prim(&ts->tx_prims);
	l2 = (uint8_t *) prim->payload;

	/* Determine payload length */
	if (prim->payload_len == GSM_MACBLOCK_LEN)
		l2_len = GSM_MACBLOCK_LEN;

	/* Shift buffer by 4 bursts back for interleaving */
	memcpy(buffer, buffer + 464, 464);

	/* Encode payload */
	rc = gsm0503_tch_fr_encode(buffer, l2, l2_len, 1);
	if (rc) {
		LOGP(DSCHD, LOGL_ERROR, "Failed to encode L2 payload\n");

		/* Remove primitive from queue and free memory */
		llist_del(&prim->list);
		talloc_free(prim);

		return -EINVAL;
	}

send_burst:
	/* Determine which burst should be sent */
	offset = buffer + bid * 116;

	/* Update mask */
	*mask |= (1 << bid);

	/* Choose proper TSC */
	tsc = sched_nb_training_bits[trx->tsc];

	/* Compose a new burst */
	memset(burst, 0, 3); /* TB */
	memcpy(burst + 3, offset, 58); /* Payload 1/2 */
	memcpy(burst + 61, tsc, 26); /* TSC */
	memcpy(burst + 87, offset + 58, 58); /* Payload 2/2 */
	memset(burst + 145, 0, 3); /* TB */

	LOGP(DSCHD, LOGL_DEBUG, "Transmitting %s fn=%u ts=%u burst=%u\n",
		lchan_desc->name, fn, ts->index, bid);

	/* Send burst to transceiver */
	rc = trx_if_tx_burst(trx, ts->index, fn, trx->tx_power, burst);
	if (rc) {
		LOGP(DSCHD, LOGL_ERROR, "Could not send burst to transceiver\n");

		/* Remove primitive from queue and free memory */
		prim = llist_entry(ts->tx_prims.next, struct trx_ts_prim, list);
		llist_del(&prim->list);
		talloc_free(prim);

		/* Reset mask */
		*mask = 0x00;

		return rc;
	}

	/* If we have sent the last (4/4) burst */
	if (*mask == 0x0f) {
		/* Get pointer to a prim which was sent */
		prim = llist_entry(ts->tx_prims.next, struct trx_ts_prim, list);

		/* Confirm data / traffic sending */
		sched_send_data_conf(trx, ts, lchan, fn, prim->payload_len);

		/* Remove primitive from queue and free memory */
		llist_del(&prim->list);
		talloc_free(prim);

		/* Reset mask */
		*mask = 0x00;
	}

	return 0;
}