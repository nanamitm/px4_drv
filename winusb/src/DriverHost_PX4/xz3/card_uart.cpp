// SPDX-License-Identifier: GPL-2.0-only
#include "card_uart.hpp"
#include <iterator>

namespace px4::xz3 {
namespace {
constexpr std::uint32_t Control0 = 0xfea000, Control1 = 0xfea004, Divider = 0xfea008, Start = 0xfea00c,
    Mode = 0xfea010, Fifo = 0xfea014, Enable = 0xfea018, Baud = 0xfea020, Fraction = 0xfea024,
    Power = 0xfea034, TxConfig = 0xfea038, Guard = 0xfea03c;
// 0xfea004のbit 3。TDA8034ATのCMDVCCNをHighにし、カードへの給電を止める。
constexpr std::uint32_t CmdvccHigh = 0x8;

struct Step { std::uint32_t address, value; unsigned pause_ms; };

int Run(dt295::CardBus &bus, const Step *steps, std::size_t count)
{
    int rc = 0;
    for (std::size_t i = 0; !rc && i < count; ++i) {
        rc = bus.Write32(steps[i].address, steps[i].value);
        if (!rc && steps[i].pause_ms) rc = bus.Pause(steps[i].pause_ms);
    }
    return rc;
}
}

// GV-MVP/XZ3 (TDA8034AT) で13バイトのB-CAS ATRを受信した手順。初回、カードの抜き差しの後、
// 前回の電源切断がCMDVCCNをLowで残した状態のいずれからも受信を確認した。
// TDA8034ATは、CMDVCCNのLowの前のHighが30 msを超えると5 Vで起動する。19項目の途中から
// 最後の0x0eまでの間はbit 3が立っていて、その後の20 msを含めて30 msを超える。
// CMDVCCNのLowで起動手順が始まり、RSTINがHighのため最後にカードのリセットが解除される。
// 抜去でTDA8034ATが緊急停止した後も、先頭でbit 3を立ててCMDVCCNをHighにすると、
// カードの検出 (0xfea01cのbit 10) が戻る。
int CardUart::Reset()
{
    int rc = bus_.Write32(Control1, 0x0c);
    if (!rc) rc = common_.Reset();
    if (!rc) rc = bus_.Pause(20);
    if (!rc) rc = bus_.Write32(Control1, 0x06);
    return rc;
}

int CardUart::ReadBytes(std::uint8_t *data, std::size_t capacity, std::size_t &count)
{
    count = 0;
    if (!bus_.Bulk())
        return common_.ReadBytes(data, capacity, count);
    // 操作14は要求の大きさに関係なく最大FifoChunkバイトを受信FIFOから取り出すため、
    // それより小さい受け皿では取り出したバイトを失う。
    if (!data || capacity < FifoChunk)
        return -EINVAL;
    count = FifoChunk;
    return bus_.Fifo(false, data, count);
}

int CardUart::Queue(const std::uint8_t *data, std::size_t size)
{
    if (!bus_.Bulk())
        return common_.Queue(data, size);
    if (!data || !size || size > MaxTransfer)
        return -EINVAL;
    // 一括転送の経路がFifoChunkごとに分ける。
    std::size_t moved = size;
    const int rc = bus_.Fifo(true, const_cast<std::uint8_t *>(data), moved);
    return rc ? rc : moved == size ? 0 : -EPROTO;
}

// 先にCMDVCCNをHighにしてTDA8034ATの停止手順 (RST、CLK、I/O、VCCの順) を終えてから、
// PIX-DT295と同じ値でUARTを待機状態へ戻す。PIX-DT295の手順は0xfea004へbit 3のない値を
// 書くため、ここでは各値にbit 3を加え、途中でCMDVCCNがLowへ戻ってカードが再起動しないようにする。
// 最後の0x0cは、カードを挿入したときの値と同じ。
int CardUart::PowerDown()
{
    static constexpr Step steps[] = {
        {Control1, 0x0c, 5},
        {Fifo, 0, 0}, {Control0, 3, 0}, {Control1, CmdvccHigh, 0}, {Enable, 0, 0}, {Power, 0, 1},
        {Enable, 0, 0}, {Power, 0, 0}, {Control1, 4 | CmdvccHigh, 0}, {Fifo, 0x20, 0}, {Control0, 7, 0},
        {Divider, 0xba7f, 1}, {Guard, 0, 0}, {Baud, 0, 0}, {Fraction, 0, 0}, {Start, 0, 0},
        {Mode, 0x200, 0}, {TxConfig, 0, 0},
    };
    return Run(bus_, steps, std::size(steps));
}

}
