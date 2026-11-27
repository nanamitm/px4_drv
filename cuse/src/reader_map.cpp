// SPDX-License-Identifier: GPL-2.0-only
#include "reader_map.hpp"
#include "msg.h"
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace px4::cuse {
// systemd のユニット (dist/px4-cused.service) では RuntimeDirectory= がディレクトリを作り、所有者と権限を決め、
// 停止時に消す。この場合はディレクトリを作らず、systemd の管理に任せる。
// 手動で起動した場合は、systemctl stop で /run/px4-cused が消えた後でも対応表を書けるよう、デーモンが
// 0755 で作る。pcscd の IFD ハンドラーが対応表を読めないと、エラーも出ずにリーダーが現れないためである。
// 作ったディレクトリは終了後も残るが、次の systemctl start では RuntimeDirectory= が既存のディレクトリを
// 引き継いで所有者と権限を設定し直すため、ユニットの動作は変わらない。
std::string ReaderMapDirectory(bool &create)
{
    const char *dir = std::getenv("RUNTIME_DIRECTORY");
    create = !(dir && *dir);
    return create ? std::string("/run/px4-cused") : std::string(dir);
}

int WriteReaderMap(const std::string &directory, bool create, const std::vector<ReaderMapEntry> &entries)
{
    if (create) {
        if (::mkdir(directory.c_str(), 0755) == 0) {
            // umask に関係なく、pcscd の利用者が読めるようにする。
            if (::chmod(directory.c_str(), 0755)) {
                const int error = errno;
                msg_err("reader map: chmod %s failed (%s)\n", directory.c_str(), std::strerror(error));
                return -error;
            }
            msg_info("reader map: created %s\n", directory.c_str());
        } else if (errno != EEXIST) {
            const int error = errno;
            msg_err("reader map: cannot create %s (%s)\n", directory.c_str(), std::strerror(error));
            return -error;
        }
    }

    const std::string path = directory + "/readers", temp = path + ".tmp";
    std::FILE *out = std::fopen(temp.c_str(), "w");
    if (!out) {
        const int error = errno;
        msg_err("reader map: cannot write %s (%s); the IFD handler will not find the card readers\n", temp.c_str(),
                std::strerror(error));
        return -error;
    }
    // 書き込みの失敗は errno を設定しない実装もあるため、その場合は EIO とする。
    int error = 0;
    const auto failed = [&error] { error = errno ? errno : EIO; };
    errno = 0;
    if (std::fputs("# bus device path\n", out) < 0)
        failed();
    for (const auto &entry : entries)
        if (!error && std::fprintf(out, "%u %u /dev/px4card/%s\n", entry.bus, entry.address, entry.card_id.c_str()) < 0)
            failed();
    if (!error && std::fflush(out))
        failed();
    if (std::fclose(out) && !error)
        failed();
    if (error) {
        std::remove(temp.c_str());
        msg_err("reader map: cannot write %s (%s); the IFD handler will not find the card readers\n", temp.c_str(),
                std::strerror(error));
        return -error;
    }
    if (std::rename(temp.c_str(), path.c_str())) {
        error = errno;
        std::remove(temp.c_str());
        msg_err("reader map: cannot rename %s to %s (%s); the IFD handler will not find the card readers\n",
                temp.c_str(), path.c_str(), std::strerror(error));
        return -error;
    }
    return 0;
}
}
