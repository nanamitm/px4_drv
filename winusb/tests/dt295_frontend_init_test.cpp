// SPDX-License-Identifier: GPL-2.0-only
// 組み込み初期化列と実機検証済みDTP1初期化セクションのSHA-256を比較する。
#include <windows.h>
#include <bcrypt.h>
#include "../src/DriverHost_PX4/dt295/frontend_init.hpp"
#include <array>
#include <cstdio>
#include <string>
#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr char ValidatedInitSha256[] = "9f2593f0aabd74ae5db55eee58fd7bc8d667945ce7a1df1c40746402c05ab358";
constexpr size_t ValidatedInitBytes = 754;
int failures = 0;

std::string Sha256(const std::array<uint8_t, ValidatedInitBytes> &bytes)
{
    std::array<uint8_t, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        return {};
    std::string text;
    char hex[3];
    for (auto byte : digest) {
        std::snprintf(hex, sizeof(hex), "%02x", byte);
        text += hex;
    }
    return text;
}

void Check(bool condition, const char *name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        ++failures;
}
}

int main()
{
    const auto &init = px4::dt295::FrontendInit;
    Check(init.size() == ValidatedInitBytes, "size matches validated DTP1 initializer");
    Check(Sha256(init) == ValidatedInitSha256, "bytes match validated DTP1 initializer");
    return failures ? 1 : 0;
}
