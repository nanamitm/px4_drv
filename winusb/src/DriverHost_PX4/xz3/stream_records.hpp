// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace px4::xz3 {

template<class Sink>
std::size_t DeliverRecords(const std::uint8_t* data, std::size_t length, Sink sink)
{
    constexpr std::size_t RecordSize = 192, PrefixSize = 4;
    std::size_t skipped = 0;
    // DMAが固定長のレコードを生成する。同期バイトが壊れても次の境界は変わらない。
    for (std::size_t offset = 0; length - offset >= RecordSize; offset += RecordSize) {
        const auto* packet = data + offset + PrefixSize;
        if (packet[0] == 0x47)
            sink(packet);
        else
            ++skipped;
    }
    return skipped;
}

}
