// SPDX-License-Identifier: GPL-2.0-only
// IFD ハンドラー (px4-drv-pcsc の ifd-px4) へ渡す、USB 機器とカードのデバイスファイルの対応表。
// CUSE のデバイスは USB 機器を親に持たず、sysfs からたどれないため、この表で対応を示す。
// 書式 ("# bus device path" の見出しと "<bus> <address> /dev/px4card/<ID>" の行) は ifd-px4 との契約。
#pragma once

#include <string>
#include <vector>

namespace px4::cuse {
struct ReaderMapEntry {
    unsigned bus;
    unsigned address;
    std::string card_id;
};

// 対応表を置くディレクトリ。systemd が RuntimeDirectory= で渡す RUNTIME_DIRECTORY を優先し、
// 手動で起動した場合は /run/px4-cused を使う。create には、存在しない場合に作るかどうかを返す。
std::string ReaderMapDirectory(bool &create);

// directory/readers を一時ファイルからの rename で置き換える。成功は 0、失敗は負の errno を返し、
// 失敗はパスと errno をログへ残す。
int WriteReaderMap(const std::string &directory, bool create, const std::vector<ReaderMapEntry> &entries);
}
