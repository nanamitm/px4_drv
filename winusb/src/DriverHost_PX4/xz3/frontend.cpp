/* SPDX-License-Identifier: GPL-2.0-only */
#include "frontend.hpp"

#include <cerrno>

namespace px4::xz3 {
namespace {
constexpr std::uint8_t TerrestrialAddress = 0x18;
constexpr std::uint8_t SatelliteAddress = 0x19;

// 地デジ側のI²Cゲートを通して送るチューナーの初期設定。
constexpr std::uint8_t TerrestrialTunerSetup[] = {
    0xfe, 0xc0, 0x2c, 0x44, 0x4d, 0x40, 0x7f, 0x02, 0x9a, 0x52,
    0x48, 0x5a, 0x76, 0x1a, 0x6a, 0x48, 0x64, 0x28, 0x66, 0xe6,
    0x35, 0x11, 0x7e, 0x01, 0x0b, 0x99, 0x0c, 0x00, 0x10, 0x00,
    0x12, 0xca, 0x16, 0x90, 0x32, 0x36, 0xd8, 0x18,
};

std::uint8_t Byte(std::uint32_t value)
{
    return static_cast<std::uint8_t>(value);
}
}

std::uint16_t Frontend::TerrestrialDivider(std::uint32_t frequency_khz)
{
    return static_cast<std::uint16_t>((frequency_khz * 64 + 500) / 1000);
}

std::array<std::uint8_t, 4> Frontend::SatellitePll(std::uint32_t frequency_khz)
{
    const auto divider = frequency_khz / 1000;
    // RF 12.2 GHzでPLLの帯域が変わる。入力はLNBの10.678 GHzを引いたIF。
    const auto band = frequency_khz < 1522000 ? 0x40u : 0x70u;
    return {Byte(divider >> 8), Byte(divider), Byte(band), 0};
}

int Frontend::Write(std::uint8_t address, std::initializer_list<std::uint8_t> data)
{
    return io_.I2c(address, data.begin(), data.size(), nullptr, 0);
}

int Frontend::PrepareTerrestrialTuner()
{
    int result = Write(TerrestrialAddress, {0xfe, 0xc0, 0xff});
    if (!result)
        result = io_.I2c(TerrestrialAddress, TerrestrialTunerSetup,
                         sizeof(TerrestrialTunerSetup), nullptr, 0);
    if (!result)
        result = Write(TerrestrialAddress, {0xfe, 0xc0, 0x05, 0x01});
    return result;
}

int Frontend::Initialize()
{
    const std::initializer_list<std::uint8_t> terrestrial[] = {
        {0x01,0,0,0,0x02,0,0x80,0,0}, {0x0c,0}, {0x0f,0x34},
        {0x11,0x27,0x0c,0x33,0x20,0x40,0,0x70,0x31,0x13,0x31,0x13,0x38,0x88,
         0xaa,0xa8,0,0xff,0x80,0x4c,0x4c,0,0,0x0c,0x60,0x6b,0x40,0x40,
         0x5a,0x05,0xff,0,0x20,0x0d,0x21},
        {0x34,0x0f}, {0x38,0,0xaa,0x2d,0x21,0x3f,0x10,8,0x0c,0x0c,0,0,0x4f,0xff},
        {0x46,0x20,0,0x90,0xe6,0x02,0x80,0}, {0x4f,5,0,0x68,0x20},
        {0x54,0x57,0xf1,0x20,0x70}, {0x5c,0x50,0}, {0x5f,0x80},
        {0x70,0x18,0,0}, {0x75,0,0x0c,1}, {0x7c,0,0xd2}, {0xba,0,0,0},
        {0xc7,0}, {0xe4,0xf4}, {0xec,0}, {0xef,1},
    };
    for (const auto& operation : terrestrial)
        if (const int result = Write(TerrestrialAddress, operation))
            return result;
    if (const int result = PrepareTerrestrialTuner())
        return result;

    const std::initializer_list<std::uint8_t> satellite[] = {
        {1,0x90}, {3,0,0}, {6,0x40,0x41,0,0,0xff},
        {0x0c,0x59,0xf2,0xf0,0x50,0xb2,0,0x30,0x80,0,0},
        {0x17,0}, {0x1a,0,0,0xaa,0xaa,0x20,0xaa,0},
        {0x38,0x40,0x10}, {0x3b,0x90}, {0x51,0xb0,0x89,0xb3}, {0x5a,0x2d,0xd3},
        {0x85,0x69}, {0x87,0}, {0x8d,0,0}, {0xa3,0x11,0,0x40,4},
        {0xfe,0xc0,9,3}, {0xfe,0xc0,6,0x2a,0xe8,0x84},
        {0xfe,0xc0,0x0a,0x20,0x78,0x1e,0x0a}, {0xfe,0xc0,0x0f,8,0x0f},
        {0xfe,0xc0,0x18,0x40}, {0x17,0},
    };
    for (const auto& operation : satellite)
        if (const int result = Write(SatelliteAddress, operation))
            return result;
    // 共有TS出力の初期設定は両方の復調器を交互に操作する。
    const struct {
        std::uint8_t address;
        std::initializer_list<std::uint8_t> data;
    } route[] = {
        {0x18,{0x1c,0x38}}, {0x19,{0x1c,0xaa}}, {0x19,{0x17,1}},
        {0x18,{3,0}}, {0x18,{0x1c,0x33}}, {0x19,{0x1c,0x0a}}, {0x19,{0xfe,0xc0,4,0}},
        {0x18,{3,0x90}}, {0x18,{3,0x90}}, {0x18,{0xfe,0xc0,5,0}},
        {0x19,{0x17,1}}, {0x19,{0xfe,0xc0,0x0e,0x2f}}, {0x19,{0xfe,0xc0,0x16,0}},
        {0x19,{0xfe,0xc0,2,0x0c,0xf6}}, {0x19,{0xfe,0xc0,0x19,0x40}},
        {0x19,{0xfe,0xc0,0x20,8}},
    };
    for (const auto& operation : route)
        if (const int result = Write(operation.address, operation.data))
            return result;
    return 0;
}

int Frontend::Terrestrial(std::uint32_t frequency_khz)
{
    if (const int result = Write(SatelliteAddress, {0x17, 1}))
        return result;
    // 衛星選局時に停止した地デジのTS出力を、地デジへ戻るときに有効にする。
    if (const int result = Write(TerrestrialAddress, {0x0f, 0x34}))
        return result;
    if (const int result = PrepareTerrestrialTuner())
        return result;
    const auto divider = TerrestrialDivider(frequency_khz);
    const std::initializer_list<std::uint8_t> operations[] = {
        {3,0}, {0x1e,0xaa}, {0x1f,0xa8}, {0x25,0x80}, {0x23,0x4d},
        {0xfe,0xc0,0x11,0,0x13,0x15,0x14,Byte(divider),0x15,Byte(divider >> 8)},
        {0x1c,0x38}, {0x47,0}, {0xfe,0xc0,0x11,2}, {0x4f,5},
        {1,0x40}, {0x23,0x4c}, {0x1e,0xa2}, {0x1f,8},
    };
    for (const auto& operation : operations)
        if (const int result = Write(TerrestrialAddress, operation))
            return result;
    return 0;
}

int Frontend::Satellite(std::uint32_t frequency_khz, std::uint16_t tsid)
{
    int result = Write(TerrestrialAddress, {0x0f,0x1c});
    if (!result)
        result = Write(TerrestrialAddress, {3,0x90});
    if (result)
        return result;
    const auto pll = SatellitePll(frequency_khz);
    const std::initializer_list<std::uint8_t> operations[] = {
        {0xfe,0xc0,2,pll[2],0}, {0xfe,0xc0,0x19,0x60}, {0xfe,0xc0,0x20,0},
        {0xfe,0xc0,0x0e,0x6f}, {0x17,0}, {0x1c,0xaa}, {0x1f,0xaa},
        {0x1c,0x0a}, {0xfe,0xc0,4,0}, {0xfe,0xc0,0,pll[0],pll[1],pll[2],pll[3]},
        {0xfe,0xc0,2,Byte(pll[2] | 2)}, {3,1}, {0x1c,0x0a}, {0x1f,0x88},
        {0x8f,Byte(tsid >> 8),Byte(tsid)},
    };
    for (const auto& operation : operations)
        if ((result = Write(SatelliteAddress, operation)))
            return result;
    return 0;
}

int Frontend::Locked(bool& locked)
{
    locked = false;
    std::uint8_t data[2]{};
    if (!satellite_) {
        const std::uint8_t reg = 0x96;
        const int result = io_.I2c(TerrestrialAddress, &reg, 1, data, 1);
        locked = !result && (data[0] & 0xe0);
        return result;
    }
    const std::uint8_t status = 0xc3, stream = 0xe6;
    int result = io_.I2c(SatelliteAddress, &status, 1, data, 1);
    if (result || (data[0] & 0xf0))
        return result;
    result = io_.I2c(SatelliteAddress, &stream, 1, data, 2);
    locked = !result && ((data[0] << 8) | data[1]) == tsid_;
    return result;
}

int Frontend::Tune(bool satellite, std::uint32_t frequency_khz, std::uint32_t tsid)
{
    if (satellite ? frequency_khz < 950000 || frequency_khz > 2150000 || tsid > 0xffff
                  : frequency_khz < 473000 || frequency_khz > 770000)
        return -EINVAL;
    satellite_ = satellite;
    tsid_ = static_cast<std::uint16_t>(tsid);
    int result = satellite ? Satellite(frequency_khz, tsid_) : Terrestrial(frequency_khz);
    for (unsigned i = 0; !result && i < 35; ++i) {
        bool locked = false;
        result = Locked(locked);
        if (result || locked)
            return result;
        result = io_.Delay(50);
    }
    return result ? result : -ENOLINK;
}

}
