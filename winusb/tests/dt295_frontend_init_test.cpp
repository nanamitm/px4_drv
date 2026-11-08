// SPDX-License-Identifier: GPL-2.0-only
// mAgicVISION 0.1.0から組み立てる初期化列が、実機で検証したDTP1バンドルの初期化セクションと一致することを確認する。
// 値そのものはリポジトリへ収録せず、SHA-256だけで比較する。機器にはアクセスしない。
#include <windows.h>
#include <bcrypt.h>
#include "../src/DriverHost_PX4/dt295/frontend_init.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr char ValidatedInitSha256[] = "9f2593f0aabd74ae5db55eee58fd7bc8d667945ce7a1df1c40746402c05ab358";
constexpr size_t ValidatedInitBytes = 754;
int failures = 0;

std::string Sha256(const std::vector<uint8_t> &bytes)
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: dt295_frontend_init_test <magicvision-dt295.bin>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> container((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Check(Sha256(container) == px4::dt295::FrontendInitContainerSha256, "reference container is mAgicVISION 0.1.0");

    std::vector<uint8_t> init;
    Check(px4::dt295::BuildFrontendInit(container.data(), container.size(), init) == 0, "build succeeds");
    Check(init.size() == ValidatedInitBytes, "size matches validated DTP1 initializer");
    Check(Sha256(init) == ValidatedInitSha256, "bytes match validated DTP1 initializer");

    Check(px4::dt295::BuildFrontendInit(container.data(), 47, init) == -EINVAL && init.empty(), "short container rejected");
    Check(px4::dt295::BuildFrontendInit(nullptr, container.size(), init) == -EINVAL, "null container rejected");
    if (container.size() >= 48) {
        auto broken = container;
        const size_t resident = static_cast<size_t>(broken[40]) << 24 | static_cast<size_t>(broken[41]) << 16 |
            static_cast<size_t>(broken[42]) << 8 | broken[43];
        const size_t script = resident + (0x803ACB8C - 0x80380000);
        if (script < broken.size()) {
            broken[script] = 0x7f;
            Check(px4::dt295::BuildFrontendInit(broken.data(), broken.size(), init) == -EINVAL, "unknown record op rejected");
        }
        broken = container;
        broken[44] = 0;
        broken[45] = 0;
        broken[46] = 0;
        broken[47] = 4;
        Check(px4::dt295::BuildFrontendInit(broken.data(), broken.size(), init) == -EINVAL, "short resident image rejected");
    }
    std::printf("%s\n", failures ? "FAIL dt295 frontend init" : "PASS dt295 frontend init. No hardware was opened.");
    return failures ? 1 : 0;
}
