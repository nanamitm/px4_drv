/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

#ifdef XZ_BOOTSTRAP
#define XZ_I2C_INIT_ONLY
#endif
#include "i2c_transfer.h"

#define PIN_DIRECTION 0x00f10014u
#define PIN_OUTPUT 0x00f10018u

#define I2C_CONFIG_FF4 0x00fd8ff4u
#define I2C_CONFIG_FF8 0x00fd8ff8u
#define I2C_BLOCK_ENABLE 0x00fd8010u
#define I2C_BLOCK_RESET 0x00fd8014u
#define I2C_PORT_C0 0x00fd80c0u
#define I2C_PORT_C4 0x00fd80c4u
#define I2C_DIVIDER_XZ 0x3cu
/*
 * GV-MVP/XZ3の実機では、0x3cを書いてCONTROLへ0x40を書いた後のDIVIDERが
 * 0xbcとして読める。bit 7の意味は未確定のため、書いた分周値の範囲だけを比べる。
 */
#define I2C_DIVIDER_MASK 0x7fu

enum { XZ_PROBE = 1, XZ_BATCH, XZ_INIT, XZ_CLOCKS, XZ_POWER, XZ_CLOCK_SETUP, XZ_TRANSFER };

/*
 * 表でbit 18を立てる各ブロックの0x404 (常駐部の0x803b0000と同じ順)。
 * CLOCKSでは読み出しの安全性を分けて確認するため別ページにする。
 */
static const uint32_t block_clock_regs[] = {
    0x014d0404u, 0x010d0404u, 0x012d0404u, 0x013d0404u, 0x00e70404u,
    0x00c70404u, 0x00d00404u, 0x01600404u, 0x01700404u,
};

/*
 * チューナー側の端子を、公開mAgicVISIONファームウェアのXZ常駐部と同じ順序で
 * 駆動する。0x00f10014は出力許可、0x00f10018は出力値と推定している。
 * 手順0〜3は初期化表 (firmware.binの0x1ac20〜0x1ac6f)、手順4と5は状態機械
 * 0x80391140の状態2と状態3に対応する。表と状態機械は各手順の後に待機を
 * 挟むため、1回の要求で1手順だけを実行し、待機はホストが要求の間で行う。
 * USB処理の中で長く待つと、制御転送がホストのタイムアウトを超えるため。
 */
#define XZ_POWER_STEPS 6u

/*
 * 公開mAgicVISIONファームウェアのXZ常駐部の状態1と同じクロック設定。
 * 0x80383840をモード0、フラグ1で呼んだときの順序と値 (表0x803b0024) に続けて、
 * 状態1が書く0xf1010c = 0x8201を適用する。GV-MVP/XZ3ではこの設定がないと
 * I²Cのバイト転送が完了しなかった。値は (旧値 & 保持マスク) | 値で、
 * 書くたびに16ビットで読み戻して照合し、不一致で中止する。
 */
static const struct {
    uint32_t address;
    uint16_t keep;
    uint16_t value;
} clock_setup_table[] = {
    {0x00f100f0u, 0x0000u, 0x0001u},
    {0x00f1010cu, 0x0001u, 0x0000u},
    {0x00f10114u, 0x0000u, 0x8008u},
    {0x00f10118u, 0x0000u, 0xffffu},
    {0x00f1011cu, 0x0000u, 0x0006u},
    {0x00f10120u, 0x1080u, 0x0100u},
    {0x00f10124u, 0x0000u, 0x128au},
    {0x00f10128u, 0x0e0eu, 0x6100u},
    {0x00f1012cu, 0x21f8u, 0x4007u},
    {0x00f1010cu, 0x0000u, 0x8201u},
};

/* 戻り値は0が成功、それ以外は照合が一致しなかった項目番号+1。 */
static unsigned clock_setup(void)
{
    for (unsigned i = 0; i < sizeof(clock_setup_table) / sizeof(*clock_setup_table); ++i) {
        const uint32_t address = clock_setup_table[i].address;
        uint16_t old = XZ_MMIO_READ16(address);
        if (i == 4) {
            /* 常駐部は0xf1011cのbit 8と9を先に解除してから値を書く。 */
            old &= (uint16_t)~0x300u;
            XZ_MMIO_WRITE16(address, old);
        }
        const uint16_t value = (uint16_t)((old & clock_setup_table[i].keep) | clock_setup_table[i].value);
        XZ_MMIO_WRITE16(address, value);
        if (XZ_MMIO_READ16(address) != value)
            return i + 1;
        if (i == 3) {
            /* 各ブロックの0x404のbit 18。先頭6ブロックは常駐部と同じく16ビットで上位側に4を立てる。 */
            for (unsigned b = 0; b < sizeof(block_clock_regs) / sizeof(*block_clock_regs); ++b) {
                const uint32_t block = block_clock_regs[b];
                if (b < 6)
                    XZ_MMIO_WRITE16(block, (uint16_t)(XZ_MMIO_READ16(block) | 4u));
                else
                    XZ_I2C_WRITE(block, XZ_I2C_READ(block) | 0x00040000u);
            }
        }
    }
    return 0;
}

static void power_step(unsigned step)
{
    uint16_t output = XZ_MMIO_READ16(PIN_OUTPUT);
    switch (step) {
    case 0:
        XZ_MMIO_WRITE16(PIN_DIRECTION, (uint16_t)(XZ_MMIO_READ16(PIN_DIRECTION) | 0xd0f4u));
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)((output & 0xff1bu) | 0x04u));
        break;
    case 1:
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)((output & 0xff1bu) | 0x44u));
        break;
    case 2:
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)((output & 0xff1bu) | 0xc4u));
        break;
    case 3:
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)((output & 0xff1bu) | 0xe4u));
        break;
    case 4:
        /* bit 0を出力値0のまま出力に切り替え、bit 6を1にする。 */
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)((output & 0xffbeu) | 0x40u));
        XZ_MMIO_WRITE16(PIN_DIRECTION, (uint16_t)(XZ_MMIO_READ16(PIN_DIRECTION) | 0x41u));
        break;
    default:
        XZ_MMIO_WRITE16(PIN_OUTPUT, (uint16_t)(output | 0x41u));
        break;
    }
}

#define XZ_MAGIC 0x585a4931u
#define XZ_REPLY 0x80000000u

#ifndef XZ_BOOTSTRAP
/*
 * XZ初期化表のクロックと端子の項目が対象とするレジスター。ホストのUSB
 * ウィンドウから読めないため、boot0での値をここから返す。表の書き込みと同じ幅で
 * 読み、書き込みは行わない。
 */
static const uint32_t clock_regs16[] = {
    0x00f1010cu, 0x00f10114u, 0x00f10118u, 0x00f1011cu, 0x00f10120u,
    0x00f10124u, 0x00f10128u, 0x00f1012cu, 0x00f10014u, 0x00f10018u,
};
/* 0x00f10004は常駐ファームウェアが表の条件付き項目 (0xf100f0) の要否判定に32ビットで読む。 */
static const uint32_t clock_regs32[] = { 0x00f10004u, I2C_BLOCK_ENABLE, I2C_BLOCK_RESET };
static unsigned validate(const uint8_t *payload, unsigned size)
{
    unsigned reads = 0;
    if (!size || size > 40)
        return XZ_INVALID;
    for (unsigned pos = 0; pos < size;) {
        if (size - pos < 3)
            return XZ_INVALID;
        unsigned address = payload[pos++], tx = payload[pos++], rx = payload[pos++];
        if (address < 0x08 || address > 0x77 || tx > 8 || rx > 8 ||
            (!tx && !rx) || tx > size - pos || rx > 40 - reads)
            return XZ_INVALID;
        pos += tx;
        reads += rx;
    }
    return XZ_OK;
}

#endif

/*
 * XZのUSBウィンドウは0x000fffffまでしか届かず、ホストから0x00fdxxxxの
 * MMIOを書けない。このため、公開mAgicVISIONファームウェアのXZ初期化表
 * (firmware.binの0x1acc0から0x1ad4f) にあるI²Cブロックの項目だけを、表と
 * 同じ順序と値でここから適用する。表の前半にあるクロック、端子、電源の項目は
 * USB動作中のboot0の状態を変えるため対象外。0xfd8ff4はtorneの8と異なり、
 * XZの表では0x10。
 */
static unsigned initialize(void)
{
    XZ_I2C_WRITE(I2C_BLOCK_ENABLE, XZ_I2C_READ(I2C_BLOCK_ENABLE) | 0x10u);
    XZ_I2C_WRITE(I2C_BLOCK_RESET, XZ_I2C_READ(I2C_BLOCK_RESET) & ~0x10u);
    XZ_I2C_WRITE(I2C_PORT_C0, 1);
    XZ_I2C_WRITE(I2C_PORT_C0, 0);
    XZ_I2C_WRITE(I2C_PORT_C4, 0);
    XZ_I2C_WRITE(I2C_CONFIG_FF8, 6);
    XZ_I2C_WRITE(I2C_CONFIG_FF4, 0x10);
    XZ_I2C_WRITE(I2C_DIVIDER, I2C_DIVIDER_XZ);
    XZ_I2C_WRITE(I2C_CONTROL, 0x40);
    return wait_idle();
}

#ifdef XZ_BOOTSTRAP
/* 初期化専用。本体は初回選局の前にI²C・TS共通版へ置き換わる。 */
void xz_i2c_relay(uint32_t packet[16])
{
    if (packet[0] != XZ_MAGIC || (packet[2] & XZ_REPLY))
        return;

    const unsigned op = packet[2], size = packet[3];
    const uint8_t step = ((const uint8_t *)(packet + 6))[0];
    unsigned error = XZ_INVALID;
    if (!packet[4] && !packet[5]) {
        if (op == XZ_INIT && !size)
            error = initialize();
        if (op == XZ_POWER && size == 1 && step < XZ_POWER_STEPS) {
            power_step(step);
            error = XZ_OK;
        }
        if (op == XZ_CLOCK_SETUP && !size) {
            error = XZ_NOT_READY;
            if ((XZ_MMIO_READ16(0x00f10004u) & 0x0c00u) == 0x0800u)
                error = clock_setup() ? XZ_VERIFY_FAILED : XZ_OK;
        }
    }
    packet[2] |= XZ_REPLY;
    packet[3] = error;
}
#else
static void snapshot(uint32_t packet[16])
{
    for (unsigned i = 6; i < 16; ++i)
        packet[i] = 0;
    packet[6] = XZ_I2C_READ(I2C_STATUS);
    packet[7] = XZ_I2C_READ(I2C_CONTROL);
    packet[8] = XZ_I2C_READ(I2C_DIVIDER);
}

void xz_i2c_relay(uint32_t packet[16])
{
    if (packet[0] != XZ_MAGIC || (packet[2] & XZ_REPLY))
        return;

    uint8_t *payload = (uint8_t *)&packet[6];
    uint8_t read[40];
    unsigned error = XZ_INVALID, count = 0, completed = 0;
    unsigned size = packet[3];
    if (packet[2] == XZ_TRANSFER) {
        const unsigned address = packet[4], rx = packet[5];
        /* 初期化の連続書き込みは、途中でSTOPを発行せず1転送で送る。 */
        if (address >= 0x08 && address <= 0x77 && size <= 40 &&
            rx <= 40 - size && (size || rx)) {
            error = XZ_NOT_READY;
            if ((XZ_I2C_READ(I2C_DIVIDER) & I2C_DIVIDER_MASK) == I2C_DIVIDER_XZ)
                error = transfer(address, payload, size, read, rx);
            if (!error) {
                completed = 1;
                count = rx;
            }
        }
        goto data_reply;
    }
    if (!packet[4] && !packet[5]) {
        if ((packet[2] == XZ_PROBE || packet[2] == XZ_INIT) && !size) {
            /* PROBEはSTARTを発行せず、レジスターの状態だけを返す。 */
            error = packet[2] == XZ_INIT ? initialize() : XZ_OK;
            /* INITの失敗時も、ホストが原因を区別できるようにレジスターを返す。 */
            snapshot(packet);
            count = 12;
            goto reply;
        }
        if (packet[2] == XZ_CLOCKS && size == 1 && payload[0] <= 1) {
            /* ページ0は16ビットのクロック設定を2個ずつ上位・下位に詰め、続けて32ビット値を返す。 */
            const unsigned page = payload[0];
            for (unsigned i = 6; i < 16; ++i)
                packet[i] = 0;
            if (!page) {
                for (unsigned i = 0; i < sizeof(clock_regs16) / sizeof(*clock_regs16); ++i)
                    packet[6 + i / 2] |= (uint32_t)XZ_MMIO_READ16(clock_regs16[i]) << (i & 1 ? 0 : 16);
                for (unsigned i = 0; i < sizeof(clock_regs32) / sizeof(*clock_regs32); ++i)
                    packet[11 + i] = XZ_I2C_READ(clock_regs32[i]);
                count = 32;
            } else {
                for (unsigned i = 0; i < sizeof(block_clock_regs) / sizeof(*block_clock_regs); ++i)
                    packet[6 + i] = XZ_I2C_READ(block_clock_regs[i]);
                count = 36;
            }
            error = XZ_OK;
            goto reply;
        }
        if (packet[2] == XZ_CLOCK_SETUP && !size) {
            /* 常駐部と同じく、0xf10004を16ビットで読んだbit 10〜11が2のときだけ適用する。 */
            unsigned failed = 0;
            error = XZ_NOT_READY;
            if ((XZ_MMIO_READ16(0x00f10004u) & 0x0c00u) == 0x0800u) {
                failed = clock_setup();
                error = failed ? XZ_VERIFY_FAILED : XZ_OK;
            }
            for (unsigned i = 6; i < 16; ++i)
                packet[i] = 0;
            packet[6] = failed;
            count = 4;
            goto reply;
        }
        if (packet[2] == XZ_POWER && size == 1 && payload[0] < XZ_POWER_STEPS) {
            power_step(payload[0]);
            /* ホストが手順の反映を照合できるよう、2つのレジスターを読み戻して返す。 */
            for (unsigned i = 6; i < 16; ++i)
                packet[i] = 0;
            packet[6] = XZ_MMIO_READ16(PIN_DIRECTION);
            packet[7] = XZ_MMIO_READ16(PIN_OUTPUT);
            error = XZ_OK;
            count = 8;
            goto reply;
        }
        if (packet[2] == XZ_BATCH) {
            error = validate(payload, size);
            /* XZの初期化表で設定される分周値を確認してからバスに触れる。 */
            if (!error && (XZ_I2C_READ(I2C_DIVIDER) & I2C_DIVIDER_MASK) != I2C_DIVIDER_XZ)
                error = XZ_NOT_READY;
        }
    }
    for (unsigned pos = 0; !error && pos < size; ++completed) {
        unsigned address = payload[pos++], tx = payload[pos++], rx = payload[pos++];
        error = transfer(address, payload + pos, tx, read + count, rx);
        if (error)
            break;
        pos += tx;
        count += rx;
    }
data_reply:
    if (error)
        count = 0;
    for (unsigned i = 0; i < 40; ++i)
        payload[i] = i < count ? read[i] : 0;
reply:
    packet[3] = error;
    packet[4] = completed;
    packet[5] = count;
    packet[2] |= XZ_REPLY;
}
#endif
