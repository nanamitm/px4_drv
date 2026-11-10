/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t read_reg(uint32_t address);
static void write_reg(uint32_t address, uint32_t value);
#define XZ_I2C_READ(address) read_reg(address)
#define XZ_I2C_WRITE(address, value) write_reg(address, value)
static uint16_t read_reg16(uint32_t address);
#define XZ_MMIO_READ16(address) read_reg16(address)
static void write_reg16(uint32_t address, uint16_t value);
#define XZ_MMIO_WRITE16(address, value) write_reg16(address, value)
#include "relay.c"

static unsigned writes, starts, stops, received, control, owned;
static unsigned control_log[64], control_count;
static unsigned nack, bus_error, byte_timeout, stuck_after_stop, busy;
static unsigned sent[32], sent_count;
static unsigned divider = 0x3c;
/* 初期化対象の他ビットが保持されることを確かめるため、既定値を非零にする。 */
static unsigned block_enable = 0x0101, block_reset = 0xffff, port_c0 = 0x55, port_c4 = 0x55;
static unsigned config_ff4, config_ff8;
static uint32_t init_log[16][2];
static unsigned init_count;

/* 各ブロックの0x404。既定値はアドレスから作り、並びを確かめられるようにする。 */
static uint32_t blocks[9];
static unsigned blocks_written[9];

static unsigned block_index(uint32_t address)
{
    assert((address & 0xffffu) == 0x0404u);
    for (unsigned i = 0; i < 9; ++i)
        if (block_clock_regs[i] == address)
            return i;
    assert(0);
    return 0;
}

static uint32_t block_value(uint32_t address)
{
    const unsigned i = block_index(address);
    return blocks_written[i] ? blocks[i] : address >> 8;
}

static void set_block_value(uint32_t address, uint32_t value)
{
    const unsigned i = block_index(address);
    blocks[i] = value;
    blocks_written[i] = 1;
}

static uint32_t read_reg(uint32_t address)
{
    switch (address) {
    case I2C_STATUS:
        return (nack ? 0x10u : 0) | ((stops && stuck_after_stop) || busy ? 0x80u : 0);
    case I2C_CONTROL: return (control & ~0x32u) | (bus_error ? 0x80u : 0) | !byte_timeout;
    case I2C_DATA: return 0xa0 + received++;
    /* 実機ではCONTROLへ0x40を書いた後、DIVIDERのbit 7が立って読める。 */
    case I2C_DIVIDER: return divider | (divider && control == 0x40 ? 0x80u : 0);
    case I2C_BLOCK_ENABLE: return block_enable;
    case I2C_BLOCK_RESET: return block_reset;
    case 0x00f10004u: return 0x00000c00u;
    default:
        return block_value(address);
    }
}

static unsigned reads16;

/* boot0の0に、手順が保持すべきbitを加えた初期値。 */
static uint16_t pin_direction = 0x0100, pin_output = 0x0800;
static unsigned pin_writes;
/* 0xf10000〜0xf10fffの16ビットレジスター。並びを確かめるため、既定値をアドレスから作る。 */
static uint16_t reg16[0x800];
static unsigned reg16_written[0x800];
/* 0xf10004を16ビットで読んだ値。GV-MVP/XZ3のboot0と同じ0x8a51を既定にする。 */
static uint16_t clock_mode = 0x8a51;
/* 書き込みを反映しないレジスター。読み戻し照合の失敗を作る。 */
static uint32_t stuck16;

static uint16_t read_reg16(uint32_t address)
{
    ++reads16;
    if ((address & 0xffffu) == 0x0404u)
        return (uint16_t)(block_value(address) >> 16);
    assert((address & 0xfffff000u) == 0x00f10000u && !(address & 1));
    if (address == PIN_DIRECTION)
        return pin_direction;
    if (address == PIN_OUTPUT)
        return pin_output;
    if (address == 0x00f10004u)
        return clock_mode;
    const unsigned index = (address & 0xfffu) / 2;
    return reg16_written[index] ? reg16[index] : (uint16_t)(0x1000u + (address & 0xfffu));
}

static void write_reg16(uint32_t address, uint16_t value)
{
    ++writes;
    if ((address & 0xffffu) == 0x0404u) {
        /* 16ビットの書き込みは32ビットレジスターの上位側にあたる。 */
        set_block_value(address, (block_value(address) & 0xffffu) | (uint32_t)value << 16);
        return;
    }
    assert((address & 0xfffff000u) == 0x00f10000u && !(address & 1));
    if (address == PIN_DIRECTION || address == PIN_OUTPUT) {
        ++pin_writes;
        if (address == PIN_DIRECTION)
            pin_direction = value;
        else
            pin_output = value;
        return;
    }
    if (address == stuck16)
        return;
    const unsigned index = (address & 0xfffu) / 2;
    reg16[index] = value;
    reg16_written[index] = 1;
}

static void write_reg(uint32_t address, uint32_t value)
{
    ++writes;
    switch (address) {
    case I2C_CONTROL:
        control = value;
        /* XZ常駐ファームウェアと同じく、bit 1を含む値は書かない。 */
        assert(value == 0x50 || value == 0x58 || value == 0x70 || value == 0x40);
        if (control_count < sizeof(control_log) / sizeof(*control_log))
            control_log[control_count] = value;
        ++control_count;
        /* バスを保持していない状態の0x50をSTART、保持中の0x40をSTOPとみなす。 */
        if (value == 0x50 && !owned) {
            ++starts;
            owned = 1;
        } else if (value == 0x70) {
            assert(owned);
        } else if (value == 0x40 && owned) {
            ++stops;
            owned = 0;
        }
        break;
    case I2C_DATA:
        assert(sent_count < sizeof(sent) / sizeof(*sent));
        sent[sent_count++] = value;
        return;
    case I2C_DIVIDER: divider = value; break;
    case I2C_BLOCK_ENABLE: block_enable = value; break;
    case I2C_BLOCK_RESET: block_reset = value; break;
    case I2C_PORT_C0: port_c0 = value; break;
    case I2C_PORT_C4: port_c4 = value; break;
    case I2C_CONFIG_FF4: config_ff4 = value; break;
    case I2C_CONFIG_FF8: config_ff8 = value; break;
    default:
        set_block_value(address, value);
        return;
    }
    if (init_count < sizeof(init_log) / sizeof(*init_log)) {
        init_log[init_count][0] = address;
        init_log[init_count][1] = value;
    }
    ++init_count;
}

static void request(uint32_t packet[16], unsigned op, const uint8_t *data, unsigned size)
{
    memset(packet, 0, 64);
    packet[0] = XZ_MAGIC;
    packet[1] = 7;
    packet[2] = op;
    packet[3] = size;
    if (size) memcpy(packet + 6, data, size);
}

static void expect_failure(unsigned result)
{
    const uint8_t batch[] = {0x11, 1, 2, 0x80};
    uint32_t packet[16];
    request(packet, XZ_BATCH, batch, sizeof(batch));
    unsigned previous_stops = stops;
    xz_i2c_relay(packet);
    assert(packet[3] == result && packet[4] == 0 && packet[5] == 0);
    assert(stops == previous_stops + 1);
}

int main(void)
{
    uint32_t packet[16];
    request(packet, XZ_PROBE, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[2] == (XZ_REPLY | XZ_PROBE) && packet[3] == XZ_OK);
    assert(packet[5] == 12 && packet[8] == 0x3c && !writes);

    const uint8_t malformed[] = {0x11, 1, 0, 0x80, 0x11, 8, 0};
    request(packet, XZ_BATCH, malformed, sizeof(malformed));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && !writes);

    const uint8_t batch[] = {0x11, 1, 2, 0x80, 0x10, 2, 0, 0x05, 0x12};
    const unsigned expected[] = {0x22, 0x80, 0x23, 0x20, 0x05, 0x12};
    request(packet, XZ_BATCH, batch, sizeof(batch));
    divider = 0;
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_NOT_READY && !writes);
    divider = 0x3c;
    request(packet, XZ_BATCH, batch, sizeof(batch));
    xz_i2c_relay(packet);
    const uint8_t *reply = (const uint8_t *)(packet + 6);
    assert(packet[1] == 7 && packet[2] == (XZ_REPLY | XZ_BATCH));
    assert(packet[3] == XZ_OK && packet[4] == 2 && packet[5] == 2);
    assert(reply[0] == 0xa0 && reply[1] == 0xa1 && reply[2] == 0);
    assert(starts == 2 && stops == 2 && sent_count == sizeof(expected) / sizeof(*expected));
    assert(!memcmp(sent, expected, sizeof(expected)));
    /* XZ常駐ファームウェアのCONTROL値: START・送信0x50、反復START 0x70、
     * 受信は後続あり0x58・最後0x50、STOP 0x40。 */
    const unsigned expected_control[] = {
        0x50, 0x50, 0x70, 0x58, 0x50, 0x40,
        0x50, 0x50, 0x50, 0x40,
    };
    assert(control_count == sizeof(expected_control) / sizeof(*expected_control));
    assert(!memcmp(control_log, expected_control, sizeof(expected_control)));
    const unsigned previous_writes = writes;
    xz_i2c_relay(packet);
    assert(writes == previous_writes);

    nack = 1;
    expect_failure(XZ_NACK);
    nack = 0;
    bus_error = 1;
    expect_failure(XZ_BUS_ERROR);
    bus_error = 0;
    byte_timeout = 1;
    expect_failure(XZ_TIMEOUT);
    byte_timeout = 0;
    stuck_after_stop = 1;
    stops = 0;
    expect_failure(XZ_STOP_TIMEOUT);
    stuck_after_stop = 0;

    /* INITは操作データを取らない。形式不正ではレジスターに触れない。 */
    const uint8_t extra[] = {0};
    request(packet, XZ_INIT, extra, sizeof(extra));
    const unsigned writes_before_init = writes;
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && packet[5] == 0 && writes == writes_before_init);

    /* boot0直後の未初期化状態から、XZ初期化表と同じ順序・値で設定する。 */
    divider = 0;
    init_count = 0;
    request(packet, XZ_INIT, 0, 0);
    xz_i2c_relay(packet);
    const uint32_t expected_init[][2] = {
        {I2C_BLOCK_ENABLE, 0x0111}, {I2C_BLOCK_RESET, 0xffef},
        {I2C_PORT_C0, 1}, {I2C_PORT_C0, 0}, {I2C_PORT_C4, 0},
        {I2C_CONFIG_FF8, 6}, {I2C_CONFIG_FF4, 0x10},
        {I2C_DIVIDER, 0x3c}, {I2C_CONTROL, 0x40},
    };
    assert(init_count == sizeof(expected_init) / sizeof(*expected_init));
    assert(!memcmp(init_log, expected_init, sizeof(expected_init)));
    assert(packet[2] == (XZ_REPLY | XZ_INIT) && packet[3] == XZ_OK);
    assert(packet[4] == 0 && packet[5] == 12 && packet[8] == 0xbc && packet[9] == 0);

    /* 初期化後はBATCHが分周値の確認を通過する。 */
    const unsigned previous_starts = starts;
    request(packet, XZ_BATCH, batch, sizeof(batch));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_OK && packet[4] == 2 && starts == previous_starts + 2);

    /* 初期化後もバスが解放されない場合は、レジスター値を添えてタイムアウトを返す。 */
    busy = 1;
    request(packet, XZ_INIT, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_TIMEOUT && packet[5] == 12 && (packet[6] & 0x80));
    busy = 0;

    /* CLOCKSは読み出しだけで、レジスターへ書き込まない。 */
    const unsigned writes_before_clocks = writes, reads_before_clocks = reads16;
    const uint8_t page0[] = {0};
    request(packet, XZ_CLOCKS, page0, sizeof(page0));
    xz_i2c_relay(packet);
    assert(packet[2] == (XZ_REPLY | XZ_CLOCKS) && packet[3] == XZ_OK && packet[5] == 32);
    assert(reads16 == reads_before_clocks + 10 && writes == writes_before_clocks);
    assert(packet[6] == 0x110c1114u && packet[7] == 0x1118111cu && packet[8] == 0x11201124u);
    assert(packet[9] == 0x1128112cu && packet[10] == 0x01000800u);
    assert(packet[11] == 0x00000c00u && packet[12] == block_enable && packet[13] == block_reset);
    assert(packet[14] == 0 && packet[15] == 0);

    const uint8_t page1[] = {1};
    request(packet, XZ_CLOCKS, page1, sizeof(page1));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_OK && packet[5] == 36 && writes == writes_before_clocks);
    assert(packet[6] == 0x014d04u && packet[14] == 0x017004u && packet[15] == 0);

    /* ページ番号のない要求と範囲外のページは、読み出し前に拒否する。 */
    const unsigned reads_before_invalid = reads16;
    const uint8_t page2[] = {2};
    request(packet, XZ_CLOCKS, page2, sizeof(page2));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && packet[5] == 0);
    request(packet, XZ_CLOCKS, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && reads16 == reads_before_invalid);

    /* POWERは1回に1手順だけを適用し、出力許可と出力値の読み戻しを返す。
     * 初期値の0x0100と0x0800は、各手順が保持すべき他のbit。 */
    const uint16_t expected_power[][2] = {
        {0xd1f4, 0x0804}, {0xd1f4, 0x0844}, {0xd1f4, 0x08c4},
        {0xd1f4, 0x08e4}, {0xd1f5, 0x08e4}, {0xd1f5, 0x08e5},
    };
    for (unsigned step = 0; step < 6; ++step) {
        const uint8_t argument[] = {(uint8_t)step};
        request(packet, XZ_POWER, argument, sizeof(argument));
        xz_i2c_relay(packet);
        assert(packet[2] == (XZ_REPLY | XZ_POWER) && packet[3] == XZ_OK && packet[5] == 8);
        assert(packet[6] == expected_power[step][0] && packet[7] == expected_power[step][1]);
        assert(packet[8] == 0);
    }

    /* 範囲外の手順と、手順番号のない要求は端子に触れない。 */
    const unsigned pin_writes_before = pin_writes;
    const uint8_t step6[] = {6};
    request(packet, XZ_POWER, step6, sizeof(step6));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && packet[5] == 0);
    request(packet, XZ_POWER, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && pin_writes == pin_writes_before);

    /* CLOCK_SETUPは0xf10004のbit 10〜11が2でないとき、何も書かずに未初期化を返す。 */
    const unsigned writes_before_setup = writes;
    clock_mode = 0x8e51;
    request(packet, XZ_CLOCK_SETUP, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_NOT_READY && packet[5] == 4 && packet[6] == 0);
    assert(writes == writes_before_setup);
    clock_mode = 0x8a51;

    /* 操作データ付きの要求は形式不正。 */
    request(packet, XZ_CLOCK_SETUP, page0, sizeof(page0));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && writes == writes_before_setup);

    /* 読み戻しが一致しない項目で中止し、その項目番号+1を返す。0xf10128は8番目。 */
    stuck16 = 0x00f10128u;
    request(packet, XZ_CLOCK_SETUP, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_VERIFY_FAILED && packet[5] == 4 && packet[6] == 8);
    stuck16 = 0;

    /* 全項目を適用すると、表の値と保持マスクから求めた値になる。 */
    memset(reg16_written, 0, sizeof(reg16_written));
    memset(blocks_written, 0, sizeof(blocks_written));
    request(packet, XZ_CLOCK_SETUP, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[2] == (XZ_REPLY | XZ_CLOCK_SETUP) && packet[3] == XZ_OK);
    assert(packet[5] == 4 && packet[6] == 0 && packet[7] == 0);
    assert(read_reg16(0x00f100f0u) == 0x0001);
    assert(read_reg16(0x00f1010cu) == 0x8201);
    assert(read_reg16(0x00f10114u) == 0x8008);
    assert(read_reg16(0x00f10118u) == 0xffff);
    assert(read_reg16(0x00f1011cu) == 0x0006);
    /* 既定値0x1120、0x1128、0x112cの保持マスク部分が残る。 */
    assert(read_reg16(0x00f10120u) == (0x1120 & 0x1080) + 0x0100);
    assert(read_reg16(0x00f10124u) == 0x128a);
    assert(read_reg16(0x00f10128u) == ((0x1128 & 0x0e0e) | 0x6100));
    assert(read_reg16(0x00f1012cu) == ((0x112c & 0x21f8) | 0x4007));
    for (unsigned i = 0; i < 9; ++i) {
        const uint32_t initial = block_clock_regs[i] >> 8;
        assert(blocks_written[i] && blocks[i] == (initial | 0x00040000u));
    }
    return 0;
}
