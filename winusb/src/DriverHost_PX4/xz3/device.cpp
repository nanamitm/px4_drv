// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"

#include "../resource.h"
#include "../tc90522_cnr.hpp"
#include "../torne/winusb_control.hpp"
#include "msg.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <thread>
#include <vector>

namespace px4::xz3 {
namespace {
using px4::torne::WinUsbControl;
using Clock = WinUsbControl::Clock;
constexpr unsigned Body = 0xea61c, Hook = 0xe9a10, Stage = 0xc4600;
constexpr unsigned BodySize = 2236;
constexpr ULONG PumpSize = 3 * 1024 * 1024;
constexpr UCHAR BulkIn = 0x81;
using Packet = std::array<std::uint8_t, 64>;
using namespace std::chrono_literals;

struct Image {
    const std::uint8_t* bytes;
    std::size_t size;
};

Image Resource(int id)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto res = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res)
        return {};
    return {static_cast<const std::uint8_t*>(LockResource(LoadResource(module, res))),
            SizeofResource(module, res)};
}

void PutWord(std::uint8_t* p, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

std::uint32_t Word(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}

std::uint64_t Fingerprint(const std::uint8_t* p, std::size_t size)
{
    std::uint64_t value = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i)
        value = (value ^ p[i]) * 1099511628211ull;
    return value;
}

class Relay {
public:
    Relay(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed)
        : control_(file, usb, removed, io_) {}

    bool Valid() const
    {
        const auto valid_body = [](const Image& image) {
            return image.bytes && image.size && image.size <= BodySize && image.size % 4 == 0;
        };
        return valid_body(bootstrap_) && valid_body(runtime_) &&
               hook_.bytes && hook_.size == saved_hook_.size();
    }

    int Init()
    {
        if (!Valid())
            return -EINVAL;
        control_.Begin(Clock::now() + 15s);
        std::array<std::uint8_t, 16> data{};
        constexpr std::array<std::uint8_t, 8> expected_hook{0xc4,0x4d,0,0x84,0x9b,0x80,0,0};
        if (!Read(0x82008, data.data(), 2) || data[0] || data[1] ||
            !Read(0xe9ce8, data.data(), 8) || Fingerprint(data.data(), 8) != 0xce1c939aaeb27baeull ||
            !Read(0xe91e4, data.data(), 16) || Fingerprint(data.data(), 16) != 0x7df1d1a09a7c2775ull ||
            !Read(0xe9a2c, data.data(), 4) || Word(data.data()) != 0x00f00000 ||
            !Read(0xc4810, data.data(), 4) || Word(data.data()) != 0x82000000 ||
            !Read(Hook, saved_hook_.data(), saved_hook_.size()) || saved_hook_ != expected_hook ||
            !Read(Body, saved_body_.data(), saved_body_.size()) ||
            Fingerprint(saved_body_.data(), saved_body_.size()) != 0x16073e2aa1fd67b1ull) {
            msg_err("GV-MVP/XZ3: unsupported boot RAM image or read failure\n");
            return -EIO;
        }
        int result = Install(bootstrap_);
        if (result)
            return result;
        Packet reply{};
        if ((result = Exchange(6, reply)))
            return result;
        constexpr unsigned delay[]{10, 10, 1000, 10, 500, 200};
        for (std::uint8_t step = 0; step < 6; ++step) {
            if ((result = Exchange(5, reply, &step, 1)) || (result = Delay(delay[step])))
                return result;
        }
        if ((result = Exchange(3, reply)))
            return result;
        // ブートストラップは機器初期化だけを担当する。以降の選局・TS取得は共通版を使う。
        return Install(runtime_);
    }

    int Install(const Image& image)
    {
        control_.Begin(Clock::now() + 15s);
        if (!Unhook())
            return -EIO;
        body_changed_ = true;
        if (!WriteRange(Body, image.bytes, image.size) || !Verify(Body, image.bytes, image.size))
            return -EIO;
        // 通常ハンドラーを残したまま末尾を置き、先頭ワードでフックを有効にする。
        hook_changed_ = true;
        if (!Write(Hook + 4, hook_.bytes + 4) || !Write(Hook, hook_.bytes))
            return -EIO;
        return 0;
    }

    bool Restore()
    {
        control_.Begin(Clock::now() + 15s);
        if (!Unhook())
            return false;
        if (!body_changed_)
            return true;
        const bool body = WriteRange(Body, saved_body_.data(), saved_body_.size()) &&
                          Verify(Body, saved_body_.data(), saved_body_.size());
        // 0xc4600はboot0のUSB IN応答バッファで、通常の読み出しでも上書きされる。
        // フックと本体を戻した後の照合で、ステージングは通常の応答に置き換わる。
        body_changed_ = !body;
        if (body_changed_)
            msg_err("GV-MVP/XZ3: boot RAM restoration failed\n");
        return !body_changed_;
    }

    int Delay(unsigned ms)
    {
        control_.Begin(Clock::now() + std::chrono::milliseconds(ms) + 1s);
        return control_.Pause(ms);
    }

    int Exchange(unsigned op, Packet& reply, const std::uint8_t* data = nullptr,
                 std::size_t size = 0, unsigned addr = 0, std::size_t rx = 0)
    {
        Packet request{};
        PutWord(request.data(), 0x585a4931);
        PutWord(request.data() + 4, ++sequence_);
        PutWord(request.data() + 8, op);
        PutWord(request.data() + 12, static_cast<std::uint32_t>(size));
        PutWord(request.data() + 16, addr);
        PutWord(request.data() + 20, static_cast<std::uint32_t>(rx));
        if (size)
            std::copy_n(data, size, request.data() + 24);
        control_.Begin(Clock::now() + (op == 12 ? 6s : 2s));
        // 最後にマジックを書き、全フィールドが揃ってから読み出しで起動する。
        // relayはヘッダー24バイトとsizeで指定された送信データだけを参照する。
        const int last = static_cast<int>(20 + ((size + 3) & ~std::size_t{3}));
        for (int at = last; at >= 0; at -= 4)
            if (!Write(Stage + at, request.data() + at))
                return -EIO;
        if (!Read(Stage, reply.data(), reply.size()) || Word(reply.data()) != 0x585a4931 ||
            Word(reply.data() + 4) != sequence_ || Word(reply.data() + 8) != (0x80000000u | op))
            return -EIO;
        if (const auto status = Word(reply.data() + 12)) {
            if (op == 12)
                msg_err("GV-MVP/XZ3: TS pump status=%u run=%08x producer=%u peak=%u cursor=%u sent=%u fifo=%08x\n",
                        status, Word(reply.data() + 28), Word(reply.data() + 32),
                        Word(reply.data() + 40), Word(reply.data() + 44),
                        Word(reply.data() + 52), Word(reply.data() + 60));
            else
                msg_err("GV-MVP/XZ3: relay op=%u status=%u\n", op, status);
            return -EIO;
        }
        return 0;
    }

private:
    bool Read(unsigned address, std::uint8_t* data, std::size_t size)
    {
        for (std::size_t at = 0; at < size; at += 64) {
            const unsigned offset = address + static_cast<unsigned>(at);
            const px4::torne::MemoryRequest request{0xc0, 0xbc,
                static_cast<std::uint16_t>((offset >> 8) & 0xff00), static_cast<std::uint16_t>(offset)};
            const auto count = (std::min)(size - at, std::size_t{64});
            std::size_t transferred = 0;
            if (control_.Transfer(request, data + at, count, transferred) || transferred != count)
                return false;
        }
        return true;
    }

    bool Write(unsigned address, const std::uint8_t* data)
    {
        std::array<std::uint8_t, 4> word{data[0], data[1], data[2], data[3]};
        const px4::torne::MemoryRequest request{0x40, 0xbc,
            static_cast<std::uint16_t>((address >> 8) & 0xff00), static_cast<std::uint16_t>(address)};
        std::size_t transferred = 0;
        return !control_.Transfer(request, word.data(), word.size(), transferred) &&
               transferred == word.size();
    }

    bool WriteRange(unsigned address, const std::uint8_t* data, std::size_t size)
    {
        for (std::size_t at = 0; at < size; at += 4)
            if (!Write(address + static_cast<unsigned>(at), data + at))
                return false;
        return true;
    }

    bool Verify(unsigned address, const std::uint8_t* data, std::size_t size)
    {
        std::array<std::uint8_t, BodySize> actual{};
        if (!Read(address, actual.data(), size)) {
            msg_err("GV-MVP/XZ3: RAM read failed address=%06x\n", address);
            return false;
        }
        const auto mismatch = std::mismatch(data, data + size, actual.data());
        if (mismatch.first == data + size)
            return true;
        msg_err("GV-MVP/XZ3: RAM mismatch address=%06x offset=%zu expected=%02x actual=%02x\n",
                address, static_cast<std::size_t>(mismatch.first - data), *mismatch.first, *mismatch.second);
        return false;
    }

    bool Unhook()
    {
        if (!hook_changed_)
            return true;
        if (!Write(Hook, saved_hook_.data()) || !Write(Hook + 4, saved_hook_.data() + 4) ||
            !Verify(Hook, saved_hook_.data(), saved_hook_.size()))
            return false;
        hook_changed_ = false;
        return true;
    }

    px4::torne::NativeControlIo io_;
    WinUsbControl control_;
    const Image bootstrap_ = Resource(IDR_XZ3_BOOTSTRAP);
    const Image runtime_ = Resource(IDR_XZ3_RUNTIME);
    const Image hook_ = Resource(IDR_XZ3_HOOK);
    std::array<std::uint8_t, BodySize> saved_body_{};
    std::array<std::uint8_t, 8> saved_hook_{};
    std::uint32_t sequence_ = 0;
    bool body_changed_ = false, hook_changed_ = false;
};

struct BulkRead {
    BulkRead(HANDLE file, WINUSB_INTERFACE_HANDLE usb)
        : data(PumpSize), file_(file), usb_(usb)
    {
        op_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    BulkRead(const BulkRead&) = delete;
    ~BulkRead()
    {
        if (pending_) {
            CancelIoEx(file_, &op_);
            ULONG unused = 0;
            WinUsb_GetOverlappedResult(usb_, &op_, &unused, TRUE);
        }
        if (op_.hEvent)
            CloseHandle(op_.hEvent);
    }

    bool Submit()
    {
        if (!op_.hEvent)
            return false;
        const auto event = op_.hEvent;
        op_ = {};
        op_.hEvent = event;
        ResetEvent(event);
        pending_ = WinUsb_ReadPipe(usb_, BulkIn, data.data(), PumpSize, nullptr, &op_) ||
                   GetLastError() == ERROR_IO_PENDING;
        return pending_;
    }

    bool Collect(HANDLE removed)
    {
        HANDLE events[]{removed, op_.hEvent};
        if (WaitForMultipleObjects(2, events, FALSE, 2000) != WAIT_OBJECT_0 + 1)
            return false;
        ULONG size = 0;
        const auto ok = WinUsb_GetOverlappedResult(usb_, &op_, &size, FALSE);
        pending_ = !ok && GetLastError() == ERROR_IO_INCOMPLETE;
        return ok && size == PumpSize;
    }

    std::vector<std::uint8_t> data;

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    OVERLAPPED op_{};
    bool pending_ = false;
};
}

class Device::Receiver final : public ReceiverBase, private FrontendIo {
public:
    explicit Receiver(Device& parent)
        : ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE), parent_(parent),
          relay_(parent.usb_dev_.dev, parent.usb_dev_.winusb, parent.removed_), frontend_(*this) {}
    ~Receiver() override { Close(); }

    bool Valid() const { return relay_.Valid(); }

    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_)
            return -EBUSY;
        if (!parent_.available_ || restore_failed_)
            return -ENODEV;
        if (!stream_buf_->Alloc(188u * 65536u))
            return -ENOMEM;
        stream_buf_->SetThresholdSize(188u * 16u);
        stream_buf_->Start();
        open_ = true;
        failed_ = capture_ = tuned_ = initialized_ = false;
        worker_error_ = 0;
        cnr_ = 0;
        return 0;
    }

    void Close() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!open_)
            return;
        StopReader();
        if (parent_.available_) {
            // DMA停止に失敗した場合はフックを残し、復元せずに電源リセットを要求する。
            restore_failed_ = stream_started_;
            if (!restore_failed_)
                restore_failed_ = !relay_.Restore();
            if (restore_failed_)
                msg_err("GV-MVP/XZ3: RAM restoration failed; reconnect USB\n");
            else if (initialized_ || failed_)
                msg_info("GV-MVP/XZ3: boot RAM restored\n");
        }
        stream_buf_->Stop();
        open_ = false;
        lock.unlock();
        closed_.notify_all();
    }

    void WaitClosed()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_.wait(lock, [this]{ return !open_; });
    }

    int SetCapture(bool enable) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || !parent_.available_)
            return -ENODEV;
        capture_ = enable;
        if (!enable)
            return StopReader();
        if (failed_ || worker_error_)
            return -EIO;
        return tuned_ ? StartReader() : 0;
    }

    int CheckLock(bool& locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked = false;
        if (!open_ || !parent_.available_ || failed_ || worker_error_)
            return -ECANCELED;
        // TS転送中の制御転送はworkerが担当するため、最後の選局結果を返す。
        locked = tuned_;
        return 0;
    }

    int SetLnbVoltage(std::int32_t voltage) override { return voltage ? -ENOSYS : 0; }
    int ReadStat(px4::command::StatType type, std::int32_t& value) override
    {
        value = 0;
        if (type != px4::command::StatType::CNR)
            return -ENOSYS;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || !parent_.available_ || failed_ || worker_error_)
            return -ECANCELED;
        // 選局後に取得し、TS workerが転送応答から更新した値を返す。
        value = cnr_.load();
        return 0;
    }

protected:
    int SetStreamId() override { return 0; }

    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || !parent_.available_ || failed_)
            return -ENODEV;
        const bool satellite = params_.system == px4::SystemType::ISDB_S;
        if (!satellite && params_.system != px4::SystemType::ISDB_T)
            return -EINVAL;
        int result = StopReader();
        tuned_ = false;
        if (result || !stream_buf_->AdvanceEpoch())
            return result ? result : -EIO;
        stream_buf_->ResetInputPacketCount();
        worker_error_ = 0;
        if (!initialized_) {
            // ReceiverManagerの一覧ロックを保持するOpenで、時間のかかる電源投入を行わない。
            if ((result = relay_.Init()) || (result = frontend_.Initialize())) {
                failed_ = true;
                return result;
            }
            initialized_ = true;
        }
        const auto tsid = satellite ? params_.stream_id : 0;
        result = frontend_.Tune(satellite, params_.freq, tsid);
        // 地デジはロック直後に訂正前のTSを出すことがある。
        // ファームウェア交換による待ちがなくなったため、DMAを始める前に安定を待つ。
        if (!result && !satellite)
            result = relay_.Delay(350);
        if (result && result != -ENOLINK && result != -EINVAL)
            failed_ = true;
        if (!result) {
            satellite_ = satellite;
            tuned_ = true;
            if (capture_)
                result = StartReader();
        }
        msg_info("GV-MVP/XZ3: tune frequency_khz=%u tsid=%04x result=%d\n", params_.freq, tsid, result);
        return result;
    }

private:
    int I2c(std::uint8_t address, const std::uint8_t* tx, std::size_t size,
            std::uint8_t* rx, std::size_t count) override
    {
        if (size > 40 || count > 40 - size)
            return -EINVAL;
        Packet reply{};
        const int result = relay_.Exchange(7, reply, tx, size, address, count);
        if (result)
            return result;
        if (Word(reply.data() + 16) != 1 || Word(reply.data() + 20) != count)
            return -EIO;
        if (count)
            std::copy_n(reply.data() + 24, count, rx);
        return 0;
    }

    int Delay(unsigned milliseconds) override { return relay_.Delay(milliseconds); }

    int StartReader()
    {
        if (worker_.joinable())
            return worker_error_ ? -EIO : 0;
        if (!stream_buf_->AdvanceEpoch() || !WinUsb_FlushPipe(parent_.usb_dev_.winusb, BulkIn))
            return -EIO;
        // 最初の3 MiBを待つと、スキャン側の最初の統計取得に間に合わない場合がある。
        // DMAを始める前に新しいチャンネルの値を1回読み、以後はTS転送応答で更新する。
        cnr_ = px4::tc90522::CnrMilliDb(satellite_, frontend_.ReadCndat());
        Packet reply{};
        // 開始の応答を失った場合にも、停止を試してからRAMを復元する。
        stream_started_ = true;
        const int result = relay_.Exchange(11, reply);
        if (result) {
            StopReader();
            return result;
        }
        stop_ = false;
        stream_buf_->Start();
        try {
            worker_ = std::thread([this, satellite = satellite_] {
                int result = 0;
                try { result = ReadLoop(satellite); }
                catch (const std::bad_alloc&) { result = -ENOMEM; }
                // 同じBonDriverセッションから再選局できるよう、接続は維持する。
                if (result && !stop_ && parent_.available_) {
                    worker_error_ = result;
                    msg_err("GV-MVP/XZ3: TS reader failed ret=%d\n", result);
                }
            });
        } catch (const std::system_error&) {
            StopReader();
            return -EIO;
        }
        return 0;
    }

    int StopReader()
    {
        stop_ = true;
        if (worker_.joinable())
            worker_.join();
        cnr_ = 0;
        if (!stream_started_ || !parent_.available_)
            return 0;
        Packet reply{};
        const int result = relay_.Exchange(13, reply);
        if (result || Word(reply.data() + 36)) {
            failed_ = true;
            return result ? result : -EIO;
        }
        stream_started_ = false;
        if (!WinUsb_FlushPipe(parent_.usb_dev_.winusb, BulkIn))
            msg_warn("GV-MVP/XZ3: EP81 flush after stop failed error=%lu\n", GetLastError());
        return 0;
    }

    int ReadLoop(bool satellite)
    {
        auto& usb = parent_.usb_dev_;
        BulkRead reads[]{{usb.dev, usb.winusb}, {usb.dev, usb.winusb}};
        if (!reads[0].Submit() || !reads[1].Submit())
            return -EIO;
        Packet reply{};
        const unsigned cn_address = satellite ? 0x19 : 0x18;
        RingStats stats(satellite);
        for (unsigned head = 0; parent_.available_ && !stop_; head ^= 1) {
            auto& read = reads[head];
            const auto begin = Clock::now();
            if (relay_.Exchange(12, reply, nullptr, 0, cn_address) || Word(reply.data() + 52) != PumpSize ||
                !read.Collect(parent_.removed_))
                return -EIO;
            const auto end = Clock::now();
            // 応答ワード4〜6は、この要求中の最大未処理量、消費位置、DMAの書き込み位置。
            stats.Record(begin, end, Word(reply.data() + 40), Word(reply.data() + 44), Word(reply.data() + 48));
            if (stop_)
                break;
            if (const int result = Deliver(read.data))
                return result;
            if (!read.Submit())
                return -EIO;
            // op 12の応答ワード4はCNDAT。読み出し失敗時の0も反映して古い値を残さない。
            cnr_ = px4::tc90522::CnrMilliDb(satellite, Word(reply.data() + 16));
        }
        return 0;
    }

    /*
     * XZ3のTSリングは6,291,456バイトで、DMAは操作12の外側でも書き込みを続ける。
     * 衛星の約6.7 MB/秒では約0.94秒で満杯になるため、要求間の空き時間と
     * ファームウェアが返す最大未処理量を記録し、ドロップの時刻と照合できるようにする。
     * 溢れた場合は未処理量が周回後の値として小さく見える可能性があるため、時間も併記する。
     */
    class RingStats {
    public:
        explicit RingStats(bool satellite) : satellite_(satellite), period_start_(Clock::now()) {}
        RingStats(const RingStats&) = delete;
        ~RingStats()
        {
            if (total_chunks_)
                msg_info("GV-MVP/XZ3: ring end satellite=%d chunks=%llu peak_bytes=%u high_chunks=%llu max_producer=%u max_host_gap_us=%lld max_op_us=%lld\n",
                         satellite_ ? 1 : 0, total_chunks_, total_peak_, total_high_, total_producer_,
                         total_gap_us_, total_op_us_);
        }

        void Record(Clock::time_point begin, Clock::time_point end, std::uint32_t peak,
                    std::uint32_t cursor, std::uint32_t producer)
        {
            const auto op_us = Micro(end - begin);
            // 前回の操作12の完了から今回の開始までは、ファームウェアがリングを送出しない。
            const auto gap_us = has_last_ ? Micro(begin - last_end_) : 0;
            has_last_ = true;
            last_end_ = end;
            ++chunks_;
            ++total_chunks_;
            peak_ = (std::max)(peak_, peak);
            total_peak_ = (std::max)(total_peak_, peak);
            // 書き込み位置がリング末尾の近くまで進めば、DMAは設定したサイズで周回している。
            producer_ = (std::max)(producer_, producer);
            total_producer_ = (std::max)(total_producer_, producer);
            gap_us_ = (std::max)(gap_us_, gap_us);
            total_gap_us_ = (std::max)(total_gap_us_, gap_us);
            op_us_ = (std::max)(op_us_, op_us);
            total_op_us_ = (std::max)(total_op_us_, op_us);
            if (peak >= HighWater) {
                ++high_;
                ++total_high_;
                msg_warn("GV-MVP/XZ3: ring high chunk=%llu peak_bytes=%u ring_bytes=%u cursor=%u producer=%u host_gap_us=%lld op_us=%lld\n",
                         total_chunks_, peak, RingBytes, cursor, producer, gap_us, op_us);
            }
            if (end - period_start_ >= std::chrono::seconds(10)) {
                msg_info("GV-MVP/XZ3: ring stats chunks=%llu peak_bytes=%u ring_bytes=%u high_chunks=%llu max_producer=%u max_host_gap_us=%lld max_op_us=%lld\n",
                         chunks_, peak_, RingBytes, high_, producer_, gap_us_, op_us_);
                period_start_ = end;
                chunks_ = high_ = 0;
                peak_ = producer_ = 0;
                gap_us_ = op_us_ = 0;
            }
        }

    private:
        // ts_probe.cのRING_SIZEと同じ値。
        static constexpr std::uint32_t RingBytes = 0x600000;
        // 3/4を超えた要求を個別に記録する。
        static constexpr std::uint32_t HighWater = RingBytes / 4 * 3;

        static long long Micro(Clock::duration value)
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
        }

        bool satellite_;
        bool has_last_ = false;
        Clock::time_point period_start_, last_end_{};
        unsigned long long chunks_ = 0, high_ = 0, total_chunks_ = 0, total_high_ = 0;
        std::uint32_t peak_ = 0, total_peak_ = 0, producer_ = 0, total_producer_ = 0;
        long long gap_us_ = 0, op_us_ = 0, total_gap_us_ = 0, total_op_us_ = 0;
    };

    int Deliver(const std::vector<std::uint8_t>& bytes)
    {
        // 1レコードは4バイトの接頭辞と188バイトのTS。1回の転送はレコード境界で終わる。
        for (std::size_t offset = 4; offset < bytes.size(); offset += 192) {
            const auto* packet = bytes.data() + offset;
            if (packet[0] != 0x47)
                return -EIO;
            std::size_t size = 188;
            if (!stream_buf_->Write(packet, size))
                return -ENOBUFS;
        }
        stream_buf_->NotifyWrite();
        return 0;
    }

    Device& parent_;
    Relay relay_;
    Frontend frontend_;
    std::mutex mutex_;
    std::condition_variable closed_;
    std::thread worker_;
    std::atomic_bool stop_{false};
    std::atomic_int worker_error_{0};
    std::atomic<std::int32_t> cnr_{0};
    bool satellite_ = false;
    bool open_ = false, capture_ = false, tuned_ = false, initialized_ = false;
    bool stream_started_ = false, failed_ = false, restore_failed_ = false;
};

Device::Device(const std::wstring& path, const DeviceDefinition& definition,
               std::uintptr_t index, ReceiverManager& manager)
    : DeviceBase(path, definition, index, manager)
{
    removed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!removed_)
        throw DeviceError("GV-MVP/XZ3: CreateEvent failed");
}

Device::~Device()
{
    Term();
    CloseHandle(removed_);
}

int Device::Init()
{
    if (usb_dev_.descriptor.idVendor != 0x04bb || usb_dev_.descriptor.idProduct != 0x0530 ||
        device_def_.receivers.size() != 1)
        return -EINVAL;
    UCHAR alternate = 0xff;
    USB_INTERFACE_DESCRIPTOR desc{};
    if (!WinUsb_GetCurrentAlternateSetting(usb_dev_.winusb, &alternate) || alternate != 0 ||
        !WinUsb_QueryInterfaceSettings(usb_dev_.winusb, 0, &desc) || desc.bInterfaceNumber != 0)
        return -EINVAL;
    bool stream = false;
    for (UCHAR i = 0; i < desc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb_dev_.winusb, 0, i, &pipe))
            return -EIO;
        stream |= pipe.PipeId == BulkIn && pipe.PipeType == UsbdPipeTypeBulk;
    }
    if (!stream)
        return -EINVAL;
    receiver_ = std::make_unique<Receiver>(*this);
    if (!receiver_->Valid())
        return -EINVAL;
    command::ReceiverInfo info{};
    const auto& receiver = device_def_.receivers.front();
    wcscpy_s(info.device_name, device_def_.name.c_str());
    info.device_guid = device_def_.guid;
    wcscpy_s(info.receiver_name, receiver.name.c_str());
    info.receiver_guid = receiver.guid;
    info.systems = SystemType::ISDB_T | SystemType::ISDB_S;
    info.index = receiver.index;
    registered_ = receiver_manager_.Register(info, receiver_.get(), GetDeviceIdentity());
    return registered_ ? 0 : -EIO;
}

void Device::SetAvailability(bool available)
{
    available_ = available;
    if (!available)
        SetEvent(removed_);
}

void Device::Term()
{
    if (registered_) {
        receiver_manager_.Unregister(receiver_.get());
        registered_ = false;
    }
    if (receiver_) {
        receiver_->WaitClosed();
        receiver_.reset();
    }
}

ReceiverBase* Device::GetReceiver(int id) const
{
    if (id)
        throw std::out_of_range("GV-MVP/XZ3 receiver index");
    return receiver_.get();
}

}
