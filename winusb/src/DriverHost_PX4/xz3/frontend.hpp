/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace px4::xz3 {

class FrontendIo {
public:
    virtual ~FrontendIo() = default;
    virtual int I2c(std::uint8_t address, const std::uint8_t* tx, std::size_t tx_size,
                    std::uint8_t* rx, std::size_t rx_size) = 0;
    virtual int Delay(unsigned milliseconds) = 0;
};

class Frontend {
public:
    explicit Frontend(FrontendIo& io) : io_(io) {}
    int Initialize();
    int Tune(bool satellite, std::uint32_t frequency_khz, std::uint32_t tsid);
    int Locked(bool& locked);
    std::uint32_t ReadCndat();

    static std::uint16_t TerrestrialDivider(std::uint32_t frequency_khz);
    static std::array<std::uint8_t, 4> SatellitePll(std::uint32_t frequency_khz);

private:
    int Write(std::uint8_t address, std::initializer_list<std::uint8_t> data);
    int Terrestrial(std::uint32_t frequency_khz);
    int Satellite(std::uint32_t frequency_khz, std::uint16_t tsid);
    int PrepareTerrestrialTuner();

    FrontendIo& io_;
    bool satellite_ = false;
    std::uint16_t tsid_ = 0;
};

}
