// SPDX-License-Identifier: GPL-2.0-only
// WinUSB 版と Linux 版で共有する torne::Session、CardSession、TsDelivery、Status の解釈を、
// 模擬の SessionPort で確かめる。USB と OS の API を使わないため、Windows と Linux の双方でビルドできる。
#include "../src/DriverHost_PX4/torne/session.hpp"
#include <algorithm>
#include <cerrno>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <vector>

extern "C" int msg_printf(const char *, ...) { return 0; }

using namespace px4::torne;

namespace {
void Require(bool condition, const char *name)
{
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        std::exit(1);
    }
}

// 32bit のレジスターを持つ模擬の機器。カード UART の FIFO だけを特別に扱う。
class MockPort final : public SessionPort, private ControlTransport, private BootstrapWait, private FrontendSignal {
public:
    ControlTransport &Transport() noexcept override { return *this; }
    BootstrapWait &Wait() noexcept override { return *this; }
    FrontendSignal &Signal() noexcept override { return *this; }
    void Begin(Clock::time_point deadline, bool stoppable) noexcept override
    {
        begins.push_back({deadline, stoppable});
    }
    int TransferPipe(std::uint8_t, std::uint8_t *, std::size_t, std::size_t &transferred, Clock::time_point) override
    {
        transferred = 0;
        ++pipe_calls;
        return 5;
    }
    bool TimedOut(int error) const noexcept override { return error == 1460; }

    int Transfer(const MemoryRequest &request, std::uint8_t *data, std::size_t length, std::size_t &transferred) override
    {
        ++transfers;
        transferred = 0;
        if (fail) return fail;
        const std::uint32_t address = (std::uint32_t(request.value & 0xff) << 24) |
            (std::uint32_t(request.value & 0xff00) << 8) | request.index;
        if (length != 4) return 87;
        if (request.type == 0xc0) {
            std::uint32_t value = registers[address];
            if (address == 0xfea014) value = static_cast<std::uint32_t>(fifo.size()) << 16;
            if (address == 0xfea02c && !fifo.empty()) { value = fifo.front(); fifo.pop_front(); }
            for (int i = 0; i < 4; ++i) data[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
        } else {
            const std::uint32_t value = (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) |
                (std::uint32_t(data[2]) << 8) | data[3];
            registers[address] = value;
            if (address == 0xfea028) sent.push_back(static_cast<std::uint8_t>(value));
        }
        transferred = length;
        return 0;
    }
    int Pause(unsigned) override { return 0; }

    State Poll(bool stoppable, int &) noexcept override
    {
        if (removed) return State::removed;
        return stoppable && stop ? State::stopped : State::clear;
    }
    bool StopRequested() noexcept override { return stop; }
    int Sleep(bool, Clock::time_point, std::int64_t) noexcept override { return 0; }
    bool Aborted(int error) const noexcept override { return error == 995; }
    int AbortError() const noexcept override { return 995; }

    struct BeginRecord {
        Clock::time_point deadline;
        bool stoppable;
    };
    std::vector<BeginRecord> begins;
    std::map<std::uint32_t, std::uint32_t> registers;
    std::deque<std::uint8_t> fifo;
    std::vector<std::uint8_t> sent;
    int fail = 0, transfers = 0, pipe_calls = 0;
    bool removed = false, stop = false;
};

std::vector<std::uint8_t> Status(bool locked, bool has_cnr, double cnr)
{
    std::vector<std::uint8_t> status(24);
    status[0] = locked ? 1 : 0;
    status[1] = has_cnr ? 1 : 0;
    std::memcpy(status.data() + 8, &cnr, sizeof(cnr));
    return status;
}

void StatusTests()
{
    Require(StatusLocked(Status(true, false, 0)), "status byte 0 reports lock");
    Require(!StatusLocked(Status(false, false, 0)), "status byte 0 reports no lock");
    Require(!StatusLocked(std::vector<std::uint8_t>(23, 1)), "short status is never locked");
    std::int32_t value = -1;
    Require(ReadStatusValue(Status(true, true, 25.5), StatusReading::cnr, value) == 0 && value == 25500,
        "C/N is read in thousandths");
    Require(ReadStatusValue(Status(true, false, 25.5), StatusReading::cnr, value) == -ENOSYS && value == 0,
        "missing C/N is ENOSYS");
    Require(ReadStatusValue(Status(true, true, 0), StatusReading::signal_strength, value) == -ENOSYS,
        "missing signal strength is ENOSYS");
    Require(ReadStatusValue(Status(true, true, std::numeric_limits<double>::quiet_NaN()), StatusReading::cnr, value) == -EIO,
        "NaN is rejected");
    Require(ReadStatusValue(Status(true, true, 250.0), StatusReading::cnr, value) == -EIO, "out-of-range value is rejected");
    Require(ReadStatusValue(std::vector<std::uint8_t>(8), StatusReading::cnr, value) == -EIO, "short status is rejected");
}

void SessionCommandTests()
{
    MockPort port;
    Session session(port);
    std::vector<std::uint8_t> response;
    Require(!session.Command(0x11, {}, response, Clock::now() + std::chrono::seconds(1)) && port.transfers == 0,
        "commands before the frontend is initialized do not touch the device");
    port.stop = true;
    Require(!session.Command(0x11, {}, response, Clock::now() + std::chrono::seconds(1), true) && port.transfers == 0,
        "stoppable command returns before any transfer once stop is requested");

    port.begins.clear();
    Require(session.PrepareStream(Clock::now() + std::chrono::seconds(1), true) == StreamResult::invalid_state && port.transfers == 0,
        "stream engine refuses to prepare before bootstrap");
    Require(port.begins.size() == 1 && port.begins[0].stoppable, "stream preparation honours receiver stop");
    port.begins.clear();
    Require(session.StopStream(Clock::now() + std::chrono::seconds(2)) == StreamResult::invalid_state, "stream stop before bootstrap");
    Require(port.begins.size() == 1 && !port.begins[0].stoppable, "stream stop is never cut short by receiver stop");
}

void CardTests()
{
    MockPort port;
    Session session(port);
    CardSession card;
    bool present = false;
    port.registers[0xfea01c] = 0;
    Require(card.Detect(session, present) == 0 && present, "torne card is present while bit 10 is clear");
    port.registers[0xfea01c] = 0x400;
    Require(card.Detect(session, present) == 0 && !present, "torne card is absent while bit 10 is set");

    // 各操作の期限は制御 mutex を取得した後から 1 秒で数える (カード操作が mutex を数百 ms 保持するため)。
    port.begins.clear();
    const auto before = Clock::now();
    Require(card.Detect(session, present) == 0, "detect again");
    Require(port.begins.size() == 1 && !port.begins[0].stoppable && port.begins[0].deadline >= before + std::chrono::seconds(1),
        "card budget starts after the control lock is held and ignores receiver stop");

    // 規格上の構造を満たす合成の ATR (T=1、LRC)。届いた分だけでは完成とみなさない。
    const std::uint8_t atr[] = {0x3b, 0x80, 0x01, 0x81};
    port.fifo.assign(atr, atr + 2);
    bool ready = true;
    Require(card.DataReady(session, ready) == 0 && !ready, "partial ATR is not ready");
    port.fifo.assign(atr + 2, atr + sizeof(atr));
    Require(card.DataReady(session, ready) == 0 && ready, "complete ATR is ready");
    std::uint8_t read[8] = {};
    Require(card.Read(read, sizeof(read)) == sizeof(atr) && std::memcmp(read, atr, sizeof(atr)) == 0,
        "ATR bytes are delivered in order");

    const std::uint8_t frame[] = {0x00, 0x00, 0x01, 0x55, 0x54};
    Require(card.Write(session, frame, sizeof(frame)) == 0 && port.sent == std::vector<std::uint8_t>(frame, frame + sizeof(frame)),
        "frame is written byte by byte to the UART FIFO");
    Require(card.SetBaudrate(session, 9600) == -ENOTSUP, "only B-CAS and A-CAS speeds are accepted");
    Require(card.SetBaudrate(session, 38400) == 0, "A-CAS speed is accepted");

    port.fail = 31;
    Require(card.Detect(session, present) == -EIO, "transport failure is reported as EIO");
    Require(card.DataReady(session, ready) == -EIO, "FIFO read failure is reported as EIO");
    Require(!card.PowerDown(session), "power down reports a transport failure");
}

// 192 バイトの単位 (到着時刻 4 バイト + TS 188 バイト) を作る。
std::vector<std::uint8_t> Units(unsigned count, std::vector<std::uint8_t> &expected)
{
    std::vector<std::uint8_t> raw;
    for (unsigned i = 0; i < count; ++i) {
        std::uint8_t unit[192] = {};
        unit[0] = static_cast<std::uint8_t>(i);
        unit[4] = 0x47;
        unit[5] = 0x01;
        unit[6] = 0x00;
        unit[7] = static_cast<std::uint8_t>(0x10 | (i & 15));
        unit[8] = static_cast<std::uint8_t>(i);
        raw.insert(raw.end(), unit, unit + sizeof(unit));
        expected.insert(expected.end(), unit + 4, unit + sizeof(unit));
    }
    return raw;
}

void DeliveryTests()
{
    std::vector<std::uint8_t> expected, output;
    const auto raw = Units(40, expected);
    TsDelivery delivery(false);
    std::size_t delivered = 0;
    // USB の転送の境界は 192 バイトの境界と一致しないため、半端な長さに分けて渡す。
    for (std::size_t offset = 0; offset < raw.size();) {
        const std::size_t size = (std::min)(raw.size() - offset, static_cast<std::size_t>(1000 + offset % 7));
        delivered += delivery.Deliver(raw.data() + offset, size, [&](const std::uint8_t *packets, std::size_t length) {
            Require(length % 188 == 0, "sink receives whole packets");
            output.insert(output.end(), packets, packets + length);
        });
        offset += size;
    }
    Require(output == expected && delivered == expected.size(), "192-byte units become 188-byte packets across split transfers");
    Require(delivery.Framer().Packets() == 40 && delivery.Framer().DiscardedBytes() == 0, "framer counts packets");
    Require(delivery.Quality().packets == 40 && delivery.Quality().cc_anomalies == 0 && delivery.Quality().malformed == 0,
        "quality counts the delivered packets");

    TsDelivery empty(false);
    bool called = false;
    Require(empty.Deliver(raw.data(), 100, [&](const std::uint8_t *, std::size_t) { called = true; }) == 0 && !called,
        "sink is not called without a complete packet");
}
} // namespace

int main()
{
    StatusTests();
    SessionCommandTests();
    CardTests();
    DeliveryTests();
    std::printf("PASS: torne session (shared with Linux; no hardware access)\n");
    return 0;
}
