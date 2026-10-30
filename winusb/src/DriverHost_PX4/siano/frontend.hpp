// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
extern "C" {
#include "protocol.h"
}

namespace px4::siano {
// Exchange は送信と指定応答の待機を一つの絶対期限内で処理する。
class Io {
public:
    virtual ~Io() = default;
    virtual int Exchange(const std::vector<std::uint8_t>& message, std::uint16_t response,
                         std::vector<std::uint8_t>& payload, unsigned timeout_ms) = 0;
    virtual int Delay(unsigned milliseconds) = 0;
};
// ドロップ調査用の受信品質。EX 応答 (firmware 8.0 以降) の sms_isdbt_stats_ex だけを解釈する。
struct SignalStats {
    bool rf_locked=false, demod_locked=false;
    std::int32_t snr=0, rssi=0;
    std::uint32_t frequency=0, layers=0;
    // segments が 255 の階層は存在しない。ber の 0xffffffff は値なし。
    struct Layer {std::uint32_t segments=255, ber=0xffffffff, error_ts=0, total_ts=0, tmcc_errors=0;} layer[3];
};
class Frontend final {
public:
    explicit Frontend(Io& io) : io_(io) {}
    int Initialize(const std::vector<std::uint8_t>& firmware);
    int Tune(std::uint32_t frequency_khz);
    int RequestStats(std::vector<std::uint8_t>& payload);
    int DecodeStats(const std::vector<std::uint8_t>& payload, bool& locked, std::int32_t& cnr) const;
    int DecodeSignal(const std::vector<std::uint8_t>& payload, SignalStats& stats) const;
    std::uint16_t Version() const { return version_; }
private:
    int Request(std::uint16_t type, std::uint16_t response,
                const std::vector<std::uint32_t>& words, std::vector<std::uint8_t>& payload, unsigned timeout_ms=10000);
    int VersionInfo();
    Io& io_;
    std::uint8_t mode_=255, protocols_=0;
    std::uint16_t version_=0;
};
}
