// SPDX-License-Identifier: GPL-2.0-only
// PIX-DT295 のWinUSB版とLinux版で共有する制御経路 (ControlChannel、CardSession) を模擬の転送で確認する。
// USBは開かない。Windowsではdt295_control_channel_test.vcxproj、Linuxではcuse/Makefileのcheckでビルドする。
#include "../src/DriverHost_PX4/dt295/control_channel.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

using px4::dt295::CardSession;
using px4::dt295::CardUart;
using px4::dt295::ControlChannel;
using px4::dt295::ControlTransport;
using px4::dt295::SourceFirmware;

extern "C" int msg_printf(const char *, ...) { return 0; }

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

// 実機のTransportと同じく、失敗した後は解除されるまで転送を始めない。
class MockTransport final : public ControlTransport {
public:
    int failed = 0;
    int fail_at = -1;          // この番号の転送を失敗させる (0から数える)
    unsigned transfers = 0, clears = 0;
    std::uint32_t status = 0;  // 読み出しで返す値 (カード検出は0x400)
    void Begin(unsigned) override {}
    void BeginRestore(unsigned) override { failed = 0; }
    int Failed() const override { return failed; }
    void ClearFailure() override { failed = 0; ++clears; }
    dt295_io Callbacks() override { return {Control, Now, Delay, nullptr, this, nullptr, nullptr}; }
    void ReportTiming(const char *) override {}
    unsigned long LastError() const override { return 0; }

private:
    static int Control(void *ctx, std::uint8_t type, std::uint8_t, std::uint16_t, std::uint16_t, void *data,
                       std::uint16_t size, unsigned)
    {
        auto &self = *static_cast<MockTransport *>(ctx);
        if (self.failed)
            return self.failed;
        if (static_cast<int>(self.transfers++) == self.fail_at)
            return self.failed = -EIO;
        if ((type & 0x80) && size == 4) {
            auto *bytes = static_cast<std::uint8_t *>(data);
            bytes[0] = static_cast<std::uint8_t>(self.status >> 24);
            bytes[1] = static_cast<std::uint8_t>(self.status >> 16);
            bytes[2] = static_cast<std::uint8_t>(self.status >> 8);
            bytes[3] = static_cast<std::uint8_t>(self.status);
        }
        return 0;
    }
    static std::uint32_t Now(void *) { return 0; }
    static void Delay(void *, unsigned) {}
};

struct Fixture {
    SourceFirmware source{true};
    MockTransport transport;
    std::atomic_bool available{true};
    std::vector<unsigned> waits;
    unsigned holds = 0;
    std::function<int(unsigned)> on_wait;
    ControlChannel channel{source, transport, available,
        [this](unsigned ms) { waits.push_back(ms); return on_wait ? on_wait(ms) : 0; },
        [this](ControlChannel::Hold kind, ControlChannel::Clock::time_point) { holds += kind == ControlChannel::Hold::Card; }};
};

void TestUnavailable()
{
    Fixture f;
    f.available = false;
    bool called = false;
    CHECK(f.channel.Card([&called](CardUart &) { called = true; return 0; }) == -ENODEV);
    CHECK(!called && !f.transport.transfers);
}

void TestPendingFailure()
{
    // 他の経路の失敗が残る間は、カード操作から転送を始めず、失敗も解除しない。
    Fixture f;
    f.transport.failed = -ETIMEDOUT;
    bool called = false;
    CHECK(f.channel.Card([&called](CardUart &) { called = true; return 0; }) == -ETIMEDOUT);
    CHECK(!called && !f.transport.transfers && f.transport.failed == -ETIMEDOUT && !f.transport.clears);
}

void TestOwnFailureCleared()
{
    // カード操作自身の転送失敗は、その操作の結果として返し、後続のために解除する。
    Fixture f;
    f.transport.fail_at = 2;
    CHECK(f.channel.Card([](CardUart &uart) { return uart.Reset(); }) == -EIO);
    CHECK(!f.transport.failed && f.transport.clears == 1);
    CHECK(f.channel.Card([](CardUart &uart) { bool present = false; return uart.Present(present); }) == 0);
}

void TestWaitReleasesLock()
{
    // 手順内の待機では制御ロックを手放し、SERVICEなど他の要求が進めるようにする。
    Fixture f;
    bool acquired_during_wait = false;
    f.on_wait = [&f, &acquired_during_wait](unsigned) {
        auto other = std::async(std::launch::async, [&f] { auto command = f.channel.Acquire(); return true; });
        acquired_during_wait = other.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        return 0;
    };
    CHECK(f.channel.Card([](CardUart &uart) { return uart.Reset(); }) == 0);
    CHECK(acquired_during_wait);
    CHECK(f.waits.size() == 5 && f.waits[0] == 20 && f.waits.back() == 50);
    // 待機の前後で保持区間を区切って記録する。
    CHECK(f.holds == f.waits.size() + 1);
}

void TestInheritedFailure()
{
    // 待機中に他の経路の転送が失敗したら、その失敗を解除せずにカード操作を終える。
    Fixture f;
    f.on_wait = [&f](unsigned) { f.transport.failed = -EPROTO; return 0; };
    CHECK(f.channel.Card([](CardUart &uart) { return uart.Reset(); }) == -EPROTO);
    CHECK(f.transport.failed == -EPROTO && !f.transport.clears);
}

void TestRemovedDuringWait()
{
    Fixture f;
    f.on_wait = [](unsigned) { return -ENODEV; };
    CHECK(f.channel.Card([](CardUart &uart) { return uart.Reset(); }) == -ENODEV);
    CHECK(f.waits.size() == 1);
}

void TestCardSession()
{
    Fixture f;
    CardSession card;
    // 未対応の速度は転送せずに断る。
    CHECK(card.SetBaudrate(f.channel, 9600) == -ENOTSUP && !f.transport.transfers);
    CHECK(card.SetBaudrate(f.channel, 19200) == 0 && f.transport.transfers == 4);
    const std::uint8_t one = 0;
    CHECK(card.Write(f.channel, nullptr, 1) == -EINVAL && card.Write(f.channel, &one, 0) == -EINVAL);
    bool present = false;
    f.transport.status = 0x400;
    CHECK(card.Detect(f.channel, present) == 0 && present);
    f.transport.status = 0;
    CHECK(card.Detect(f.channel, present) == 0 && !present);
    // 書き込みはFIFOへ区切って積み、最後に1回だけ送信を始める。
    const unsigned before = f.transport.transfers;
    std::uint8_t frame[40]{};
    CHECK(card.Write(f.channel, frame, sizeof(frame)) == 0);
    CHECK(f.transport.transfers > before);
}

void TestRunServiceStops()
{
    Fixture f;
    ControlChannel::ServiceStats stats;
    unsigned observed = 0;
    CHECK(f.channel.RunService([] { return true; },
        [&observed](ControlChannel::Clock::time_point, ControlChannel::Clock::time_point, int) { ++observed; }, stats) == 0);
    CHECK(!stats.calls && !observed);
}
}

int main()
{
    TestUnavailable();
    TestPendingFailure();
    TestOwnFailureCleared();
    TestWaitReleasesLock();
    TestInheritedFailure();
    TestRemovedDuringWait();
    TestCardSession();
    TestRunServiceStops();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS dt295_control_channel_test\n");
    return 0;
}
