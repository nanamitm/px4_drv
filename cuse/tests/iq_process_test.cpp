// SPDX-License-Identifier: GPL-2.0-only
// PX-S1UD の J.83B の helper を子プロセスとして動かす PosixIqProcess を、模擬の helper (fake_iq_helper.sh) で試す。
// 起動、出力の解釈、標準入力の終わりによる停止、異常終了、形式の異常、停止の期限を過ぎた helper の強制終了、
// 実行ファイルがない場合を確かめる。
#include "iq_process.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace px4::cuse;
using Clock = std::chrono::steady_clock;

namespace {
void Require(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

bool WaitFor(const std::function<bool()> &done, std::chrono::milliseconds limit)
{
    const auto deadline = Clock::now() + limit;
    while (!done()) {
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: iq_process_test FAKE_HELPER\n");
        return 2;
    }
    // px4-cused (main.cpp) と同じく、終わった helper へ書いたときの SIGPIPE を無視して EPIPE で受け取る
    std::signal(SIGPIPE, SIG_IGN);
    const std::string helper = argv[1];
    std::vector<std::uint8_t> block(px4::siano::IqBlockBytes, 0);

    {
        setenv("FAKE_IQ_HELPER", "normal", 1);
        PosixIqProcess process(helper);
        std::mutex mutex;
        std::vector<std::uint8_t> ts;
        Require(process.Start([&](const std::uint8_t *data, std::size_t size) {
                    std::lock_guard<std::mutex> lock(mutex);
                    ts.insert(ts.end(), data, data + size);
                    return true;
                }) == 0,
                "start the helper");
        Require(process.Running(), "running after start");
        Require(WaitFor([&] { return process.Ready() && process.MerX100() == 3150; }, std::chrono::seconds(5)),
                "ready, TS and MER are parsed");
        {
            std::lock_guard<std::mutex> lock(mutex);
            Require(ts.size() == 188 && ts[0] == 0x47, "TS is delivered");
        }
        Require(process.LastTs() != 0 && !process.Failed(), "TS time recorded");
        for (int i = 0; i < 8; ++i)
            process.Feed(block.data(), block.size());
        const auto started = Clock::now();
        process.Stop();
        Require(Clock::now() - started < std::chrono::seconds(3), "stop by closing standard input");
        Require(!process.Running() && !process.Ready() && process.MerX100() == -1, "state cleared after stop");
        Require(process.Dropped() == 0, "no dropped blocks");
    }
    {
        setenv("FAKE_IQ_HELPER", "crash", 1);
        PosixIqProcess process(helper);
        Require(process.Start([](const std::uint8_t *, std::size_t) { return true; }) == 0, "start crashing helper");
        Require(WaitFor([&] { return process.Failed(); }, std::chrono::seconds(5)), "unexpected exit is a failure");
        process.Feed(block.data(), block.size());
        process.Stop();
    }
    {
        setenv("FAKE_IQ_HELPER", "bad", 1);
        PosixIqProcess process(helper);
        Require(process.Start([](const std::uint8_t *, std::size_t) { return true; }) == 0, "start malformed helper");
        Require(WaitFor([&] { return process.Failed(); }, std::chrono::seconds(5)), "malformed frame is a failure");
        process.Stop();
    }
    {
        setenv("FAKE_IQ_HELPER", "hang", 1);
        PosixIqProcess process(helper);
        Require(process.Start([](const std::uint8_t *, std::size_t) { return true; }) == 0, "start hanging helper");
        Require(WaitFor([&] { return process.Ready(); }, std::chrono::seconds(5)), "hanging helper is ready");
        const auto started = Clock::now();
        process.Stop();
        const auto elapsed = Clock::now() - started;
        Require(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(8),
                "stop deadline kills the process group");
        Require(!process.Running(), "stopped after the deadline");
    }
    {
        PosixIqProcess process("/nonexistent/msi2500_frontend");
        Require(process.Start([](const std::uint8_t *, std::size_t) { return true; }) < 0 && !process.Running(),
                "missing helper executable");
    }
    std::puts("PASS: J.83B helper process start, output, stop, crash, malformed output, stop deadline and missing executable");
    return 0;
}
