// SPDX-License-Identifier: GPL-2.0-only
#include "platform.hpp"
#include "../resource.h"
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace px4::dt295::platform {
bool EmbeddedBundle(bool direct, const std::uint8_t *&data, std::size_t &size)
{
    data = nullptr;
    size = 0;
    const auto module = GetModuleHandleW(nullptr);
    const auto id = direct ? IDR_DT295_DIRECT_FIRMWARE : IDR_DT295_SOURCE_FIRMWARE;
    // UNICODEを定義しない試験のビルドでも同じ型になるよう、RT_RCDATA (10) をワイド文字版で渡す。
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource)
        return false;
    const auto handle = LoadResource(module, resource);
    data = handle ? static_cast<const std::uint8_t *>(LockResource(handle)) : nullptr;
    size = data ? SizeofResource(module, resource) : 0;
    return data != nullptr;
}

bool Sha256(const std::uint8_t *data, std::size_t size, std::array<std::uint8_t, 32> &digest)
{
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data),
                      static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size())) >= 0;
}
}
