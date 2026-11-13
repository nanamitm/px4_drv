// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../dt295/card_uart.hpp"

namespace px4::xz3 {

// GV-MVP/XZ3の内蔵カードUART。レジスター配置はPIX-DT295 (MB86M01) と同じで、
// カードの検出、速度の切り替え、FIFOの読み書き、送信はPIX-DT295の実装を使う。
// XZ3ではカードとUARTの間にTDA8034ATがあり、0xfea004のbit 3がCMDVCCNを駆動する
// (1でHigh、0でLow)。PIX-DT295の電源投入はbit 3を立てて終えるためカードが起動せず、
// 電源切断はbit 3を落として終えるためカードへの給電が残る。この2つの手順だけを置き換える。
// 所有者が制御ロックと操作全体の期限を保証して呼ぶ。
class CardUart final {
public:
    static constexpr std::size_t MaxTransfer = dt295::CardUart::MaxTransfer;
    // xz3-runtime.binの操作14が1回に移す最大のバイト数。ファームウェアは本体の窓に余裕が
    // ないため、要求の大きさに関係なく受信FIFOから最大31バイトを読み出す。
    static constexpr std::size_t FifoChunk = 31;
    explicit CardUart(dt295::CardBus &bus) : bus_(bus), common_(bus) {}
    int Present(bool &present) { return common_.Present(present); }
    int Reset();
    int PowerDown();
    int SetBaudrate(unsigned rate) { return common_.SetBaudrate(rate); }
    // capacityはFifoChunk以上。一括転送では受信数を先に読まず、操作14の1往復で読む。
    int ReadBytes(std::uint8_t *data, std::size_t capacity, std::size_t &count);
    int BeginTransmit() { return common_.BeginTransmit(); }
    int Queue(const std::uint8_t *data, std::size_t size);
    int Transmit(std::size_t size) { return common_.Transmit(size); }
private:
    dt295::CardBus &bus_;
    dt295::CardUart common_;
};

}
