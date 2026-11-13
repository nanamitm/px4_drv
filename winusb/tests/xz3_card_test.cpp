// SPDX-License-Identifier: GPL-2.0-only
// GV-MVP/XZ3 内蔵カードUARTの電源手順と、操作14の一括転送の使い方を模擬I/Oで確認する。USBは開かない。
#include "../src/DriverHost_PX4/xz3/card_uart.hpp"
#include <cerrno>
#include <cstdio>
#include <vector>

using px4::dt295::CardBus;
using px4::xz3::CardUart;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

constexpr std::uint32_t Control1 = 0xfea004, FifoStatus = 0xfea014, RxData = 0xfea02c;

struct Op { char kind; std::uint32_t address, value; };

class MockBus final : public CardBus {
public:
    explicit MockBus(bool bulk = false) : bulk_(bulk) {}
    std::vector<Op> ops;
    std::vector<std::uint8_t> rx, tx;
    std::size_t fifo_reads = 0, fifo_writes = 0;
    int Read32(std::uint32_t address, std::uint32_t &value) override
    {
        ops.push_back({'r', address, 0});
        value = 0;
        if (address == FifoStatus)
            value = static_cast<std::uint32_t>(rx.size()) << 16;
        else if (address == RxData && !rx.empty()) {
            value = rx.front();
            rx.erase(rx.begin());
        }
        return 0;
    }
    int Write32(std::uint32_t address, std::uint32_t value) override
    {
        ops.push_back({'w', address, value});
        return 0;
    }
    int Pause(unsigned milliseconds) override
    {
        ops.push_back({'p', 0, milliseconds});
        return 0;
    }
    bool Bulk() const override { return bulk_; }
    // 操作14と同じく、受信は要求の大きさに関係なく最大31バイトを取り出す。
    int Fifo(bool write, std::uint8_t *data, std::size_t &count) override
    {
        if (write) {
            ++fifo_writes;
            tx.insert(tx.end(), data, data + count);
            return 0;
        }
        ++fifo_reads;
        std::size_t moved = 0;
        while (!rx.empty() && moved < CardUart::FifoChunk) {
            data[moved++] = rx.front();
            rx.erase(rx.begin());
        }
        count = moved;
        return 0;
    }
private:
    bool bulk_;
};

std::vector<std::uint32_t> Control1Writes(const MockBus &bus)
{
    std::vector<std::uint32_t> values;
    for (const auto &op : bus.ops)
        if (op.kind == 'w' && op.address == Control1)
            values.push_back(op.value);
    return values;
}

void TestResetEndsWithCmdvccLow()
{
    MockBus bus;
    CardUart uart(bus);
    CHECK(uart.Reset() == 0);
    const auto control1 = Control1Writes(bus);
    // 先頭でCMDVCCNをHighにし、PIX-DT295の19項目 (最後は0x0e) の後、20 ms待ってLowにする。
    CHECK(!control1.empty() && control1.front() == 0x0c);
    CHECK(control1.size() >= 2 && control1[control1.size() - 2] == 0x0e);
    CHECK(control1.back() == 0x06);
    CHECK(bus.ops.size() >= 2);
    const auto &pause = bus.ops[bus.ops.size() - 2];
    CHECK(pause.kind == 'p' && pause.value == 20);
    CHECK(bus.ops.back().kind == 'w' && bus.ops.back().address == Control1);
}

void TestPowerDownKeepsCmdvccHigh()
{
    MockBus bus;
    CardUart uart(bus);
    CHECK(uart.PowerDown() == 0);
    const auto control1 = Control1Writes(bus);
    // 最初の書き込みでCMDVCCNをHighにし、以後の0xfea004の値はすべてbit 3を持つ。
    CHECK(!bus.ops.empty() && bus.ops.front().kind == 'w' && bus.ops.front().address == Control1 &&
          bus.ops.front().value == 0x0c);
    bool high = !control1.empty();
    for (auto value : control1)
        high = high && (value & 0x8);
    CHECK(high);
}

void TestBulkReadUsesOneFifoTransfer()
{
    MockBus bus(true);
    CardUart uart(bus);
    for (std::uint8_t i = 0; i < 40; ++i)
        bus.rx.push_back(i);
    std::uint8_t data[CardUart::FifoChunk]{};
    std::size_t count = 0;
    CHECK(uart.ReadBytes(data, sizeof(data), count) == 0);
    CHECK(count == CardUart::FifoChunk && data[0] == 0 && data[30] == 30);
    // 受信数のレジスターを読まず、1回の一括転送で読む。
    CHECK(bus.ops.empty() && bus.fifo_reads == 1);
    // 31バイト未満の受け皿は、取り出したバイトを失うため拒否する。
    std::uint8_t small[8]{};
    CHECK(uart.ReadBytes(small, sizeof(small), count) == -EINVAL && count == 0);
    CHECK(bus.fifo_reads == 1);
}

void TestDirectReadFallsBack()
{
    MockBus bus(false);
    CardUart uart(bus);
    bus.rx = {0x3b, 0xf0, 0x12};
    std::uint8_t data[CardUart::FifoChunk]{};
    std::size_t count = 0;
    CHECK(uart.ReadBytes(data, sizeof(data), count) == 0);
    CHECK(count == 3 && data[0] == 0x3b && data[2] == 0x12);
    CHECK(bus.fifo_reads == 0);
}

void TestBulkQueueSendsEverything()
{
    MockBus bus(true);
    CardUart uart(bus);
    std::vector<std::uint8_t> command(100);
    for (std::size_t i = 0; i < command.size(); ++i)
        command[i] = static_cast<std::uint8_t>(i);
    CHECK(uart.Queue(command.data(), command.size()) == 0);
    CHECK(bus.tx == command);
    CHECK(uart.Queue(command.data(), 0) == -EINVAL);
}
}

int main()
{
    TestResetEndsWithCmdvccLow();
    TestPowerDownKeepsCmdvccHigh();
    TestBulkReadUsesOneFifoTransfer();
    TestDirectReadFallsBack();
    TestBulkQueueSendsEverything();
    if (failures)
        std::printf("%d failure(s)\n", failures);
    else
        std::printf("xz3_card_test: all tests passed\n");
    return failures ? 1 : 0;
}
