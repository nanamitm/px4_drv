// SPDX-License-Identifier: GPL-2.0-only
// RTL-SDR の helper の子プロセス (RtlSdrHelperProcess) を模擬の helper (fake_rtlsdr_helper.sh) で試す。
// 標準入力の I/Q の受け渡し、rtlsdr::Session と組み合わせた TS と MER、異常終了の終了コード、停止の期限を過ぎた helper の強制終了、
// 標準入力のキューのあふれを確かめる。
#include "rtlsdr_helper.hpp"
#include "rtlsdr/session.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace px4::cuse;
namespace {
std::string helper;
void Require(bool ok, const char *what) {if (!ok) {std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1);}}

template <class F> bool WaitFor(F condition, int ms = 5000)
{
    for (int i = 0; i < ms / 5; ++i) {if (condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    return condition();
}

// RtlSdrDevice と同じく、Session の HelperPort を helper の子プロセスへつなぐ (USB の代わりに試験が I/Q を渡す)
struct Port final : px4::rtlsdr::HelperPort {
    RtlSdrHelperProcess process;
    const char *mode = "normal";
    int Launch(std::uint32_t hz, bool) override
    {
        setenv("FAKE_RTLSDR_HELPER", mode, 1);
        return process.Spawn({helper, "--stdin", "--frequency", std::to_string(hz)});
    }
    int Read(std::uint8_t *b, std::size_t n, std::chrono::milliseconds w) override {return process.Read(b, n, w);}
    void RequestStop() override {process.RequestStop();}
    void WaitStopped(std::chrono::milliseconds limit) override {process.WaitStopped(limit);}
    std::uint32_t Kill() override {return process.Kill();}
    void Release() override {process.Release();}
    bool DevicePresent() override {return true;}
};

void SessionReceivesTs()
{
    Port port;
    std::size_t packets = 0;
    px4::rtlsdr::Session session(port, [&](const std::uint8_t *data, std::size_t size, std::uint64_t) {
        Require(data[0] == 0x47 && size == 188, "TS packet");
        ++packets;
    });
    Require(session.Start(473142857) == 0, "start");
    Require(WaitFor([&] {return session.Ready();}), "ready frame");
    Require(!session.Locked(), "no TS before I/Q");
    const std::vector<std::uint8_t> iq(600, 0x80);
    Require(port.process.Feed(iq.data(), iq.size()) && port.process.Feed(iq.data(), iq.size()), "I/Q queued");
    Require(WaitFor([&] {return session.Locked() && session.MerX100() == 1353;}), "TS and MER after I/Q");
    Require(packets == 1, "one TS packet");
    session.Stop();
    Require(!port.process.Running() && session.RestartCount() == 0, "stopped by stdin EOF without restart");
}

void CrashExitCode()
{
    RtlSdrHelperProcess process;
    setenv("FAKE_RTLSDR_HELPER", "crash", 1);
    Require(process.Spawn({helper, "--stdin"}) == 0, "spawn");
    std::uint8_t buffer[256];
    int got = 0;
    Require(WaitFor([&] {got = process.Read(buffer, sizeof(buffer), std::chrono::milliseconds(10)); return got < 0;}), "EOF after crash");
    Require(process.Kill() == 3, "exit code of the crashed helper");
}

void StopDeadline()
{
    RtlSdrHelperProcess process;
    setenv("FAKE_RTLSDR_HELPER", "hang", 1);
    Require(process.Spawn({helper, "--stdin"}) == 0, "spawn");
    const auto started = std::chrono::steady_clock::now();
    process.RequestStop();
    process.WaitStopped(std::chrono::milliseconds(300));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(!process.Running(), "hung helper collected");
    Require(elapsed < std::chrono::seconds(3), "stop deadline enforced");
    process.Release();
}

void QueueOverflow()
{
    RtlSdrHelperProcess process;
    setenv("FAKE_RTLSDR_HELPER", "stall", 1);
    Require(process.Spawn({helper, "--stdin"}) == 0, "spawn");
    // helper は標準入力を読まない。パイプの容量とキューを超えたところで失敗にする
    const std::vector<std::uint8_t> block(262144, 0x80);
    bool rejected = false;
    for (int i = 0; i < 200 && !rejected; ++i) rejected = !process.Feed(block.data(), block.size());
    Require(rejected, "full queue rejects I/Q");
    std::uint8_t buffer[256];
    int got = 0;
    Require(WaitFor([&] {got = process.Read(buffer, sizeof(buffer), std::chrono::milliseconds(10)); return got < 0;}), "read fails after overflow");
    process.Kill();
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {std::fprintf(stderr, "usage: %s FAKE_HELPER\n", argv[0]); return 2;}
    helper = argv[1];
    // デーモン (main.cpp) と同じく SIGPIPE を無視し、終わった helper への書き込みを EPIPE として扱う
    std::signal(SIGPIPE, SIG_IGN);
    SessionReceivesTs();
    CrashExitCode();
    StopDeadline();
    QueueOverflow();
    std::puts("RTL-SDR helper process: stdin I/Q, session TS and MER, crash, stop deadline and overflow cases passed");
    return 0;
}
