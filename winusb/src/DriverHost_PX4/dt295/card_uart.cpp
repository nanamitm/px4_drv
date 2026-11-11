// SPDX-License-Identifier: GPL-2.0-only
#include "card_uart.hpp"
#include <algorithm>
#include <cerrno>
#include <iterator>

namespace px4::dt295 {
namespace {
constexpr std::uint32_t Control0 = 0xfea000, Control1 = 0xfea004, Divider = 0xfea008, Start = 0xfea00c,
    Mode = 0xfea010, Fifo = 0xfea014, Enable = 0xfea018, Status = 0xfea01c, Baud = 0xfea020,
    Fraction = 0xfea024, TxData = 0xfea028, RxData = 0xfea02c, Power = 0xfea034, TxConfig = 0xfea038,
    Guard = 0xfea03c;

std::uint32_t Be(const std::uint8_t *bytes)
{
    return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 | std::uint32_t(bytes[2]) << 8 | bytes[3];
}
}

// PIX-DT295 Rev.01で、ブート直後の状態からこの手順の後に13バイトのB-CAS ATRを受信した。
// mAgicVISION 0.1.0がPIX-DT295でカード電源投入後に残すレジスター値 (0xfea004=0x0e など) とも一致する。
// 待機の値はmAgicVISIONの手順表のティック数をミリ秒として扱い、実機で受信を確認した。
int CardUart::Reset()
{
    static constexpr Step steps[] = {
        {Enable, 0, 0}, {Power, 0, 0}, {Status, 0x3ff, 0}, {Control1, 0, 0}, {Fifo, 0, 0},
        {Control0, 0, 20}, {Control0, 7, 20}, {Control1, 8, 0}, {Divider, 0xba7f, 20},
        {Control1, 0xa, 0}, {Control0, 0, 20}, {Control0, 7, 0}, {Guard, 0x3b, 0},
        {Baud, 9600, 0}, {Fraction, 0, 0}, {Enable, 0, 0}, {Fifo, 0x20, 50}, {Power, 1, 0},
        {Control1, 0xe, 0},
    };
    return Run(steps, std::size(steps));
}

// 電源を落とした後、カードUARTをブート直後と同じ待機値へ戻す。
// mAgicVISION 0.1.0もPIX-DT295でカード電源を落とした後に同じ値を残す。
int CardUart::PowerDown()
{
    static constexpr Step steps[] = {
        {Fifo, 0, 0}, {Control0, 3, 0}, {Control1, 0, 0}, {Enable, 0, 0}, {Power, 0, 1},
        {Enable, 0, 0}, {Power, 0, 0}, {Control1, 4, 0}, {Fifo, 0x20, 0}, {Control0, 7, 0},
        {Divider, 0xba7f, 1}, {Guard, 0, 0}, {Baud, 0, 0}, {Fraction, 0, 0}, {Start, 0, 0},
        {Mode, 0x200, 0}, {TxConfig, 0, 0},
    };
    return Run(steps, std::size(steps));
}

// PIX-DT295では0xfea01cのbit 10が1のときカードが挿入されている。torneとは極性が逆。
int CardUart::Present(bool &present)
{
    std::uint32_t status = 0;
    const int rc = bus_.Read32(Status, status);
    if (!rc) present = (status & 0x400) != 0;
    return rc;
}

// 値はtorneのMB86H47Aと同じ。同じUARTの配置で、B-CASが要求する19200 bpsを設定する。
int CardUart::SetBaudrate(unsigned rate)
{
    if (rate != 9600 && rate != 19200 && rate != 38400) return -EINVAL;
    const Step steps[] = {
        {Divider, rate == 9600 ? 0xba7fu : rate == 19200 ? 0x5d5du : 0x2eaeu, 0},
        {Mode, rate == 9600 ? 0u : 0x10000u, 0},
        {Baud, rate == 9600 ? 9600u : 107u, 0},
        {Fraction, rate == 9600 ? 0u : rate == 19200 ? 36559u : 0xffffu, 0},
    };
    return Run(steps, std::size(steps));
}

int CardUart::ReadBytes(std::uint8_t *data, std::size_t capacity, std::size_t &count)
{
    count = 0;
    if (!data || !capacity || capacity > MaxTransfer) return -EINVAL;
    // 受信数を先に1回だけ読み、空のときは一括転送の往復を使わない。
    std::uint32_t fifo = 0;
    int rc = bus_.Read32(Fifo, fifo);
    const auto length = (std::min)({std::size_t(fifo >> 16), capacity, bus_.Bulk() ? BulkChunk : DirectChunk});
    if (rc || !length) return rc;
    if (bus_.Bulk()) {
        std::size_t moved = length;
        rc = bus_.Fifo(false, data, moved);
        if (!rc) count = moved;
        return rc;
    }
    while (!rc && count < length) {
        std::uint32_t value = 0;
        rc = bus_.Read32(RxData, value);
        if (!rc) data[count++] = static_cast<std::uint8_t>(value);
    }
    return rc;
}

// 送信はtorneのMB86H47Aと同じ順序。FIFOへの積み込みを分割できるよう3段階に分ける。
int CardUart::BeginTransmit()
{
    int rc = bus_.Write32(Guard, 0);
    if (!rc) rc = bus_.Write32(TxConfig, 0);
    return rc;
}

int CardUart::Queue(const std::uint8_t *data, std::size_t size)
{
    if (!data || !size || size > MaxTransfer) return -EINVAL;
    int rc = 0;
    if (bus_.Bulk()) {
        for (std::size_t at = 0; !rc && at < size; at += BulkChunk) {
            std::size_t moved = (std::min)(BulkChunk, size - at);
            rc = bus_.Fifo(true, const_cast<std::uint8_t *>(data + at), moved);
        }
        return rc;
    }
    for (std::size_t i = 0; !rc && i < size; ++i) rc = bus_.Write32(TxData, data[i]);
    return rc;
}

int CardUart::Transmit(std::size_t size)
{
    if (!size || size > MaxTransfer) return -EINVAL;
    int rc = bus_.Write32(Status, 0x3ff);
    if (!rc) rc = bus_.Write32(Mode, size >= 256 ? 0x280 : 0x200);
    if (!rc) rc = bus_.Write32(Start, 1);
    return rc;
}

int CardUart::Run(const Step *steps, std::size_t count)
{
    int rc = 0;
    for (std::size_t i = 0; !rc && i < count; ++i) {
        rc = bus_.Write32(steps[i].address, steps[i].value);
        if (!rc && steps[i].pause_ms) rc = bus_.Pause(steps[i].pause_ms);
    }
    return rc;
}

// BC要求は番地のbit 16〜23をvalueの上位バイト、bit 0〜15をindexに載せる。
int DirectBus::Control(std::uint32_t address, std::uint8_t *bytes, bool read)
{
    // カードUARTの16語だけを許可し、任意のMMIOへ書かない。
    if (error_) return error_;
    if (address < Control0 || address > Guard || (address & 3)) return -EINVAL;
    const int rc = io_.control(io_.opaque, read ? 0xc0 : 0x40, 0xbc,
        static_cast<std::uint16_t>(address >> 8 & 0xff00), static_cast<std::uint16_t>(address),
        bytes, 4, 1000);
    return rc ? error_ = rc : 0;
}

int DirectBus::Read32(std::uint32_t address, std::uint32_t &value)
{
    std::uint8_t bytes[4]{};
    const int rc = Control(address, bytes, true);
    if (!rc) value = Be(bytes);
    return rc;
}

int DirectBus::Write32(std::uint32_t address, std::uint32_t value)
{
    std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return Control(address, bytes, false);
}

int DirectBus::Fifo(bool write, std::uint8_t *data, std::size_t &count)
{
    const std::size_t requested = count;
    count = 0;
    if (error_) return error_;
    if (!bulk_) return -EOPNOTSUPP;
    std::size_t moved = requested;
    if (int rc = bulk_(write, data, moved)) return error_ = rc;
    // 書込みは全量、読出しは要求以下でなければ、FIFOの位置が分からなくなる。
    if (moved > requested || (write && moved != requested)) return error_ = -EPROTO;
    count = moved;
    return 0;
}

int DirectBus::Pause(unsigned milliseconds)
{
    if (error_) return error_;
    if (!yield_) {
        io_.delay_ms(io_.opaque, milliseconds);
        return 0;
    }
    // 待機中は制御ロックを手放し、TSのSERVICEを止めない。
    if (int rc = yield_(milliseconds)) return error_ = rc;
    return 0;
}
}
