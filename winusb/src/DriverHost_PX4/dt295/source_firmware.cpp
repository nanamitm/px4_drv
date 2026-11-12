// SPDX-License-Identifier: GPL-2.0-only
#include "source_firmware.hpp"
#include "frontend_init.hpp"
#include "../resource.h"
#include "msg.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cerrno>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace px4::dt295 {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t Boot = 0xe9000, BootSize = 0x3910, Hook = 0xea6fc;
constexpr uint32_t Stage = 0xc4620, Resident = 0xe1060, Adapter = 0xe1000;
constexpr uint32_t Protect = 0xfd814c, EndpointControl = 0xfdbc14, EndpointBuffer = 0xfdbd08;
// 192 KiBのリングは地上波で約51 ms、CSで約29 ms分しかなく、SERVICEの停止で上書きが起きた。
// torne (MB86H47) と同じ6 MiBにする。192バイトのレコードと512バイトの送出単位の倍数で、
// 0x8138ec00〜0x8198ebffは受信中もTS以外に書き込まれないことを確認している。
// ファームウェアのRING_SIZE (direct_pump.c / bulk_pump.c) と一致させる。
constexpr uint32_t Ring = 0x8138ec00, RingSize = 0x600000, Run = 0x01902010, Clock = 0x01900404;
constexpr uint8_t Gr = 0x10, Sat = 0x11;
constexpr char BootDigest[] = "0bdf3af8c2d4ab49edd69740bd751e698e337e2df542ac4cedcbe4ba99c3e9b0";

struct Register { uint32_t address; unsigned width; uint32_t value; };
constexpr Register Registers[] = {
    {0x01900000,4,0x20000}, {0x01900004,4,0x1100},
    {0x01901040,4,0}, {0x01901064,4,0}, {0x019010a0,4,0}, {0x019010a4,4,0},
    {0x019010a8,4,0}, {0x01901000,2,0x600}, {0x01901020,4,0x02000000},
    {0x01901004,4,0x04000000}, {0x01901002,2,0}, {0x01901044,4,0x00bc0000},
    {0x01901048,4,0x47000000}, {0x0190104c,4,0x00ffffff}, {0x01901050,4,0},
    {0x01901060,4,0}, {0x01901080,4,0x01000000}, {0x01901088,4,0},
    {0x0190108c,4,0xffffffff}, {0x01901084,4,0}, {0x01901090,4,0x01000000},
    {0x01902020,4,Ring & 0x7fffffff}, {0x01902024,4,RingSize},
    {0x01902028,4,0}, {0x0190202c,4,0}, {0x01902030,4,0}, {0x01902000,4,0x03020300},
};

uint32_t Be(const uint8_t *bytes, size_t count = 4)
{
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

Bytes Encode(uint32_t value, unsigned width = 4)
{
    Bytes bytes(width);
    for (unsigned i = width; i; --i) {
        bytes[i - 1] = static_cast<uint8_t>(value);
        value >>= 8;
    }
    return bytes;
}

std::string Digest(const Bytes &bytes)
{
    std::array<uint8_t, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
}

int SourceFirmware::Failure(const char *stage, int code) const
{
    msg_err("PIX-DT295 %s: %s failed (result=%d)\n", direct_ ? "direct-fr" : "source-fr", stage, code);
    return code;
}

int SourceFirmware::LoadEmbedded()
{
    const auto module = GetModuleHandleW(nullptr);
    const auto id = direct_ ? IDR_DT295_DIRECT_FIRMWARE : IDR_DT295_SOURCE_FIRMWARE;
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource)
        return Failure("embedded bundle", -ENOENT);
    const auto handle = LoadResource(module, resource);
    const auto *bytes = handle ? static_cast<const uint8_t *>(LockResource(handle)) : nullptr;
    const auto size = SizeofResource(module, resource);
    if (!bytes || size < 16 || size > 32768)
        return Failure("bundle size", -EINVAL);
    // DTP3/4はDriverHostの組み込み初期化列を使う。
    if (Be(bytes) != (direct_ ? 0x44545033u : 0x44545034u))
        return Failure("bundle backend magic", -EINVAL);

    const std::array sections{&resident_, &adapter_, &hook_};
    // 本体は0x000e1060からresident.ldの上限0x000e2000まで。
    const size_t limits[] = {0xfa0, 0x60, 8};
    size_t offset = 4 + 4 * sections.size();
    if (size < offset)
        return Failure("bundle size", -EINVAL);
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto count = Be(bytes + 4 + 4 * i);
        if (!count || count > limits[i] || count > size - offset)
            return Failure("bundle section size", -EINVAL);
        sections[i]->assign(bytes + offset, bytes + offset + count);
        offset += count;
    }
    if (offset != size || hook_ != Bytes{0x9b,0xec,0x10,0,0x97,0x0c,0x9f,0xa0})
        return Failure("bundle hook or trailing data", -EINVAL);
    for (auto *section : {&resident_, &adapter_})
        section->resize((section->size() + 3) & ~size_t(3));
    bool satellite = false;
    for (size_t i = 0; i < FrontendInit.size();) {
        if (FrontendInit.size() - i < 2)
            return Failure("frontend initializer header", -EINVAL);
        const auto op = FrontendInit[i++], n = FrontendInit[i++];
        if (!n || n > 8 || n > FrontendInit.size() - i ||
            (op != 0 && op != 2 && op != 128 && op != 255) ||
            (op == 128 && n != 4) || (op == 255 && n != 1) ||
            (satellite && (op == 0 || op == 128)))
            return Failure("frontend initializer record", -EINVAL);
        satellite |= op == 2;
        i += n;
    }
    return satellite ? 0 : Failure("missing satellite initializer", -EINVAL);
}

void SourceFirmware::Control(uint32_t address, uint8_t *bytes, size_t count, bool read, uint8_t selector)
{
    const auto rc = io_.control(io_.opaque, read ? 0xc0 : 0x40, 0xbc,
        static_cast<uint16_t>((address >> 8 & 0xff00) | selector), static_cast<uint16_t>(address),
        bytes, static_cast<uint16_t>(count), 1000);
    if (rc)
        throw rc;
}

SourceFirmware::Bytes SourceFirmware::Read(uint32_t address, size_t count)
{
    Bytes result(count);
    for (size_t i = 0; i < count; i += 64)
        Control(address + static_cast<uint32_t>(i), result.data() + i, (std::min)(count - i, size_t(64)), true);
    return result;
}

void SourceFirmware::Write(uint32_t address, const Bytes &bytes, size_t chunk, bool verify)
{
    for (size_t i = 0; i < bytes.size(); i += chunk)
        Control(address + static_cast<uint32_t>(i), const_cast<uint8_t *>(bytes.data() + i),
                (std::min)(bytes.size() - i, chunk), false);
    if (verify && Read(address, bytes.size()) != bytes) {
        msg_err("PIX-DT295 %s: RAM readback mismatch at %08x\n", direct_ ? "direct-fr" : "source-fr", address);
        throw -EIO;
    }
}

SourceFirmware::Bytes SourceFirmware::MappedRead(uint32_t address, size_t count)
{
    constexpr uint32_t window = 0xe9ebc;
    const auto stock = Encode(0xf00000);
    if (count > 0x20000 - (address & 0x1ffff) || Read(window, 4) != stock)
        throw -EPROTO;
    Bytes bytes;
    try {
        Write(window, Encode(address & ~0x1ffffu), 4, true);
        bytes = Read(0x80000 + (address & 0x1ffff), count);
    } catch (...) {
        Write(window, stock, 4, true);
        throw;
    }
    Write(window, stock, 4, true);
    return bytes;
}

void SourceFirmware::MappedWrite(uint32_t address, const Bytes &bytes)
{
    constexpr uint32_t window = 0xea3d8;
    const auto stock = Encode(0xf00000);
    if (bytes.size() > 0x20000 - (address & 0x1ffff) || Read(window, 4) != stock)
        throw -EPROTO;
    try {
        Write(window, Encode(address & ~0x1ffffu), 4, true);
        Write(0x80000 + (address & 0x1ffff), bytes, 64, false);
    } catch (...) {
        Write(window, stock, 4, true);
        throw;
    }
    Write(window, stock, 4, true);
}

uint32_t SourceFirmware::Get(uint32_t address, unsigned width)
{
    return Be(MappedRead(address, width).data(), width);
}

void SourceFirmware::Put(uint32_t address, uint32_t value, unsigned width)
{
    MappedWrite(address, Encode(value, width));
}

void SourceFirmware::Checked(uint32_t address, uint32_t value, unsigned width)
{
    Put(address, value, width);
    if (Get(address, width) != value) {
        msg_err("PIX-DT295 %s: MMIO readback mismatch at %08x\n", direct_ ? "direct-fr" : "source-fr", address);
        throw -EIO;
    }
}

void SourceFirmware::Save(uint32_t address, size_t count)
{
    direct_saved_.push_back({address, Read(address, count)});
}

void SourceFirmware::Delay(unsigned ms)
{
    io_.delay_ms(io_.opaque, ms);
}

SourceFirmware::Bytes SourceFirmware::Exchange(unsigned op, const Bytes &payload, uint32_t argument)
{
    Bytes request;
    for (auto word : {0x50494631u, ++sequence_, op, 1u, op == 3 ? static_cast<uint32_t>(payload.size()) : argument, 0u}) {
        const auto bytes = Encode(word);
        request.insert(request.end(), bytes.begin(), bytes.end());
    }
    request.insert(request.end(), payload.begin(), payload.end());
    request.resize(64);
    Control(Stage, request.data(), request.size(), false, 0x7e);
    auto reply = Read(Stage, 64);
    const auto length = Be(&reply[20]);
    const bool expected_length = op == 3 || op == 5 ? length >= 8 && length <= 40 :
        length == (op == 1 ? 16u : op == 4 ? (direct_ ? 24u : 20u) : 0u);
    if (Be(reply.data()) != 0x50494631 || Be(&reply[4]) != sequence_ ||
        Be(&reply[8]) != (op | 0x80000000) || Be(&reply[12]) || Be(&reply[16]) != 1 ||
        !expected_length || std::any_of(reply.begin() + 24 + length, reply.end(), [](auto b) { return b; }))
        throw -EPROTO;
    return reply;
}

// BC制御転送は1回約3 msかかり、1バイトずつのFIFO操作では受信中のSERVICEを圧迫する。
// PIF1の操作5で最大32バイトを1往復で移す。要求のword 4は方向 (1=読出し、2=書込み) とバイト数。
int SourceFirmware::CardFifo(bool write, uint8_t *data, size_t &count)
{
    const size_t requested = count;
    count = 0;
    if (!ready_ || !data || !requested || requested > CardChunk)
        return -EINVAL;
    try {
        const Bytes payload = write ? Bytes(data, data + requested) : Bytes{};
        const auto reply = Exchange(5, payload, (write ? 2u : 1u) << 8 | static_cast<uint32_t>(requested));
        const auto result = Be(&reply[24]), moved = Be(&reply[28]), length = Be(&reply[20]);
        if (result || moved > requested || (write && moved != requested) || length != 8 + (write ? 0 : moved)) {
            msg_err("PIX-DT295 %s: card FIFO result=%u moved=%u/%zu\n", direct_ ? "direct-fr" : "source-fr",
                result, moved, requested);
            return -EIO;
        }
        if (!write)
            std::copy_n(reply.begin() + 32, moved, data);
        count = moved;
        return 0;
    } catch (int error) {
        return error;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

SourceFirmware::Bytes SourceFirmware::I2c(uint8_t address, std::initializer_list<uint8_t> tx, unsigned rx)
{
    return I2c(address, tx.begin(), tx.size(), rx);
}

SourceFirmware::Bytes SourceFirmware::I2c(uint8_t address, const uint8_t *tx, size_t size, unsigned rx)
{
    Bytes ops{address, static_cast<uint8_t>(size), static_cast<uint8_t>(rx)};
    ops.insert(ops.end(), tx, tx + size);
    return DirectI2c(ops, 1, rx);
}

SourceFirmware::Bytes SourceFirmware::DirectI2c(const Bytes &ops, unsigned count, unsigned read)
{
    const auto reply = Exchange(3, ops);
    const auto result = Be(&reply[24]), completed = Be(&reply[28]);
    if (result || completed != count || Be(&reply[20]) != 8 + read) {
        msg_err("PIX-DT295 %s: I2C result=%u completed=%u/%u\n", direct_ ? "direct-fr" : "source-fr", result, completed, count);
        throw result == 2 ? -ETIMEDOUT : result == 4 ? -ENXIO : -EIO;
    }
    return Bytes(reply.begin() + 32, reply.begin() + 32 + read);
}

void SourceFirmware::InitializeFrontend()
{
    Bytes batch;
    unsigned pending = 0;
    const auto flush = [&] {
        if (pending) {
            DirectI2c(batch, pending, 0);
            batch.clear();
            pending = 0;
        }
    };
    for (size_t i = 0; i < FrontendInit.size();) {
        const auto op = FrontendInit[i++], n = FrontendInit[i++];
        const auto data = FrontendInit.data() + i;
        if (op == 255) {
            flush();
            Delay(data[0]);
        } else if (op == 128) {
            flush();
            Field(data[1], data[2], data[3]);
        } else if (direct_) {
            if (batch.size() + 3 + n > 40)
                flush();
            batch.insert(batch.end(), {op == 2 ? Sat : Gr, n, 0});
            batch.insert(batch.end(), data, data + n);
            ++pending;
        } else {
            I2c(op == 2 ? Sat : Gr, data, n, 0);
        }
        i += n;
    }
    flush();
}

void SourceFirmware::InstallPif()
{
    Save(Resident, resident_.size());
    Save(Adapter, adapter_.size());
    Save(Hook, hook_.size());
    Write(Resident, resident_, 4, true);
    Write(Adapter, adapter_, 4, true);
    Write(Hook, hook_, 8, true);
    const auto caps = Exchange(1);
    if (Be(&caps[24]) != 1u || Be(&caps[28]) != 40 ||
        Be(&caps[32]) != 40 || Be(&caps[36]) != 0x06b8103b)
        throw -EPROTO;
    Exchange(2);
}

uint32_t SourceFirmware::DirectReg(uint32_t address, unsigned width)
{
    return Be(Read(address, width).data(), width);
}

void SourceFirmware::SetDirectReg(uint32_t address, uint32_t value, unsigned width)
{
    Write(address, Encode(value, width), 4, false);
}

void SourceFirmware::EnableTsSystem()
{
    Save(0xf10118, 2);
    Save(0xf1011c, 2);
    Save(0xf001f0, 4);
    Write(0xf10118, {0xff,0xff}, 4, true);
    Write(0xf1011c, Encode((0x479f & 0xfa1f) | 0x580, 2), 4, true);
    Write(0xf001f0, Encode(0x1a000000), 4, true);
}

void SourceFirmware::PrepareDirectI2c()
{
    const auto save = [this](uint32_t address, unsigned width = 4) {
        i2c_saved_.push_back({address, Read(address, width)});
        return Be(i2c_saved_.back().bytes.data(), width);
    };
    const auto memory = save(0xfd0090, 2);
    SetDirectReg(0xfd0090, memory | 2, 2);
    const auto start = io_.now_ms(io_.opaque);
    while (DirectReg(0xfd0090, 2) & 0x10)
        if (io_.now_ms(io_.opaque) - start >= 2000)
            throw -ETIMEDOUT;
    SetDirectReg(0xfd0090, (DirectReg(0xfd0090, 2) & ~2u) | 1u, 2);

    // 主CPUからI2Cを使う場合も、チップ側のクロックと停止状態を確認する。
    const auto power = Get(0x01600000), reset = Get(0x01600004), clock = Get(0x01600404);
    if (power != 0x000200ff || reset != 0x01000000 ||
        ((DirectReg(0xf10004, 2) >> 10) & 3) != 2)
        throw -EPROTO;
    board_saved_ = {{0x01600404, Encode(clock)}, {0x01600000, Encode(power)}, {0x01600004, Encode(reset)}};
    Checked(0x01600404, clock | 0x40000);
    Put(0x01600004, 0xffffffff);
    Put(0x01600000, 0xffffffff);
    Put(0x01600004, 0x00800000);
    if (Get(0x01600004) != 0x00800000 || !(Get(0x01600000) & 0x20000))
        throw -EIO;

    // USBクロックの既存ビットを維持し、I2Cブロックだけを有効にする。
    SetDirectReg(0xf10014, 0xd082, 2);
    SetDirectReg(0xf1001c, (save(0xf1001c, 2) & ~0x5080u) | 2, 2);
    SetDirectReg(0xf10114, save(0xf10114) | 0x00200000);
    SetDirectReg(0xfd8010, save(0xfd8010) | 0x10);
    SetDirectReg(0xfd8014, save(0xfd8014) & ~0x10u);
    SetDirectReg(0xfd80b0, save(0xfd80b0) | 1);
    for (const auto &reg : {Register{0xfd80c0,4,0}, Register{0xfd8ff8,4,6},
                            Register{0xfd8ff4,4,0x10}, Register{0xfd8fe8,4,0x3c},
                            Register{0xfd8fe4,4,0x42}}) {
        save(reg.address);
        SetDirectReg(reg.address, reg.value);
    }
    if ((DirectReg(0xfd8fe4) & ~1u) != 0x42 ||
        DirectReg(0xfd8ff8) != 6 || DirectReg(0xfd8ff4) != 0x10)
        throw -EIO;
}

void SourceFirmware::SetEndpoint(uint32_t group, uint32_t address, uint32_t value)
{
    try {
        SetDirectReg(Protect, group);
        SetDirectReg(address, value);
    } catch (...) {
        SetDirectReg(Protect, 15);
        throw;
    }
    SetDirectReg(Protect, 15);
}

void SourceFirmware::StartEndpoint()
{
    if (DirectReg(Protect) != 15)
        throw -EPROTO;
    endpoint_mode_ = DirectReg(0xfdbc04);
    endpoint_enable_ = DirectReg(EndpointControl);
    endpoint_buffer_ = DirectReg(EndpointBuffer);
    endpoint_saved_ = true;
    SetEndpoint(1, 0xfdbc04, 0x85000000);
    SetEndpoint(8, EndpointBuffer, 1);
    SetEndpoint(6, EndpointBuffer, 0x00ff6000);
    SetEndpoint(6, EndpointBuffer, DirectReg(EndpointBuffer));
    const auto enable = endpoint_enable_ & 0xfffdfffdu;
    SetEndpoint(2, EndpointControl, enable);
    SetEndpoint(8, EndpointControl, enable);
}

void SourceFirmware::StopEndpoint()
{
    if (!endpoint_saved_)
        return;
    SetEndpoint(8, EndpointBuffer, 1);
    SetEndpoint(6, EndpointBuffer, endpoint_buffer_);
    SetEndpoint(2, EndpointControl, endpoint_enable_);
    SetEndpoint(8, EndpointControl, endpoint_enable_);
    SetEndpoint(1, 0xfdbc04, endpoint_mode_);
    if (DirectReg(Protect) != 15)
        throw -EIO;
    endpoint_saved_ = false;
}

int SourceFirmware::Start(dt295_io io)
{
    io_ = io;
    const char *stage = "boot validation";
    try {
        if (Read(0x82008, 2) != Bytes(2))
            throw -EPROTO;
        boot_ = Read(Boot, BootSize);
        if (Digest(boot_) != BootDigest || Read(0xf10014, 2) != Bytes(2) || Read(0xf10018, 2) != Bytes(2) ||
            Read(0xf10118, 2) != Bytes{0x90,0} || Read(0xf1011c, 2) != Bytes{0x47,0x9f} ||
            Read(0xf001f0, 4) != Bytes(4))
            throw -EPROTO;
        Save(Stage, 64); // BCブロック書込みの作業領域は最後に復元する。
        Save(0xf10014, 2);
        Save(0xf10018, 2);
        stage = "demodulator reset";
        Write(0xf10014, {0,2}, 4, true);
        Write(0xf10018, {0,0}, 4, true);
        Delay(20);
        Write(0xf10018, {0,2}, 4, true);
        Delay(5);
        stage = "main CPU I2C setup";
        EnableTsSystem();
        PrepareDirectI2c();
        stage = "PIF1 install";
        InstallPif();
        stage = "frontend initialization";
        InitializeFrontend();
        stage = "TS engine snapshot";
        if (Get(Run))
            throw -EBUSY;
        clock_ = Get(Clock);
        // リングのSDRAMは保存しない。BC転送は64バイト単位に限られ、6 MiBの保存と復元に各約5分かかる。
        // 電源投入直後の内容は規則的な模様で、Startの照合も次のDMAも参照しない。
        clock_changed_ = true;
        Checked(Clock, clock_ | 0x40000);
        for (const auto &reg : Registers)
            ts_saved_.push_back({reg.address, Encode(Get(reg.address, reg.width), reg.width)});
        ready_ = true;
        msg_info("PIX-DT295: %s ready\n", direct_ ? "direct-fr PIF1" : "source-fr PIF1 raw TS");
        return 0;
    } catch (int error) {
        return Failure(stage, error);
    } catch (const std::bad_alloc &) {
        return Failure(stage, -ENOMEM);
    }
}

void SourceFirmware::Reg(uint8_t address, uint8_t reg, uint8_t value)
{
    I2c(address, {reg, value});
}

void SourceFirmware::Rf(uint8_t address, uint8_t reg, uint8_t value)
{
    I2c(address, {0xfe, 0xc0, reg, value});
}

void SourceFirmware::Field(uint8_t reg, uint8_t mask, uint8_t value)
{
    I2c(Gr, {0xfe, 0xc0, 0xfb, reg});
    Rf(Gr, reg, (I2c(Gr, {0xfe, 0xc1}, 1)[0] & mask) | value);
}

void SourceFirmware::TerrestrialSleep()
{
    Reg(Gr, 0x25, 0);
    Reg(Gr, 0x23, 0x4d);
    Reg(Gr, 3, 0x90);
    Rf(Gr, 0x12, 0);
    Rf(Gr, 0x0b, 0);
}

void SourceFirmware::SatelliteSleep()
{
    constexpr uint8_t values[][2] = {{0x1c,0xaa},{0x1f,0x2a},{0x1e,0x28},{0x0a,0},{3,1},{0x17,1}};
    for (const auto &pair : values)
        Reg(Sat, pair[0], pair[1]);
    Reg(Sat, 0x1e, (I2c(Sat, {0x1e}, 1)[0] & 0xcf) | 0x30);
    constexpr uint8_t rf[][2] = {{0x2a,0xb9},{0x29,0x70},{0x28,0x3e},{0x2a,0x19},{0x1c,0}};
    for (const auto &pair : rf)
        Rf(Sat, pair[0], pair[1]);
    I2c(Sat, {0xfe,0xc0,0x11,0xc0,0xa7});
}

void SourceFirmware::TerrestrialTune(uint32_t khz)
{
    Rf(Gr, 0x0b, 1);
    Rf(Gr, 0x12, 1);
    Delay(20);
    constexpr uint8_t mute[][2] = {{3,0},{0x1d,0xaa},{0x1e,0xaa},{0x1f,0xaa},{0x25,0},{0x23,0x4d}};
    for (const auto &pair : mute)
        Reg(Gr, pair[0], pair[1]);
    const auto divider = khz * 64 / 1000;
    Field(0x0d, 0xfe, 0);
    Rf(Gr, 0x0f, 0x20);
    Rf(Gr, 0x10, static_cast<uint8_t>(divider));
    Rf(Gr, 0x11, static_cast<uint8_t>(divider >> 8));
    Rf(Gr, 0, 1);
    Rf(Gr, 0x31, khz < 700000 ? 0x81 : 0x91);
    Rf(Gr, 0, 0);
    Rf(Gr, 0x7c, khz < 700000 ? 0x1f : 0x9f);
    Rf(Gr, 0x12, 0);
    Field(0x6e, 0xff, 0x80);
    Field(0x14, 0xdf, 0);
    Rf(Gr, 0x12, 1);
    Delay(20);
    Field(0x6e, 0x7f, 0);
    Reg(Gr, 0x0e, (I2c(Gr, {0x0e}, 1)[0] & 0x70) | 1);
    Reg(Gr, 0x0f, 0x10);
    Reg(Gr, 1, 0x40);
    Reg(Gr, 0x23, 0x4c);
    Delay(100);
    Reg(Gr, 0x1d, 2);
    Reg(Gr, 0x1e, 0x2a);
    Reg(Gr, 0x1f, 0x8a);
}

void SourceFirmware::SatelliteTune(uint32_t khz, uint16_t tsid)
{
    Reg(Sat, 0x1e, (I2c(Sat, {0x1e}, 1)[0] & 0xcf) | 0x20);
    I2c(Sat, {0xfe,0xc0,0x11,0,0xa7});
    Rf(Sat, 0x2a, 0xb9);
    Rf(Sat, 0x1c, 0xc0);
    Rf(Sat, 0x29, 0x71);
    constexpr uint8_t setup[][2] = {{0x17,0},{0x1e,0x20},{0x0a,0xff},{3,1},
        {0x1c,0xaa},{0x1f,0x2a},{0x8f,0},{0x90,0},{0x1e,0x28},{0x0a,0},{3,1}};
    for (const auto &pair : setup)
        Reg(Sat, pair[0], pair[1]);

    const auto rounded = (khz + 500) / 1000 * 1000;
    const bool low = rounded <= 1100000;
    const auto divider = (rounded * (low ? 2 : 1) + 500) / 1000;
    struct Band { uint32_t upper; uint8_t band, mode; };
    constexpr Band bands[] = {{975000,0x1c,1},{1050000,0x18,2},{1150000,0x14,2},
        {1250000,0x10,3},{1350000,0x0c,4},{1450000,0x0a,4},{1600000,7,5},
        {1800000,4,2},{2000000,2,1},{2151000,0,0}};
    const auto band = std::find_if(std::begin(bands), std::end(bands), [rounded](const Band &b) {
        return rounded < b.upper;
    });
    if (band == std::end(bands))
        throw -EINVAL;
    I2c(Sat, {0xfe,0xc0,0,static_cast<uint8_t>(divider >> 7),
        static_cast<uint8_t>(divider * 2),0,0,static_cast<uint8_t>(low ? 0x80 : 0)});
    Rf(Sat, 9, (band->mode << 5) | band->band);
    Rf(Sat, 0x37, 0xac);
    Rf(Sat, 5, 0x80);
    Rf(Sat, 0x2a, 0xbb);
    Delay(10);
    Reg(Gr, 0x0e, 0x77);
    Reg(Gr, 0x0f, 0x77);
    Reg(Sat, 7, (I2c(Sat, {7}, 1)[0] & 0x70) | 1);
    Reg(Sat, 8, 0x10);
    Rf(Sat, 0x50, (I2c(Sat, {0xfe,0xc1}, 2)[1] & 0x60) == 0x60 ? 0xf1 : 0xf0);
    Delay(40);
    Reg(Sat, 0x1e, 0x20);
    Reg(Sat, 0x0a, 0xff);
    Reg(Sat, 3, 1);
    Reg(Sat, 3, 1);
    Delay(100);
    Reg(Sat, 0x1c, 8);
    Reg(Sat, 0x1f, 8);
    Reg(Sat, 0xa5, I2c(Sat, {0xa5}, 1)[0] | 0x40);
    Reg(Sat, 0xa3, (I2c(Sat, {0xa3}, 1)[0] & 0x88) | 0x11);
    Reg(Sat, 0x8f, static_cast<uint8_t>(tsid >> 8));
    Reg(Sat, 0x90, static_cast<uint8_t>(tsid));
}

bool SourceFirmware::IsLocked()
{
    if (satellite_) {
        const auto status = I2c(Sat, {0xc3}, 1)[0];
        return !(status & 0x10) && Be(I2c(Sat, {0xe6}, 2).data(), 2) == tsid_;
    }
    const auto status = I2c(Gr, {0x80}, 4)[0];
    return !(status & 0x28) && (I2c(Gr, {0xb0}, 1)[0] & 15) >= 8;
}

int SourceFirmware::Tune(bool satellite, uint32_t khz, uint32_t tsid)
{
    if (!ready_ || ring_running_ ||
        (satellite ? khz < 950000 || khz > 2150000 || tsid > 0xffff
                   : khz < 473000 || khz >= 700000))
        return -EINVAL;
    tuned_ = false;
    try {
        if (satellite) {
            if (!satellite_)
                TerrestrialSleep();
            satellite_ = true;
            SatelliteTune(khz, static_cast<uint16_t>(tsid));
        } else {
            if (satellite_)
                SatelliteSleep();
            satellite_ = false;
            TerrestrialTune(khz);
        }
        tsid_ = tsid;
        const auto start = io_.now_ms(io_.opaque);
        do {
            if (IsLocked()) {
                tuned_ = true;
                if (satellite)
                    msg_info("PIX-DT295 %s: locked system=ISDB-S IF_kHz=%u TSID=%u\n", direct_ ? "direct-fr" : "source-fr", khz, tsid);
                else
                    msg_info("PIX-DT295 %s: locked system=ISDB-T IF_kHz=%u\n", direct_ ? "direct-fr" : "source-fr", khz);
                return 0;
            }
            Delay(100);
        } while (io_.now_ms(io_.opaque) - start < 4000);
        // 無信号・TSID不一致は機器の異常ではない。USB転送の期限切れ(-ETIMEDOUT)と区別し、次の選局を受け付ける。
        msg_info("PIX-DT295 %s: no lock system=%s IF_kHz=%u\n", direct_ ? "direct-fr" : "source-fr", satellite ? "ISDB-S" : "ISDB-T", khz);
        return -ENOLINK;
    } catch (int error) {
        return Failure("frontend tune", error);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int SourceFirmware::Locked(int &locked)
{
    // TuneでロックとTSIDを検証済み。受信中のEP0はFIFO補充に使う。
    locked = tuned_;
    return 0;
}

int SourceFirmware::ReadCn(bool &satellite, uint32_t &raw)
{
    if (!ready_ || !tuned_)
        return -EINVAL;
    try {
        satellite = satellite_;
        // TC90532XBGはTC90522系と同じCN監視レジスターを持つ。地デジは0x8bから3 bytes、衛星は0xbcから2 bytes。
        raw = satellite_ ? Be(I2c(Sat, {0xbc}, 2).data(), 2) : Be(I2c(Gr, {0x8b}, 3).data(), 3);
        return 0;
    } catch (int error) {
        return Failure("CN read", error);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int SourceFirmware::Capture(bool enable)
{
    try {
        if (!enable) {
            if (ring_services_)
                msg_info("PIX-DT295: TS ring services=%llu peak_pending=%u ring=%u high=%llu\n",
                    static_cast<unsigned long long>(ring_services_), ring_peak_, RingSize,
                    static_cast<unsigned long long>(ring_high_));
            ring_services_ = ring_high_ = 0;
            ring_peak_ = 0;
            StopRing();
            StopEndpoint();
            return 0;
        }
        if (!ready_ || !tuned_ || ring_running_)
            return -EINVAL;
        const int rc = mb86H_stream_input_init(&input_, MB86H_STREAM_FORMAT_RAW_TS, 0, direct_ ? MB86H_TS_PACKET_SIZE : MB86H_TS_RECORD_BYTES);
        if (rc)
            return rc;

        ts_changed_ = true;
        Put(0x01900004, 0x1f11ff);
        Put(0x01900000, 0x3002ffff);
        Put(0x01900000, 0x20000);
        Put(0x01900004, 0x1100);
        for (size_t i = 2; i < std::size(Registers); ++i)
            Checked(Registers[i].address, Registers[i].value, Registers[i].width);
        Put(0x01901000, 0x607, 2);
        Put(0x01901040, 0);
        Put(0x01901020, 0);
        Put(0x01901040, 0x03000000);
        Put(0x019010a4, 0x13170400);
        cursor_ = 0;
        stream_phase_ = 0xffffffff;
        if (direct_)
            StartEndpoint();
        ring_running_ = true;
        Put(Run, 0x02000000);
        return 0;
    } catch (int error) {
        return Failure("TS engine", error);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

// DMAはSERVICEの外でもリングへ書き続ける。応答の生成位置と送出後の消費位置の差を未送信量とし、
// 最大値と、リングの3/4を超えた回数を記録してドロップの時刻と照合できるようにする。
// 溢れた場合は周回後の小さな値に見えるため、超過の回数と最大値を合わせて読む。
void SourceFirmware::RecordRing(uint32_t producer)
{
    if (producer >= RingSize)
        return;
    const auto pending = producer >= cursor_ ? producer - cursor_ : RingSize - cursor_ + producer;
    ++ring_services_;
    last_pending_ = pending;
    ring_peak_ = (std::max)(ring_peak_, pending);
    if (pending > RingSize / 4 * 3 && !ring_high_++)
        msg_warn("PIX-DT295: TS ring high pending=%u ring=%u cursor=%u producer=%u\n", pending, RingSize, cursor_, producer);
}

int SourceFirmware::Service()
{
    try {
        // 主CPU版は1回の往復で最大2048ブロック (1 MiB) を送る。往復の間は機器側の送出が止まり、
        // 512ブロックでは取り出しが約4.7 MB/秒でCSのレコードの生成量 (約4.87 MB/秒) に届かなかった。
        const unsigned budget = direct_ ? 2048 : 256;
        auto payload = Encode(cursor_);
        const auto amount = Encode(budget);
        payload.insert(payload.end(), amount.begin(), amount.end());
        if (direct_) {
            const auto phase = Encode(stream_phase_);
            payload.insert(payload.end(), phase.begin(), phase.end());
        }
        const auto reply = Exchange(4, payload);
        const auto next = Be(&reply[24]), count = Be(&reply[28]), result = Be(&reply[40]);
        if (result || count > budget || next >= RingSize) {
            msg_err("PIX-DT295: PIF1 pump result=%u cursor=%u sent=%u producer=%u FIFO=%08x\n",
                    result, next, count, Be(&reply[32]), Be(&reply[36]));
            return Failure("PIF1 Bulk pump response", -EPROTO);
        }
        if (direct_) {
            const auto state = Be(&reply[44]);
            if (state == 0xffffffff) {
                if (stream_phase_ != state || count || next)
                    return -EPROTO;
            } else {
                const auto offset = state >> 8;
                const auto phase = state & 255;
                const bool initial = stream_phase_ == 0xffffffff;
                const auto consumed = (initial ? 0 : stream_phase_ & 255) + count * 512;
                const auto raw = (initial ? offset : cursor_) + count * 512 + 4 * (consumed / 188);
                if (offset >= 192 || (!initial && offset != (stream_phase_ >> 8)) ||
                    phase != consumed % 188 || next != raw % RingSize)
                    return -EPROTO;
            }
            stream_phase_ = state;
        } else if (next != (cursor_ + count * 512) % RingSize) {
            return -EPROTO;
        }
        cursor_ = next;
        last_sent_ = count;
        RecordRing(Be(&reply[32]));
        return 0;
    } catch (int error) {
        return Failure("PIF1 Bulk pump", error);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

void SourceFirmware::StopRing()
{
    if (!ring_running_)
        return;
    Put(0x01901040, 0);
    Put(0x01901020, 0x02000000);
    Put(Run, 0x04000000);
    const auto start = io_.now_ms(io_.opaque);
    while (Get(Run)) {
        if (io_.now_ms(io_.opaque) - start >= 1000)
            throw -ETIMEDOUT;
    }
    ring_running_ = false;
}

void SourceFirmware::Restore()
{
    StopRing();
    StopEndpoint();
    if (ts_changed_) {
        for (auto i = ts_saved_.rbegin(); i != ts_saved_.rend(); ++i)
            MappedWrite(i->address, i->bytes);
    }
    if (clock_changed_)
        Checked(Clock, clock_);
    int sleep_error = 0;
    if (satellite_ && ready_) {
        try {
            SatelliteSleep();
        } catch (int error) {
            // RFの停止要求が失敗してもI²C設定とブート領域の復元を試みる。
            sleep_error = error;
        }
    }

    for (auto i = i2c_saved_.rbegin(); i != i2c_saved_.rend(); ++i)
        Write(i->address, i->bytes, 4, false);
    for (auto i = board_saved_.rbegin(); i != board_saved_.rend(); ++i)
        MappedWrite(i->address, i->bytes);
    // フックを先に戻し、参照先の常駐コードを後から戻す。
    for (auto i = direct_saved_.rbegin(); i != direct_saved_.rend(); ++i)
        Write(i->address, i->bytes, i->address == Hook || i->address == Stage ? i->bytes.size() : 4, true);
    if (Read(Boot, BootSize) != boot_ || Read(0x82008, 2) != Bytes(2))
        throw -EIO;
    if (sleep_error)
        throw sleep_error;
}

int SourceFirmware::Close()
{
    if (direct_saved_.empty())
        return 0;
    try {
        Restore();
        direct_saved_.clear();
        ts_saved_.clear();
        i2c_saved_.clear();
        board_saved_.clear();
        clock_changed_ = ts_changed_ = false;
        ready_ = satellite_ = tuned_ = false;
        msg_info("PIX-DT295 %s: boot RAM and TS engine restoration verified (TS ring SDRAM is not restored)\n",
            direct_ ? "direct-fr" : "source-fr");
        return 0;
    } catch (int error) {
        return Failure("restore; USB power cycle required", error);
    } catch (const std::bad_alloc &) {
        return Failure("restore; USB power cycle required", -ENOMEM);
    }
}

int SourceFirmware::Feed(const uint8_t *data, size_t size, mb86H_stream_packet_sink packet, void *ctx)
{
    return mb86H_stream_input_push(&input_, 0, data, size, packet, ctx);
}

void SourceFirmware::InputStats(uint64_t &discarded, uint64_t &desyncs) const
{
    discarded = input_.framer.discarded_bytes;
    desyncs = input_.framer.desyncs;
}
}
