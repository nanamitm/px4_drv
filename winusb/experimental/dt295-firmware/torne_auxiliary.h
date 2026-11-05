/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DTP_TORNE_AUXILIARY_H
#define DTP_TORNE_AUXILIARY_H
#include "torne_hal.h"
#define DTP_TA_MAGIC 0x50414632u /* PAF2: PAF1と別の実験ABI */
#define DTP_TA_ABI 2u
#define DTP_TA_ADDRESS 0x000a3900u
#define DTP_TA_CLOCK_POLLS 0x40000u
enum { DTP_TA_PING=1, DTP_TA_CAPS=2, DTP_TA_I2C=3, DTP_TA_PREPARE=4, DTP_TA_QUIESCE=5, DTP_TA_CLOCK_WAIT=6 };
enum { DTP_TA_OK, DTP_TA_UNSUPPORTED, DTP_TA_NOT_READY, DTP_TA_BAD_REQUEST,
    DTP_TA_TRANSFER_FAILED, DTP_TA_CLOCK_STALLED, DTP_TA_REVOKED };
enum { DTP_TA_FEATURE_I2C=1, DTP_TA_FEATURE_HOST_CLOCK=2, DTP_TA_FEATURE_DEVICE_CLOCK=4 };
struct dtp_torne_mailbox {
    uint32_t magic,abi,heartbeat,request_sequence,response_sequence,operation,status,features;
    uint32_t host_us,lease,request_lease,address,tx_size,rx_size,timeout_us,result;
    uint8_t tx[40],rx[40];
    uint32_t tx_completed,rx_completed,state,control,timer_ticks,timer_us,reserved[6];
};
#ifdef __cplusplus
static_assert(sizeof(dtp_torne_mailbox)==192,"PAF2 mailbox size");
#else
_Static_assert(sizeof(struct dtp_torne_mailbox)==192,"PAF2 mailbox size");
#endif
struct dtp_torne_aux {
    struct dtp_torne_hal hal;
    volatile struct dtp_torne_mailbox *mailbox;
    uint32_t sequence,token,last_us,clock_polls,clock_feature,wait_started,wait_budget;
    uint8_t clock_seen,pending,revoked;
};
void dtp_torne_aux_init(struct dtp_torne_aux *, volatile struct dtp_torne_mailbox *, const struct dtp_torne_bus *,uint32_t clock_feature);
void dtp_torne_aux_step(struct dtp_torne_aux *);
#endif
