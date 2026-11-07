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

int dtp_aux_step(volatile struct dtp_aux_mailbox *m, void (*read_status)(struct dtp_aux_i2c_status *, uint32_t operation))
{
    uint32_t sequence, operation, status;
    struct dtp_aux_i2c_status i2c = {0};
    if (m->magic != DTP_AUX_MAGIC || m->abi != DTP_AUX_ABI) return 0;
    m->heartbeat++;
    sequence = m->request_sequence;
    if (!sequence || sequence == m->response_sequence) return 0;
    operation = m->operation;
    publish();
    if (sequence != m->request_sequence) return 0;
    if (operation == DTP_AUX_PING || operation == DTP_AUX_CAPS) status = DTP_AUX_OK;
    else if ((operation == DTP_AUX_I2C_STATUS || operation == DTP_AUX_I2C_CLOCK_CHECK || operation == DTP_AUX_DEMOD_READ || operation == DTP_AUX_TRANSFER) && read_status) {
        read_status(&i2c, operation);
        status = DTP_AUX_OK;
    } else if (operation == DTP_AUX_I2C || operation == DTP_AUX_I2C_STATUS || operation == DTP_AUX_I2C_CLOCK_CHECK || operation == DTP_AUX_DEMOD_READ || operation == DTP_AUX_TRANSFER)
        status = DTP_AUX_NOT_READY;
    else status = DTP_AUX_UNSUPPORTED;
    /* I2Cコードを含むイメージでも、HAL/所有権/実時間タイマーの検証前は接続しない。 */
    m->features = 0;
    m->status = status;
    m->i2c = i2c;
    publish();
    m->response_sequence = sequence;
    return 1;
}

/* These registers are polled by the reference controller code. No I2C
 * address, controller reset or interrupt acknowledgement is written here. */
static uint32_t read32(uintptr_t address)
{
    return *(volatile const uint32_t *)address;
}

#define I2C_REG(offset) (*(volatile uint32_t *)(uintptr_t)(0x01684000u + (offset)))

static int wait_byte(struct dtp_aux_i2c_status *i2c)
{
    /* The loop bounds diagnostics even before a device timer is available. */
    for (unsigned remaining = 4096; remaining; --remaining) {
        if (I2C_REG(0x90) & I2C_REG(0xb0) & 1u) {
            i2c->state = I2C_REG(0xfe0);
            i2c->control = I2C_REG(0xfe4);
            i2c->irq_pending = I2C_REG(0x90);
            return !(i2c->control & 0x80u);
        }
    }
    i2c->state = I2C_REG(0xfe0);
    i2c->control = I2C_REG(0xfe4);
    i2c->irq_pending = I2C_REG(0x90);
    return 0;
}

#define TRANSFER_MAX 8u
#define TRANSFER_RAM ((volatile uint8_t *)(uintptr_t)(DTP_AUX_MAILBOX + 64u))

struct transfer {
    uint8_t address, tx_size, rx_size;
    uint8_t tx[TRANSFER_MAX];
    uint8_t rx[TRANSFER_MAX];
};

static void transfer_bytes(struct dtp_aux_i2c_status *i2c, struct transfer *transfer)
{
    i2c->config_check = 2; /* Initial address */
    I2C_REG(0xff0) = ((uint32_t)transfer->address << 1) | !transfer->tx_size;
    I2C_REG(0xfe4) |= 0x10u;
    if (!wait_byte(i2c) || (i2c->state & 0x10u)) return;

    for (unsigned n = 0; n < transfer->tx_size; ++n) {
        i2c->config_check = 3; /* Write data */
        I2C_REG(0xff0) = transfer->tx[n];
        I2C_REG(0xfe4) &= ~1u;
        if (!wait_byte(i2c) || (i2c->state & 0x10u)) return;
    }

    if (transfer->tx_size && transfer->rx_size) {
        i2c->config_check = 4; /* Repeated START */
        I2C_REG(0xff0) = ((uint32_t)transfer->address << 1) | 1u;
        I2C_REG(0xfe4) |= 0x20u;
        if (!wait_byte(i2c) || (i2c->state & 0x10u)) return;
    }

    for (unsigned n = 0; n < transfer->rx_size; ++n) {
        i2c->config_check = 5; /* Read data */
        if (n + 1 == transfer->rx_size) I2C_REG(0xfe4) &= ~8u;
        else I2C_REG(0xfe4) |= 8u;
        I2C_REG(0xfe4) &= ~1u;
        if (!wait_byte(i2c)) return;
        transfer->rx[n] = (uint8_t)I2C_REG(0xff0);
    }
    i2c->config_check = 6;
}
static void demod_checkpoint(uint32_t step)
{
    volatile struct dtp_aux_mailbox *m = (volatile struct dtp_aux_mailbox *)(uintptr_t)DTP_AUX_MAILBOX;
    m->i2c.config_check = step;
}

static void demod_read(struct dtp_aux_i2c_status *i2c, volatile uint16_t *config, volatile uint16_t *gate, struct transfer *transfer)
{
    static const uint16_t offsets[] = {0x10, 0x14, 0xc0, 0xc4, 0xb0, 0xff8, 0xff4, 0xfe8, 0xfe4};
    uint32_t saved[sizeof(offsets) / sizeof(offsets[0])];
    const unsigned count = sizeof(saved) / sizeof(saved[0]);

    if (i2c->system_config != 0x8000u || i2c->state || i2c->control ||
        i2c->irq_pending || i2c->irq_mask) return;
    volatile uint16_t *clock = (volatile uint16_t *)(uintptr_t)0x00f1010cu;
    uint16_t saved_clock = *clock;
    if (saved_clock != 1u) return;
    *clock = 0x000b;
    if (*clock != 0x000b) {
        *clock = saved_clock;
        i2c->config_check = 8;
        if (*clock != saved_clock) i2c->config_check |= 0x400u;
        return;
    }
    *config = 0x8020;
    i2c->config_check = 1;
    if (*config == 0x8020) {
        *gate = 0;
        demod_checkpoint(0x11);
        for (unsigned n = 0; n < count; ++n) saved[n] = I2C_REG(offsets[n]);
        i2c->state = saved[0];
        i2c->irq_pending = saved[1];
        I2C_REG(0x10) = saved[0] | 0x10u;
        I2C_REG(0x14) = saved[1] & ~0x10u;
        demod_checkpoint(0x13);
        I2C_REG(0xc0) = 1;
        demod_checkpoint(0x14);
        I2C_REG(0xc0) = 0;
        demod_checkpoint(0x15);
        I2C_REG(0xc4) = 0;
        I2C_REG(0xb0) = 1;
        I2C_REG(0xff8) = 6;
        I2C_REG(0xff4) = 0x10;
        I2C_REG(0xfe8) = 0x3c;
        I2C_REG(0xfe4) = 0x42;
        demod_checkpoint(0x16);

        i2c->control = I2C_REG(0xfe4);
        i2c->irq_mask = I2C_REG(0xb0);
        i2c->config_check = 7; /* Controller configuration did not latch. */
        if (i2c->control == 0x42 && i2c->irq_mask == 1) {
            transfer_bytes(i2c, transfer);
            I2C_REG(0xfe4) = 0x40;
            unsigned remaining = 4096;
            while ((I2C_REG(0xfe0) & 0x80u) && --remaining) {}
            if (!remaining) i2c->config_check |= 0x100u;
        }
        I2C_REG(0xfe8) = 0x3c;
        for (unsigned n = count; n; --n) I2C_REG(offsets[n - 1]) = saved[n - 1];
        demod_checkpoint(0x19);
        for (unsigned n = 0; n < count; ++n)
            if (I2C_REG(offsets[n]) != saved[n]) i2c->config_check |= 0x200u;
        if (*gate & 1u) {
            i2c->config_check |= 0x800u;
            return;
        }
    }
    *config = (uint16_t)i2c->system_config;
    *clock = saved_clock;
    if (*config != i2c->system_config || *clock != saved_clock) i2c->config_check |= 0x400u;
}

static void read_i2c_status(struct dtp_aux_i2c_status *i2c, uint32_t operation)
{
    struct transfer transfer = { .address = 0x10, .tx_size = 1, .rx_size = 1, .tx = {0x0e} };
    if (operation == DTP_AUX_TRANSFER) {
        transfer.address = TRANSFER_RAM[0];
        transfer.tx_size = TRANSFER_RAM[1];
        transfer.rx_size = TRANSFER_RAM[2];
        if ((transfer.address != 0x10 && transfer.address != 0x11) ||
            TRANSFER_RAM[3] || transfer.tx_size > TRANSFER_MAX || transfer.rx_size > TRANSFER_MAX ||
            (!transfer.tx_size && !transfer.rx_size)) {
            i2c->config_check = 9;
            return;
        }
        for (unsigned n = 0; n < transfer.tx_size; ++n) transfer.tx[n] = TRANSFER_RAM[4 + n];
        for (unsigned n = 0; n < TRANSFER_MAX; ++n) TRANSFER_RAM[12 + n] = 0;
    }
    i2c->state = read32(0x01684fe0u);
    i2c->control = read32(0x01684fe4u);
    i2c->irq_pending = read32(0x01684090u);
    i2c->irq_mask = read32(0x016840b0u);
    /* The reference reads this semaphore at halfword width and releases
     * it with a zero write only when that read returned bit 0 clear. */
    volatile uint16_t *gate = (volatile uint16_t *)(uintptr_t)0x00f10090u;
    volatile uint16_t *config = (volatile uint16_t *)(uintptr_t)0x00f10114u;
    i2c->system_gate = *gate;
    i2c->system_config = *config;
    if (i2c->system_gate & 1u) return;

    if (operation == DTP_AUX_DEMOD_READ || operation == DTP_AUX_TRANSFER) {
        demod_read(i2c, config, gate, &transfer);
        if (i2c->config_check == 6) {
            if (operation == DTP_AUX_DEMOD_READ) i2c->irq_mask = transfer.rx[0];
            else for (unsigned n = 0; n < transfer.rx_size; ++n) TRANSFER_RAM[12 + n] = transfer.rx[n];
        }
    }
    if (i2c->config_check & 0x800u) return;

    /* Restrict the reversible experiment to the measured cold state. */
    if (operation == DTP_AUX_I2C_CLOCK_CHECK && i2c->system_config == 0x8000u) {
        uint16_t requested = (uint16_t)((i2c->system_config & 0xffcfu) | 0x20u);
        *config = requested;
        uint16_t applied = *config;
        *config = (uint16_t)i2c->system_config;
        if (*config != i2c->system_config) i2c->config_check = 3;
        else i2c->config_check = applied == requested ? 1 : 2;
    }
    *gate = 0;
    if (!(*gate & 1u)) {
        *gate = 0;
        i2c->gate_reacquired = 1;
    }
}
void dtp_aux_main(void)
{
    volatile struct dtp_aux_mailbox *m = (volatile struct dtp_aux_mailbox *)(uintptr_t)DTP_AUX_MAILBOX;
    dtp_aux_init(m);
    for (;;) (void)dtp_aux_step(m, read_i2c_status);
}
