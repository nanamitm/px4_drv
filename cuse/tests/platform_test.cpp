// SPDX-License-Identifier: GPL-2.0-only
// Linux 版の SHA-256 (公開されている試験ベクター) と、実行ファイルへ埋め込んだファームウェアのバンドルを確認する。
#include "dt295/platform.hpp"
#include "dt295/source_firmware.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

std::string Hex(const std::vector<std::uint8_t> &input)
{
    std::array<std::uint8_t, 32> digest{};
    if (!px4::dt295::platform::Sha256(input.data(), input.size(), digest))
        return {};
    std::string out;
    char byte[3];
    for (auto value : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        out += byte;
    }
    return out;
}

std::vector<std::uint8_t> Bytes(const char *text) { return {text, text + std::strlen(text)}; }
}

int main()
{
    // FIPS 180-2 の例と、長さの格納が 2 ブロック目へ回る境界 (56 バイト) と 64 バイトちょうど。
    CHECK(Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Hex(Bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Hex(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Hex(std::vector<std::uint8_t>(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(Hex(std::vector<std::uint8_t>(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // 埋め込んだバンドルは、WinUSB 版と同じ検証 (LoadEmbedded) を通る。
    for (bool direct : {true, false}) {
        const std::uint8_t *data = nullptr;
        std::size_t size = 0;
        CHECK(px4::dt295::platform::EmbeddedBundle(direct, data, size) && data && size >= 16);
        px4::dt295::SourceFirmware source(direct);
        CHECK(source.LoadEmbedded() == 0);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS platform_test\n");
    return 0;
}
