/* SPDX-License-Identifier: MIT */
#ifndef MB86H_DEVICE_QUERY_WIRE_H
#define MB86H_DEVICE_QUERY_WIRE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif
#include "mb86H/protocol/constants.h"

/* Nine BE32 payload words after version/page/source/flags. Keep the shape
 * canonical here; no native C struct is sent over either bus. */
struct mb86H_device_overview {
    uint32_t rf_mask,stream_mask,resident_abi,window_version;
};
struct mb86H_source_descriptor {
    uint32_t frequency_min,frequency_max,frequency_step;
    uint32_t record_size,stream_format,window_records,layers; /* DVB layers: A=1,B=2,C=4 */
    uint8_t flags;
};
struct mb86H_source_state {
    uint32_t generation,phase,flags,capture_generation,window_sequence,tsid,capture_mask;
    int32_t error,owner_error;
};

static inline uint32_t mb86H_query_word(const uint8_t data[40],unsigned word)
{
    const uint8_t *p=data+4+word*4;
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static inline void mb86H_query_set_word(uint8_t data[40],unsigned word,uint32_t value)
{
    uint8_t *p=data+4+word*4;
    p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value;
}
static inline void mb86H_query_overview_encode(uint8_t d[40],const struct mb86H_device_overview *s)
{
    mb86H_query_set_word(d,0,s->rf_mask);mb86H_query_set_word(d,1,s->stream_mask);
    mb86H_query_set_word(d,2,s->resident_abi);mb86H_query_set_word(d,3,s->window_version);
}
static inline void mb86H_query_overview_decode(const uint8_t d[40],struct mb86H_device_overview *s)
{
    *s=(struct mb86H_device_overview){mb86H_query_word(d,0),mb86H_query_word(d,1),
                                    mb86H_query_word(d,2),mb86H_query_word(d,3)};
}
static inline void mb86H_query_source_encode(uint8_t d[40],const struct mb86H_source_descriptor *s)
{
    d[3]=s->flags;
    mb86H_query_set_word(d,0,s->frequency_min);mb86H_query_set_word(d,1,s->frequency_max);
    mb86H_query_set_word(d,2,s->frequency_step);mb86H_query_set_word(d,3,s->record_size);
    mb86H_query_set_word(d,4,s->stream_format);mb86H_query_set_word(d,5,s->window_records);
    mb86H_query_set_word(d,6,s->layers);
}
static inline void mb86H_query_source_decode(const uint8_t d[40],struct mb86H_source_descriptor *s)
{
    *s=(struct mb86H_source_descriptor){mb86H_query_word(d,0),mb86H_query_word(d,1),
        mb86H_query_word(d,2),mb86H_query_word(d,3),mb86H_query_word(d,4),
        mb86H_query_word(d,5),mb86H_query_word(d,6),d[3]};
}
static inline void mb86H_query_state_encode(uint8_t d[40],const struct mb86H_source_state *s)
{
    mb86H_query_set_word(d,0,s->generation);mb86H_query_set_word(d,1,(uint32_t)s->error);
    mb86H_query_set_word(d,2,(uint32_t)s->owner_error);mb86H_query_set_word(d,3,s->phase);
    mb86H_query_set_word(d,4,s->flags);mb86H_query_set_word(d,5,s->capture_generation);
    mb86H_query_set_word(d,6,s->window_sequence);mb86H_query_set_word(d,7,s->tsid);
    mb86H_query_set_word(d,8,s->capture_mask);
}
static inline void mb86H_query_state_decode(const uint8_t d[40],struct mb86H_source_state *s)
{
    *s=(struct mb86H_source_state){.generation=mb86H_query_word(d,0),
        .error=(int32_t)mb86H_query_word(d,1),.owner_error=(int32_t)mb86H_query_word(d,2),
        .phase=mb86H_query_word(d,3),.flags=mb86H_query_word(d,4),
        .capture_generation=mb86H_query_word(d,5),.window_sequence=mb86H_query_word(d,6),
        .tsid=mb86H_query_word(d,7),.capture_mask=mb86H_query_word(d,8)};
}
#endif
