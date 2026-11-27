// SPDX-License-Identifier: GPL-2.0-only
#include "msi2500_ops.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace px4::cuse::msi2500 {
bool ParseOps(const std::string &text, std::vector<ControlOp> &setup, std::vector<ControlOp> &stop, std::string &error)
{
    setup.clear();
    stop.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        char stage[8] = {};
        unsigned request = 0, value = 0, index = 0;
        if (std::sscanf(line.c_str(), "%7s %x %x %x", stage, &request, &value, &index) != 4 || request > 0xff ||
            value > 0xffff || index > 0xffff) {
            error = "invalid helper ops line: " + line;
            return false;
        }
        const ControlOp op{static_cast<std::uint8_t>(request), static_cast<std::uint16_t>(value),
                           static_cast<std::uint16_t>(index)};
        if (!std::strcmp(stage, "setup"))
            setup.push_back(op);
        else if (!std::strcmp(stage, "stop"))
            stop.push_back(op);
        else {
            error = "invalid helper ops stage: " + line;
            return false;
        }
    }
    if (setup.empty()) {
        error = "helper ops returned no setup";
        return false;
    }
    return true;
}

int RunOps(const std::vector<std::string> &args, std::chrono::milliseconds limit, std::string &text, bool &spawned,
           int &status)
{
    using Clock = std::chrono::steady_clock;
    text.clear();
    spawned = false;
    status = -1;
    // helper の ops は計算だけで終わる。標準出力を読み切ってから終了を待つ
    int out[2];
    if (pipe2(out, O_CLOEXEC))
        return -errno;
    std::vector<std::string> copy = args;
    std::vector<char *> argv;
    for (auto &a : copy)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (rc) {
        close(out[0]);
        return -rc;
    }
    spawned = true;
    const auto deadline = Clock::now() + limit;
    char buffer[4096];
    for (;;) {
        pollfd p{out[0], POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0)
            break;
        const ssize_t got = read(out[0], buffer, sizeof(buffer));
        if (got <= 0)
            break;
        text.append(buffer, static_cast<std::size_t>(got));
    }
    close(out[0]);
    // 標準出力の EOF は helper が記述子を閉じた時点で届き、回収できる状態になるのはその後になる。
    // EOF の直後に回収できないのは正常な終了の途中のため、読み出しと同じ期限まで待ってから強制終了する
    int raw = 0;
    pid_t done = 0;
    while (((done = waitpid(pid, &raw, WNOHANG)) == 0 || (done < 0 && errno == EINTR)) && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (done != pid) {
        kill(pid, SIGKILL);
        while ((done = waitpid(pid, &raw, 0)) < 0 && errno == EINTR) {
        }
    }
    // 回収できなかった場合は終了状態が分からないため、成功として扱わない
    if (done != pid)
        return -EIO;
    status = raw;
    return WIFEXITED(raw) && !WEXITSTATUS(raw) ? 0 : -EIO;
}

void CollectBlocks(const libusb_transfer &transfer, std::vector<std::uint8_t> &blocks, PacketCounts &counts)
{
    blocks.clear();
    // libusb_get_iso_packet_buffer_simple は const の転送を受け取らないが、転送を変更しない
    auto *t = const_cast<libusb_transfer *>(&transfer);
    for (int i = 0; i < transfer.num_iso_packets; ++i) {
        const auto &d = transfer.iso_packet_desc[i];
        ++counts.packets;
        if (d.status != LIBUSB_TRANSFER_COMPLETED) {
            ++counts.bad;
            continue;
        }
        if (!d.actual_length) {
            ++counts.empty;
            continue;
        }
        if (d.actual_length % Block)
            ++counts.odd;
        const std::size_t whole = d.actual_length / Block * Block;
        const auto *data = libusb_get_iso_packet_buffer_simple(t, static_cast<unsigned>(i));
        blocks.insert(blocks.end(), data, data + whole);
    }
}
}
