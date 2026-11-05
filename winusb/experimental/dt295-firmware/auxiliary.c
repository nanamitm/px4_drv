/* SPDX-License-Identifier: GPL-2.0-only */
#include "auxiliary.h"

/* コンパイラーの並べ替えを防ぐだけで、CPU間のキャッシュ同期の代用ではない。 */
static void publish(void) { __asm__ volatile ("" ::: "memory"); }

void dtp_aux_init(volatile struct dtp_aux_mailbox *m)
{
    uint32_t i;
    volatile unsigned char *bytes = (volatile unsigned char *)m;
    for (i = 0; i < sizeof(*m); ++i) bytes[i] = 0;
    m->abi = DTP_AUX_ABI;
    publish();
    m->magic = DTP_AUX_MAGIC;
}

int dtp_aux_step(volatile struct dtp_aux_mailbox *m)
{
    uint32_t sequence, operation, status;
    if (m->magic != DTP_AUX_MAGIC || m->abi != DTP_AUX_ABI) return 0;
    m->heartbeat++;
    sequence = m->request_sequence;
    if (!sequence || sequence == m->response_sequence) return 0;
    operation = m->operation;
    publish();
    if (sequence != m->request_sequence) return 0;
    if (operation == DTP_AUX_PING || operation == DTP_AUX_CAPS) status = DTP_AUX_OK;
    else if (operation == DTP_AUX_I2C) status = DTP_AUX_NOT_READY;
    else status = DTP_AUX_UNSUPPORTED;
    /* I2Cコードを含むイメージでも、HAL/所有権/実時間タイマーの検証前は接続しない。 */
    m->features = 0;
    m->status = status;
    publish();
    m->response_sequence = sequence;
    return 1;
}

void dtp_aux_main(void)
{
    volatile struct dtp_aux_mailbox *m = (volatile struct dtp_aux_mailbox *)(uintptr_t)DTP_AUX_MAILBOX;
    dtp_aux_init(m);
    for (;;) (void)dtp_aux_step(m);
}
