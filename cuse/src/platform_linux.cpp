// SPDX-License-Identifier: GPL-2.0-only
// SourceFirmware が使う OS 依存の 2 つの処理の Linux 版 (Windows 版は dt295/platform_win32.cpp)。
// SHA-256 の用途はブート ROM の内容の 1 回の確認だけのため、外部ライブラリへの依存を加えずにここで計算する。
#include "dt295/platform.hpp"
#include <cstring>

// firmware.S が実行ファイルへ埋め込むバンドル。内容は Windows 版の EXE のリソースと同じファイル。
extern "C" const std::uint8_t px4_dt295_direct_fr[], px4_dt295_direct_fr_end[];
extern "C" const std::uint8_t px4_dt295_source_fr[], px4_dt295_source_fr_end[];

namespace px4::dt295::platform {
bool EmbeddedBundle(bool direct, const std::uint8_t *&data, std::size_t &size)
{
    data = direct ? px4_dt295_direct_fr : px4_dt295_source_fr;
    size = static_cast<std::size_t>((direct ? px4_dt295_direct_fr_end : px4_dt295_source_fr_end) - data);
    return size != 0;
}

namespace {
constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotate(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Block(std::uint32_t h[8], const std::uint8_t *p)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = std::uint32_t(p[i * 4]) << 24 | std::uint32_t(p[i * 4 + 1]) << 16 | std::uint32_t(p[i * 4 + 2]) << 8 |
               p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const std::uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        const std::uint32_t t1 = hh + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const std::uint32_t t2 = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}
}

bool Sha256(const std::uint8_t *data, std::size_t size, std::array<std::uint8_t, 32> &digest)
{
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t at = 0;
    for (; size - at >= 64; at += 64)
        Block(h, data + at);
    // 残りと 0x80、64 ビットの長さを 1〜2 ブロックへ詰める。
    std::uint8_t tail[128] = {};
    const std::size_t rest = size - at;
    if (rest)
        std::memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    const std::size_t blocks = rest < 56 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 0; i < 8; i++)
        tail[blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
    for (std::size_t i = 0; i < blocks; i++)
        Block(h, tail + i * 64);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            digest[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
    return true;
}
}
