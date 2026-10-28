// SPDX-License-Identifier: GPL-2.0-only
/* PX-BCUD sequences and QM1D1C004x tuning adapted from knight-rider/ptx:
 * Copyright (C) Budi Rachmanto, AreMa Inc. <info@are.ma>
 * PX-BCUD em28xx support: Copyright (C) 2016 Satoshi Nagahama.
 * QM1D1C0042 full initialization: Copyright (C) 2014 Akihiro Tsukada.
 * USB/I2C protocol follows Linux v4.5 drivers/media/usb/em28xx (GPL-2.0).
 * See docs/px-bcud-winusb.md for source revisions and validation limits.
 */
#include "frontend.hpp"
#include <cerrno>
#include <algorithm>

namespace px4::pxbcud {
int Frontend::Transfer(bool input, std::uint8_t request, std::uint16_t index,
                       std::uint8_t *data, std::size_t size)
{
    const int result = io_.Transfer(input, request, index, data, size);
    // USB 短転送を成功扱いすると、未取得の値で選局やロックを判断してしまう。
    return result < 0 ? result : (static_cast<std::size_t>(result) == size ? 0 : -EIO);
}
int Frontend::ReadRegister(std::uint8_t reg, std::uint8_t &value)
{ return Transfer(true, 0, reg, &value, 1); }
int Frontend::WriteRegister(std::uint8_t reg, std::uint8_t value)
{ return Transfer(false, 0, reg, &value, 1); }
int Frontend::UpdateRegister(std::uint8_t reg, std::uint8_t value, std::uint8_t mask)
{
    std::uint8_t previous = 0;
    int ret = ReadRegister(reg, previous);
    return ret ? ret : WriteRegister(reg, (previous & ~mask) | (value & mask));
}
int Frontend::I2c(bool input, std::uint8_t address, std::uint8_t *data,
                  std::size_t size, bool stop)
{
    if (!size || size > 64 || address > 0x7f) return -EINVAL;
    // EM28178 のバス1は R06 bit2。初期化中に R06 を書き換えるため毎回選択する。
    int ret = UpdateRegister(0x06, 0x04, 0x04);
    if (ret) return ret;
    ret = Transfer(input, (input || stop) ? 2 : 3,
                   static_cast<std::uint16_t>(address << 1), data, size);
    if (ret) return ret;
    // Linux em28xx の完了判定を維持。ACK 不応答と転送期限切れを区別する。
    for (unsigned elapsed = 0; elapsed <= 35; elapsed += 5) {
        std::uint8_t status = 0xff;
        ret = ReadRegister(0x05, status);
        if (ret) return ret;
        if (!status) return 0;
        if (status == 0x10) return -ENXIO;
        if (input || elapsed == 35)
            return (status == 2 || status == 4) ? -ETIMEDOUT : -EIO;
        if ((ret = io_.Wait(5))) return ret;
    }
    return -ETIMEDOUT;
}
int Frontend::DemodWrite(std::uint8_t reg, std::uint8_t value)
{
    std::uint8_t data[] = {reg, value};
    return I2c(false, 0x15, data, sizeof(data));
}
int Frontend::DemodRead(std::uint8_t reg, std::uint8_t *data, std::size_t size)
{
    int ret = I2c(false, 0x15, &reg, 1, false);
    return ret ? ret : I2c(true, 0x15, data, size);
}
int Frontend::TunerWrite(std::uint8_t reg, std::uint8_t value)
{
    std::uint8_t data[] = {0xfe, 0xc2, reg, value};
    int ret = I2c(false, 0x15, data, sizeof(data));
    if (!ret) registers_[reg] = value;
    return ret;
}
int Frontend::TunerRead(std::uint8_t reg, std::uint8_t &value)
{
    std::uint8_t write[] = {0xfe, 0xc2, reg};
    std::uint8_t read[] = {0xfe, 0xc3};
    int ret = I2c(false, 0x15, write, sizeof(write), false);
    if (!ret) ret = I2c(false, 0x15, read, sizeof(read), false);
    return ret ? ret : I2c(true, 0x15, &value, 1);
}
int Frontend::Agc(bool automatic)
{
    int ret = DemodWrite(0x0a, automatic ? 0xff : 0);
    if (!ret) ret = DemodWrite(0x10, automatic ? 0xb1 : 0xb0);
    if (!ret) ret = DemodWrite(0x11, automatic ? 0x40 : 0);
    return ret ? ret : DemodWrite(0x03, 1);
}
int Frontend::Initialize()
{
    awake_ = false;
    tuner_id_ = 0;
    std::uint8_t chip = 0;
    int ret = ReadRegister(0x0a, chip);
    if (ret) return ret;
    if (chip != 114) return -ENODEV;
    struct Setting { std::uint8_t reg, value, mask; unsigned wait; };
    // ptx の基板 GPIO 初期化。待機値は公開手順から維持している。
    const Setting board[] = {
        {0x0f,0x06,0xff,0}, {0x80,0xff,0xff,0}, {0x0d,0xff,0xff,0},
        {0x50,1,0xff,0}, {0x06,0x40,0xff,0}, {0x80,0xfd,0xff,100},
        {0x12,0x20,0x20,0}, {0x0d,0x42,0xff,1000},
        {0x80,0xfc,0xff,10}, {0x80,0xfd,0xff,10}, {0x73,0xfd,0xff,100}
    };
    for (const auto &s : board) {
        if ((ret = UpdateRegister(s.reg, s.value, s.mask))) return ret;
        if (s.wait && (ret = io_.Wait(s.wait))) return ret;
    }
    // tuner probe の復調器経由 I2C 設定を px_bcud_init より前に行う。
    for (const auto &pair : {std::array<std::uint8_t,2>{0x1e,0x10},
                            {0x1c,0x15}, {0x1f,0x04}})
        if ((ret = DemodWrite(pair[0], pair[1]))) return ret;
    if ((ret = WriteRegister(0x06, 0x46))) return ret;
    for (const auto &pair : {std::array<std::uint8_t,2>{0x0e,0x77},
                            {0x0f,0x77}, {0x03,0x90}}) {
        std::uint8_t data[] = {pair[0], pair[1]};
        if ((ret = I2c(false, 0x14, data, sizeof(data)))) return ret;
    }
    const std::uint8_t demod[][2] = {
        {0x07,1},{0x08,0x10},{0x13,0},{0x17,0},{0x03,1},
        {0x10,0xb1},{0x11,0x40},{0x85,0x7a},{0x87,4}
    };
    for (const auto &pair : demod)
        if ((ret = DemodWrite(pair[0], pair[1]))) return ret;
    const Setting gpio[] = {
        {0x06,0x40,0xff,300},{0x80,0xfd,0xff,60},
        {0x15,0x20,0xff,0},{0x16,0x20,0xff,0},{0x17,0x20,0xff,0},
        {0x18,0,0xff,0},{0x19,0,0xff,0},{0x1a,0,0xff,0},
        {0x23,0,0xff,0},{0x24,0,0xff,0},{0x26,0,0xff,0},
        {0x13,8,0xff,0},{0x12,0x27,0xff,0},{0x0c,0x10,0xff,0},
        {0x27,0,0xff,0},{0x10,0,0xff,0},{0x11,0x11,0xff,0},
        {0x50,1,0xff,0},{0x5f,0x80,0xff,0},{0x06,0x46,0xff,0}
    };
    for (const auto &s : gpio) {
        if ((ret = UpdateRegister(s.reg,s.value,s.mask))) return ret;
        if (s.wait && (ret = io_.Wait(s.wait))) return ret;
    }
    return Wake();
}
int Frontend::Wake()
{
    awake_ = false;
    selected_stream_=0;
    // Linux v4.8 qm1d1c0042_init に従い、電源投入時の値へ依存せず全設定を行う。
    // ptx の簡略版はレジスター表をキャッシュするだけで、多くの値を送信しない。
    int ret=0;
    for (unsigned i=0;i<3;++i) if ((ret=TunerWrite(1,0x0c))) return ret;
    if ((ret=io_.Wait(3))) return ret;
    if ((ret=TunerWrite(1,0x1c))) return ret;
    ret = TunerRead(0, tuner_id_);
    if (ret) return ret;
    // PX-BCUD で記載されている 0x68 のみ受理し、別機種へ推測で設定しない。
    if (tuner_id_ != 0x68) return -ENODEV;
    registers_ = {{0x68,0x1c,0xc0,0x10,0xbc,0xc1,0x11,0x33,
                  0x03,0,0,0,0x03,0,0,0,0,0xff,0xf3,0,
                  0x3f,0x25,0x5c,0xd6,0x55,0xcf,0x95,0xf6,0x36,0xf2,9,0}};
    if ((ret=io_.Wait(3))) return ret;
    if ((ret=TunerWrite(0x0c,registers_[0x0c]|0x40))) return ret;
    if ((ret=io_.Wait(20))) return ret;
    for (std::uint8_t reg=1;reg<=0x0c;++reg)
        if ((ret=TunerWrite(reg,registers_[reg]))) return ret;
    for (std::uint8_t reg=0x11;reg<32;++reg)
        if ((ret=TunerWrite(reg,registers_[reg]))) return ret;
    ret = DemodWrite(0x17,0);
    if (!ret) ret = TunerWrite(1, (registers_[1] | 8) & ~1);
    if (!ret) ret = TunerWrite(5, registers_[5] & ~8);
    if (!ret) ret = TunerWrite(3, registers_[3] & ~1);
    awake_ = !ret;
    return ret;
}
int Frontend::Sleep()
{
    awake_ = false;
    int ret = Capture(false);
    if (!ret) ret = Agc(false);
    if (!ret) ret = TunerWrite(5, registers_[5] | 8);
    if (!ret) ret = TunerWrite(1, (registers_[1] & ~8) | 1);
    return ret ? ret : DemodWrite(0x17,1);
}
int Frontend::Tune(std::uint32_t frequency_khz)
{
    selected_stream_=0;
    if (!awake_) return -EINVAL;
    // 公開 PLL テーブルの範囲外を他の周波数へ置換せず、明示的に拒否する。
    if (frequency_khz < 950500 || frequency_khz >= 2151500) return -EINVAL;
    const std::uint32_t f = frequency_khz - 500;
    const std::uint32_t rounded = ((f + 8000) / 16000) * 16000;
    const std::int32_t delta = static_cast<std::int32_t>(f) - static_cast<std::int32_t>(rounded);
    const std::int32_t sd = 65 * delta + (delta < 0 ? 0x400000 : 0);
    const auto n = rounded / 64000 - 3;
    const auto a = rounded / 16000 - 4 * (n + 1) - 5;
    const std::uint32_t bands[][3] = {
        {2151000,1,7},{1950000,1,6},{1800000,1,5},{1600000,1,4},
        {1450000,1,3},{1250000,1,2},{1200000,0,7},{975000,0,6},{950000,0,0}
    };
    int ret = Agc(false);
    if (ret) return ret;
    for (std::size_t i=0;i<8;++i) {
        if (f >= bands[i+1][0] && f < bands[i][0]) {
            ret = TunerWrite(2, static_cast<std::uint8_t>((registers_[2]&0x0f) |
                              (bands[i][1]<<7) | (bands[i][2]<<4)));
            if (ret) return ret;
            break;
        }
    }
    const std::uint8_t values[][2] = {
        {6,static_cast<std::uint8_t>((registers_[6]&0x40)|n)},
        {7,static_cast<std::uint8_t>((registers_[7]&0xf0)|(a&0xf))},
        {8,static_cast<std::uint8_t>((registers_[8]&0xf0)|2)},
        {9,static_cast<std::uint8_t>((registers_[9]&0xc0)|((sd>>16)&0x3f))},
        {10,static_cast<std::uint8_t>((sd>>8)&0xff)}, {11,static_cast<std::uint8_t>(sd&0xff)},
        {12,static_cast<std::uint8_t>(registers_[12]&0x3f)}
    };
    for (const auto &pair: values)
        if ((ret=TunerWrite(pair[0],pair[1]))) return ret;
    if ((ret=io_.Wait(1))) return ret;
    if ((ret=TunerWrite(12,registers_[12]|0xc0))) return ret;
    if ((ret=TunerWrite(8,9))) return ret;
    if ((ret=TunerWrite(0x13,(registers_[0x13]&0x9f)|0x20))) return ret;
    // 期限は回数ではなく経過時間を Io 側でも制限する。ここでは公開手順の待機を維持。
    for (unsigned i=0;i<500;++i) {
        std::uint8_t status=0;
        if ((ret=TunerRead(0x0d,status))) return ret;
        if (status&0x40) return Agc(true);
        if ((ret=io_.Wait(1))) return ret;
    }
    return -ETIMEDOUT;
}
int Frontend::Locked(bool &locked)
{
    locked=false;
    std::uint8_t status=0, tsid[2]{};
    int ret=DemodRead(0xc3,&status,1);
    if (!ret && !(status&0x10)) {
        ret=DemodRead(0xce,tsid,2);
        if (!ret) locked = (tsid[0] || tsid[1]) && !(tsid[0]==0xff && tsid[1]==0xff);
    }
    return ret;
}
int Frontend::SelectStream(std::uint32_t stream_id)
{
    if (stream_id>0xffff) return -EINVAL;
    std::uint8_t data[16]{};
    int ret=0;
    if (stream_id<8) {
        if ((ret=DemodRead(0xce,data,sizeof(data)))) return ret;
        stream_id=(data[stream_id*2]<<8)|data[stream_id*2+1];
    }
    if (!stream_id || stream_id==0xffff) return -EINVAL;
    if ((ret=DemodWrite(0x8f,static_cast<std::uint8_t>(stream_id>>8)))) return ret;
    if ((ret=DemodWrite(0x90,static_cast<std::uint8_t>(stream_id)))) return ret;
    for (unsigned i=0;i<50;++i) {
        if ((ret=DemodRead(0xe6,data,2))) return ret;
        if (static_cast<std::uint32_t>((data[0]<<8)|data[1])==stream_id) {
            selected_stream_=static_cast<std::uint16_t>(stream_id);
            return 0;
        }
        if ((ret=io_.Wait(20))) return ret;
    }
    return -ETIMEDOUT;
}
int Frontend::ReadCn(std::uint16_t &cn)
{
    std::uint8_t data[2]{};
    int ret=DemodRead(0xbc,data,2);
    if (!ret) cn=static_cast<std::uint16_t>((data[0]<<8)|data[1]);
    return ret;
}
int Frontend::Capture(bool enabled)
{
    // px4_drv の TC90522 TS ピン制御と同じ並列出力設定。
    // PX-BCUD の bridge は R5F bit7 を設定して並列入力として使う。
    if (!enabled) {
        // I2C の切断・故障時も bridge の停止と両方の出力ピン解除を試みる。
        const int bridge_ret=UpdateRegister(0x5f,0,1);
        const int clock_ret=DemodWrite(0x1c,0x80);
        const int pins_ret=DemodWrite(0x1f,0x22);
        return bridge_ret ? bridge_ret : clock_ret ? clock_ret : pins_ret;
    }
    int ret=DemodWrite(0x1c,0);
    if (!ret) ret=DemodWrite(0x1f,0);
    return ret ? ret : UpdateRegister(0x5f,1,1);
}
} // namespace px4::pxbcud
