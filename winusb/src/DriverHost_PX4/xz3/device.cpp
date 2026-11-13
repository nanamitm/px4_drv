// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "card_uart.hpp"
#include "frontend.hpp"

#include "../resource.h"
#include "../tc90522_cnr.hpp"
#include "../torne/winusb_control.hpp"
#include "../../WinSCard_PX4/bcas_atr.hpp"
#include "msg.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <functional>
#include <thread>
#include <vector>

namespace px4::xz3 {
namespace {
using px4::torne::WinUsbControl;
using Clock = WinUsbControl::Clock;
constexpr unsigned Body = 0xea61c, Hook = 0xe9a10, Stage = 0xc4600;
constexpr unsigned BodySize = 2236;
// 操作12の1回の送出量の最大値。xz3-runtime.binのSTREAM_UNITと同じ値で、
// 要求のワード5の段階 (0〜3) で右へずらした大きさを送る (3 MiB、1.5 MiB、768 KiB、384 KiB)。
constexpr ULONG PumpMax = 3 * 1024 * 1024;
constexpr unsigned PumpStages = 4;
constexpr ULONG PumpSize(unsigned stage) { return PumpMax >> stage; }
/*
 * 操作12の間はEP0が応答せず、カードUARTの操作を挟めない。一方、ファームウェアのUSB FIFOへの
 * 送出は約6.6 MB/秒で、110度CS (約6.5 MB/秒) との差は約2%しかなく、要求ごとの固定の負担
 * (約0.85 ms) が積み重なると送出が追いつかない。実測では384 KiBの連続で110度CSの未処理量が
 * 増え続け、約60秒でリングが溢れた。このため、直前の要求の時間から次の要求の時間がこの値を
 * 超えない最大の段階を選び、地デジでは384 KiB (約0.18秒)、110度CSでは1.5 MiB (約0.24秒) にする。
 */
constexpr auto PumpTarget = std::chrono::milliseconds(250);
/*
 * カード操作の後のCardBusyWindowの間の目標。T=1の応答待ちの間に続くカード操作の待ちを短くする。
 * 要求ごとに約21 msの制御転送があり、110度CSでは384 KiBの連続で送出が追いつかないため、
 * 110度CSで768 KiB (約0.12秒)、地デジとBSで384 KiBになる値にする。
 */
constexpr auto PumpCardTarget = std::chrono::milliseconds(125);
constexpr auto CardBusyWindow = std::chrono::seconds(1);
/*
 * SmartCardは応答を待つ間、約5 msおきにカード操作を繰り返す。その間に操作12が始まると、
 * 次のカード操作は操作12の終わりまで待つ。流量に余裕があるときは、カード操作が続く間、
 * 操作12の開始をCardIdleGapの空きが生じるまで、最長CardHoldMaxだけ遅らせる。
 * 余裕の目安はUSB FIFOへの送出 (約6.6 MB/秒) との差で、110度CS (約6.5 MB/秒) のように
 * 余裕がない流量では遅らせない。遅らせた分の未処理量は、送出との差で解消する。
 */
// 空だった受信FIFOの読み出しから、次に機器へ問い合わせるまでの間隔。
constexpr auto CardPollInterval = std::chrono::milliseconds(20);
// カード操作の間隔は、CardPollIntervalとWindowsの待機の粒度 (約16 ms) を足した長さになる。
constexpr auto CardIdleGap = std::chrono::milliseconds(50);
constexpr auto CardHoldMax = std::chrono::milliseconds(150);
constexpr double CardHoldRate = 4.0e6;
// CNDATを読む操作12の間隔（回数）。操作12ごとのI²Cの読み出しはその間リングを送らないため、
// 110度CSで約2秒、地デジで約1.5秒に1回にとどめる。
constexpr unsigned CnInterval = 8;
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
        runtime_installed_ = image.bytes == runtime_.bytes;
        return 0;
    }

    // 共通版 (操作7、11〜14) が導入され、フックが有効な間だけtrue。
    bool RuntimeInstalled() const { return runtime_installed_; }

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

    // カードUARTの1語をboot0のメモリ要求で読み書きする。boot0のUSBハンドラーは、フックの導入中も
    // ステージング以外の番地を処理する。呼び出し側が制御ロックを保持し、Beginで期限を設定する。
    void Begin(Clock::time_point deadline) { control_.Begin(deadline); }

    bool ReadWord(unsigned address, std::uint32_t& value)
    {
        std::array<std::uint8_t, 4> bytes{};
        if (!Read(address, bytes.data(), bytes.size()))
            return false;
        value = Word(bytes.data());
        return true;
    }

    bool WriteWord(unsigned address, std::uint32_t value)
    {
        std::array<std::uint8_t, 4> bytes{};
        PutWord(bytes.data(), value);
        return Write(address, bytes.data());
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
        // boot0のメモリ要求は1語ごとに約3 msかかる。ステージングに直前の応答が残っている間は、
        // 要求と同じ値の語を書き直さない。先頭のマジックは、boot0の書き込みの受け取りで
        // 上書きされるため、毎回最後に書く。操作12では7回の転送が6回、操作14の読み出しでは4回になる。
        const int last = static_cast<int>(20 + ((size + 3) & ~std::size_t{3}));
        for (int at = last; at >= 0; at -= 4) {
            if (at && staged_valid_ &&
                std::equal(request.data() + at, request.data() + at + 4, staged_.data() + at))
                continue;
            if (!Write(Stage + at, request.data() + at))
                return -EIO;
        }
        const bool read = Read(Stage, reply.data(), reply.size());
        // 応答の読み出しの後、ステージングには応答がそのまま残る。
        staged_ = reply;
        staged_valid_ = read;
        if (!read || Word(reply.data()) != 0x585a4931 ||
            Word(reply.data() + 4) != sequence_ || Word(reply.data() + 8) != (0x80000000u | op))
            return -EIO;
        if (const auto status = Word(reply.data() + 12)) {
            // 共通版は本体の窓を節約するため、DMAの状態とFIFOの状態の語を返さない。
            if (op == 12)
                msg_err("GV-MVP/XZ3: TS pump status=%u peak=%u cursor=%u producer=%u sent=%u\n",
                        status, Word(reply.data() + 40), Word(reply.data() + 44),
                        Word(reply.data() + 48), Word(reply.data() + 52));
            else
                msg_err("GV-MVP/XZ3: relay op=%u status=%u\n", op, status);
            return -EIO;
        }
        return 0;
    }

private:
    // ステージング (0xc4600) はboot0のUSB IN応答バッファで、ほかの番地の読み書きでも
    // 上書きされる可能性がある。ステージング以外への要求では、保持した応答の写しを無効にする。
    void Touch(unsigned address, std::size_t size)
    {
        if (address < Stage || address + size > Stage + staged_.size())
            staged_valid_ = false;
    }

    bool Read(unsigned address, std::uint8_t* data, std::size_t size)
    {
        Touch(address, size);
        // 要求が失敗した場合も、ステージングの内容は分からなくなる。
        if (address == Stage)
            staged_valid_ = false;
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
        Touch(address, 4);
        std::array<std::uint8_t, 4> word{data[0], data[1], data[2], data[3]};
        const px4::torne::MemoryRequest request{0x40, 0xbc,
            static_cast<std::uint16_t>((address >> 8) & 0xff00), static_cast<std::uint16_t>(address)};
        std::size_t transferred = 0;
        const bool ok = !control_.Transfer(request, word.data(), word.size(), transferred) &&
                        transferred == word.size();
        // ステージングへの書き込みは写しにも反映する。失敗した場合は内容が分からなくなる。
        if (address >= Stage && address + 4 <= Stage + staged_.size()) {
            if (ok)
                std::copy(word.begin(), word.end(), staged_.begin() + (address - Stage));
            else
                staged_valid_ = false;
        }
        return ok;
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
        runtime_installed_ = false;
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
    bool body_changed_ = false, hook_changed_ = false, runtime_installed_ = false;
    // ステージングに残っている直前の応答の写し。
    Packet staged_{};
    bool staged_valid_ = false;
};

struct BulkRead {
    BulkRead(HANDLE file, WINUSB_INTERFACE_HANDLE usb)
        : data(PumpMax), file_(file), usb_(usb)
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

    // 1回の読み出しは1回の操作12の送出量と同じ大きさにする。送出量は512バイトの倍数で
    // 短いパケットで終わらないため、大きく読むと次の操作12のデータまで受け取る。
    bool Submit(unsigned stage)
    {
        if (!op_.hEvent)
            return false;
        stage_ = stage;
        const auto event = op_.hEvent;
        op_ = {};
        op_.hEvent = event;
        ResetEvent(event);
        pending_ = WinUsb_ReadPipe(usb_, BulkIn, data.data(), PumpSize(stage), nullptr, &op_) ||
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
        return ok && size == PumpSize(stage_);
    }

    unsigned Stage() const { return stage_; }
    std::size_t Size() const { return PumpSize(stage_); }

    std::vector<std::uint8_t> data;

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    OVERLAPPED op_{};
    bool pending_ = false;
    unsigned stage_ = PumpStages - 1;
};

// カードUARTの16語だけを、Relayのboot0のメモリ要求で読み書きする。
// 共通版の導入中は、FIFOの読み書きを操作14で最大31バイトずつまとめて行う。
// 失敗後は、同じ操作内の後続の転送を止める。
class RelayCardBus final : public px4::dt295::CardBus {
public:
    RelayCardBus(Relay& relay, std::function<int(unsigned)> yield)
        : relay_(relay), yield_(std::move(yield)), bulk_(relay.RuntimeInstalled()) {}

    bool Bulk() const override { return bulk_; }

    int Fifo(bool write, std::uint8_t* data, std::size_t& count) override
    {
        const std::size_t requested = count;
        count = 0;
        if (error_)
            return error_;
        if (!bulk_)
            return -EOPNOTSUPP;
        Packet reply{};
        if (write) {
            for (std::size_t at = 0; at < requested; at += CardUart::FifoChunk) {
                const auto size = (std::min)(CardUart::FifoChunk, requested - at);
                if (relay_.Exchange(14, reply, data + at, size))
                    return error_ = -EIO;
            }
            count = requested;
            return 0;
        }
        // 操作14は最大31バイトを取り出す。それより小さい受け皿では取り出した分を失う。
        if (requested < CardUart::FifoChunk)
            return error_ = -EINVAL;
        if (relay_.Exchange(14, reply))
            return error_ = -EIO;
        const auto received = Word(reply.data() + 20);
        if (received > CardUart::FifoChunk)
            return error_ = -EPROTO;
        std::copy_n(reply.data() + 24, received, data);
        count = received;
        return 0;
    }

    int Read32(std::uint32_t address, std::uint32_t& value) override
    {
        if (const int rc = Check(address))
            return rc;
        return relay_.ReadWord(address, value) ? 0 : error_ = -EIO;
    }

    int Write32(std::uint32_t address, std::uint32_t value) override
    {
        if (const int rc = Check(address))
            return rc;
        return relay_.WriteWord(address, value) ? 0 : error_ = -EIO;
    }

    int Pause(unsigned milliseconds) override
    {
        if (error_)
            return error_;
        if (const int rc = yield_(milliseconds))
            error_ = rc;
        return error_;
    }

    int Error() const { return error_; }

private:
    int Check(std::uint32_t address) const
    {
        if (error_)
            return error_;
        return address < 0xfea000 || address > 0xfea03c || (address & 3) ? -EINVAL : 0;
    }

    Relay& relay_;
    std::function<int(unsigned)> yield_;
    bool bulk_;
    int error_ = 0;
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
            if (!restore_failed_) {
                std::lock_guard<std::mutex> control(control_mutex_);
                restore_failed_ = !relay_.Restore();
            }
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

    /*
     * 内蔵カードUARTの操作。受信の有無に関係なく、boot0のメモリ要求でカードUARTを読み書きする。
     * 操作12の間はEP0が応答しないため、TS workerと同じ制御ロックで操作12の合間に行い、
     * 手順内の待機では制御ロックを手放す。カード操作の後のCardBusyWindowの間は、TS workerが
     * 最小の送出量を使い、続くカード操作の待ちを短くする。
     */
    int Card(const std::function<int(CardUart&)>& operation)
    {
        if (!parent_.available_)
            return -ENODEV;
        const auto mark_busy = [this] {
            card_until_ = (Clock::now() + CardBusyWindow).time_since_epoch().count();
        };
        mark_busy();
        ++card_waiters_;
        std::unique_lock<std::mutex> control(control_mutex_);
        --card_waiters_;
        if (!parent_.available_)
            return -ENODEV;
        relay_.Begin(Clock::now() + 2s);
        RelayCardBus bus(relay_, [this, &control](unsigned ms) {
            control.unlock();
            const int rc = WaitRemoved(ms);
            ++card_waiters_;
            control.lock();
            --card_waiters_;
            relay_.Begin(Clock::now() + 2s);
            return rc;
        });
        CardUart uart(bus);
        const int rc = operation(uart);
        mark_busy();
        card_last_ = Clock::now().time_since_epoch().count();
        if (rc && bus.Error())
            msg_err("GV-MVP/XZ3: card transfer failed errno=%d\n", bus.Error());
        return rc;
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
        // 初期化と選局の間はカード操作を待たせる。どちらもI²Cと待機を交互に行う連続した手順で、
        // 途中でRelayを譲るとフックの導入途中や選局途中の状態でカードUARTへ要求が入る。
        std::unique_lock<std::mutex> control(control_mutex_);
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
        control.unlock();
        // 地デジはロック直後に訂正前のTSを出すことがある。
        // ファームウェア交換による待ちがなくなったため、DMAを始める前に安定を待つ。
        // この待機は機器を操作しないため、制御ロックを手放してカード操作を通す。
        if (!result && !satellite)
            result = WaitRemoved(350);
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
        Packet reply{};
        int result = 0;
        {
            std::lock_guard<std::mutex> control(control_mutex_);
            // 最初の転送を待つと、スキャン側の最初の統計取得に間に合わない場合がある。
            // DMAを始める前に新しいチャンネルの値を1回読み、以後はTS転送応答で更新する。
            cnr_ = px4::tc90522::CnrMilliDb(satellite_, frontend_.ReadCndat());
            // 開始の応答を失った場合にも、停止を試してからRAMを復元する。
            stream_started_ = true;
            result = relay_.Exchange(11, reply);
        }
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
        int result = 0;
        {
            std::lock_guard<std::mutex> control(control_mutex_);
            result = relay_.Exchange(13, reply);
        }
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
        // 流量を測る前は最小の段階から始める。読み出しは2本を先に発行するため、
        // 選んだ段階は2回後の操作12から使われる。
        unsigned stage = PumpStages - 1;
        // 流量を測るまでは余裕がないものとして扱い、操作12を遅らせない。
        double rate = CardHoldRate;
        if (!reads[0].Submit(stage) || !reads[1].Submit(stage))
            return -EIO;
        Packet reply{};
        const unsigned cn_address = satellite ? 0x19 : 0x18;
        RingStats stats(satellite);
        for (unsigned head = 0, count = 0; parent_.available_ && !stop_; head ^= 1, ++count) {
            auto& read = reads[head];
            // 待っているカード操作があれば、次の操作12の前に先に通す。
            while (card_waiters_.load() && parent_.available_ && !stop_)
                std::this_thread::yield();
            if (rate < CardHoldRate)
                HoldForCard();
            const bool read_cn = count % CnInterval == 0;
            const auto begin = Clock::now();
            {
                std::lock_guard<std::mutex> control(control_mutex_);
                if (relay_.Exchange(12, reply, nullptr, 0, read_cn ? cn_address : 0, read.Stage()) ||
                    Word(reply.data() + 52) != read.Size())
                    return -EIO;
            }
            if (!read.Collect(parent_.removed_))
                return -EIO;
            const auto end = Clock::now();
            // 応答ワード4〜6は、この要求中の最大未処理量、消費位置、DMAの書き込み位置。
            stats.Record(begin, end, Word(reply.data() + 40), Word(reply.data() + 44), Word(reply.data() + 48),
                         read.Stage());
            if (stop_)
                break;
            if (const int result = Deliver(read.data, read.Size()))
                return result;
            stage = NextStage(end - begin, read.Stage());
            // 未処理量がない状態の操作12の時間は、送出量を流量で割った値に近い。
            if (const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count())
                rate = static_cast<double>(read.Size()) * 1e6 / static_cast<double>(us);
            if (!read.Submit(stage))
                return -EIO;
            // op 12の応答ワード4はCNDAT。読み出し失敗時の0も反映して古い値を残さない。
            if (read_cn)
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
                    std::uint32_t cursor, std::uint32_t producer, unsigned stage)
        {
            const auto op_us = Micro(end - begin);
            min_stage_ = (std::min)(min_stage_, stage);
            max_stage_ = (std::max)(max_stage_, stage);
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
                // stageは操作12の送出量の段階 (0=3 MiB、3=384 KiB) の範囲。
                msg_info("GV-MVP/XZ3: ring stats chunks=%llu peak_bytes=%u ring_bytes=%u high_chunks=%llu max_producer=%u max_host_gap_us=%lld max_op_us=%lld stage=%u-%u\n",
                         chunks_, peak_, RingBytes, high_, producer_, gap_us_, op_us_, min_stage_, max_stage_);
                period_start_ = end;
                chunks_ = high_ = 0;
                peak_ = producer_ = 0;
                gap_us_ = op_us_ = 0;
                min_stage_ = PumpStages;
                max_stage_ = 0;
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
        unsigned min_stage_ = PumpStages, max_stage_ = 0;
        Clock::time_point period_start_, last_end_{};
        unsigned long long chunks_ = 0, high_ = 0, total_chunks_ = 0, total_high_ = 0;
        std::uint32_t peak_ = 0, total_peak_ = 0, producer_ = 0, total_producer_ = 0;
        long long gap_us_ = 0, op_us_ = 0, total_gap_us_ = 0, total_op_us_ = 0;
    };

    // 直前の操作12の時間から、次の要求が目標の時間を超えない最大の段階を選ぶ。
    // カード操作の後のCardBusyWindowの間は、カード操作の待ちを短くするため目標をPumpCardTargetにする。
    // 段階を流量に関係なく最小にすると、要求ごとの固定の負担 (約21 msの制御転送) で
    // 110度CSの送出が追いつかなくなるため、時間で選ぶ。
    unsigned NextStage(Clock::duration elapsed, unsigned stage) const
    {
        const bool card = Clock::now().time_since_epoch().count() < card_until_.load();
        const auto target = std::chrono::duration_cast<std::chrono::microseconds>(
            card ? PumpCardTarget : PumpTarget).count();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        for (unsigned next = 0; next < PumpStages - 1; ++next) {
            // 送出量は段階ごとに半分になるため、時間も比例すると見積もる。
            const auto estimate = next <= stage ? us << (stage - next) : us >> (next - stage);
            if (estimate <= target)
                return next;
        }
        return PumpStages - 1;
    }

    // カード操作が続く間、次の操作12の開始を遅らせる。CardIdleGapの間カード操作がないか、
    // CardHoldMaxを超えたら戻る。
    void HoldForCard()
    {
        const auto begin = Clock::now();
        while (parent_.available_ && !stop_ && Clock::now() - begin < CardHoldMax) {
            const auto now = Clock::now();
            if (now.time_since_epoch().count() >= card_until_.load())
                return;
            const auto last = Clock::time_point(Clock::duration(card_last_.load()));
            if (!card_waiters_.load() && now - last >= CardIdleGap)
                return;
            if (card_waiters_.load())
                std::this_thread::yield();
            else
                Sleep(1);
        }
    }

    int WaitRemoved(unsigned milliseconds)
    {
        return WaitForSingleObject(parent_.removed_, milliseconds) == WAIT_TIMEOUT ? 0 : -ENODEV;
    }

    int Deliver(const std::vector<std::uint8_t>& bytes, std::size_t length)
    {
        // 1レコードは4バイトの接頭辞と188バイトのTS。1回の転送はレコード境界で終わる。
        for (std::size_t offset = 4; offset < length; offset += 192) {
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
    // mutex_は受信機の状態、control_mutex_はRelay (EP0の制御転送) を守る。
    // 取得順はmutex_、control_mutex_の順。カード操作はcontrol_mutex_だけを取得する。
    std::mutex mutex_, control_mutex_;
    std::atomic<int> card_waiters_{0};
    // 最後のカード操作からCardBusyWindowが経つ時刻と、最後のカード操作を終えた時刻
    // (steady_clockの値)。
    std::atomic<Clock::rep> card_until_{0}, card_last_{0};
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
    card_reader_ = registered_;
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
    {
        // 実行中のカード操作がreceiver_を使い終わるまで待ち、受信途中のカード応答を破棄する。
        std::lock_guard<std::mutex> card(card_mutex_);
        card_open_ = false;
        ResetCardBuffer();
    }
    if (receiver_) {
        receiver_->WaitClosed();
        receiver_.reset();
    }
}

void Device::ResetCardBuffer()
{
    card_received_.clear();
    card_remaining_ = 0;
    card_atr_ = true;
    card_polled_empty_ = false;
}

int Device::OpenCard()
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!receiver_ || !available_)
        return -ENODEV;
    if (card_open_)
        return -EBUSY;
    card_open_ = true;
    ResetCardBuffer();
    return 0;
}

void Device::CloseCard()
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!card_open_)
        return;
    card_open_ = false;
    ResetCardBuffer();
    // CMDVCCNをHighにしてカードへの給電を止める。
    if (available_ && receiver_ && receiver_->Card([](CardUart& uart) { return uart.PowerDown(); }))
        msg_warn("GV-MVP/XZ3: card power down failed\n");
}

int Device::DetectCard(bool& detected)
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    detected = false;
    if (!card_open_ || !available_ || !receiver_)
        return -ENODEV;
    const int rc = receiver_->Card([&detected](CardUart& uart) { return uart.Present(detected); });
    // 抜去を検出したら、受信途中のATRやブロックを次のカードへ持ち越さない。
    if (rc || !detected)
        ResetCardBuffer();
    return rc;
}

int Device::ResetCard()
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!card_open_ || !available_ || !receiver_)
        return -ENODEV;
    ResetCardBuffer();
    return receiver_->Card([](CardUart& uart) { return uart.Reset(); });
}

int Device::SetCardBaudrate(std::uint32_t baudrate)
{
    // B-CASが要求する速度だけを受け付ける。PIX-DT295とtorneの内蔵リーダーと同じ範囲。
    if (baudrate != 19200)
        return -ENOTSUP;
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!card_open_ || !available_ || !receiver_)
        return -ENODEV;
    return receiver_->Card([](CardUart& uart) { return uart.SetBaudrate(19200); });
}

int Device::IsCardDataReady(bool& ready) try
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    ready = false;
    if (!card_open_ || !available_ || !receiver_)
        return -ENODEV;
    if (card_remaining_) {
        ready = true;
        return 0;
    }
    /*
     * SmartCardは応答を待つ間、約5 msおきにここを呼ぶ。XZ3の受信FIFOの読み出しは1回で
     * 約9〜21 msの制御転送になり、110度CSでは操作12の送出の余裕を削る。カードが応答を
     * 計算している間の空の読み出しを減らすため、空だった読み出しからCardPollIntervalの間は
     * 機器へ問い合わせない。受信が続いている間は間隔を空けない。
     */
    const auto now = std::chrono::steady_clock::now();
    if (card_polled_empty_ && now - card_polled_ < CardPollInterval)
        return 0;
    // 共通版の導入中は操作14の1往復で最大31バイトを読む。導入前の直接アクセスは
    // 1バイトごとに約3 msかかるが、TSを受信していないため操作12を待たせない。
    std::uint8_t bytes[CardUart::FifoChunk]{};
    std::size_t count = 0;
    const int rc = receiver_->Card([&](CardUart& uart) { return uart.ReadBytes(bytes, sizeof(bytes), count); });
    if (rc)
        return rc;
    card_polled_ = now;
    card_polled_empty_ = count == 0;
    if (card_received_.size() + count > 518)
        return -EMSGSIZE;
    card_received_.insert(card_received_.end(), bytes, bytes + count);
    if (card_atr_) {
        if (px4::winscard::IsBcasAtr(card_received_.data(), card_received_.size()))
            card_remaining_ = card_received_.size();
        else if (card_received_.size() >= 33)
            return -ENOTSUP;
    } else if (card_received_.size() >= 3) {
        const auto length = std::size_t(4) + card_received_[2];
        if (card_received_.size() >= length)
            card_remaining_ = length;
    }
    ready = card_remaining_ != 0;
    return 0;
} catch (...) {
    return -ENOMEM;
}

int Device::ReadCardData(std::uint8_t* buf, std::uint8_t& len)
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!card_open_ || !available_)
        return -ENODEV;
    if (!buf || !len)
        return -EINVAL;
    const auto count = (std::min)(std::size_t(len), card_remaining_);
    std::copy_n(card_received_.begin(), count, buf);
    card_received_.erase(card_received_.begin(), card_received_.begin() + static_cast<std::ptrdiff_t>(count));
    card_remaining_ -= count;
    if (count && card_atr_ && !card_remaining_)
        card_atr_ = false;
    len = static_cast<std::uint8_t>(count);
    return 0;
}

int Device::WriteCardData(const std::uint8_t* buf, std::uint8_t len)
{
    std::lock_guard<std::mutex> lock(card_mutex_);
    if (!card_open_ || !available_ || !receiver_)
        return -ENODEV;
    if (!buf || !len)
        return -EINVAL;
    // FIFOへの積み込みを区切り、各回の制御ロックの保持を短くする。送信開始は最後に1回だけ行う。
    // 共通版の導入中は操作14の1往復で31バイトを積む。導入前は1バイトごとに1回の制御転送になる。
    constexpr std::size_t Chunk = CardUart::FifoChunk;
    int rc = 0;
    for (std::size_t at = 0; !rc && at < len; at += Chunk) {
        const auto size = (std::min)(Chunk, std::size_t(len) - at);
        rc = receiver_->Card([&](CardUart& uart) {
            int result = at ? 0 : uart.BeginTransmit();
            if (!result)
                result = uart.Queue(buf + at, size);
            return result;
        });
    }
    if (!rc)
        rc = receiver_->Card([len](CardUart& uart) { return uart.Transmit(len); });
    return rc;
}

ReceiverBase* Device::GetReceiver(int id) const
{
    if (id)
        throw std::out_of_range("GV-MVP/XZ3 receiver index");
    return receiver_.get();
}

}
