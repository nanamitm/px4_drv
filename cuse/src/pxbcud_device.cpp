// SPDX-License-Identifier: GPL-2.0-only
#include "pxbcud_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace px4::cuse {
namespace {
// isochronous の転送の本数と 1 本のマイクロフレーム数。WinUSB 版の既定 (MaxUrbs=10、64 マイクロフレーム = 8 ms) と同じ。
// WinUSB 版では Linux em28xx と同じ 5 本 (40 ms) で完了待ちの最大が 33〜42 ms に並び isoch が枯れたため 10 本にした。
// libusb は完了の通知の中で出し直すため必要な本数が変わる可能性があり、Linux の実測で見直す。
constexpr int TransferCount = 10, PacketsPerTransfer = 64;
// TS が届かない時間と、選んだ TSID の PAT を待つ時間 (WinUSB 版と同じ)。
constexpr auto NoDataTimeout = std::chrono::seconds(5);
constexpr auto PatTimeout = std::chrono::seconds(5);
// 選局のロックを待つ時間。カーネルの ptx_chrdev_tune() の check_lock (10 ms × 300 回) と同じ。
constexpr auto LockTimeout = std::chrono::milliseconds(3000);

std::int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Device::Clock::now().time_since_epoch()).count();
}

const char *FailureName(int failure)
{
    static const char *const names[] = {"none", "complete", "submit", "packet_descriptor", "no_data", "pat_timeout"};
    return failure >= 0 && failure < 6 ? names[failure] : "unknown";
}
}

// 自動復旧の試験用の模擬の故障 (WinUSB 版の pxbcud_live_test の --auto-recover、--auto-limit、--auto-nodata に当たる)。
// PX4_CUSED_FAULT_INJECTION を定義したビルドだけが、受信の開始時に環境変数 PX4_CUSED_PXBCUD_FAULT を読む。
//   error:COUNT:AFTER   受信の開始から AFTER 秒後に完了した転送を失敗として扱い、再開した受信でも
//                       データが 300 ms 届いた後に同じ故障を続けて合計 COUNT 回起こす (連続して実らない復旧)
//   nodata:SECONDS:AFTER 受信の開始から AFTER 秒後から SECONDS 秒の間、届いたパケットの長さを 0 として扱う
struct PxBcudDevice::Fault {
#ifdef PX4_CUSED_FAULT_INJECTION
    enum class Kind { Error, NoData } kind = Kind::Error;
    unsigned remaining = 0, injected = 0;
    std::int64_t base_ns = 0, after_ns = 0, duration_ns = 0;

    static std::unique_ptr<Fault> FromEnvironment(std::int64_t now)
    {
        const char *text = std::getenv("PX4_CUSED_PXBCUD_FAULT");
        char kind[16] = {};
        unsigned value = 0, after = 0;
        if (!text || std::sscanf(text, "%15[a-z]:%u:%u", kind, &value, &after) != 3)
            return nullptr;
        auto fault = std::make_unique<Fault>();
        if (!std::strcmp(kind, "error"))
            fault->remaining = value;
        else if (!std::strcmp(kind, "nodata")) {
            fault->kind = Kind::NoData;
            fault->duration_ns = static_cast<std::int64_t>(value) * 1000000000;
        } else
            return nullptr;
        fault->base_ns = now;
        fault->after_ns = static_cast<std::int64_t>(after) * 1000000000;
        return fault;
    }
    // 完了した転送を失敗として扱うか。最初は AFTER の後、以降は再開した受信でデータが 300 ms 届いた後。
    bool Error(std::int64_t now, const Stream &s)
    {
        if (kind != Kind::Error || !remaining)
            return false;
        if (!injected ? now - base_ns < after_ns
                      : !s.received_any || now - s.first_data_ns < 300000000)
            return false;
        --remaining;
        ++injected;
        return true;
    }
    bool NoData(std::int64_t now) const
    {
        return kind == Kind::NoData && now - base_ns >= after_ns && now - base_ns < after_ns + duration_ns;
    }
#endif
};

// pxbcud::Io を libusb の同期制御転送で実装する。WinUSB 版の UsbIo と同じく、Begin() で設定した期限を
// 操作全体の絶対期限とし、USB の抜去では待機と転送が -ECANCELED で直ちに戻る。
class PxBcudDevice::UsbIo final : public pxbcud::Io {
public:
    UsbIo(libusb_device_handle *handle, RemovalSignal &removal, const std::string &label)
        : handle_(handle), removal_(removal), label_(label)
    {
    }
    void Begin(unsigned milliseconds) { deadline_ = Clock::now() + std::chrono::milliseconds(milliseconds); }
    int Wait(unsigned milliseconds) override
    {
        const auto remaining = Remaining();
        if (remaining <= 0)
            return -ETIMEDOUT;
        if (removal_.Wait(static_cast<unsigned>(std::min<long long>(remaining, milliseconds))))
            return -ECANCELED;
        return Clock::now() < deadline_ ? 0 : -ETIMEDOUT;
    }
    int Transfer(bool input, std::uint8_t request, std::uint16_t index, std::uint8_t *data, std::size_t size) override
    {
        if (removal_.Removed())
            return -ECANCELED;
        const auto remaining = Remaining();
        if (remaining <= 0)
            return -ETIMEDOUT;
        const int rc = libusb_control_transfer(handle_, input ? 0xc0 : 0x40, request, 0, index, data,
                                               static_cast<std::uint16_t>(size),
                                               static_cast<unsigned>(std::min<long long>(remaining, 1000)));
        if (rc >= 0)
            return rc;
        msg_err("%s: USB control failed input=%d request=0x%02x index=0x%04x length=%zu libusb=%s\n", label_.c_str(),
                input, request, index, size, libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_NO_DEVICE)
            return -ECANCELED;
        if (rc == LIBUSB_ERROR_TIMEOUT)
            return -ETIMEDOUT;
        return -EIO;
    }

private:
    using Clock = Device::Clock;
    long long Remaining() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now()).count();
    }
    libusb_device_handle *handle_;
    RemovalSignal &removal_;
    const std::string &label_;
    Clock::time_point deadline_{};
};

PxBcudDevice::PxBcudDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                           std::function<void()> changed)
    : Device(handle, bus, address, "PX-BCUD", "pxbcudvideo", std::move(changed)),
      io_(std::make_unique<UsbIo>(handle, removal_, label_)), frontend_(std::make_unique<pxbcud::Frontend>(*io_))
{
}

PxBcudDevice::~PxBcudDevice()
{
    StopSessions();
}

void PxBcudDevice::OnShutdown()
{
    if (!opened_ || !available_)
        return;
    std::lock_guard<std::mutex> lock(control_);
    io_->Begin(2000);
    if (const int rc = frontend_->Sleep())
        msg_err("%s: sleep failed result=%d\n", label_.c_str(), rc);
    opened_ = false;
}

int PxBcudDevice::VideoOpen()
{
    std::lock_guard<std::mutex> lock(control_);
    // WinUSB 版の Receiver::Open と同じく、開くたびにブリッジ、復調器、チューナーの全レジスターを初期化する。
    io_->Begin(8000);
    const int rc = frontend_->Initialize();
    if (rc) {
        msg_err("%s: frontend initialization failed result=%d tuner_id=0x%02x\n", label_.c_str(), rc,
                frontend_->TunerId());
        io_->Begin(1000);
        frontend_->Capture(false);
        return rc;
    }
    msg_info("%s: tuner ID=0x%02x\n", label_.c_str(), frontend_->TunerId());
    opened_ = true;
    return 0;
}

void PxBcudDevice::VideoClose()
{
    if (!opened_)
        return;
    opened_ = false;
    if (!available_)
        return;
    std::lock_guard<std::mutex> lock(control_);
    io_->Begin(2000);
    if (const int rc = frontend_->Sleep())
        msg_err("%s: sleep failed result=%d\n", label_.c_str(), rc);
}

bool PxBcudDevice::AcceptsSystem(std::uint32_t system) const
{
    // 衛星側の復調器とチューナーだけを持つため、地デジ (ISDB-T) は受け付けない。
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_S_SYSTEM;
}

int PxBcudDevice::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    Tune tune;
    int rc = ChannelToTune(system, freq, tune);
    if (!rc && !tune.satellite)
        rc = -EINVAL;
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc) {
        std::lock_guard<std::mutex> lock(control_);
        io_->Begin(5000);
        rc = frontend_->Tune(tune.khz);
        // カーネルの ptx_chrdev_tune() と同じく、ロックを待ってから TSID を選ぶ。
        // 相対 TS 番号は TMCC から TSID を求めるため、ロックの前には選べない。
        const auto deadline = Clock::now() + LockTimeout;
        bool locked = false;
        while (!rc) {
            io_->Begin(1000);
            rc = frontend_->Locked(locked);
            if (rc || locked)
                break;
            if (Clock::now() >= deadline) {
                rc = -EAGAIN;
                break;
            }
            if (removal_.Wait(10))
                rc = -ECANCELED;
        }
        if (!rc) {
            io_->Begin(2000);
            rc = frontend_->SelectStream(tune.tsid);
            // TMCC にない相対 TS 番号 (-EINVAL) と、TSID が切り替わらない場合 (-ETIMEDOUT) は、
            // 受信できない TS の指定として利用者が選び直せるようにする。
            if (rc == -EINVAL || rc == -ETIMEDOUT)
                rc = -EAGAIN;
        }
        // 引数不正と未ロックは機器の状態を壊さないため、選び直せる失敗として扱う。
        if (rc && rc != -EINVAL && rc != -EAGAIN)
            fatal = true;
    }
    msg_info("%s: set channel freq_no=%d slot=%d -> kHz=%u %s=%u selected=0x%04x result=%d\n", label_.c_str(),
             freq.freq_no, freq.slot, tune.khz, tune.relative ? "relative" : "tsid", tune.tsid,
             frontend_->SelectedStream(), rc);
    return rc;
}

int PxBcudDevice::LockStatus(bool &locked)
{
    std::lock_guard<std::mutex> lock(control_);
    io_->Begin(1000);
    return frontend_->Locked(locked);
}

int PxBcudDevice::ReadCn(std::uint32_t &raw)
{
    // TC90532 の衛星側の CN (0xbc〜0xbd) の生の値。dB への換算は、ほかの機種の ISDB-S と同じく利用者側で行う。
    std::uint16_t cn = 0;
    std::lock_guard<std::mutex> lock(control_);
    io_->Begin(1000);
    const int rc = frontend_->ReadCn(cn);
    raw = cn;
    return rc;
}

int PxBcudDevice::CheckEndpoint()
{
    // WinUSB 版の実機 (revision 0205) と同じ alternate setting 1 の EP 0x84、isochronous、940 バイト、interval 1 を確かめる。
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle_), &config))
        return -EIO;
    bool found = false;
    if (config->bNumInterfaces >= 1) {
        const auto &interface = config->interface[0];
        for (int i = 0; i < interface.num_altsetting && !found; ++i) {
            const auto &alt = interface.altsetting[i];
            if (alt.bAlternateSetting != 1)
                continue;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto &ep = alt.endpoint[e];
                if (ep.bEndpointAddress == 0x84 &&
                    (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS &&
                    ep.wMaxPacketSize == PxBcudStream::PacketSize && ep.bInterval == 1)
                    found = true;
            }
        }
    }
    libusb_free_config_descriptor(config);
    if (!found) {
        msg_err("%s: expected alt1 EP84 isoch interval1 940-byte endpoint was not found\n", label_.c_str());
        return -ENOSYS;
    }
    return 0;
}

int PxBcudDevice::StopCaptureLocked()
{
    // WinUSB 版と同じく、ブリッジの capture を先に止めてから帯域を返す。
    io_->Begin(1000);
    const int capture = frontend_->Capture(false);
    const int idle = libusb_set_interface_alt_setting(handle_, 0, 0);
    if (capture)
        return capture;
    return idle ? (idle == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO) : 0;
}

int PxBcudDevice::StartStream(bool &fatal)
{
    (void)fatal;
    auto &s = stream_;
    if (!frontend_->SelectedStream())
        return -EINVAL;
    if (s.leaked)
        return -EIO;
    if (const int rc = CheckEndpoint())
        return rc;
    try {
        s.ts = std::make_unique<PxBcudStream>(frontend_->SelectedStream());
        s.buffer.assign(static_cast<std::size_t>(TransferCount) * PacketsPerTransfer * PxBcudStream::PacketSize, 0);
        for (int i = 0; i < TransferCount; ++i) {
            auto *transfer = libusb_alloc_transfer(PacketsPerTransfer);
            if (!transfer)
                throw std::bad_alloc();
            libusb_fill_iso_transfer(transfer, handle_, 0x84,
                                     s.buffer.data() +
                                         static_cast<std::size_t>(i) * PacketsPerTransfer * PxBcudStream::PacketSize,
                                     PacketsPerTransfer * static_cast<int>(PxBcudStream::PacketSize),
                                     PacketsPerTransfer, IsoDone, this, 0);
            libusb_set_iso_packet_lengths(transfer, PxBcudStream::PacketSize);
            s.transfers.push_back(transfer);
        }
    } catch (...) {
        for (auto *transfer : s.transfers)
            libusb_free_transfer(transfer);
        s.transfers.clear();
        return -ENOMEM;
    }
    s.stop = false;
    s.terminal_error = 0;
    s.recovery_count = 0;
    s.recovering = false;
#ifdef PX4_CUSED_FAULT_INJECTION
    fault_ = Fault::FromEnvironment(NowNs());
    if (fault_)
        msg_warn("%s: fault injection enabled PX4_CUSED_PXBCUD_FAULT=%s\n", label_.c_str(),
                 std::getenv("PX4_CUSED_PXBCUD_FAULT"));
#endif
    int rc;
    {
        std::lock_guard<std::mutex> lock(control_);
        rc = libusb_set_interface_alt_setting(handle_, 0, 1) ? -EIO : 0;
        if (!rc) {
            io_->Begin(1000);
            rc = frontend_->Capture(true);
        }
        if (!rc)
            rc = Submit(false);
    }
    if (!rc) {
        try {
            s.worker = std::thread([this] { Run(); });
        } catch (...) {
            rc = -ENOMEM;
        }
    }
    // VideoNode は StartStream の失敗で StopStream を呼ばないため、転送の回収と capture の停止をここで行う。
    if (rc) {
        bool ignored = false;
        StopStream(ignored);
    }
    return rc;
}

int PxBcudDevice::Submit(bool restart)
{
    auto &s = stream_;
    if (restart)
        s.ts->Restart();
    s.error = 0;
    s.failure = Failure::None;
    s.pat_ready = s.ts->Ready();
    s.received_any = false;
    s.previous_completion_ns = 0;
    s.max_gap_us = 0;
    const auto now = NowNs();
    s.started_ns = now;
    s.first_data_ns = 0;
    s.last_data_ns = now;
    s.resubmit = true;
    for (auto *transfer : s.transfers) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.in_flight;
        }
        if (const int rc = libusb_submit_transfer(transfer)) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                --s.in_flight;
            }
            msg_err("%s: isoch submit failed libusb=%s\n", label_.c_str(), libusb_error_name(rc));
            s.resubmit = false;
            CancelAndWait();
            return rc == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO;
        }
    }
    msg_info("%s: timing start requests=%d microframes_per_request=%d nominal_queue_us=%d restart=%d\n",
             label_.c_str(), TransferCount, PacketsPerTransfer, TransferCount * PacketsPerTransfer * 125,
             restart ? 1 : 0);
    return 0;
}

bool PxBcudDevice::CancelAndWait()
{
    auto &s = stream_;
    s.resubmit = false;
    for (auto *transfer : s.transfers)
        libusb_cancel_transfer(transfer);
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.changed.wait_for(lock, std::chrono::seconds(5), [&s] { return s.in_flight == 0; })) {
        msg_err("%s: %d isoch transfers did not complete\n", label_.c_str(), s.in_flight);
        s.leaked = true;
        return false;
    }
    return true;
}

void PxBcudDevice::SetFailure(int error, Failure failure)
{
    auto &s = stream_;
    int expected = 0;
    if (!s.error.compare_exchange_strong(expected, error))
        return;
    s.failure = failure;
    s.resubmit = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
    }
    s.changed.notify_all();
}

void LIBUSB_CALL PxBcudDevice::IsoDone(libusb_transfer *transfer)
{
    auto &self = *static_cast<PxBcudDevice *>(transfer->user_data);
    auto &s = self.stream_;
    bool no_data = false, injected = false;
#ifdef PX4_CUSED_FAULT_INJECTION
    if (self.fault_ && transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        const auto now = NowNs();
        no_data = self.fault_->NoData(now);
        injected = !s.error && self.fault_->Error(now, s);
        if (injected)
            msg_warn("%s: injected isoch transfer failure injected=%u remaining=%u\n", self.label_.c_str(),
                     self.fault_->injected, self.fault_->remaining);
    }
#endif
    if (injected) {
        self.SetFailure(-EIO, Failure::Complete);
    } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        const auto now = NowNs();
        if (s.previous_completion_ns)
            s.max_gap_us = std::max<std::uint64_t>(s.max_gap_us, (now - s.previous_completion_ns) / 1000);
        s.previous_completion_ns = now;
        bool data = false, valid = true;
        for (int i = 0; i < transfer->num_iso_packets && valid; ++i) {
            const auto &packet = transfer->iso_packet_desc[i];
            const bool ok = packet.status == LIBUSB_TRANSFER_COMPLETED;
            const unsigned length = no_data ? 0 : packet.actual_length;
            data = data || (ok && length);
            valid = s.ts->Packet(ok, libusb_get_iso_packet_buffer_simple(transfer, static_cast<unsigned>(i)), length);
            if (!valid)
                msg_err("%s: invalid isoch descriptor packet=%d status=%d length=%u\n", self.label_.c_str(), i,
                        packet.status, packet.actual_length);
        }
        if (data) {
            s.last_data_ns = now;
            if (!s.received_any.exchange(true))
                s.first_data_ns = now;
        }
        if (!valid) {
            self.SetFailure(-EIO, Failure::PacketDescriptor);
        } else if (s.ts->Flush([&self](const std::uint8_t *packets, std::size_t size) {
                       self.video_.Push(packets, size);
                   })) {
            if (!s.pat_ready.exchange(true) || s.recovering.exchange(false))
                msg_info("%s: TS delivery started startup_discarded_packets=%llu recovery_count=%llu\n",
                         self.label_.c_str(), static_cast<unsigned long long>(s.ts->startup_discarded),
                         static_cast<unsigned long long>(s.recovery_count));
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        // 抜去では発行済みの転送がまとめて失敗するため、最初の 1 本だけを記録する。
        if (!s.error)
            msg_err("%s: isoch transfer status=%d\n", self.label_.c_str(), transfer->status);
        self.SetFailure(transfer->status == LIBUSB_TRANSFER_NO_DEVICE ? -ENODEV : -EIO, Failure::Complete);
    }
    if (s.resubmit && !s.error && transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        const int rc = libusb_submit_transfer(transfer);
        if (!rc)
            return;
        msg_err("%s: isoch resubmit failed libusb=%s\n", self.label_.c_str(), libusb_error_name(rc));
        self.SetFailure(rc == LIBUSB_ERROR_NO_DEVICE ? -ENODEV : -EIO, Failure::Submit);
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        --s.in_flight;
    }
    s.changed.notify_all();
}

void PxBcudDevice::LogStreamEnd(Failure failure)
{
    auto &s = stream_;
    msg_info("%s: TS stopped packets=%llu tei=%llu discarded_bytes=%llu error=%d stage=%s iso_errors=%llu "
             "empty_iso_packets=%llu startup_discarded_packets=%llu ring_overflow=%llu max_completion_gap_us=%llu\n",
             label_.c_str(), static_cast<unsigned long long>(s.ts->Packets()),
             static_cast<unsigned long long>(s.ts->Tei()), static_cast<unsigned long long>(s.ts->DiscardedBytes()),
             s.error.load(), FailureName(static_cast<int>(failure)),
             static_cast<unsigned long long>(s.ts->iso_errors), static_cast<unsigned long long>(s.ts->empty_packets),
             static_cast<unsigned long long>(s.ts->startup_discarded),
             static_cast<unsigned long long>(video_.Overflow()), static_cast<unsigned long long>(s.max_gap_us));
}

void PxBcudDevice::Run()
{
    auto &s = stream_;
    pxbcud::RecoveryBudget budget;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            while (!s.stop && !s.error) {
                const auto now = NowNs();
                // 受信が 5 秒途切れた場合と、選んだ TSID の PAT が 5 秒届かない場合は WinUSB 版と同じく失敗にする。
                if (std::chrono::nanoseconds(now - s.last_data_ns) > NoDataTimeout) {
                    lock.unlock();
                    SetFailure(-ETIMEDOUT, Failure::NoData);
                    lock.lock();
                    break;
                }
                if (!s.pat_ready && std::chrono::nanoseconds(now - s.started_ns) > PatTimeout) {
                    lock.unlock();
                    msg_err("%s: selected TSID PAT was not received tsid=0x%04x\n", label_.c_str(),
                            frontend_->SelectedStream());
                    SetFailure(-EIO, Failure::PatTimeout);
                    lock.lock();
                    break;
                }
                s.changed.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (s.stop)
                break;
        }
        const auto failed_at = Clock::now();
        const int error = s.error;
        const Failure failure = s.failure;
        if (!CancelAndWait()) {
            s.terminal_error = -EIO;
            video_.Fail(-EIO);
            return;
        }
        LogStreamEnd(failure);
        if (!available_ || error == -ENODEV) {
            s.terminal_error = -ENODEV;
            video_.Fail(-ENODEV);
            return;
        }
        // 自動復旧の対象は、WinUSB 版のエラー 31・87・タイムアウトに当たる転送の失敗と無受信に限る。
        // PAT が届かない場合と記述子の異常は、再開しても同じ状態が続くため受信を終える。
        const bool retryable = failure == Failure::Complete || failure == Failure::Submit || failure == Failure::NoData;
        if (!retryable) {
            s.terminal_error = error;
            video_.Fail(error);
            return;
        }
        // 受信できていた区間は、最初にデータが届いてから最後に届くまでで数える (WinUSB 版と同じ)。
        budget.NoteStream(s.received_any ? std::chrono::nanoseconds(s.last_data_ns - s.first_data_ns)
                                         : Clock::duration::zero());
        const bool no_data = failure == Failure::NoData && !s.received_any;
        if (!Recover(error, failure, no_data, budget, failed_at)) {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.stop)
                break;
            s.terminal_error = error;
            video_.Fail(error);
            return;
        }
    }
    CancelAndWait();
    LogStreamEnd(s.failure);
}

bool PxBcudDevice::Recover(int error, Failure failure, bool waited_for_input, pxbcud::RecoveryBudget &budget,
                           Clock::time_point failed_at)
{
    auto &s = stream_;
    for (;;) {
        const auto delay = budget.RestartDelay(waited_for_input);
        if (!delay) {
            msg_err("%s: auto recovery exhausted limit=%u last_error=%d recovery_count=%llu\n", label_.c_str(),
                    static_cast<unsigned>(pxbcud::RecoveryBudget::Limit), error,
                    static_cast<unsigned long long>(s.recovery_count));
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            if (s.changed.wait_for(lock, *delay, [&s] { return s.stop; }) || !available_)
                return false;
        }
        ++s.recovery_count;
        msg_warn("%s: auto recovery begin count=%llu error=%d stage=%s consecutive=%u backoff_ms=%lld no_data=%u "
                 "possible_ts_loss=1\n",
                 label_.c_str(), static_cast<unsigned long long>(s.recovery_count), error,
                 FailureName(static_cast<int>(failure)), static_cast<unsigned>(budget.Consecutive()),
                 static_cast<long long>(delay->count()), waited_for_input ? 1u : 0u);
        int rc;
        {
            std::lock_guard<std::mutex> lock(control_);
            // 転送はすべて回収済み。配信のリングと選局は保ったまま、capture の停止、alt0、alt1、capture の開始の順に戻す。
            rc = StopCaptureLocked();
            if (!rc)
                rc = libusb_set_interface_alt_setting(handle_, 0, 1) ? -EIO : 0;
            if (!rc) {
                io_->Begin(1000);
                rc = frontend_->Capture(true);
            }
            if (!rc) {
                s.recovering = true;
                rc = Submit(true);
            }
            if (!rc) {
                msg_warn("%s: auto recovery restarted count=%llu outage_ms=%lld\n", label_.c_str(),
                         static_cast<unsigned long long>(s.recovery_count),
                         static_cast<long long>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - failed_at).count()));
                return true;
            }
            s.recovering = false;
            msg_err("%s: auto recovery restart failed result=%d original_error=%d count=%llu\n", label_.c_str(), rc,
                    error, static_cast<unsigned long long>(s.recovery_count));
            // 途中で失敗しても、停止と帯域の解放を両方確かめられれば再試行できる。
            if (const int cleanup = StopCaptureLocked()) {
                msg_err("%s: auto recovery could not confirm capture stopped result=%d\n", label_.c_str(), cleanup);
                return false;
            }
        }
        if (rc != -EIO && rc != -ETIMEDOUT)
            return false;
        // USB の再開だけが失敗した試行は無受信として扱わず、残りの回数と待機を適用する。
        waited_for_input = false;
    }
}

int PxBcudDevice::StopStream(bool &fatal)
{
    auto &s = stream_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stop = true;
    }
    s.changed.notify_all();
    if (s.worker.joinable())
        s.worker.join();
    // worker を起動する前の失敗では、ここで転送を回収する。
    if (!s.leaked)
        CancelAndWait();
    if (!s.leaked) {
        for (auto *transfer : s.transfers)
            libusb_free_transfer(transfer);
        s.transfers.clear();
    } else {
        fatal = true;
    }
    if (available_) {
        std::lock_guard<std::mutex> lock(control_);
        if (const int rc = StopCaptureLocked()) {
            msg_err("%s: capture stop failed result=%d\n", label_.c_str(), rc);
            fatal = true;
        }
    }
    return s.terminal_error;
}
}
