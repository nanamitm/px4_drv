// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// PIX-DT295 の制御転送の経路を、WinUSB版 (DriverHost_PX4) とLinux版 (CUSEのデーモン) で共有する。
// SERVICEのループ、制御ロック、内蔵カードUARTの操作、TSの受け渡しは実機の測定から決めた処理を含み、
// OSごとに複製すると片方だけが修正されるため、USBの読み出しの待ち方以外をここへ集める。
#include "card_response.hpp"
#include "card_uart.hpp"
#include "control_transport.hpp"
#include "runtime.h"
#include "source_firmware.hpp"
#include "ts_continuity.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace px4::dt295 {
class ControlChannel final {
public:
    using Clock = std::chrono::steady_clock;
    enum class Hold { Card, Cn };
    // 制御ロックを手放した状態でmilliseconds待つ。機器が外れたら-ENODEV、待ち終えたら0を返す。
    using Wait = std::function<int(unsigned milliseconds)>;
    // カード操作とC/Nの読み出しが制御ロックを保持した区間を、滞留の記録へ渡す。
    using HoldRecorder = std::function<void(Hold, Clock::time_point begin)>;
    // SERVICEを1回呼ぶたびに、制御ロックを保持したまま呼ぶ。
    using ServiceObserver = std::function<void(Clock::time_point begin, Clock::time_point completed, int rc)>;
    struct ServiceStats {
        std::uint64_t calls = 0, total_gap_us = 0, max_gap_us = 0, max_call_us = 0;
    };

    ControlChannel(SourceFirmware &source, ControlTransport &transport, const std::atomic_bool &available,
                   Wait wait, HoldRecorder hold = {})
        : source_(source), transport_(transport), available_(available), wait_(std::move(wait)), hold_(std::move(hold)) {}
    ControlChannel(const ControlChannel &) = delete;
    ControlChannel &operator=(const ControlChannel &) = delete;

    ControlTransport &Transport() noexcept { return transport_; }
    // 初期化、選局、受信の開始と停止、復元のように、SERVICEが動いていない間か、
    // SERVICEより先に通す必要のない操作が使う。
    std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(mutex_); }
    // SERVICEの後ろで待つ要求として制御ロックを取る。SERVICEのループは待機者がいれば1回分を先に通す。
    std::unique_lock<std::mutex> Acquire();

    // 内蔵カードUARTの操作。受信の有無に関係なく、TSと同じ制御ロックと転送路を使う。
    // 1回の保持で行う転送を短く区切り、手順内の待機ではロックを手放してSERVICEを止めない。
    int Card(const std::function<int(CardUart &)> &operation);
    // 復調器のC/Nを読む。I²Cの制御転送でSERVICEを待たせるため、呼び出し側が間隔を制限する。
    int ReadCn(bool &satellite, std::uint32_t &raw);
    // stopping()がtrueを返すまでSERVICEを待機なしで続ける。SERVICEが失敗したらその値を返す。
    int RunService(const std::function<bool()> &stopping, const ServiceObserver &observe, ServiceStats &stats);

private:
    void RecordHold(Hold kind, Clock::time_point begin) const
    {
        if (hold_)
            hold_(kind, begin);
    }
    SourceFirmware &source_;
    ControlTransport &transport_;
    const std::atomic_bool &available_;
    Wait wait_;
    HoldRecorder hold_;
    std::mutex mutex_;
    std::atomic<int> waiters_{0};
};

// 内蔵カードリーダーの1つのセッションの応答の状態。ATRとT=1ブロックの受信途中の状態を持ち、
// 抜去、リセット、電源断で一括して破棄する。呼び出し側がカードのロックの下で使う。
class CardSession final {
public:
    void Discard() { response_.Reset(); }
    int Detect(ControlChannel &channel, bool &present);
    int Reset(ControlChannel &channel);
    int PowerDown(ControlChannel &channel);
    // ATRで指定されるB-CASの19200bpsとA-CASの38400bpsを受け付ける。
    int SetBaudrate(ControlChannel &channel, std::uint32_t baudrate);
    int DataReady(ControlChannel &channel, bool &ready);
    std::size_t Read(std::uint8_t *data, std::size_t capacity) { return response_.Read(data, capacity); }
    int Write(ControlChannel &channel, const std::uint8_t *data, std::size_t size);

private:
    CardResponse response_;
};

// SourceFirmware::Feed() が組み立てた188バイトのパケットを出力先へ渡し、TSの品質を計数する。
// Bulk INの読み出しを所有する1つのスレッドだけが使う。
class TsDelivery final {
public:
    using Sink = std::function<void(const std::uint8_t *packets, std::size_t size)>;
    TsDelivery();
    // 出力先へは188バイトの倍数を渡す。失敗した場合は負のerrnoを返す。
    int Deliver(SourceFirmware &source, const std::uint8_t *data, std::size_t size, const Sink &sink);
    const px4::TsContinuityMonitor::Counters &Counts() const noexcept { return quality_->Counts(); }

private:
    static int Packet(void *ctx, const std::uint8_t *packet) noexcept;
    std::unique_ptr<px4::TsContinuityMonitor> quality_;
    std::vector<std::uint8_t> output_;
};
}
