// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <filesystem>
#include <vector>
#include <utility>

namespace px4::siano {
inline std::vector<std::filesystem::path> FirmwareCandidates(
    const std::filesystem::path& host_directory,const std::filesystem::path& configured)
{
    std::vector<std::filesystem::path> paths;
    auto add=[&](std::filesystem::path p) {
        if (p.is_relative()) p=host_directory/p;
        p=p.lexically_normal();
        for (const auto& existing:paths) if (existing==p) return;
        paths.push_back(std::move(p));
    };
    if (!configured.empty()) add(configured);
    add(host_directory/L"isdbt_rio.inp");
    add(host_directory/L"firmware"/L"isdbt_rio.inp");
    return paths;
}

inline std::filesystem::path ResolveFirmwarePath(
    const std::filesystem::path& host_directory,const std::filesystem::path& configured)
{
    const auto paths=FirmwareCandidates(host_directory,configured);
    for (const auto& p:paths) {
        std::error_code error;
        // 同名のディレクトリーは読み込み候補から除く。内容の妥当性は選択後に検証する。
        if (std::filesystem::is_regular_file(p,error)) return p;
    }
    // 既存の warm 起動を維持し、見つからない場合も呼び出し元で探索先をログへ残す。
    return paths.front();
}
}
