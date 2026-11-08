// SPDX-License-Identifier: GPL-2.0-only
#include "source_firmware.hpp"
#include "frontend_init.hpp"
#include "msg.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace px4::dt295 {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t Boot = 0xe9000, BootSize = 0x3910, Hook = 0xea6fc;
constexpr uint32_t Stage = 0xc4620, Resident = 0xe1100, Adapter = 0xe1000;
constexpr uint32_t AuxRam = 0x017a0000, Mail = AuxRam + 0x3900;
constexpr uint32_t Cluster = 0x01700000, Vector = 0x017ffffc;
constexpr uint32_t Ring = 0x8138ec00, RingSize = 0x30000, Run = 0x01902010, Clock = 0x01900404;
constexpr uint32_t FirstRingWindow = 0x20000 - (Ring & 0x1ffff);
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
    msg_err("PIX-DT295 source-fr: %s failed (result=%d)\n", stage, code);
    return code;
}

int SourceFirmware::Load(const std::wstring &path, const std::wstring &digest, const std::wstring &frontend_reference)
{
    std::ifstream stream(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!stream)
        return Failure("open bundle", -ENOENT);
    const auto size = stream.tellg();
    if (size < 20 || size > 32768)
        return Failure("bundle size", -EINVAL);
    Bytes bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), size))
        return Failure("read bundle", -EIO);
    const auto actual = Digest(bytes);
    // DTP1は初期化列を含む。DTP2は初期化列を含まず、配布物のmAgicVISION 0.1.0から実行時に組み立てる。
    const bool runtime_init = Be(bytes.data()) == 0x44545032;
    if (actual.size() != 64 || std::wstring(actual.begin(), actual.end()) != digest ||
        (Be(bytes.data()) != 0x44545031 && !runtime_init))
        return Failure("bundle SHA-256 or magic", -EINVAL);

    Bytes *sections[] = {&auxiliary_, &resident_, &adapter_, &hook_, &init_};
    constexpr size_t limits[] = {0x2000, 0x700, 0x100, 8, 0x2000};
    const size_t count = runtime_init ? 4 : 5;
    size_t offset = 4 + 4 * count;
    if (bytes.size() < offset)
        return Failure("bundle size", -EINVAL);
    for (size_t i = 0; i < count; ++i) {
        const auto count = Be(bytes.data() + 4 + 4 * i);
        if (!count || count > limits[i] || count > bytes.size() - offset)
            return Failure("bundle section size", -EINVAL);
        sections[i]->assign(bytes.begin() + offset, bytes.begin() + offset + count);
        offset += count;
    }
    if (offset != bytes.size() || hook_ != Bytes{0x9b,0xec,0x10,0,0x97,0x0c,0x9f,0xa0})
        return Failure("bundle hook or trailing data", -EINVAL);
    if (runtime_init) {
        std::ifstream reference(std::filesystem::path(frontend_reference), std::ios::binary | std::ios::ate);
        if (!reference)
            return Failure("open frontend initialization reference", -ENOENT);
        const auto reference_size = reference.tellg();
        if (reference_size < 48 || reference_size > 1024 * 1024)
            return Failure("frontend initialization reference size", -EINVAL);
        Bytes container(static_cast<size_t>(reference_size));
        reference.seekg(0);
        if (!reference.read(reinterpret_cast<char *>(container.data()), reference_size))
            return Failure("read frontend initialization reference", -EIO);
        if (Digest(container) != FrontendInitContainerSha256)
            return Failure("frontend initialization reference SHA-256 (mAgicVISION 0.1.0 required)", -EINVAL);
        if (const int rc = BuildFrontendInit(container.data(), container.size(), init_))
            return Failure("frontend initialization from reference", rc);
    }
    for (auto *section : {&auxiliary_, &resident_, &adapter_})
        section->resize((section->size() + 3) & ~size_t(3));
    bool satellite = false;
    for (size_t i = 0; i < init_.size();) {
        if (init_.size() - i < 2)
            return Failure("frontend initializer header", -EINVAL);
        const auto op = init_[i++], n = init_[i++];
        if (!n || n > 8 || n > init_.size() - i ||
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
        msg_err("PIX-DT295 source-fr: RAM readback mismatch at %08x\n", address);
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
        msg_err("PIX-DT295 source-fr: MMIO readback mismatch at %08x\n", address);
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

SourceFirmware::Bytes SourceFirmware::Mailbox()
{
    return MappedRead(Mail, 64);
}

SourceFirmware::Bytes SourceFirmware::Exchange(unsigned op, uint32_t cursor, uint32_t budget)
{
    Bytes request;
    for (auto word : {0x50494631u, ++sequence_, op, 1u, 0u, 0u, cursor, budget}) {
        const auto bytes = Encode(word);
        request.insert(request.end(), bytes.begin(), bytes.end());
    }
    request.resize(64);
    Control(Stage, request.data(), request.size(), false, 0x7e);
    auto reply = Read(Stage, 64);
    const unsigned length = op == 1 ? 16 : op == 4 ? 20 : 0;
    if (Be(reply.data()) != 0x50494631 || Be(&reply[4]) != sequence_ ||
        Be(&reply[8]) != (op | 0x80000000) || Be(&reply[12]) || Be(&reply[16]) != 1 ||
        Be(&reply[20]) != length || std::any_of(reply.begin() + 24 + length, reply.end(), [](auto b) { return b; }))
        throw -EPROTO;
    return reply;
}

SourceFirmware::Bytes SourceFirmware::I2c(uint8_t address, std::initializer_list<uint8_t> tx, unsigned rx)
{
    return I2c(address, tx.begin(), tx.size(), rx);
}

SourceFirmware::Bytes SourceFirmware::I2c(uint8_t address, const uint8_t *tx, size_t size, unsigned rx)
{
    Bytes args{address, static_cast<uint8_t>(size), static_cast<uint8_t>(rx), 0};
    args.insert(args.end(), tx, tx + size);
    args.resize(20);
    MappedWrite(Mail + 64, args);
    Put(Mail + 20, 7);
    const auto seq = ++sequence_;
    Put(Mail + 12, seq);
    const auto start = io_.now_ms(io_.opaque);
    do {
        const auto reply = Mailbox();
        if (Be(reply.data()) == 0x50414631 && Be(&reply[4]) == 1 && Be(&reply[16]) == seq) {
            if (Be(&reply[20]) != 7 || Be(&reply[24]) || Be(&reply[28]) ||
                Be(&reply[56]) != 1 || Be(&reply[60]) != 6) {
                msg_err("PIX-DT295 source-fr: I2C address=%02x reg=%02x stage=%u state=%08x\n",
                    address, size ? tx[0] : 0, Be(&reply[60]), Be(&reply[32]));
                throw -EIO;
            }
            return rx ? MappedRead(Mail + 76, rx) : Bytes{};
        }
        Delay(10);
    } while (io_.now_ms(io_.opaque) - start < 3000);
    throw -ETIMEDOUT;
}

int SourceFirmware::Start(dt295_io io)
{
    io_ = io;
    const char *stage = "boot validation";
    try {
        if (Read(0x82008, 2) != Bytes(2))
            throw -EPROTO;
        boot_ = Read(Boot, BootSize);
        if (Digest(boot_) != BootDigest || MappedRead(Cluster, 8) != Bytes{0,2,0,0xff,0x0d,0x8f,0x7f,0xf3} ||
            Get(Vector) || Read(0xf10014, 2) != Bytes(2) || Read(0xf10018, 2) != Bytes(2) ||
            Read(0xf10118, 2) != Bytes{0x90,0} || Read(0xf1011c, 2) != Bytes{0x47,0x9f} ||
            Read(0xf001f0, 4) != Bytes(4))
            throw -EPROTO;
        aux_ram_ = MappedRead(AuxRam, 0x4000);
        Save(Stage, 64); // BCブロック書込みの作業領域は最後に復元する。
        Save(0xf10014, 2);
        Save(0xf10018, 2);
        stage = "demodulator reset";
        Write(0xf10014, {0,2}, 4, true);
        Write(0xf10018, {0,0}, 4, true);
        Delay(20);
        Write(0xf10018, {0,2}, 4, true);
        Delay(5);
        stage = "PAF1 start";
        aux_changed_ = true;
        MappedWrite(AuxRam, auxiliary_);
        if (MappedRead(AuxRam, auxiliary_.size()) != auxiliary_)
            throw -EIO;
        MappedWrite(Mail, Bytes(64));
        Put(Vector, 0xa0000);
        const auto vector = Get(Vector);
        if (vector != 0 && vector != 0xa0000)
            throw -EIO;
        aux_started_ = true;
        Put(Cluster + 4, 0xffffffff);
        Put(Cluster, 0xffffffff);
        Put(Cluster + 4, 0x00800000);
        Put(Cluster + 4, 0);
        Put(Cluster, 0xfffcffff);
        uint32_t heartbeat = 0;
        bool seen = false, running = false;
        const auto started = io_.now_ms(io_.opaque);
        do {
            const auto mail = Mailbox();
            if (Be(mail.data()) == 0x50414631 && Be(&mail[4]) == 1) {
                const auto tick = Be(&mail[8]);
                if (seen && tick != heartbeat) {
                    running = true;
                    break;
                }
                heartbeat = tick;
                seen = true;
            }
            Delay(10);
        } while (io_.now_ms(io_.opaque) - started < 3000);
        if (!running)
            throw -ETIMEDOUT;

        stage = "frontend initialization";
        for (size_t i = 0; i < init_.size();) {
            const auto op = init_[i++], n = init_[i++];
            const auto data = init_.data() + i;
            if (op == 255)
                Delay(data[0]);
            else if (op == 128)
                Field(data[1], data[2], data[3]);
            else
                I2c(op == 2 ? Sat : Gr, data, n, 0);
            i += n;
        }

        stage = "PIF1 install";
        Save(Resident, resident_.size());
        Save(Adapter, adapter_.size());
        Save(Hook, hook_.size());
        Write(Resident, resident_, 4, true);
        Write(Adapter, adapter_, 4, true);
        Write(Hook, hook_, 8, true);
        const auto caps = Exchange(1);
        if (Be(&caps[24]) || Be(&caps[28]) != 40 || Be(&caps[32]) != 40 || Be(&caps[36]) != 0x06b8103b)
            throw -EPROTO;
        Exchange(2);
        stage = "TS engine snapshot";
        Save(0xf10118, 2);
        Save(0xf1011c, 2);
        Save(0xf001f0, 4);
        Write(0xf10118, {0xff,0xff}, 4, true);
        Write(0xf1011c, Encode((0x479f & 0xfa1f) | 0x580, 2), 4, true);
        Write(0xf001f0, Encode(0x1a000000), 4, true);
        if (Get(Run))
            throw -EBUSY;
        clock_ = Get(Clock);
        ring_ = MappedRead(Ring, FirstRingWindow);
        const auto tail = MappedRead(Ring + FirstRingWindow, RingSize - FirstRingWindow);
        ring_.insert(ring_.end(), tail.begin(), tail.end());
        clock_changed_ = true;
        Checked(Clock, clock_ | 0x40000);
        for (const auto &reg : Registers)
            ts_saved_.push_back({reg.address, Encode(Get(reg.address, reg.width), reg.width)});
        ready_ = true;
        msg_info("PIX-DT295 source-fr: PAF1/PIF1 ready\n");
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
                    msg_info("PIX-DT295 source-fr: locked system=ISDB-S IF_kHz=%u TSID=%u\n", khz, tsid);
                else
                    msg_info("PIX-DT295 source-fr: locked system=ISDB-T IF_kHz=%u\n", khz);
                return 0;
            }
            Delay(100);
        } while (io_.now_ms(io_.opaque) - start < 4000);
        // 無信号・TSID不一致は機器の異常ではない。USB転送の期限切れ(-ETIMEDOUT)と区別し、次の選局を受け付ける。
        msg_info("PIX-DT295 source-fr: no lock system=%s IF_kHz=%u\n", satellite ? "ISDB-S" : "ISDB-T", khz);
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
            StopRing();
            return 0;
        }
        if (!ready_ || !tuned_ || ring_running_)
            return -EINVAL;
        const int rc = mb86H_stream_input_init(&input_, MB86H_STREAM_FORMAT_RAW_TS, 0, MB86H_TS_RECORD_BYTES);
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
        ring_running_ = true;
        Put(Run, 0x02000000);
        return 0;
    } catch (int error) {
        return Failure("TS engine", error);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int SourceFirmware::Service()
{
    try {
        constexpr unsigned budget = 256;
        const auto reply = Exchange(4, cursor_, budget);
        const auto next = Be(&reply[24]), count = Be(&reply[28]), result = Be(&reply[40]);
        if (result || count > budget || next != (cursor_ + count * 512) % RingSize)
            return Failure("PIF1 Bulk pump response", -EPROTO);
        cursor_ = next;
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
    if (ts_changed_) {
        for (auto i = ts_saved_.rbegin(); i != ts_saved_.rend(); ++i)
            MappedWrite(i->address, i->bytes);
        const Bytes head(ring_.begin(), ring_.begin() + FirstRingWindow);
        const Bytes tail(ring_.begin() + FirstRingWindow, ring_.end());
        const auto restore = [this](uint32_t address, const Bytes &bytes) {
            if (MappedRead(address, bytes.size()) != bytes) {
                MappedWrite(address, bytes);
                if (MappedRead(address, bytes.size()) != bytes)
                    throw -EIO;
            }
        };
        restore(Ring, head);
        restore(Ring + FirstRingWindow, tail);
    }
    if (clock_changed_)
        Checked(Clock, clock_);
    int sleep_error = 0;
    if (satellite_ && ready_) {
        try {
            SatelliteSleep();
        } catch (int error) {
            // RFの停止要求が失敗しても補助CPUとブート領域の復元を試みる。
            sleep_error = error;
        }
    }

    if (aux_started_) {
        Put(Cluster + 4, 0xffffffff);
        Put(Cluster, 0xffffffff);
        const auto state = MappedRead(Cluster, 8);
        const auto heartbeat = Get(Mail + 8);
        Delay(10);
        if (!(state[1] & 2) || !(state[7] & 0x80) || Get(Mail + 8) != heartbeat)
            throw -EIO;
    }
    if (aux_changed_) {
        MappedWrite(AuxRam, aux_ram_);
        Put(Vector, 0);
        if (MappedRead(AuxRam, aux_ram_.size()) != aux_ram_ || Get(Vector) ||
            MappedRead(Cluster, 8) != Bytes{0,2,0,0xff,0x0d,0x8f,0x7f,0xf3} ||
            Read(0xf10114, 2) != Bytes{0x80,0} || Read(0xf1010c, 2) != Bytes{0,1})
            throw -EIO;
    }
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
        aux_changed_ = aux_started_ = clock_changed_ = ts_changed_ = false;
        ready_ = satellite_ = tuned_ = false;
        msg_info("PIX-DT295 source-fr: boot RAM, auxiliary and TS ring restoration verified\n");
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
