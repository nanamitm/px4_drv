// SPDX-License-Identifier: GPL-2.0-only
#include "dt295_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <ctime>

namespace px4::cuse {
namespace {
// Bulk IN の読み出し。WinUSB 版の direct-fr と同じ 8 KiB × 64 本。
constexpr std::size_t BulkCount = 64, BulkSize = 8192;

std::uint64_t Micros(Device::Clock::duration d)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

// カーネルの px4_card_reader_id() と WinUSB 版の HashDevicePath() と同じ ID を求める。
// GUID は winusb/pkg/inf/PIX-DT295.inf の DeviceInterfaceGUIDs。
bool CardReaderId(libusb_device_handle *handle, std::string &id)
{
    libusb_device_descriptor desc{};
    unsigned char serial[256] = {};
    if (libusb_get_device_descriptor(libusb_get_device(handle), &desc) || !desc.iSerialNumber ||
        libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, serial, sizeof(serial)) <= 0)
        return false;
    const std::string s(reinterpret_cast<const char *>(serial));
    if (s.empty() || s == "." || s == ".." ||
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
        return false;
    char path[512];
    std::snprintf(path, sizeof(path), "\\\\?\\usb#vid_%04x&pid_%04x#%s#%s", desc.idVendor, desc.idProduct, s.c_str(),
                  "{f5b71c91-0634-4a6b-902f-fd5756f591d3}");
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char *p = path; *p; ++p) {
        hash ^= static_cast<std::uint16_t>(std::tolower(static_cast<unsigned char>(*p)));
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX", static_cast<unsigned long long>(hash));
    id = out;
    return true;
}
}

Dt295Device::Dt295Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                         std::function<void()> changed)
    : Device(handle, bus, address, "PIX-DT295", "dt295video", std::move(changed)), transport_(handle, removal_),
      channel_(source_, transport_, available_, [this](unsigned ms) { return removal_.Wait(ms); })
{
}

Dt295Device::~Dt295Device()
{
    StopSessions();
}

bool Dt295Device::UsbTransfersPending() const
{
    std::lock_guard<std::mutex> lock(receive_->mutex);
    return receive_->deferred != 0;
}

int Dt295Device::InitModel()
{
    if (const int rc = source_.LoadEmbedded())
        return rc;
    if (!CardReaderId(handle_, card_id_))
        msg_warn("%s: serial number unavailable; card reader disabled\n", label_.c_str());
    return 0;
}

void Dt295Device::OnRemove()
{
    // 切断後の機器へは復元の転送を行えないため、初期化済みの状態も破棄する。
    initialized_ = false;
}

void Dt295Device::OnShutdown()
{
    VideoClose();
}

int Dt295Device::VideoOpen()
{
    // 復元できなかった機器は、WinUSB 版と同じく挿し直すまで開かせない。
    return restore_failed_ ? -ENODEV : 0;
}

void Dt295Device::VideoClose()
{
    // WinUSB 版の受信機の Close と同じく、閉じるたびに機器を起動前の状態へ戻す。
    if (initialized_ && available_ && !restore_failed_) {
        auto command = channel_.Lock();
        RestoreLocked();
    }
    initialized_ = false;
}

bool Dt295Device::AcceptsSystem(std::uint32_t system) const
{
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM || system == PTX_ISDB_S_SYSTEM;
}

int Dt295Device::EnsureInitialized()
{
    if (initialized_)
        return 0;
    auto command = channel_.Lock();
    msg_info("%s: initializing backend=direct-fr\n", label_.c_str());
    transport_.Begin(120000);
    const int rc = source_.Start(transport_.Callbacks());
    transport_.ReportTiming("initialize");
    if (rc) {
        msg_err("%s: initialize failed result=%d libusb=%lu\n", label_.c_str(), rc, transport_.LastError());
        // Start が途中まで書き込んだ場合も、カード処理を再開する前に前像を戻す。
        if (available_)
            RestoreLocked();
        return rc;
    }
    initialized_ = true;
    return 0;
}

void Dt295Device::RestoreLocked()
{
    transport_.BeginRestore(120000);
    const int rc = source_.Close();
    transport_.ReportTiming("source-close");
    restore_failed_ = rc != 0;
    if (rc) {
        transport_.Block();
        msg_err("%s: restore failed result=%d\n", label_.c_str(), rc);
    }
    initialized_ = false;
}

int Dt295Device::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    Tune tune;
    int rc = ChannelToTune(system, freq, tune);
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc) {
        rc = EnsureInitialized();
        if (rc)
            fatal = true;
    }
    if (!rc) {
        auto command = channel_.Lock();
        transport_.Begin(20000);
        // 衛星の tsid 0〜7 は、共有コードが相対 TS 番号として TMCC から TSID を求める。
        rc = source_.Tune(tune.satellite, tune.khz, tune.tsid);
        transport_.ReportTiming("tune");
        // 引数不正、無信号 (-ENOLINK)、存在しない相対 TS 番号や TSID の不一致 (-EAGAIN) は機器の状態を壊さないため、
        // 次の選局を受け付ける。それ以外は転送の異常として、閉じて復元するまで操作を断る。
        if (rc && rc != -EINVAL && rc != -ENOLINK && rc != -EAGAIN)
            fatal = true;
    }
    msg_info("%s: set channel freq_no=%d slot=%d -> %s%s kHz=%u tsid=%u result=%d\n", label_.c_str(), freq.freq_no,
             freq.slot, tune.satellite ? "ISDB-S" : "ISDB-T", tune.relative ? " relative" : "", tune.khz, tune.tsid,
             rc);
    // 復調器 TC90532XBG は地デジのロック直後に訂正前の TS を数十 ms 出力するため、WinUSB 版
    // (RECEIVER_WAIT_AFTER_LOCK_TC_T) と同じく 350 ms 待ってから選局を完了する。
    // TS の取り込みは START_STREAMING から始まるため、この間の TS は利用者へ渡らない。
    if (!rc && !tune.satellite)
        removal_.Wait(350);
    return rc;
}

int Dt295Device::LockStatus(bool &locked)
{
    // Locked() は選局で検証した状態を返すだけで USB 転送を行わないため、制御ロックを取らない。
    int value = 0;
    const int rc = source_.Locked(value);
    locked = value != 0;
    return rc;
}

int Dt295Device::ReadCn(std::uint32_t &raw)
{
    bool satellite = false;
    return channel_.ReadCn(satellite, raw);
}

void LIBUSB_CALL Dt295Device::BulkDone(libusb_transfer *usb)
{
    auto &transfer = *static_cast<Transfer *>(usb->user_data);
    const auto state = transfer.state;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->owner) {
        delete &transfer;
        --state->deferred;
        return;
    }

    auto &self = *state->owner;
    auto &s = self.stream_;
    if (s.running && !s.error) {
        int error = 0;
        if (usb->status == LIBUSB_TRANSFER_COMPLETED) {
            const auto now = Clock::now();
            if (s.previous_read != Clock::time_point{})
                s.max_read_gap_us = std::max(s.max_read_gap_us, Micros(now - s.previous_read));
            s.previous_read = now;
            if (s.deliver && usb->actual_length) {
                error = s.delivery->Deliver(self.source_, usb->buffer, static_cast<std::size_t>(usb->actual_length),
                                            [&self](const std::uint8_t *packets, std::size_t size) {
                                                self.video_.Push(packets, size);
                                            });
            }
            if (!error) {
                const int rc = libusb_submit_transfer(usb);
                if (!rc)
                    return;
                error = rc == LIBUSB_ERROR_NO_DEVICE ? -ENODEV : -EIO;
                msg_err("%s: bulk resubmit failed (%s)\n", self.label_.c_str(), libusb_error_name(rc));
            }
        } else if (usb->status != LIBUSB_TRANSFER_CANCELLED) {
            error = usb->status == LIBUSB_TRANSFER_NO_DEVICE ? -ENODEV : -EIO;
            msg_err("%s: bulk status=%d\n", self.label_.c_str(), usb->status);
        }
        int expected = 0;
        if (error && s.error.compare_exchange_strong(expected, error))
            self.video_.Fail(error);
    }
    transfer.pending = false;
    if (--s.in_flight == 0)
        state->idle.notify_all();
}

int Dt295Device::StartStream(bool &fatal)
{
    auto &s = stream_;
    if (restore_failed_) {
        fatal = true;
        return -EIO;
    }
    try {
        s.delivery = std::make_unique<dt295::TsDelivery>();
        s.transfers.reserve(BulkCount);
        for (std::size_t i = 0; i < BulkCount; ++i) {
            auto transfer = std::make_unique<Transfer>();
            transfer->state = receive_;
            transfer->buffer.resize(BulkSize);
            transfer->usb = libusb_alloc_transfer(0);
            if (!transfer->usb)
                throw std::bad_alloc();
            libusb_fill_bulk_transfer(transfer->usb, handle_, 0x81, transfer->buffer.data(),
                                      static_cast<int>(transfer->buffer.size()), BulkDone, transfer.get(), 0);
            s.transfers.push_back(std::move(transfer));
        }
    } catch (...) {
        s.transfers.clear();
        s.delivery.reset();
        return -ENOMEM;
    }
    s.error = 0;
    s.stop = false;
    s.previous_read = Clock::time_point{};
    s.max_read_gap_us = 0;
    int submit_error = 0;
    {
        std::lock_guard<std::mutex> lock(receive_->mutex);
        s.running = true;
        for (auto &transfer : s.transfers) {
            submit_error = libusb_submit_transfer(transfer->usb);
            if (submit_error)
                break;
            transfer->pending = true;
            ++s.in_flight;
        }
    }
    if (submit_error) {
        msg_err("%s: bulk submit failed (%s)\n", label_.c_str(), libusb_error_name(submit_error));
        StopTransfers(fatal);
        return submit_error == LIBUSB_ERROR_NO_DEVICE ? -ENODEV : -EIO;
    }
    if (const int error = s.error) {
        StopTransfers(fatal);
        return error;
    }
    int rc;
    {
        auto command = channel_.Lock();
        transport_.Begin(20000);
        rc = source_.Capture(true);
    }
    if (rc) {
        StopTransfers(fatal);
        fatal = true;
        return rc;
    }
    // input の初期化が終わる前の USB のデータは破棄し、フレーマーとの競合を避ける。
    {
        std::lock_guard<std::mutex> lock(receive_->mutex);
        s.deliver = true;
        rc = s.error;
    }
    if (rc) {
        StopTransfers(fatal);
        return rc;
    }
    try {
        s.service = std::thread([this] {
            auto &s = stream_;
            dt295::ControlChannel::ServiceStats stats;
            std::time_t max_at = 0;
            std::uint64_t max_us = 0;
            const int rc = channel_.RunService(
                [this] { return !available_ || stream_.stop || stream_.error; },
                [&max_at, &max_us](Clock::time_point begin, Clock::time_point completed, int) {
                    // SERVICE の所要時間の最大値が、受信中と停止の手順のどちらで起きたかを区別できるよう時刻を残す。
                    const auto us = Micros(completed - begin);
                    if (us > max_us) {
                        max_us = us;
                        max_at = std::time(nullptr);
                    }
                },
                stats);
            if (rc) {
                msg_err("%s: SERVICE failed result=%d\n", label_.c_str(), rc);
                s.error = rc;
                video_.Fail(rc);
            }
            char at[16] = "-";
            std::tm local{};
            if (max_at && localtime_r(&max_at, &local))
                std::strftime(at, sizeof(at), "%H:%M:%S", &local);
            msg_info("%s: service calls=%llu mean_gap_us=%llu max_gap_us=%llu max_call_us=%llu max_call_at=%s\n",
                     label_.c_str(), static_cast<unsigned long long>(stats.calls),
                     static_cast<unsigned long long>(stats.calls ? stats.total_gap_us / stats.calls : 0),
                     static_cast<unsigned long long>(stats.max_gap_us), static_cast<unsigned long long>(stats.max_call_us),
                     at);
        });
    } catch (...) {
        StopTransfers(fatal);
        return -ENOMEM;
    }
    return 0;
}

int Dt295Device::StopStream(bool &fatal)
{
    return StopTransfers(fatal);
}

int Dt295Device::StopTransfers(bool &fatal)
{
    auto &s = stream_;
    s.stop = true;
    if (s.service.joinable())
        s.service.join();
    {
        // 再投入の判定・実行と、停止の判定・キャンセルを同じロックで行う。
        std::lock_guard<std::mutex> lock(receive_->mutex);
        s.running = s.deliver = false;
        for (auto &transfer : s.transfers)
            if (transfer->pending)
                libusb_cancel_transfer(transfer->usb);
    }
    {
        std::unique_lock<std::mutex> lock(receive_->mutex);
        if (!receive_->idle.wait_for(lock, std::chrono::seconds(5), [&s] { return s.in_flight == 0; })) {
            msg_err("%s: %d bulk transfers did not complete; reconnect USB\n", label_.c_str(), s.in_flight);
            receive_->owner = nullptr;
            receive_->deferred = static_cast<unsigned>(s.in_flight);
            s.in_flight = 0;
            // 遅れて完了した転送はデバイスに触れず、コールバックがバッファごと解放する。
            for (auto &transfer : s.transfers)
                if (transfer->pending)
                    transfer.release();
            transport_.Block();
            restore_failed_ = fatal = true;
            if (!s.error)
                s.error = -EIO;
        }
    }
    s.transfers.clear();
    if (available_ && !restore_failed_) {
        auto command = channel_.Lock();
        transport_.Begin(20000);
        const int rc = source_.Capture(false);
        transport_.ReportTiming("capture-stop");
        if (rc) {
            msg_err("%s: capture stop result=%d\n", label_.c_str(), rc);
            fatal = true;
        }
    }
    std::uint64_t discarded = 0, desyncs = 0;
    source_.InputStats(discarded, desyncs);
    px4::TsContinuityMonitor::Counters q{};
    if (s.delivery)
        q = s.delivery->Counts();
    msg_info("%s: stream end packets=%llu framing_desync=%llu sync=%llu tei=%llu cc_jump=%llu "
             "malformed=%llu ring_overflow=%llu max_read_gap_us=%llu error=%d\n",
             label_.c_str(), static_cast<unsigned long long>(q.packets), static_cast<unsigned long long>(desyncs),
             static_cast<unsigned long long>(q.sync), static_cast<unsigned long long>(q.tei),
             static_cast<unsigned long long>(q.cc_jump), static_cast<unsigned long long>(q.malformed),
             static_cast<unsigned long long>(video_.Overflow()), static_cast<unsigned long long>(s.max_read_gap_us),
             s.error.load());
    s.delivery.reset();
    return s.error;
}
}
