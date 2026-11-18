// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/xz3/stream_records.hpp"
#include <array>
#include <cstdio>
#include <vector>

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
    std::puts("xz3_stream_test: passed");
    return 0;
}
