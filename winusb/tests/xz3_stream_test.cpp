// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/xz3/stream_records.hpp"
#include "../src/DriverHost_PX4/xz3/pump_progress.hpp"
#include <array>
#include <cstdio>
#include <vector>

static bool TestPartialPump()
{
    using namespace px4::xz3;
    const auto small = PumpSize(3), large = PumpSize(0);
    PumpProgress progress;
    // 512バイトだけ先に届き、しばらく無入力が続く。残りのバイトを次の操作12で補う。
    if (!ValidPumpReply(2, 0, 512, 512, 512) || !ValidPumpReply(2, 0, 0, 512, 512))
        return false;
    progress.Sent(512);
    progress.Sent(0);
    if (progress.Complete(large) || progress.Stage(0, large + small) != 0)
        return false;
    progress.Sent(large);
    if (!progress.Complete(large))
        return false;
    progress.Collected(large);
    // 先行した512バイトは次の読み出しの一部で、切り捨てるとTS境界がずれる。
    if (progress.Complete(small) || progress.Stage(2, small * 2) != 3)
        return false;
    progress.Sent(small - 512);
    if (!progress.Complete(small))
        return false;
    progress.Collected(small);
    return !progress.Complete(small) && progress.Stage(2, small * 2) == 2 &&
           ValidPumpReply(0, 3, small, small, small) &&
           !ValidPumpReply(0, 3, 512, 512, 512) &&
           !ValidPumpReply(2, 3, 513, 512, 512) &&
           !ValidPumpReply(2, 3, 512, 512, RingBytes) &&
           !ValidPumpReply(6, 3, 0, 0, 0);
}

int main()
{
    // 壊れたレコードのペイロードに0x47があっても、既知の境界を維持する。
    std::array<std::uint8_t, 4 * 192> data{};
    data[4] = data[2 * 192 + 4] = data[3 * 192 + 4] = 0x47;
    data[192 + 5] = 0x47;
    data[3 * 192 + 5] = 0x42;
    std::vector<std::uint8_t> received;
    const auto skipped = px4::xz3::DeliverRecords(data.data(), data.size(), [&](const std::uint8_t* packet) {
        received.push_back(packet[1]);
    });
    if (skipped != 1 || received != std::vector<std::uint8_t>{0, 0, 0x42})
        return 1;
    if (!TestPartialPump()) {
        std::puts("xz3_stream_test: partial pump recovery failed");
        return 1;
    }
    std::puts("xz3_stream_test: passed");
    return 0;
}
