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
class Frontend final {
public:
    explicit Frontend(Io& io) : io_(io) {}
    int Initialize(const std::vector<std::uint8_t>& firmware);
    int Tune(std::uint32_t frequency_khz);
    int RequestStats(std::vector<std::uint8_t>& payload);
    int DecodeStats(const std::vector<std::uint8_t>& payload, bool& locked, std::int32_t& cnr) const;
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
