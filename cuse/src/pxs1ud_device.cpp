// SPDX-License-Identifier: GPL-2.0-only
#include "pxs1ud_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>

#ifndef PX4_CUSED_LIBDIR
#define PX4_CUSED_LIBDIR "/usr/local/lib/px4-cused"
#endif

namespace px4::cuse {
namespace {
// Bulk IN の転送の本数と大きさ。WinUSB 版 (上流の Windows 版と同じ 32 × 16 KiB) と同じ値で始め、Linux の実測で見直す
constexpr int TransferCount = 32;
constexpr std::size_t TransferSize = 16384;
// ロックの待機。地デジはカーネルの ptx_chrdev_tune() (10 ms × 300 回) と同じ。J.83B は WinUSB 版の PX-S1UD と同じく、
// helper がブロック間の欠落の長さを推定して等化器を収束させるまで 1 秒を超えることがあるため 2 秒
constexpr auto IsdbtLockTimeout = std::chrono::milliseconds(3000);
constexpr auto J83bLockTimeout = std::chrono::milliseconds(2000);

// helper と J.83B 用の firmware の置き場所。試験では環境変数 PX4_CUSED_LIBDIR で導入前の場所を指定できる
std::string LibDir()
{
    const char *dir = std::getenv("PX4_CUSED_LIBDIR");
    return dir && *dir ? dir : PX4_CUSED_LIBDIR;
}

int ReadFile(const std::string &path, std::vector<std::uint8_t> &data)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return -ENOENT;
    const auto n = f.tellg();
    if (n <= 0 || n > 4 * 1024 * 1024)
        return -EBADMSG;
    data.resize(static_cast<std::size_t>(n));
    f.seekg(0);
    return f.read(reinterpret_cast<char *>(data.data()), n) ? 0 : -EIO;
}
}

PxS1udDevice::PxS1udDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                           std::function<void()> changed)
    : Device(handle, bus, address, "PX-S1UD", "pxs1udvideo", std::move(changed)),
      helper_(std::make_unique<PosixIqProcess>(LibDir() + "/msi2500_frontend")),
      session_(std::make_unique<siano::Session>(static_cast<siano::SessionPort &>(*this), helper_.get(),
                                                static_cast<unsigned>(TransferCount)))
{
}

PxS1udDevice::~PxS1udDevice()
{
    StopSessions();
    if (opened_)
        CloseSession();
}

int PxS1udDevice::InitModel()
{
    // WinUSB 版の Device::Init と同じく、interface 0 の Bulk IN (1 つ) と Bulk OUT 0x02 を確かめる。
    // 応答の境界は IN の最大パケット長からヘッダーを除いた単位で揃う (smsusb_onresponse() と同じ)
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle_), &config))
        return -EIO;
    int rc = 0;
    std::uint16_t packet = 0;
    if (config->bNumInterfaces >= 1 && config->interface[0].num_altsetting >= 1) {
        const auto &alt = config->interface[0].altsetting[0];
        for (int e = 0; e < alt.bNumEndpoints && !rc; ++e) {
            const auto &ep = alt.endpoint[e];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                if (in_)
                    rc = -ENOSYS;
                in_ = ep.bEndpointAddress;
                packet = ep.wMaxPacketSize;
            } else if (ep.bEndpointAddress == 0x02) {
                out_ = ep.bEndpointAddress;
            }
        }
    }
    libusb_free_config_descriptor(config);
    if (!rc && (!in_ || !out_ || packet <= SMS_HEADER_SIZE))
        rc = -ENOSYS;
    if (rc) {
        msg_err("%s: expected bulk IN and bulk OUT 0x02 endpoints were not found\n", label_.c_str());
        return rc;
    }
    alignment_ = packet - SMS_HEADER_SIZE;
    msg_info("%s: bulk in=0x%02x out=0x%02x alignment=%zu J.83B helper=%s\n", label_.c_str(), in_, out_, alignment_,
             helper_->Executable().c_str());
    return 0;
}

void PxS1udDevice::CloseSession()
{
    session_->Close();
    StopReceive();
    opened_ = false;
}

void PxS1udDevice::OnRemove()
{
    // 待機中の Exchange を抜去で起こしてから、USB の転送を行わずに helper と受信を止める
    session_->Interrupt();
    if (opened_)
        CloseSession();
}

void PxS1udDevice::OnShutdown()
{
    // J.83B 用の firmware なら通常の firmware へ戻してから受信を止める。カーネルの smsusb へ機器を返す前に
    // 地デジの状態へ揃える (WinUSB 版の Close と同じ)
    if (opened_ && available_)
        CloseSession();
}

int PxS1udDevice::VideoOpen()
{
    if (opened_)
        return 0;
    if (leaked_)
        return -EIO;
    const int rc = session_->Open();
    if (rc)
        return rc;
    opened_ = true;
    return 0;
}

void PxS1udDevice::VideoClose()
{
    if (opened_)
        CloseSession();
}

bool PxS1udDevice::AcceptsSystem(std::uint32_t system) const
{
    // 地デジだけを PTX_SET_SYSTEM_MODE で受け付ける。J.83B はカーネルと同じく拡張 ioctl でだけ指定する
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM;
}

bool PxS1udDevice::AcceptsParams(std::uint32_t system) const
{
    return system == PTX_J83B_SYSTEM;
}

int PxS1udDevice::WaitLock(std::chrono::milliseconds limit, const char *system, std::uint32_t khz)
{
    const auto started = Clock::now();
    const auto deadline = started + limit;
    for (;;) {
        bool locked = false;
        std::int32_t cnr = 0;
        if (const int rc = session_->CheckLock(locked, cnr))
            return rc;
        if (locked) {
            msg_info("%s: %s locked kHz=%u after_ms=%lld cnr_x1000=%d\n", label_.c_str(), system, khz,
                     static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()),
                     cnr);
            return 0;
        }
        if (Clock::now() >= deadline)
            return -EAGAIN;
        if (removal_.Wait(10))
            return -ECANCELED;
    }
}

int PxS1udDevice::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    // 衛星を持たないため、方式の指定がなければ地デジとして換算する (カーネルの system_cap による判定と同じ)
    Tune tune;
    int rc = ChannelToTune(system == PTX_UNSPECIFIED_SYSTEM ? PTX_ISDB_T_SYSTEM : system, freq, tune);
    if (!rc && tune.satellite)
        rc = -EINVAL;
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc)
        rc = session_->Tune(siano::Session::System::Isdbt, tune.khz);
    if (!rc)
        rc = WaitLock(IsdbtLockTimeout, "ISDB-T", tune.khz);
    // 制御の失敗はこの open を失敗状態にする (Session が要求と応答の対応を保証できないため)。未ロックは選び直せる
    fatal = session_->Error() != 0;
    msg_info("%s: set channel freq_no=%d slot=%d -> kHz=%u result=%d\n", label_.c_str(), freq.freq_no, freq.slot,
             tune.khz, rc);
    return rc;
}

int PxS1udDevice::TuneParams(std::uint32_t system, std::uint32_t khz, bool streaming, bool &fatal)
{
    int rc = system == PTX_J83B_SYSTEM ? 0 : -EINVAL;
    if (!rc && streaming)
        rc = -EBUSY;
    // helper と J.83B 用の firmware を導入していない場合は、地デジの受信に影響させずに J.83B の選局だけを断る
    const auto firmware = LibDir() + "/s1ud_j83b.inp";
    if (!rc && access(helper_->Executable().c_str(), X_OK)) {
        msg_err("%s: J.83B helper is not installed: %s\n", label_.c_str(), helper_->Executable().c_str());
        rc = -ENOENT;
    }
    if (!rc && access(firmware.c_str(), R_OK)) {
        msg_err("%s: J.83B firmware is not installed: %s\n", label_.c_str(), firmware.c_str());
        rc = -ENOENT;
    }
    if (!rc)
        rc = session_->Tune(siano::Session::System::J83b, khz);
    if (!rc)
        rc = WaitLock(J83bLockTimeout, "J.83B", khz);
    // helper だけの失敗 (起動の失敗と異常終了) は Session を失敗状態にせず、次の選局で起動し直せる
    fatal = session_->Error() != 0;
    msg_info("%s: tune J.83B kHz=%u result=%d\n", label_.c_str(), khz, rc);
    return rc;
}

int PxS1udDevice::LockStatus(bool &locked)
{
    std::int32_t cnr = 0;
    return session_->CheckLock(locked, cnr);
}

int PxS1udDevice::ReadCn(std::uint32_t &raw)
{
    // 地デジは firmware の SNR、J.83B は helper の復調器の MER (どちらも dB×1000)。torne と同じく、
    // recpt1 などの地デジの換算式で同じ dB になる値へ換算する (ロックしていなければ式の下限)
    bool locked = false;
    std::int32_t cnr = 0;
    const int rc = session_->CheckLock(locked, cnr);
    if (rc)
        return rc;
    raw = DbToCndat(locked ? cnr / 1000.0 : 0);
    return 0;
}

int PxS1udDevice::StartStream(bool &fatal)
{
    const int rc = session_->SetCapture(true);
    fatal = session_->Error() != 0;
    return rc;
}

int PxS1udDevice::StopStream(bool &fatal)
{
    // 抜去の後は Session が -ECANCELED を返す。この値を受信中の異常として返すと、read が抜去の契約の EIO
    // ではなく ECANCELED になるため、EIO を返す。0 を返すと、VideoNode::Remove が EIO を設定する前
    // (続く OnRemove の CloseSession の間) に、待機中の read と poll の後の read が EOF を受け取る
    // (RTL-SDR の実機で起きた。docs/20261122_rtlsdr_linux_cuse_design.md)。
    // 配信の停止と Session の状態の破棄は、続く OnRemove の CloseSession が行う
    if (removal_.Removed()) {
        fatal = false;
        return -EIO;
    }
    const int rc = session_->SetCapture(false);
    fatal = rc != 0;
    return rc;
}

int PxS1udDevice::Send(const std::uint8_t *data, std::size_t size, siano::Clock::time_point deadline)
{
    ResubmitParked();
    if (removal_.Removed())
        return -ECANCELED;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - siano::Clock::now()).count();
    if (remaining <= 0)
        return -ETIMEDOUT;
    int actual = 0;
    const int rc = libusb_bulk_transfer(handle_, out_, const_cast<std::uint8_t *>(data), static_cast<int>(size),
                                        &actual, static_cast<unsigned>(remaining));
    if (!rc && static_cast<std::size_t>(actual) == size)
        return 0;
    msg_err("%s: USB write failed bytes=%zu actual=%d libusb=%s\n", label_.c_str(), size, actual,
            libusb_error_name(rc));
    if (rc == LIBUSB_ERROR_NO_DEVICE)
        return -ECANCELED;
    if (rc == LIBUSB_ERROR_TIMEOUT)
        return -ETIMEDOUT;
    return -EIO;
}

int PxS1udDevice::Delay(unsigned milliseconds)
{
    ResubmitParked();
    return removal_.Wait(milliseconds) ? -ECANCELED : 0;
}

int PxS1udDevice::ReadFirmware(FirmwareKind kind, std::vector<std::uint8_t> &firmware)
{
    if (kind == FirmwareKind::J83b) {
        const auto path = LibDir() + "/s1ud_j83b.inp";
        const int rc = ReadFile(path, firmware);
        if (rc)
            msg_err("%s: J.83B firmware read failed result=%d path=%s\n", label_.c_str(), rc, path.c_str());
        return rc;
    }
    // linux-firmware の isdbt_rio.inp を、カーネルの request_firmware() と同じ順に探す
    for (const char *path : {"/lib/firmware/updates/isdbt_rio.inp", "/lib/firmware/isdbt_rio.inp"}) {
        const int rc = ReadFile(path, firmware);
        if (rc == -ENOENT)
            continue;
        if (rc)
            msg_err("%s: firmware read failed result=%d path=%s\n", label_.c_str(), rc, path);
        return rc;
    }
    msg_warn("%s: isdbt_rio.inp was not found in /lib/firmware/updates or /lib/firmware\n", label_.c_str());
    return -ENOENT;
}

std::function<void(const std::uint8_t *, std::size_t)> PxS1udDevice::HelperWriter()
{
    return [this](const std::uint8_t *data, std::size_t size) { video_.Push(data, size); };
}

int PxS1udDevice::StartReceive()
{
    if (removal_.Removed())
        return -ECANCELED;
    if (leaked_)
        return -EIO;
    try {
        for (int i = 0; i < TransferCount; ++i) {
            auto t = std::make_unique<Transfer>();
            t->owner = this;
            t->buffer.resize(TransferSize);
            t->transfer = libusb_alloc_transfer(0);
            if (!t->transfer)
                throw std::bad_alloc();
            libusb_fill_bulk_transfer(t->transfer, handle_, in_, t->buffer.data(), static_cast<int>(TransferSize),
                                      BulkDone, t.get(), 0);
            transfers_.push_back(std::move(t));
        }
    } catch (...) {
        for (auto &t : transfers_)
            libusb_free_transfer(t->transfer);
        transfers_.clear();
        return -ENOMEM;
    }
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        parked_.clear();
        parked_any_ = false;
    }
    receive_failures_ = 0;
    resubmit_ = true;
    for (auto &t : transfers_) {
        t->generation = session_->Ticket();
        ++in_flight_;
        if (const int rc = libusb_submit_transfer(t->transfer)) {
            --in_flight_;
            msg_err("%s: bulk submit failed libusb=%s\n", label_.c_str(), libusb_error_name(rc));
            StopReceive();
            return rc == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO;
        }
    }
    return 0;
}

void PxS1udDevice::StopReceive()
{
    resubmit_ = false;
    for (auto &t : transfers_)
        libusb_cancel_transfer(t->transfer);
    {
        std::unique_lock<std::mutex> lock(receive_mutex_);
        if (!receive_changed_.wait_for(lock, std::chrono::seconds(5), [this] { return in_flight_ == 0; })) {
            // libusb が使い続ける可能性があるため、転送とバッファーを解放せずに残す
            msg_err("%s: %d bulk transfers did not complete\n", label_.c_str(), in_flight_.load());
            leaked_ = true;
            for (auto &t : transfers_)
                t.release();
            transfers_.clear();
            return;
        }
        parked_.clear();
        parked_any_ = false;
    }
    for (auto &t : transfers_)
        libusb_free_transfer(t->transfer);
    transfers_.clear();
    if (receive_failures_)
        msg_info("%s: bulk receive failures during firmware reload=%llu\n", label_.c_str(),
                 static_cast<unsigned long long>(receive_failures_));
}

void PxS1udDevice::ResubmitParked()
{
    if (!parked_any_)
        return;
    std::vector<Transfer *> parked;
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        parked.swap(parked_);
        parked_any_ = false;
    }
    if (parked.empty() || !resubmit_)
        return;
    // J.83B 用の firmware が ROM へ戻るときの受信の失敗 (WinUSB 版では ERROR_GEN_FAILURE) の後、
    // WinUSB 版が IN をリセットするのと同じく halt を解除してから受信を出し直す
    if (const int rc = libusb_clear_halt(handle_, in_))
        msg_warn("%s: clear halt failed in=0x%02x libusb=%s\n", label_.c_str(), in_, libusb_error_name(rc));
    for (auto *t : parked) {
        t->generation = session_->Ticket();
        ++in_flight_;
        if (const int rc = libusb_submit_transfer(t->transfer)) {
            --in_flight_;
            msg_err("%s: bulk resubmit failed libusb=%s\n", label_.c_str(), libusb_error_name(rc));
            session_->Fail(rc == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO);
            break;
        }
    }
    receive_changed_.notify_all();
}

void LIBUSB_CALL PxS1udDevice::BulkDone(libusb_transfer *transfer)
{
    auto &t = *static_cast<Transfer *>(transfer->user_data);
    auto &self = *t.owner;
    bool resubmit = false, parked = false;
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED: {
        // TS は CaptureGate を通ったものだけが VideoNode へ届く。VideoNode は START の前と STOP の後の TS を捨てる
        const auto received = self.session_->Receive(t.generation, transfer->buffer,
                                                     static_cast<std::size_t>(transfer->actual_length),
                                                     self.alignment_, [&self](const std::uint8_t *data, std::size_t size) {
                                                         self.video_.Push(data, size);
                                                         return true;
                                                     });
        resubmit = !received.error && !self.session_->Error();
        break;
    }
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        // 抜去。待機中の Exchange を起こし、受信を終える (デバイスファイルの扱いは Device::Remove が行う)
        self.session_->Interrupt();
        break;
    default:
        if (self.resubmit_ && self.session_->ReloadRecovery()) {
            // firmware の読み込み直しの間の失敗。制御のスレッドが halt を解除して出し直す
            ++self.receive_failures_;
            msg_warn("%s: bulk receive failed during firmware reload status=%d\n", self.label_.c_str(),
                     transfer->status);
            parked = true;
        } else if (self.resubmit_) {
            msg_err("%s: bulk receive failed status=%d\n", self.label_.c_str(), transfer->status);
            self.session_->Fail(-EIO);
        }
        break;
    }
    if (resubmit && self.resubmit_) {
        t.generation = self.session_->Ticket();
        const int rc = libusb_submit_transfer(transfer);
        if (!rc)
            return;
        msg_err("%s: bulk resubmit failed libusb=%s\n", self.label_.c_str(), libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_NO_DEVICE)
            self.session_->Interrupt();
        else
            self.session_->Fail(-EIO);
    }
    {
        std::lock_guard<std::mutex> lock(self.receive_mutex_);
        if (parked) {
            self.parked_.push_back(&t);
            self.parked_any_ = true;
        }
        --self.in_flight_;
    }
    self.receive_changed_.notify_all();
}
}
