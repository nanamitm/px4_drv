/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DTP_AUXILIARY_H
#define DTP_AUXILIARY_H
#include <stdint.h>

/* DTI2との誤接続を検出できる、診断用の別ABI。共有RAMでは各wordはBE。 */
#define DTP_AUX_MAGIC 0x50414631u /* PAF1 */
#define DTP_AUX_ABI 1u
#define DTP_AUX_MAILBOX 0x000a3900u
enum { DTP_AUX_PING = 1, DTP_AUX_CAPS = 2, DTP_AUX_I2C = 3, DTP_AUX_I2C_STATUS = 4, DTP_AUX_I2C_CLOCK_CHECK = 5, DTP_AUX_DEMOD_READ = 6, DTP_AUX_TRANSFER = 7 };
enum { DTP_AUX_OK, DTP_AUX_UNSUPPORTED, DTP_AUX_NOT_READY };
struct dtp_aux_i2c_status {
    uint32_t state, control, irq_pending, irq_mask, system_gate, system_config, gate_reacquired, config_check;
};
struct dtp_aux_mailbox {
    uint32_t magic, abi, heartbeat, request_sequence, response_sequence;
    uint32_t operation, status, features;
    struct dtp_aux_i2c_status i2c;
};
_Static_assert(sizeof(struct dtp_aux_mailbox) == 64, "auxiliary mailbox ABI size");
void dtp_aux_init(volatile struct dtp_aux_mailbox *);
int dtp_aux_step(volatile struct dtp_aux_mailbox *, void (*read_status)(struct dtp_aux_i2c_status *, uint32_t operation));
void dtp_aux_main(void);
#endif
