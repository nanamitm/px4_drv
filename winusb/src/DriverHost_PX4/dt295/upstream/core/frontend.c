/* SPDX-License-Identifier: MIT */
#include "mb86H/frontend.h"

int mb86H_dvb_caps_valid(const struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT])
{
	unsigned int i, count = 0;
	if (!caps)
		return 0;
	for (i = 0; i < MB86H_CHANNEL_COUNT; i++) {
		if (!caps[i].present)
			continue;
		count++;
		if (!caps[i].frequency_min ||
		    caps[i].frequency_min > caps[i].frequency_max ||
		    caps[i].mux_channel != i ||
		    (caps[i].record_size != MB86H_DVB_TS188 &&
		     caps[i].record_size != MB86H_DVB_TS192))
			return 0;
	}
	return count > 0;
}

int mb86H_dvb_request_valid(const struct mb86H_dvb_caps *caps,
			  const struct mb86H_dvb_request *r)
{
	if (!caps || !r || !caps->present || !caps->frequency_min ||
	    caps->frequency_min > caps->frequency_max ||
	    r->frequency < caps->frequency_min || r->frequency > caps->frequency_max)
		return 0;
	if (r->system == MB86H_DVB_GR)
		return (!r->bandwidth_hz || r->bandwidth_hz == 6000000) &&
		       !r->partial_reception && !r->sound_broadcasting &&
		       !(r->layers & ~7u) && r->stream_id == (uint32_t)-1;
	if (r->system != MB86H_DVB_BSCS)
		return 0;
	return r->stream_id <= 0xffffu ||
	       (r->stream_id == (uint32_t)-1 && caps->auto_tsid);
}

const struct mb86H_dvb_caps mb86H_frontend_caps[MB86H_CHANNEL_COUNT] = {
	{ .frequency_min = 473142857, .frequency_max = 707143000,
	  .present = 1, .record_size = MB86H_DVB_TS188,
	  .mux_channel = 0 },
	{ .frequency_min = 950000, .frequency_max = 2150000,
	  .present = 1, .record_size = MB86H_DVB_TS188,
	  .auto_tsid = 1, .mux_channel = 1 },
};
