// SPDX-License-Identifier: GPL-2.0-only
#include "control_channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <new>
#include <thread>

namespace px4::dt295 {
namespace {
std::uint64_t Micros(ControlChannel::Clock::duration d)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}
}

std::unique_lock<std::mutex> ControlChannel::Acquire()
{
    ++waiters_;
    std::unique_lock<std::mutex> command(mutex_);
    --waiters_;
    return command;
}

int ControlChannel::Card(const std::function<int(CardUart &)> &operation)
{
    if (!available_)
        return -ENODEV;
    auto command = Acquire();
    // 選局、SERVICE、CN、復元の転送失敗は、それぞれの経路が受信の停止と次回のCloseでの復元で扱う。
    // 応答の分からない要求が残る間は、カード操作からも転送を始めない。
    if (const int failed = transport_.Failed())
        return failed;
    auto hold = Clock::now();
    transport_.Begin(2000);
    bool inherited = false;
    // PIF1の導入後は、FIFOをファームウェアの一括転送で移す。導入前はTSを受信していない。
    DirectBus::BulkFifo bulk;
    if (source_.Ready())
        bulk = [this](bool write, std::uint8_t *data, std::size_t &count) { return source_.CardFifo(write, data, count); };
    DirectBus bus(transport_.Callbacks(), [this, &command, &hold, &inherited](unsigned ms) {
        RecordHold(Hold::Card, hold);
        command.unlock();
        const int waited = wait_(ms);
        ++waiters_;
        command.lock();
        --waiters_;
        hold = Clock::now();
        // 待機中に他の経路の転送が失敗した場合は、その失敗を解除せずにカード操作を終える。
        if (const int failed = transport_.Failed()) {
            inherited = true;
            return failed;
        }
        transport_.Begin(2000);
        return waited;
    }, std::move(bulk));
    CardUart uart(bus);
    const int rc = operation(uart);
    RecordHold(Hold::Card, hold);
    if (rc && bus.Error())
        msg_err("PIX-DT295: card transfer failed errno=%d os_error=%lu\n", bus.Error(), transport_.LastError());
    // カード操作の転送失敗は、DirectBusがこの操作の後続を止め、SmartCardが次のAPDUでカードをリセットし直す。
    // 次の操作は失敗した要求の結果に依存しないため、ここで解除する。解除しないと、受信していない間は
    // カードがデバイスの再初期化まで使えず、受信中はSERVICEも失敗してTSが止まる。
    // PIF1の応答は連番で照合するため、遅れて実行された要求の応答はSERVICEが-EPROTOとして検出する。
    if (!inherited && transport_.Failed())
        transport_.ClearFailure();
    return rc;
}

int ControlChannel::ReadCn(bool &satellite, std::uint32_t &raw)
{
    auto command = Acquire();
    const auto hold = Clock::now();
    transport_.Begin(2000);
    const int rc = source_.ReadCn(satellite, raw);
    RecordHold(Hold::Cn, hold);
    return rc;
}

int ControlChannel::RunService(const std::function<bool()> &stopping, const ServiceObserver &observe, ServiceStats &stats)
{
    auto previous = Clock::now();
    while (!stopping()) {
        // PIF1は機器側でリング/FIFOを待つため、ホストでは待機を重ねない。
        // source-frのSERVICEは待機なしで連続するため、std::mutexの取り直しで統計要求が数秒待たされる。
        // 待機中の要求があれば1回分だけ先に通し、CNの読み出しをSERVICE1回程度の遅れに収める。
        while (waiters_.load() && !stopping())
            std::this_thread::yield();
        std::lock_guard<std::mutex> command(mutex_);
        const auto begin = Clock::now();
        const auto gap = Micros(begin - previous);
        stats.total_gap_us += gap;
        stats.max_gap_us = (std::max)(stats.max_gap_us, gap);
        previous = begin;
        ++stats.calls;
        transport_.Begin(2000);
        const int rc = source_.Service();
        const auto completed = Clock::now();
        if (observe)
            observe(begin, completed, rc);
        stats.max_call_us = (std::max)(stats.max_call_us, Micros(Clock::now() - begin));
        if (rc)
            return rc;
    }
    return 0;
}

int CardSession::Detect(ControlChannel &channel, bool &present)
{
    present = false;
    const int rc = channel.Card([&present](CardUart &uart) { return uart.Present(present); });
    // 抜去を検出したら、受信途中のATRやブロックを次のカードへ持ち越さない。
    if (rc || !present)
        response_.Reset();
    return rc;
}

int CardSession::Reset(ControlChannel &channel)
{
    response_.Reset();
    return channel.Card([](CardUart &uart) { return uart.Reset(); });
}

int CardSession::PowerDown(ControlChannel &channel)
{
    response_.Reset();
    return channel.Card([](CardUart &uart) { return uart.PowerDown(); });
}

int CardSession::SetBaudrate(ControlChannel &channel, std::uint32_t baudrate)
{
    if (baudrate != 19200 && baudrate != 38400)
        return -ENOTSUP;
    return channel.Card([baudrate](CardUart &uart) { return uart.SetBaudrate(baudrate); });
}

int CardSession::DataReady(ControlChannel &channel, bool &ready)
{
    ready = false;
    if (response_.Ready()) {
        ready = true;
        return 0;
    }
    // 1回に読むバイト数はCardUartが転送方式に合わせて区切り、TSのSERVICEを長く待たせない。
    std::uint8_t bytes[CardUart::BulkChunk]{};
    std::size_t count = 0;
    const int rc = channel.Card([&](CardUart &uart) { return uart.ReadBytes(bytes, sizeof(bytes), count); });
    if (rc)
        return rc;
    int appended = 0;
    try {
        appended = response_.Append(bytes, count);
    } catch (...) {
        return -ENOMEM;
    }
    if (appended)
        return appended;
    ready = response_.Ready();
    return 0;
}

int CardSession::Write(ControlChannel &channel, const std::uint8_t *data, std::size_t size)
{
    if (!data || !size)
        return -EINVAL;
    // FIFOへの積み込みを区切り、各回の制御ロックの保持を短くする。送信開始は最後に1回だけ行う。
    // 一括転送では32バイトが1往復、導入前の直接アクセスは1バイトごとに1回の転送になる。
    constexpr std::size_t Chunk = CardUart::BulkChunk;
    int rc = 0;
    for (std::size_t at = 0; !rc && at < size; at += Chunk) {
        const auto count = (std::min)(Chunk, size - at);
        rc = channel.Card([&](CardUart &uart) {
            int result = at ? 0 : uart.BeginTransmit();
            if (!result)
                result = uart.Queue(data + at, count);
            return result;
        });
    }
    if (!rc)
        rc = channel.Card([size](CardUart &uart) { return uart.Transmit(size); });
    return rc;
}

TsDelivery::TsDelivery() : quality_(std::make_unique<px4::TsContinuityMonitor>())
{
    output_.reserve(32768);
}

int TsDelivery::Packet(void *ctx, const std::uint8_t *packet) noexcept
{
    try {
        auto &out = *static_cast<std::vector<std::uint8_t> *>(ctx);
        out.insert(out.end(), packet, packet + 188);
        return 0;
    } catch (...) {
        return -ENOMEM;
    }
}

int TsDelivery::Deliver(SourceFirmware &source, const std::uint8_t *data, std::size_t size, const Sink &sink)
{
    output_.clear();
    const int rc = source.Feed(data, size, Packet, &output_);
    if (rc)
        return rc;
    if (output_.empty())
        return 0;
    quality_->Feed(output_.data(), output_.size(), [](const px4::TsContinuityMonitor::Event &) {});
    sink(output_.data(), output_.size());
    return 0;
}
}
