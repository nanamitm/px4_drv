// SPDX-License-Identifier: GPL-2.0-only
// WinUSB 版と Linux 版で共有する xz3::Session の TS の開始と停止を、模擬の SessionPort で確かめる。
// USB と OS の API を使わないため、Windows と Linux の双方でビルドできる。
//
// 模擬の機器は、操作12 の送出量を EP81 の FIFO へ積み、応答の時点で末尾 tail バイトだけを
// ホストへ渡さずに残す。読み出しの取り消し、FlushStream、操作11 は FIFO を空にしない。
// この条件で、操作12 の応答の後に停止しても、前の受信の末尾が次の受信の先頭へ混ざらないことを確かめる。
#include "../src/DriverHost_PX4/xz3/session.hpp"
#include "../src/DriverHost_PX4/xz3/pump_progress.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::atomic<int> invalid_sync_logs{0};
}

extern "C" int msg_printf(const char* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (std::strstr(text, "invalid sync"))
        ++invalid_sync_logs;
    return 0;
}

using namespace px4::xz3;

namespace {
constexpr unsigned StageAddress = 0xc4600;

void Require(bool condition, const char* name)
{
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        std::exit(1);
    }
}

class MockPort;

class MockRead final : public BulkReader {
public:
    explicit MockRead(MockPort& port) : port_(port) {}
    MockRead(const MockRead&) = delete;
    MockRead& operator=(const MockRead&) = delete;
    ~MockRead() override;
    bool Submit(unsigned stage) override;
    bool Collect() override;
    unsigned Stage() const override { return stage_; }
    const std::uint8_t* Data() const override { return buffer_.data(); }

    std::vector<std::uint8_t> buffer_;
    std::size_t need_ = 0;

private:
    MockPort& port_;
    unsigned stage_ = PumpStages - 1;
};

class MockPort final : public SessionPort {
public:
    void Begin(Clock::time_point) noexcept override {}
    int Pause(unsigned ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return 0;
    }
    bool Available() const noexcept override { return true; }
    int WaitRemoved(unsigned) override { return 0; }
    void Nap() noexcept override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    // WinUsb_FlushPipe と同じく、機器の FIFO は変えない。
    int FlushStream() override { return 0; }
    int ReadStream(std::uint8_t* data, std::size_t size, unsigned, std::size_t& transferred) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        transferred = 0;
        ++stream_reads;
        if (read_error)
            return read_error;
        if (endless) {
            std::fill_n(data, size, std::uint8_t{0});
            transferred = size;
            return 0;
        }
        while (transferred < size && !fifo.empty()) {
            data[transferred++] = fifo.front();
            fifo.pop_front();
        }
        return 0;
    }
    std::unique_ptr<BulkReader> CreateReader() override { return std::make_unique<MockRead>(*this); }

    int Transfer(const px4::torne::MemoryRequest& request, std::uint8_t* data, std::size_t length,
                 std::size_t& transferred) override
    {
        bool pumped = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const unsigned address = ((unsigned(request.value) & 0xff00) << 8) | request.index;
            const bool in_stage = address >= StageAddress && address + length <= StageAddress + stage.size();
            if (request.type == 0x40) {
                if (in_stage)
                    std::copy_n(data, length, stage.begin() + (address - StageAddress));
            } else {
                if (address == StageAddress && length == stage.size())
                    pumped = Process();
                if (in_stage)
                    std::copy_n(stage.begin() + (address - StageAddress), length, data);
                else
                    std::fill_n(data, length, std::uint8_t{0});
            }
            transferred = length;
        }
        if (pumped && on_op12)
            on_op12();
        return 0;
    }

    // 投入済みの読み出しへ、FIFO の先頭から limit バイトまで投入の順に渡す。
    void DrainLocked(std::size_t limit)
    {
        for (auto* read : posted) {
            while (limit && read->need_ && !fifo.empty()) {
                read->buffer_.push_back(fifo.front());
                fifo.pop_front();
                --read->need_;
                --limit;
            }
        }
    }

    std::mutex mutex;
    std::array<std::uint8_t, 64> stage{};
    std::deque<std::uint8_t> fifo;
    std::vector<MockRead*> posted;
    std::size_t tail = 0;
    std::uint8_t channel = 1;
    std::function<void()> on_op12;
    unsigned op12_count = 0, op11_count = 0, stream_reads = 0;
    // 操作11 を受けた時点の FIFO の残り。
    std::size_t fifo_at_op11 = 0;
    bool endless = false;
    int read_error = 0;

private:
    // ステージングの要求を処理して応答を書く。操作12 なら true。
    bool Process()
    {
        const unsigned op = Word(stage.data() + 8);
        const unsigned rx = Word(stage.data() + 20);
        PutWord(stage.data() + 8, 0x80000000u | op);
        PutWord(stage.data() + 12, 0);
        switch (op) {
        case 7:
            PutWord(stage.data() + 16, 1);
            PutWord(stage.data() + 20, rx);
            std::fill(stage.begin() + 24, stage.end(), std::uint8_t{0});
            return false;
        case 11:
            // 中継プログラムの start_capture と同じく、読む位置だけを 0 へ戻す。
            ++op11_count;
            fifo_at_op11 = fifo.size();
            cursor_ = 0;
            generated_ = 0;
            return false;
        case 12: {
            const unsigned target = PumpSize(rx & 3);
            for (unsigned i = 0; i < target; ++i)
                fifo.push_back(Byte(generated_++));
            cursor_ = (cursor_ + target) % RingBytes;
            PutWord(stage.data() + 16, 0);
            PutWord(stage.data() + 40, target);
            PutWord(stage.data() + 44, cursor_);
            PutWord(stage.data() + 48, cursor_);
            PutWord(stage.data() + 52, target);
            DrainLocked(fifo.size() > tail ? fifo.size() - tail : 0);
            ++op12_count;
            return true;
        }
        case 13:
            PutWord(stage.data() + 36, 0);
            return false;
        default:
            PutWord(stage.data() + 12, 1);
            return false;
        }
    }

    // 192 バイトのレコード: 4 バイトの前置き、0x47、チャンネル番号、レコード番号 (4 バイト)。
    std::uint8_t Byte(std::uint64_t offset) const
    {
        const auto record = offset / 192;
        switch (offset % 192) {
        case 4: return 0x47;
        case 5: return channel;
        case 6: return static_cast<std::uint8_t>(record >> 24);
        case 7: return static_cast<std::uint8_t>(record >> 16);
        case 8: return static_cast<std::uint8_t>(record >> 8);
        case 9: return static_cast<std::uint8_t>(record);
        default: return 0;
        }
    }

    std::uint32_t cursor_ = 0;
    std::uint64_t generated_ = 0;
};

MockRead::~MockRead()
{
    // 取り消し。受け取り済みのバイトは失われ、機器の FIFO の残りはそのまま。
    std::lock_guard<std::mutex> lock(port_.mutex);
    port_.posted.erase(std::remove(port_.posted.begin(), port_.posted.end(), this), port_.posted.end());
}

bool MockRead::Submit(unsigned stage)
{
    std::lock_guard<std::mutex> lock(port_.mutex);
    stage_ = stage;
    buffer_.clear();
    need_ = PumpSize(stage);
    port_.posted.push_back(this);
    return true;
}

bool MockRead::Collect()
{
    std::lock_guard<std::mutex> lock(port_.mutex);
    port_.DrainLocked(port_.fifo.size());
    if (need_)
        return false;
    port_.posted.erase(std::remove(port_.posted.begin(), port_.posted.end(), this), port_.posted.end());
    return true;
}

class Sink final : public StreamSink {
public:
    void Started() override {}
    void PutPacket(const std::uint8_t* packet) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint32_t record = (std::uint32_t(packet[2]) << 24) | (std::uint32_t(packet[3]) << 16) |
                                     (std::uint32_t(packet[4]) << 8) | packet[5];
        packets.push_back({packet[1], record});
    }
    void Flush() override { ++flushes; }

    struct Record {
        std::uint8_t channel;
        std::uint32_t number;
    };
    std::mutex mutex;
    std::vector<Record> packets;
    std::atomic<int> flushes{0};
};

const std::uint8_t dummy_image[8]{};
const Images images{{dummy_image, 4}, {dummy_image, 4}, {dummy_image, 8}};

// 3回目の操作12 の応答を返す前に 100 ms 待たせ、その間に StopStream で stop_ を立てる。
// worker は応答を受け取った後に停止を見て、回収していない読み出しを取り消す。
void StopAfterThirdPump(MockPort& port, Session& session)
{
    std::atomic_bool reached{false};
    port.on_op12 = [&] {
        if (port.op12_count != 3 || reached.exchange(true))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    Sink sink;
    bool stop_failed = false;
    Require(session.StartStream(false, sink, stop_failed) == 0, "first StartStream");
    while (!reached)
        std::this_thread::yield();
    Require(session.StopStream() == 0, "first StopStream");
    port.on_op12 = nullptr;
}

// 機器の FIFO に tail バイトを残して止めた後、次の受信がチャンネル2 の先頭から始まる。
void TestResidueIsDiscarded(std::size_t tail)
{
    MockPort port;
    port.tail = tail;
    Session session(port, images);
    StopAfterThirdPump(port, session);
    {
        std::lock_guard<std::mutex> lock(port.mutex);
        Require(port.fifo.size() == tail, "the stop left the expected residue");
        port.channel = 2;
    }

    invalid_sync_logs = 0;
    Sink sink;
    bool stop_failed = false;
    Require(session.StartStream(false, sink, stop_failed) == 0, "second StartStream");
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (sink.flushes < 4 && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(session.StopStream() == 0, "second StopStream");

    Require(port.fifo_at_op11 == 0, "EP81 is empty when operation 11 starts the DMA");
    std::lock_guard<std::mutex> lock(sink.mutex);
    Require(sink.flushes >= 4 && !sink.packets.empty(), "the second stream delivers TS");
    Require(sink.packets.front().number == 0, "the second stream starts at its first record");
    for (std::size_t i = 0; i < sink.packets.size(); ++i) {
        Require(sink.packets[i].channel == 2, "no packet of the previous channel");
        Require(!i || sink.packets[i].number == sink.packets[i - 1].number + 1, "records stay contiguous");
    }
    Require(invalid_sync_logs == 0, "no record is discarded for invalid sync");
}

// 停止後も EP81 が送り続ける場合は、DMA を始めずに失敗にする。
void TestEndlessResidueFails()
{
    MockPort port;
    port.endless = true;
    Session session(port, images);
    Sink sink;
    bool stop_failed = false;
    Require(session.StartStream(false, sink, stop_failed) == -EIO, "endless EP81 data fails the start");
    Require(!session.Running() && port.op11_count == 0, "operation 11 is not sent");
    Require(port.stream_reads > 1 && port.stream_reads <= 64 * 1024 / 512 + 2, "residue reads are bounded");
}

// 読み捨ての読み出しが失敗した場合も、DMA を始めずにそのエラーを返す。
void TestResidueReadErrorFails()
{
    MockPort port;
    port.read_error = -ENODEV;
    Session session(port, images);
    Sink sink;
    bool stop_failed = false;
    Require(session.StartStream(false, sink, stop_failed) == -ENODEV, "residue read error is returned");
    Require(!session.Running() && port.op11_count == 0, "operation 11 is not sent after a read error");
}
}

int main()
{
    // 残りなし (対照)、192 バイトの倍数でない残り (境界がずれる)、192 バイトの倍数の残り (前のチャンネルが混ざる)。
    TestResidueIsDiscarded(0);
    TestResidueIsDiscarded(1024);
    TestResidueIsDiscarded(1536);
    TestEndlessResidueFails();
    TestResidueReadErrorFails();
    std::puts("xz3_session_test: passed");
    return 0;
}
