// SPDX-License-Identifier: GPL-2.0-only
// IFD ハンドラーへ渡す対応表 (cuse/src/reader_map.cpp) の書式、ディレクトリの作成、失敗の記録を確認する。
// /run の代わりに一時ディレクトリを使うため、root は不要。
#include "reader_map.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using px4::cuse::ReaderMapDirectory;
using px4::cuse::ReaderMapEntry;
using px4::cuse::WriteReaderMap;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

std::string Read(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Exists(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// WriteReaderMap のログ (標準エラー出力) をファイルへ移して読む。
class StderrCapture {
public:
    explicit StderrCapture(const std::string &path) : path_(path)
    {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        std::FILE *file = std::freopen(path.c_str(), "w", stderr);
        (void)file;
    }
    std::string Finish()
    {
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        return Read(path_);
    }

private:
    std::string path_;
    int saved_;
};

bool Contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}
}

int main()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string root = std::string(tmp && *tmp ? tmp : "/tmp") + "/px4-reader-map-XXXXXX";
    if (!::mkdtemp(&root[0])) {
        std::printf("FAIL mkdtemp %s (%s)\n", root.c_str(), std::strerror(errno));
        return 1;
    }
    const std::string log = root + "/stderr.log";
    const std::vector<ReaderMapEntry> two = {{1, 5, "1A2B3C4D"}, {3, 12, "5E6F7A8B"}};

    // systemd の RuntimeDirectory= が渡すディレクトリはデーモンが作らない。手動の起動では /run/px4-cused を作る。
    bool create = false;
    ::setenv("RUNTIME_DIRECTORY", "/run/px4-cused", 1);
    CHECK(ReaderMapDirectory(create) == "/run/px4-cused" && !create);
    ::setenv("RUNTIME_DIRECTORY", "", 1);
    CHECK(ReaderMapDirectory(create) == "/run/px4-cused" && create);
    ::unsetenv("RUNTIME_DIRECTORY");
    CHECK(ReaderMapDirectory(create) == "/run/px4-cused" && create);

    // ディレクトリがなく作らない場合は ENOENT を返し、パスと errno の説明をログへ残す。
    const std::string missing = root + "/missing";
    {
        StderrCapture capture(log);
        const int rc = WriteReaderMap(missing, false, two);
        const std::string text = capture.Finish();
        CHECK(rc == -ENOENT);
        CHECK(Contains(text, "[ERR]") && Contains(text, missing + "/readers.tmp") &&
              Contains(text, std::strerror(ENOENT)));
        CHECK(!Exists(missing));
    }

    // systemctl stop で消えた後の手動の起動。umask に関係なく 0755 で作り、書式は ifd-px4 との契約のまま。
    {
        const mode_t old = ::umask(077);
        StderrCapture capture(log);
        const int rc = WriteReaderMap(missing, true, two);
        const std::string text = capture.Finish();
        ::umask(old);
        CHECK(rc == 0);
        CHECK(Contains(text, "[INFO]") && Contains(text, "created " + missing));
        struct stat st;
        CHECK(::stat(missing.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0755);
        CHECK(Read(missing + "/readers") ==
              "# bus device path\n1 5 /dev/px4card/1A2B3C4D\n3 12 /dev/px4card/5E6F7A8B\n");
        CHECK(!Exists(missing + "/readers.tmp"));
    }

    // 既存のディレクトリへの書き直しはログを残さず、権限も変えない。
    {
        ::chmod(missing.c_str(), 0750);
        StderrCapture capture(log);
        const int rc = WriteReaderMap(missing, true, {});
        const std::string text = capture.Finish();
        CHECK(rc == 0 && text.empty());
        CHECK(Read(missing + "/readers") == "# bus device path\n");
        struct stat st;
        CHECK(::stat(missing.c_str(), &st) == 0 && (st.st_mode & 07777) == 0750);
        ::chmod(missing.c_str(), 0755);
    }

    // 同じ名前のファイルがある場合は作れず、ENOTDIR を記録する。
    const std::string file = root + "/file";
    {
        std::ofstream(file) << "x";
        StderrCapture capture(log);
        const int rc = WriteReaderMap(file, true, two);
        const std::string text = capture.Finish();
        CHECK(rc == -ENOTDIR);
        CHECK(Contains(text, "[ERR]") && Contains(text, file + "/readers.tmp") &&
              Contains(text, std::strerror(ENOTDIR)));
    }

    // 書き込めないディレクトリ。root は権限の確認を受けないため、一般ユーザーでだけ確かめる。
    if (::geteuid() != 0) {
        const std::string locked = root + "/locked";
        ::mkdir(locked.c_str(), 0555);
        StderrCapture capture(log);
        const int rc = WriteReaderMap(locked, true, two);
        const std::string text = capture.Finish();
        CHECK(rc == -EACCES);
        CHECK(Contains(text, "[ERR]") && Contains(text, locked + "/readers.tmp") &&
              Contains(text, std::strerror(EACCES)));
        ::rmdir(locked.c_str());
    } else {
        std::printf("SKIP read-only directory (running as root)\n");
    }

    std::remove((missing + "/readers").c_str());
    ::rmdir(missing.c_str());
    std::remove(file.c_str());
    std::remove(log.c_str());
    ::rmdir(root.c_str());

    if (failures) {
        std::printf("reader_map_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("reader_map_test: ok\n");
    return 0;
}
