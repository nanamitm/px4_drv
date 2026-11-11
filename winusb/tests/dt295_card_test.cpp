// SPDX-License-Identifier: GPL-2.0-only
// PIX-DT295 内蔵カードUARTの手順と、BC要求での直接アクセスを模擬I/Oで確認する。USBは開かない。
#include "../src/DriverHost_PX4/dt295/card_uart.hpp"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using px4::dt295::CardBus;
using px4::dt295::CardUart;
using px4::dt295::DirectBus;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

struct Op { char kind; std::uint32_t address, value; };

class MockBus final : public CardBus {
public:
    std::vector<Op> ops;
    std::map<std::uint32_t, std::uint32_t> registers;
    std::vector<std::uint8_t> rx;
    int fail_at = -1, error = -EIO;
    int Read32(std::uint32_t address, std::uint32_t &value) override
    {
        if (Fail()) return error;
        ops.push_back({'r', address, 0});
        if (address == 0xfea02c && !rx.empty()) {
            value = rx.front();
            rx.erase(rx.begin());
            return 0;
        }
        value = registers[address];
        return 0;
    }
    int Write32(std::uint32_t address, std::uint32_t value) override
    {
        if (Fail()) return error;
        ops.push_back({'w', address, value});
        return 0;
    }
    int Pause(unsigned milliseconds) override
    {
        if (Fail()) return error;
        ops.push_back({'p', 0, milliseconds});
        return 0;
    }
private:
    bool Fail() { return fail_at >= 0 && static_cast<int>(ops.size()) == fail_at; }
};

void TestPresent()
{
    MockBus bus;
    CardUart uart(bus);
    bool present = false;
    bus.registers[0xfea01c] = 0x400;
    CHECK(!uart.Present(present) && present);
    bus.registers[0xfea01c] = 0;
    CHECK(!uart.Present(present) && !present);
    bus.registers[0xfea01c] = 0x3ff;
    CHECK(!uart.Present(present) && !present);
    MockBus failing;
    failing.fail_at = 0;
    CardUart broken(failing);
    present = true;
    CHECK(broken.Present(present) == -EIO && present);
}

void TestReset()
{
    MockBus bus;
    CardUart uart(bus);
    CHECK(!uart.Reset());
    // 実機でATRを受信した手順。値と順序、待機の位置を固定する。
    const std::vector<Op> expected = {
        {'w', 0xfea018, 0}, {'w', 0xfea034, 0}, {'w', 0xfea01c, 0x3ff}, {'w', 0xfea004, 0},
        {'w', 0xfea014, 0}, {'w', 0xfea000, 0}, {'p', 0, 20}, {'w', 0xfea000, 7}, {'p', 0, 20},
        {'w', 0xfea004, 8}, {'w', 0xfea008, 0xba7f}, {'p', 0, 20}, {'w', 0xfea004, 0xa},
        {'w', 0xfea000, 0}, {'p', 0, 20}, {'w', 0xfea000, 7}, {'w', 0xfea03c, 0x3b},
        {'w', 0xfea020, 9600}, {'w', 0xfea024, 0}, {'w', 0xfea018, 0}, {'w', 0xfea014, 0x20},
        {'p', 0, 50}, {'w', 0xfea034, 1}, {'w', 0xfea004, 0xe},
    };
    CHECK(bus.ops.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < bus.ops.size(); ++i)
        CHECK(bus.ops[i].kind == expected[i].kind && bus.ops[i].address == expected[i].address &&
              bus.ops[i].value == expected[i].value);
    for (int at = 0; at < static_cast<int>(expected.size()); ++at) {
        MockBus failing;
        failing.fail_at = at;
        CardUart broken(failing);
        CHECK(broken.Reset() == -EIO);
        CHECK(static_cast<int>(failing.ops.size()) == at);
    }
}

void TestPowerDown()
{
    MockBus bus;
    CardUart uart(bus);
    CHECK(!uart.PowerDown());
    // ブート直後の待機値 (0xfea000=7, 0xfea004=4, 0xfea014=0x20) で終わる。
    std::map<std::uint32_t, std::uint32_t> last;
    for (const auto &op : bus.ops) if (op.kind == 'w') last[op.address] = op.value;
    CHECK(last[0xfea000] == 7 && last[0xfea004] == 4 && last[0xfea014] == 0x20 && last[0xfea008] == 0xba7f);
    CHECK(last[0xfea020] == 0 && last[0xfea034] == 0 && last[0xfea010] == 0x200);
}

void TestBaudrate()
{
    MockBus bus;
    CardUart uart(bus);
    CHECK(uart.SetBaudrate(14400) == -EINVAL && bus.ops.empty());
    CHECK(!uart.SetBaudrate(19200));
    CHECK(bus.ops.size() == 4 && bus.ops[0].value == 0x5d5d && bus.ops[1].value == 0x10000 &&
          bus.ops[2].value == 107 && bus.ops[3].value == 36559);
}

void TestReadBytes()
{
    MockBus bus;
    CardUart uart(bus);
    std::uint8_t data[8]{};
    std::size_t count = 9;
    CHECK(uart.ReadBytes(data, 0, count) == -EINVAL && !count && bus.ops.empty());
    CHECK(uart.ReadBytes(nullptr, 4, count) == -EINVAL);
    CHECK(uart.ReadBytes(data, CardUart::MaxTransfer + 1, count) == -EINVAL);
    bus.registers[0xfea014] = 5u << 16;
    bus.rx = {1, 2, 3, 4, 5};
    CHECK(!uart.ReadBytes(data, 3, count) && count == 3 && data[0] == 1 && data[2] == 3);
    bus.registers[0xfea014] = 2u << 16;
    CHECK(!uart.ReadBytes(data, 8, count) && count == 2 && data[0] == 4 && data[1] == 5);
    bus.registers[0xfea014] = 0;
    CHECK(!uart.ReadBytes(data, 8, count) && !count);
    MockBus failing;
    failing.registers[0xfea014] = 4u << 16;
    failing.rx = {9, 9, 9, 9};
    failing.fail_at = 3;
    CardUart broken(failing);
    CHECK(broken.ReadBytes(data, 8, count) == -EIO && count == 2);
}

// ファームウェアの一括転送を模擬する。読み書きの要求長と回数を記録する。
class BulkBus final : public CardBus {
public:
    MockBus direct;
    std::vector<std::size_t> reads, writes;
    std::vector<std::uint8_t> rx, tx;
    int Read32(std::uint32_t address, std::uint32_t &value) override { return direct.Read32(address, value); }
    int Write32(std::uint32_t address, std::uint32_t value) override { return direct.Write32(address, value); }
    int Pause(unsigned milliseconds) override { return direct.Pause(milliseconds); }
    bool Bulk() const override { return true; }
    int Fifo(bool write, std::uint8_t *data, std::size_t &count) override
    {
        if (write) {
            writes.push_back(count);
            tx.insert(tx.end(), data, data + count);
            return 0;
        }
        reads.push_back(count);
        const auto n = (std::min)(count, rx.size());
        std::copy_n(rx.begin(), n, data);
        rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(n));
        count = n;
        return 0;
    }
};

void TestBulk()
{
    BulkBus bus;
    CardUart uart(bus);
    std::uint8_t data[CardUart::BulkChunk]{};
    std::size_t count = 9;
    // 空のFIFOでは一括転送の往復を使わない。
    bus.direct.registers[0xfea014] = 0;
    CHECK(!uart.ReadBytes(data, sizeof(data), count) && !count && bus.reads.empty());
    for (int i = 0; i < 40; ++i) bus.rx.push_back(static_cast<std::uint8_t>(i));
    bus.direct.registers[0xfea014] = 40u << 16;
    CHECK(!uart.ReadBytes(data, sizeof(data), count) && count == 32 && data[31] == 31);
    CHECK(bus.reads == std::vector<std::size_t>({32}));
    bus.direct.registers[0xfea014] = 8u << 16;
    CHECK(!uart.ReadBytes(data, 5, count) && count == 5 && data[0] == 32 && bus.reads.back() == 5);
    // 1バイトずつのFIFO読み出しを使わない。
    for (const auto &op : bus.direct.ops) CHECK(op.address != 0xfea02c);

    std::uint8_t block[70];
    for (int i = 0; i < 70; ++i) block[i] = static_cast<std::uint8_t>(0x80 + i);
    CHECK(!uart.Queue(block, sizeof(block)));
    CHECK(bus.writes == std::vector<std::size_t>({32, 32, 6}));
    CHECK(bus.tx.size() == 70 && bus.tx[69] == 0x80 + 69);
    for (const auto &op : bus.direct.ops) CHECK(op.address != 0xfea028);

    // 一括転送がない経路は、1回の呼び出しを8バイトまでに区切る。
    MockBus slow;
    CardUart direct(slow);
    slow.registers[0xfea014] = 20u << 16;
    slow.rx.assign(20, 0x55);
    CHECK(!direct.ReadBytes(data, sizeof(data), count) && count == CardUart::DirectChunk);
}

void TestTransmit()
{
    MockBus bus;
    CardUart uart(bus);
    const std::uint8_t block[3] = {0x00, 0xc1, 0x01};
    CHECK(uart.Queue(block, 0) == -EINVAL && uart.Transmit(0) == -EINVAL && bus.ops.empty());
    CHECK(!uart.BeginTransmit() && !uart.Queue(block, 2) && !uart.Queue(block + 2, 1) && !uart.Transmit(3));
    const std::vector<Op> expected = {
        {'w', 0xfea03c, 0}, {'w', 0xfea038, 0}, {'w', 0xfea028, 0x00}, {'w', 0xfea028, 0xc1},
        {'w', 0xfea028, 0x01}, {'w', 0xfea01c, 0x3ff}, {'w', 0xfea010, 0x200}, {'w', 0xfea00c, 1},
    };
    CHECK(bus.ops.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < bus.ops.size(); ++i)
        CHECK(bus.ops[i].address == expected[i].address && bus.ops[i].value == expected[i].value);
}

// BC要求の番地の符号化と、カードUARTの16語を模擬する。
struct Device {
    std::map<std::uint32_t, std::uint32_t> mmio;
    std::vector<std::uint32_t> transfers;
    int fail_at = -1;
    static int Control(void *ctx, std::uint8_t type, std::uint8_t request, std::uint16_t value,
                       std::uint16_t index, void *data, std::uint16_t size, unsigned)
    {
        auto &d = *static_cast<Device *>(ctx);
        if (request != 0xbc || (value & 0xff) || size != 4) return -EPROTO;
        const std::uint32_t address = std::uint32_t(value & 0xff00) << 8 | index;
        d.transfers.push_back(address);
        if (d.fail_at >= 0 && static_cast<int>(d.transfers.size()) - 1 == d.fail_at) return -EIO;
        auto *bytes = static_cast<std::uint8_t *>(data);
        if (type & 0x80) {
            const auto v = d.mmio[address];
            bytes[0] = std::uint8_t(v >> 24); bytes[1] = std::uint8_t(v >> 16);
            bytes[2] = std::uint8_t(v >> 8); bytes[3] = std::uint8_t(v);
        } else {
            d.mmio[address] = std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 |
                              std::uint32_t(bytes[2]) << 8 | bytes[3];
        }
        return 0;
    }
    static std::uint32_t Now(void *) { return 0; }
    static void Delay(void *, unsigned) {}
    dt295_io Io() { return {Control, Now, Delay, nullptr, this, nullptr, nullptr}; }
};

void TestDirect()
{
    Device device;
    device.mmio[0x00fea01c] = 0x400;
    {
        DirectBus bus(device.Io());
        std::uint32_t value = 0;
        CHECK(!bus.Read32(0xfea01c, value) && value == 0x400);
        CHECK(!bus.Write32(0xfea004, 0xe) && device.mmio[0x00fea004] == 0xe);
        CHECK(device.transfers.size() == 2 && device.transfers[0] == 0x00fea01c && device.transfers[1] == 0x00fea004);
        // カードUARTの16語以外は転送せずに拒否する。
        CHECK(bus.Read32(0xfe9ffc, value) == -EINVAL && bus.Write32(0xfea040, 0) == -EINVAL);
        CHECK(bus.Read32(0xfea002, value) == -EINVAL && bus.Write32(0xf10014, 0) == -EINVAL);
        CHECK(device.transfers.size() == 2 && !bus.Error());
    }
    // 失敗後は同じ操作内の転送を止める。
    Device failing;
    failing.fail_at = 1;
    {
        DirectBus bus(failing.Io());
        CardUart uart(bus);
        CHECK(uart.Reset() == -EIO && bus.Error() == -EIO);
        CHECK(failing.transfers.size() == 2);
        std::uint32_t value = 0;
        CHECK(bus.Read32(0xfea01c, value) == -EIO && failing.transfers.size() == 2);
        CHECK(bus.Pause(1) == -EIO);
    }
    // 待機はyieldへ委ね、所有者が制御ロックを手放せるようにする。
    Device yielding;
    {
        std::vector<unsigned> waits;
        DirectBus bus(yielding.Io(), [&waits](unsigned ms) { waits.push_back(ms); return 0; });
        CardUart uart(bus);
        CHECK(!uart.Reset());
        CHECK(waits == std::vector<unsigned>({20, 20, 20, 20, 50}));
        CHECK(yielding.mmio[0x00fea004] == 0xe && yielding.mmio[0x00fea034] == 1);
    }
    Device removed;
    {
        DirectBus bus(removed.Io(), [](unsigned) { return -ENODEV; });
        CardUart uart(bus);
        CHECK(uart.Reset() == -ENODEV && bus.Error() == -ENODEV);
        CHECK(removed.transfers.size() == 6);
    }
    // 一括転送の応答が要求と合わなければ、FIFOの位置が不明になるため失敗として止める。
    Device bulk;
    {
        std::uint8_t data[4] = {1, 2, 3, 4};
        std::size_t count = 4;
        DirectBus none(bulk.Io());
        CHECK(!none.Bulk() && none.Fifo(true, data, count) == -EOPNOTSUPP && !count);
        DirectBus shortWrite(bulk.Io(), {}, [](bool, std::uint8_t *, std::size_t &n) { n -= 1; return 0; });
        count = 4;
        CHECK(shortWrite.Bulk() && shortWrite.Fifo(true, data, count) == -EPROTO && !count);
        CHECK(shortWrite.Error() == -EPROTO);
        std::uint32_t value = 0;
        CHECK(shortWrite.Read32(0xfea01c, value) == -EPROTO);
        DirectBus overRead(bulk.Io(), {}, [](bool, std::uint8_t *, std::size_t &n) { n += 1; return 0; });
        count = 4;
        CHECK(overRead.Fifo(false, data, count) == -EPROTO);
        DirectBus failed(bulk.Io(), {}, [](bool, std::uint8_t *, std::size_t &) { return -EIO; });
        count = 4;
        CHECK(failed.Fifo(false, data, count) == -EIO && failed.Error() == -EIO);
        DirectBus good(bulk.Io(), {}, [](bool, std::uint8_t *d, std::size_t &n) { d[0] = 0x3b; n = 1; return 0; });
        count = 4;
        CHECK(!good.Fifo(false, data, count) && count == 1 && data[0] == 0x3b && !good.Error());
        CHECK(bulk.transfers.size() == 0);
    }
}
}

int main()
{
    TestPresent();
    TestReset();
    TestPowerDown();
    TestBaudrate();
    TestReadBytes();
    TestTransmit();
    TestBulk();
    TestDirect();
    if (failures) {
        std::printf("FAIL: %d DT295 card checks\n", failures);
        return 1;
    }
    std::puts("PASS: DT295 card UART sequences, detect polarity, FIFO bounds and direct BC access (no hardware)");
    return 0;
}
