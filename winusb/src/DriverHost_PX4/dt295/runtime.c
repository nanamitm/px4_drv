/* SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "mb86H/loader.h"
#include "mb86H/resident_image.h"
#include "mb86H/device_query.h"
#include "mb86H/stream_input.h"
#include "mb86H/usb/window.h"
#include <errno.h>
#include <stdlib.h>

struct dt295_runtime {
    struct dt295_io io;
    struct mb86H_loader loader;
    struct mb86H_loader_ops loader_ops;
    struct mb86H_session session;
    struct mb86H_source_descriptor sources[2];
    struct mb86H_stream_input input;
    unsigned source;
    unsigned rf_mask;
    const struct usb_native_window_profile *window;
    bool block_writes;
    int touched, started;
};
static int control(void *ctx, const struct usb_window_control *c, void *data, unsigned timeout)
{
    struct dt295_runtime *r = ctx;
    return r->io.control(r->io.opaque,c->request_type,c->request,c->value,c->index,data,c->length,timeout);
}
static const struct usb_native_ops native_ops = {control};
static int read_window(void *ctx,uint32_t address,void *data,uint16_t bytes)
{
    struct dt295_runtime *r=ctx;
    return usb_native_window(&native_ops,ctx,r->window,address,data,bytes,false,r->block_writes);
}
static int write_window(void *ctx,uint32_t address,const void *data,uint16_t bytes)
{
    struct dt295_runtime *r=ctx;
    return usb_native_window(&native_ops,ctx,r->window,address,(void *)data,bytes,true,r->block_writes);
}
static int exchange(void *ctx,const uint8_t request[64],uint8_t response[64])
{
    struct dt295_runtime *r=ctx;
    return usb_native_exchange(&native_ops,ctx,r->window,request,response,r->block_writes);
}
static int set_block_writes(void *ctx,bool enable) {((struct dt295_runtime *)ctx)->block_writes=enable;return 0;}
static uint32_t now(void *ctx) { struct dt295_runtime *r=ctx;return r->io.now_ms(r->io.opaque); }
static void delay(void *ctx,unsigned ms) { struct dt295_runtime *r=ctx;r->io.delay_ms(r->io.opaque,ms); }
static void observe(void *ctx,const uint8_t *request,const uint8_t *response,int rc)
{
    struct dt295_runtime *r=ctx;
    if(r->io.observe)r->io.observe(r->io.opaque,request,response,rc);
}
static void *allocate(void *ctx,size_t size) { (void)ctx;return malloc(size); }
static void release(void *ctx,void *ptr) { (void)ctx;free(ptr); }
static void mismatch(void *ctx,uint32_t address,uint32_t expected,uint32_t observed)
{
    struct dt295_runtime *r=ctx;
    if(r->io.mismatch)r->io.mismatch(r->io.opaque,address,expected,observed);
}
static int snapshot(void *ctx,const struct mb86H_loader *loader)
{
    struct dt295_runtime *r=ctx;
    return r->io.snapshot?mb86H_loader_snapshot_write(loader,r->io.snapshot,r->io.opaque):0;
}
static const struct mb86H_session_ops session_ops = {
    .exchange=exchange,.observe=observe,.now_ms=now,.delay_ms=delay
};
int dt295_validate(const uint8_t *data,size_t size)
{
    return dt295_validate_target(data,size,DT295_TARGET_PIX_DT295);
}
int dt295_validate_target(const uint8_t *data,size_t size,enum dt295_target target)
{
    struct mb86H_bootstrap boot;
    const uint8_t *resident;
    if(!data || size<64 || size>1024u*1024u || target>DT295_TARGET_TORNE || (int)target<0)return -EINVAL;
    int rc=mb86H_bootstrap_parse(&boot,data,size,target==DT295_TARGET_TORNE?MB86H_BOOTSTRAP_TORNE:MB86H_BOOTSTRAP_DT295,&resident);
    return rc?rc:mb86H_resident_image_validate(resident);
}
int dt295_create(struct dt295_runtime **out,const uint8_t *data,size_t size,const struct dt295_io *io)
{
    return dt295_create_target(out,data,size,io,DT295_TARGET_PIX_DT295);
}
int dt295_create_target(struct dt295_runtime **out,const uint8_t *data,size_t size,
    const struct dt295_io *io,enum dt295_target target)
{
    if(!out)return -EINVAL;
    *out=NULL;
    if(!io || !io->control || !io->now_ms || !io->delay_ms)return -EINVAL;
    int rc=dt295_validate_target(data,size,target);
    if(rc)return rc;
    struct dt295_runtime *r=calloc(1,sizeof(*r));
    if(!r)return -ENOMEM;
    r->io=*io;
    r->rf_mask=target==DT295_TARGET_TORNE?1u:3u;
    r->window=target==DT295_TARGET_TORNE?&usb_torne_window_profile:&usb_dt295_window_profile;
    if(target==DT295_TARGET_TORNE){r->session.tune_timeout_ms=70000;r->session.tune_poll_ms=1;}
    rc=mb86H_loader_parse_target(&r->loader,data,size,target==DT295_TARGET_TORNE?MB86H_BOOTSTRAP_TORNE:MB86H_BOOTSTRAP_DT295);
    if(rc){free(r);return rc;}
    r->loader_ops=(struct mb86H_loader_ops){.window_read=read_window,.window_write=write_window,
        .session=&r->session,.session_ops=&session_ops,.now_ms=now,.delay_ms=delay,
        .allocate=allocate,.release=release,.mismatch=mismatch,.block_writes=set_block_writes,.snapshot=snapshot};
    *out=r;return 0;
}
int dt295_start(struct dt295_runtime *r)
{
    struct mb86H_device_overview overview;
    if(!r || r->touched)return -EINVAL;
    r->touched=1;
    int rc=mb86H_loader_start(&r->loader,&r->loader_ops,r);
    if(!rc)rc=mb86H_session_prepare(&r->session,&session_ops,r,&overview);
    if(!rc && (overview.rf_mask!=r->rf_mask || overview.stream_mask!=r->rf_mask))rc=-EPROTO;
    for(unsigned i=0;!rc && i<2;++i) {
        if(!(r->rf_mask&(1u<<i)))continue;
        rc=mb86H_device_query_source(&r->session,&session_ops,r,i,&r->sources[i]);
        if(!rc && !(r->sources[i].flags&MB86H_DEVICE_QUERY_STREAM))rc=-EPROTO;
    }
    if(!rc)r->started=1;
    return rc;
}
int dt295_close(struct dt295_runtime *r)
{
    if(!r || !r->touched)return 0;
    int rc=mb86H_loader_close(&r->loader,&r->loader_ops,r);
    r->started=0;
    if(!rc)r->touched=0;
    return rc;
}
void dt295_destroy(struct dt295_runtime *r)
{
    if(!r)return;
    free(r->loader.resident_saved);free(r->loader.torne_boot0_saved);
    free(r->loader.torne_ts_ring_saved);free(r);
}
int dt295_tune(struct dt295_runtime *r,unsigned source,uint32_t frequency,uint32_t tsid)
{
    if(!r || !r->started || source>1 || !(r->rf_mask&(1u<<source)) || (!source && frequency>UINT32_MAX/1000u))return -EINVAL;
    const struct mb86H_source_descriptor *d=&r->sources[source];
    struct mb86H_dvb_caps caps={.frequency_min=d->frequency_min,.frequency_max=d->frequency_max,
        .present=1,.auto_tsid=!!(d->flags&MB86H_DEVICE_QUERY_AUTO_TSID)};
    struct mb86H_dvb_request request={.system=source?MB86H_DVB_BSCS:MB86H_DVB_GR,
        .frequency=source?frequency:frequency*1000u,.bandwidth_hz=source?0:6000000,
        .stream_id=source?tsid:UINT32_MAX,.layers=source?0:7};
    if(!mb86H_dvb_request_valid(&caps,&request))return -EINVAL;
    int rc=mb86H_session_tune(&r->session,&session_ops,r,source,&request,false);
    if(!rc)r->source=source;
    return rc;
}
int dt295_capture(struct dt295_runtime *r,int enable)
{
    if(!r || !r->started || !r->session.request_valid)return -EINVAL;
    int rc;
    if(enable) {
        const struct mb86H_source_descriptor *d=&r->sources[r->source];
        rc=mb86H_stream_input_init(&r->input,d->stream_format,r->source,d->record_size);
        if(rc)return rc;
    }
    rc=mb86H_session_stream(&r->session,&session_ops,r,r->source,!!enable);
    if(!rc && !enable)rc=mb86H_session_wait_stopped(&r->session,&session_ops,r);
    return rc;
}
int dt295_service(struct dt295_runtime *r)
{
    return r?mb86H_session_service(&r->session,&session_ops,r):-EINVAL;
}
int dt295_locked(struct dt295_runtime *r,int *out)
{
    if(!r || !out)return -EINVAL;
    bool locked=false;
    int rc=mb86H_session_locked(&r->session,&session_ops,r,r->source,&locked);
    *out=locked;return rc;
}
int dt295_feed(struct dt295_runtime *r,const uint8_t *data,size_t size,
    int (*packet)(void *,const uint8_t *),void *ctx)
{
    return r?mb86H_stream_input_push(&r->input,r->source,data,size,packet,ctx):-EINVAL;
}
uint32_t dt295_generation(const struct dt295_runtime *r) { return r?r->session.generation:0; }
void dt295_input_stats(const struct dt295_runtime *r,uint64_t *discarded,uint64_t *desyncs)
{
    if(discarded)*discarded=r?r->input.framer.discarded_bytes:0;
    if(desyncs)*desyncs=r?r->input.framer.desyncs:0;
}
