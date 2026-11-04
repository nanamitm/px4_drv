/* SPDX-License-Identifier: MIT */
#ifndef MB86H_DVB_REQUEST_H
#define MB86H_DVB_REQUEST_H

#ifdef __KERNEL__
#include <linux/types.h>
#define MB86H_DVB_U32 u32
#else
#include <stdint.h>
#define MB86H_DVB_U32 uint32_t
#endif

#include "mb86H/protocol/constants.h"

/* Frequencies use DVB units: GR Hz, BSCS LNB IF kHz. */
struct mb86H_dvb_caps {
	MB86H_DVB_U32 frequency_min, frequency_max;
	unsigned int present, record_size, auto_tsid;
	unsigned int mux_channel;
};

struct mb86H_dvb_request {
	enum mb86H_dvb_system system;
	MB86H_DVB_U32 frequency, bandwidth_hz, stream_id, symbol_rate;
	unsigned int layers, partial_reception, sound_broadcasting;
};

extern const struct mb86H_dvb_caps mb86H_frontend_caps[MB86H_CHANNEL_COUNT];

/* Pure validation, no rounding, channel tables or hardware access. */
int mb86H_dvb_request_valid(const struct mb86H_dvb_caps *caps,
			  const struct mb86H_dvb_request *request);
/* Delivery-system profiles for one frontend; mux_channel is a FW source tag. */
int mb86H_dvb_caps_valid(const struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT]);

#undef MB86H_DVB_U32

#endif
