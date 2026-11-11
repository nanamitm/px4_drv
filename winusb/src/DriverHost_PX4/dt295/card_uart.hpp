// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "runtime.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace px4::dt295 {

// カードUARTのレジスターを読み書きする経路。戻り値は0または負のerrno。
class CardBus {
public:
    virtual ~CardBus() = default;
    virtual int Read32(std::uint32_t address, std::uint32_t &value) = 0;
    virtual int Write32(std::uint32_t address, std::uint32_t value) = 0;
    virtual int Pause(unsigned milliseconds) = 0;
    // FIFOを複数バイトまとめて移せる経路だけがtrueを返す。readではcountまでの溜まった分を読む。
    virtual bool Bulk() const { return false; }
    virtual int Fifo(bool write, std::uint8_t *data, std::size_t &count)
    {
        (void)write;
        (void)data;
        count = 0;
        return -EOPNOTSUPP;
    }
};

// PIX-DT295 (MB86M01) の内蔵カードUART。レジスター配置はtorneのMB86H47Aと同じだが、
// カード検出の極性と電源投入の手順が異なるため、機種別の実装として分けている。
// 所有者が制御ロックと操作全体の期限を保証して呼ぶ。失敗した操作内では後続のアクセスを止める。
class CardUart final {
public:
    static constexpr std::size_t MaxTransfer = 259;
    // 1回の呼び出しで移す上限。一括転送はファームウェアの1往復分、直接アクセスは約3 ms×8回。
    static constexpr std::size_t BulkChunk = 32, DirectChunk = 8;
    explicit CardUart(CardBus &bus) : bus_(bus) {}
    int Present(bool &present);
    int Reset();
    int PowerDown();
    int SetBaudrate(unsigned rate);
    // 1回の呼び出しで読むバイト数をcapacityまでに制限し、制御ロックの保持時間を抑える。
    int ReadBytes(std::uint8_t *data, std::size_t capacity, std::size_t &count);
    // 送信はBeginTransmit、Queue (分割可)、Transmit の順に呼ぶ。sizeはQueueした合計。
    int BeginTransmit();
    int Queue(const std::uint8_t *data, std::size_t size);
    int Transmit(std::size_t size);
private:
    struct Step { std::uint32_t address, value; unsigned pause_ms; };
    int Run(const Step *steps, std::size_t count);
    CardBus &bus_;
};

// ブートROMのBC要求で、カードUARTのレジスターを24bitの番地で直接読み書きする。
// SourceFirmwareが0x00f10014などを読み書きするのと同じ要求で、窓の付け替えを伴わない。
// 対象はカードUARTの16語に限り、失敗後は同じ操作内の転送を止める。
class DirectBus final : public CardBus {
public:
    // yieldを指定すると、Pause中はyield(ms)を呼ぶ。所有者はその中で制御ロックを手放して待ち、
    // 再取得してから戻る。未指定のときはio.delay_msで待つ。
    // bulkを指定すると、FIFOの読み書きをファームウェアの一括転送 (PIF1の操作5) で行う。
    using BulkFifo = std::function<int(bool write, std::uint8_t *data, std::size_t &count)>;
    explicit DirectBus(const dt295_io &io, std::function<int(unsigned)> yield = {}, BulkFifo bulk = {})
        : io_(io), yield_(std::move(yield)), bulk_(std::move(bulk)) {}
    int Read32(std::uint32_t address, std::uint32_t &value) override;
    int Write32(std::uint32_t address, std::uint32_t value) override;
    int Pause(unsigned milliseconds) override;
    bool Bulk() const override { return static_cast<bool>(bulk_); }
    int Fifo(bool write, std::uint8_t *data, std::size_t &count) override;
    int Error() const noexcept { return error_; }
private:
    int Control(std::uint32_t address, std::uint8_t *bytes, bool read);
    dt295_io io_;
    std::function<int(unsigned)> yield_;
    BulkFifo bulk_;
    int error_ = 0;
};
}
