// SPDX-License-Identifier: GPL-2.0-only
// rtlsdr::Session (WinUSB 版と Linux 版の共有の helper の監視) を模擬の helper で試す。
// 周波数の換算、ロックと MER、停止の手順、予期しない終了からの起動し直し、回数の使い切り、抜去の後に起動し直さないことを確かめる。
#include "rtlsdr/session.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace px4::rtlsdr;
namespace {
void Require(bool ok, const char *what) {if (!ok) {std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1);}}

void Put(std::vector<std::uint8_t> &out, std::uint32_t v) {for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));}

std::vector<std::uint8_t> MakeFrame(std::uint32_t kind, const std::vector<std::uint8_t> &payload, std::uint32_t quality = 0)
{
    std::vector<std::uint8_t> out;
    for (std::uint32_t v : {Magic, kind, static_cast<std::uint32_t>(payload.size()), quality, 0u, 0u}) Put(out, v);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
std::vector<std::uint8_t> Ts(std::uint8_t marker)
{
    std::vector<std::uint8_t> p(188, 0xff);
    p[0] = 0x47; p[1] = 0x1f; p[2] = 0xff; p[3] = 0x10; p[4] = marker;
    return p;
}
std::vector<std::uint8_t> Mer(std::uint32_t x100) {std::vector<std::uint8_t> p; Put(p, x100); return p;}

// 1 回の起動で helper が出す出力。ended なら出し終えた後に終了する
struct Script {
    std::vector<std::vector<std::uint8_t>> chunks;
    bool ended = false;
};

struct FakePort final : HelperPort {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Script> scripts;
    Script current;
    bool running = false, present = true;
    int launch_error = 0;
    unsigned launches = 0, restarted_launches = 0, kills = 0, stop_requests = 0, waits = 0, releases = 0;
    std::uint32_t last_hz = 0;

    int Launch(std::uint32_t hz, bool restarted) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++launches; if (restarted) ++restarted_launches;
        last_hz = hz;
        if (launch_error) return launch_error;
        current = scripts.empty() ? Script{{}, true} : scripts.front();
        if (!scripts.empty()) scripts.pop_front();
        running = true;
        changed.notify_all();
        return 0;
    }
    int Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!current.chunks.empty()) {
            auto chunk = current.chunks.front();
            current.chunks.erase(current.chunks.begin());
            Require(chunk.size() <= size, "chunk fits");
            std::memcpy(buffer, chunk.data(), chunk.size());
            return static_cast<int>(chunk.size());
        }
        if (!running || current.ended) return -1;
        changed.wait_for(lock, wait);
        return 0;
    }
    void RequestStop() override {std::lock_guard<std::mutex> lock(mutex); ++stop_requests; running = false; changed.notify_all();}
    void WaitStopped(std::chrono::milliseconds) override {std::lock_guard<std::mutex> lock(mutex); ++waits;}
    std::uint32_t Kill() override {std::lock_guard<std::mutex> lock(mutex); ++kills; running = false; return 1;}
    void Release() override {std::lock_guard<std::mutex> lock(mutex); ++releases;}
    bool DevicePresent() override {std::lock_guard<std::mutex> lock(mutex); return present;}
    unsigned Launches() {std::lock_guard<std::mutex> lock(mutex); return launches;}
    unsigned Kills() {std::lock_guard<std::mutex> lock(mutex); return kills;}
};

struct Sink {
    std::mutex mutex;
    std::vector<std::uint8_t> markers;
    std::vector<std::uint64_t> runs;
    void operator()(const std::uint8_t *data, std::size_t size, std::uint64_t run)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < size; i += 188) {markers.push_back(data[i + 4]); runs.push_back(run);}
    }
    std::size_t Count() {std::lock_guard<std::mutex> lock(mutex); return markers.size();}
};

template <class F> bool WaitFor(F condition, int ms = 5000)
{
    for (int i = 0; i < ms / 5; ++i) {if (condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    return condition();
}

void Frequencies()
{
    std::uint32_t hz = 0;
    Require(HelperFrequency(473143, hz) && hz == 473142857, "13ch");
    Require(HelperFrequency(479143, hz) && hz == 479142857, "14ch");
    Require(HelperFrequency(707143, hz) && hz == 707142857, "52ch");
    Require(!HelperFrequency(713143, hz), "53ch is outside the supported range");
    Require(!HelperFrequency(467143, hz), "12ch is outside the supported range");
    Require(!HelperFrequency(473144, hz), "off-grid frequency");
    Require(!HelperFrequency(0, hz), "zero frequency");
}

void LockAndStop()
{
    FakePort port; Sink sink;
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(3, {}), MakeFrame(2, Ts(1), 9000), MakeFrame(3, Mer(1353), 9000), MakeFrame(2, Ts(2), 9000)}, false});
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(!session.Locked(), "not locked before start");
    Require(session.Start(479142857) == 0, "start");
    Require(port.last_hz == 479142857 && port.restarted_launches == 0, "first launch is not a restart");
    Require(WaitFor([&] {return sink.Count() == 2;}), "both TS packets delivered");
    Require(WaitFor([&] {return session.MerX100() == 1353;}), "MER from the status frame");
    Require(session.Ready() && session.Locked() && session.Quality() == 9000, "locked after TS");
    session.Stop();
    Require(port.stop_requests >= 1 && port.waits >= 1 && port.releases >= 1, "stop requests, waits and releases");
    Require(port.Kills() == 0, "a requested stop is not a failure");
    Require(!session.Locked() && session.MerX100() == -1 && session.LastTs() == 0, "status cleared after stop");
    Require(port.Launches() == 1, "no restart after a requested stop");
}

void LaunchFailure()
{
    FakePort port; Sink sink;
    port.launch_error = -5;
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == -5, "launch error returned");
    Require(!session.Locked() && !session.Exhausted(), "no lock or exhaustion after a launch error");
    session.Stop();
    Require(port.Launches() == 1, "no retry after a launch error");
}

void RestartAfterExit()
{
    FakePort port; Sink sink;
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(2, Ts(1))}, true});
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(2, Ts(2))}, false});
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == 0, "start");
    Require(WaitFor([&] {return sink.Count() == 2;}), "TS from the restarted helper");
    Require(port.Launches() == 2 && port.restarted_launches == 1 && port.Kills() == 1, "one restart at the same frequency");
    Require(port.last_hz == 473142857, "same frequency after restart");
    Require(session.RestartCount() == 1, "restart counted");
    Require(sink.runs[0] != sink.runs[1], "restart starts a new delivery run");
    Require(session.Locked() && !session.Exhausted(), "locked again after restart");
    session.Stop();
    // 起動し直しの後の選局は起動し直しではない (helper のログを追記にしない)
    port.scripts.push_back({{MakeFrame(1, {})}, false});
    Require(session.Start(479142857) == 0, "retune after restart");
    Require(port.Launches() == 3 && port.restarted_launches == 1, "retune after a restart is not a restart");
    session.Stop();
}

void RestartWaitsForNewTs()
{
    FakePort port; Sink sink;
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(2, Ts(1)), MakeFrame(3, Mer(1353))}, true});
    port.scripts.push_back({{MakeFrame(1, {})}, false});
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == 0, "start");
    Require(WaitFor([&] {return port.Launches() == 2 && session.Ready();}), "replacement helper ready");
    Require(sink.Count() == 1, "replacement helper has not sent TS");
    Require(!session.Locked() && session.LastTs() == 0 && session.MerX100() == -1,
            "replacement Ready does not reuse lock or MER from the previous helper");
    {
        std::lock_guard<std::mutex> lock(port.mutex);
        port.current.chunks.push_back(MakeFrame(2, Ts(2)));
        port.changed.notify_all();
    }
    Require(WaitFor([&] {return sink.Count() == 2 && session.Locked();}), "replacement TS restores lock");
    session.Stop();
}

void RestartAfterInvalidOrError()
{
    FakePort port; Sink sink;
    auto bad = MakeFrame(2, Ts(1));
    bad[0] ^= 0xff;
    const std::string text = "USB I/Q stalled";
    port.scripts.push_back({{MakeFrame(1, {}), bad}, false});
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(4, std::vector<std::uint8_t>(text.begin(), text.end()))}, false});
    port.scripts.push_back({{MakeFrame(1, {}), MakeFrame(2, Ts(3))}, false});
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == 0, "start");
    Require(WaitFor([&] {return sink.Count() == 1;}), "TS after two failed helpers");
    Require(port.Launches() == 3 && port.Kills() == 2, "invalid frame and helper error are restarted");
    Require(sink.markers[0] == 3, "no TS from the failed helpers");
    session.Stop();
}

void Exhaustion()
{
    FakePort port; Sink sink;
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == 0, "start");
    // RecoveryBudget の 8 回の起動し直し (待機の合計は約 7 秒)
    Require(WaitFor([&] {return session.Exhausted();}, 15000), "budget exhausted");
    Require(port.Launches() == 9, "first launch and eight restarts");
    Require(!session.Locked(), "not locked after exhaustion");
    session.Stop();
    Require(session.Start(479142857) == 0 && !session.Exhausted(), "a retune resets exhaustion");
    session.Stop();
}

void NoRestartAfterRemoval()
{
    FakePort port; Sink sink;
    port.present = false;
    Session session(port, [&](const std::uint8_t *d, std::size_t n, std::uint64_t r) {sink(d, n, r);});
    Require(session.Start(473142857) == 0, "start");
    Require(WaitFor([&] {return port.Kills() == 1;}), "failed helper collected");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    session.Disconnect();
    session.Stop();
    Require(port.Launches() == 1, "no restart while the device is missing and then removed");
}
}

int main()
{
    Frequencies();
    LockAndStop();
    LaunchFailure();
    RestartAfterExit();
    RestartWaitsForNewTs();
    RestartAfterInvalidOrError();
    Exhaustion();
    NoRestartAfterRemoval();
    std::puts("RTL-SDR session: frequency, lock, stop, restart, exhaustion and removal cases passed");
    return 0;
}
