// SPDX-License-Identifier: GPL-2.0-only
// PX-BCUD の isochronous のパケットから TS を組み立てる処理 (PxBcudStream) の試験。USB は使わない。
#include "pxbcud_stream.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using px4::cuse::PxBcudStream;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                                 \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

namespace {
constexpr std::uint16_t Tsid = 0x4010;

std::uint32_t Crc32(const std::uint8_t *data, std::size_t size)
{
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
    return crc;
}

// PID と continuity counter を持つ 188 バイトの TS。payload の先頭に PID の下位を置いて区別できるようにする。
std::vector<std::uint8_t> Packet(std::uint16_t pid, std::uint8_t cc)
{
    std::vector<std::uint8_t> p(188, 0xff);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>(pid >> 8);
    p[2] = static_cast<std::uint8_t>(pid);
    p[3] = static_cast<std::uint8_t>(0x10 | (cc & 15));
    p[4] = static_cast<std::uint8_t>(pid);
    return p;
}

std::vector<std::uint8_t> Pat(std::uint16_t tsid, std::uint8_t cc)
{
    auto p = Packet(0, cc);
    p[1] |= 0x40;
    p[4] = 0;  // pointer_field
    std::uint8_t section[] = {0x00, 0xb0, 13, static_cast<std::uint8_t>(tsid >> 8), static_cast<std::uint8_t>(tsid),
                              0xc1, 0x00, 0x00, 0x04, 0x00, 0xe1, 0x00, 0, 0, 0, 0};
    const auto crc = Crc32(section, 12);
    section[12] = static_cast<std::uint8_t>(crc >> 24);
    section[13] = static_cast<std::uint8_t>(crc >> 16);
    section[14] = static_cast<std::uint8_t>(crc >> 8);
    section[15] = static_cast<std::uint8_t>(crc);
    std::memcpy(&p[5], section, sizeof(section));
    return p;
}

void Append(std::vector<std::uint8_t> &to, const std::vector<std::uint8_t> &from)
{
    to.insert(to.end(), from.begin(), from.end());
}

// data を isochronous のパケットの長さ (最大 940 バイト) に分けて渡し、1 転送分として Flush する。
std::vector<std::uint8_t> Transfer(PxBcudStream &stream, const std::vector<std::uint8_t> &data, std::size_t chunk)
{
    for (std::size_t offset = 0; offset < data.size(); offset += chunk)
        CHECK(stream.Packet(true, data.data() + offset, std::min(chunk, data.size() - offset)));
    std::vector<std::uint8_t> out;
    stream.Flush([&out](const std::uint8_t *p, std::size_t n) { out.insert(out.end(), p, p + n); });
    return out;
}

void GateOpensOnSelectedPat()
{
    PxBcudStream stream(Tsid);
    std::vector<std::uint8_t> data;
    // 前のチャンネルの TS と、別の TSID の PAT は配信しない。
    Append(data, Packet(0x100, 0));
    Append(data, Packet(0x100, 1));
    Append(data, Pat(0x4011, 0));
    Append(data, Packet(0x100, 2));
    Append(data, Pat(Tsid, 1));
    Append(data, Packet(0x111, 0));
    Append(data, Packet(0x111, 1));
    // TS の境界と isochronous のパケットの境界は一致しない。
    const auto out = Transfer(stream, data, 940);
    CHECK(stream.Ready());
    CHECK(stream.startup_discarded == 4);
    CHECK(out.size() == 3 * 188);
    CHECK(out[1] == 0x40 && out[2] == 0x00);  // 選んだ TSID の PAT から配信する
    CHECK(out[188 + 2] == 0x11 && out[2 * 188 + 2] == 0x11);
}

void ShortPacketsAndEmptyPackets()
{
    PxBcudStream stream(Tsid);
    std::vector<std::uint8_t> data;
    Append(data, Pat(Tsid, 0));
    for (std::uint8_t i = 0; i < 10; ++i)
        Append(data, Packet(0x111, i));
    // 188 の倍数にならない長さに分けても、同じ TS になる。
    const auto out = Transfer(stream, data, 333);
    CHECK(stream.Packet(true, nullptr, 0));
    CHECK(stream.empty_packets == 1);
    CHECK(out.size() == 11 * 188);
    CHECK(std::memcmp(out.data(), data.data(), out.size()) == 0);
}

void ErrorPacketDropsPartialTs()
{
    PxBcudStream stream(Tsid);
    std::vector<std::uint8_t> first;
    Append(first, Pat(Tsid, 0));
    for (std::uint8_t i = 0; i < 3; ++i)
        Append(first, Packet(0x111, i));
    // 4 つ目の TS (cc 2) の途中で、次のマイクロフレームが欠けたとする。
    const std::size_t cut = 3 * 188 + 100;
    CHECK(stream.Packet(true, first.data(), cut));
    CHECK(stream.Packet(false, nullptr, 0));
    CHECK(stream.iso_errors == 1);
    std::vector<std::uint8_t> rest(first.begin() + cut, first.end());
    std::vector<std::uint8_t> next;
    for (std::uint8_t i = 3; i < 8; ++i)
        Append(next, Packet(0x111, i));
    // 欠けた後に残りの 88 バイトが届いても、つなげずに次の同期を探す。
    CHECK(stream.Packet(true, rest.data(), rest.size()));
    for (std::size_t offset = 0; offset < next.size(); offset += 940)
        CHECK(stream.Packet(true, next.data() + offset, std::min<std::size_t>(940, next.size() - offset)));
    std::vector<std::uint8_t> out;
    stream.Flush([&out](const std::uint8_t *p, std::size_t n) { out.insert(out.end(), p, p + n); });
    // PAT、cc 0〜1、cc 3〜7 の 8 個。欠けた cc 2 は渡さない。
    CHECK(stream.Ready());
    CHECK(out.size() == 8 * 188);
    CHECK(out[1] == 0x40);
    const std::uint8_t expected[] = {0, 1, 3, 4, 5, 6, 7};
    for (std::size_t i = 0; i < 7; ++i)
        CHECK(out[(i + 1) * 188] == 0x47 && out[(i + 1) * 188 + 2] == 0x11 &&
              (out[(i + 1) * 188 + 3] & 15) == expected[i]);
}

void OversizedPacketIsRejected()
{
    PxBcudStream stream(Tsid);
    std::vector<std::uint8_t> data(941, 0x47);
    CHECK(!stream.Packet(true, data.data(), data.size()));
    CHECK(stream.Packet(true, data.data(), 940));
}

void RestartKeepsConfirmedPat()
{
    PxBcudStream stream(Tsid);
    std::vector<std::uint8_t> data;
    Append(data, Pat(Tsid, 0));
    Append(data, Packet(0x111, 0));
    Append(data, Packet(0x111, 1));
    Transfer(stream, data, 940);
    CHECK(stream.Ready());
    // 同じ選局の自動復旧では、PAT を待たずに配信を続ける (WinUSB 版と同じ)。
    stream.Restart();
    CHECK(stream.Ready());
    CHECK(stream.Packets() == 0 && stream.startup_discarded == 0);
    std::vector<std::uint8_t> after;
    for (std::uint8_t i = 2; i < 6; ++i)
        Append(after, Packet(0x111, i));
    const auto out = Transfer(stream, after, 940);
    CHECK(out.size() == 4 * 188);
}
}

int main()
{
    GateOpensOnSelectedPat();
    ShortPacketsAndEmptyPackets();
    ErrorPacketDropsPartialTs();
    OversizedPacketIsRejected();
    RestartKeepsConfirmedPat();
    std::puts("pxbcud_stream_test: OK");
    return 0;
}
