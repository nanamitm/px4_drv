// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace px4::pxbcud {
// USB 転送と待機を抽象化し、機器を接続しなくても転送失敗と初期化を検証する。
struct Io {
    virtual ~Io() = default;
    virtual int Transfer(bool input, std::uint8_t request, std::uint16_t index,
                         std::uint8_t *data, std::size_t size) = 0;
    virtual int Wait(unsigned milliseconds) = 0;
};

class Frontend {
public:
    explicit Frontend(Io &io) : io_(io) {}
    int Initialize();
    int Wake();
    int Sleep();
    int Tune(std::uint32_t frequency_khz);
    int Locked(bool &locked);
    int SelectStream(std::uint32_t stream_id);
    int ReadCn(std::uint16_t &cn);
    int Capture(bool enabled);
    int ReadRegister(std::uint8_t reg, std::uint8_t &value);
    int ReadDemodRegister(std::uint8_t reg, std::uint8_t &value) { return DemodRead(reg,&value,1); }
    std::uint8_t TunerId() const { return tuner_id_; }
    std::uint16_t SelectedStream() const { return selected_stream_; }
private:
    int Transfer(bool input, std::uint8_t request, std::uint16_t index,
                 std::uint8_t *data, std::size_t size);
    int WriteRegister(std::uint8_t reg, std::uint8_t value);
    int UpdateRegister(std::uint8_t reg, std::uint8_t value, std::uint8_t mask);
    int I2c(bool input, std::uint8_t address, std::uint8_t *data, std::size_t size, bool stop = true);
    int DemodWrite(std::uint8_t reg, std::uint8_t value);
    int DemodRead(std::uint8_t reg, std::uint8_t *data, std::size_t size);
    int TunerWrite(std::uint8_t reg, std::uint8_t value);
    int TunerRead(std::uint8_t reg, std::uint8_t &value);
    int Agc(bool automatic);
    Io &io_;
    std::array<std::uint8_t, 32> registers_{};
    std::uint8_t tuner_id_ = 0;
    bool awake_ = false;
    std::uint16_t selected_stream_=0;
};
} // namespace px4::pxbcud
