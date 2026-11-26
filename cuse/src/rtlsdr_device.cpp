// SPDX-License-Identifier: GPL-2.0-only
#include "rtlsdr_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include "rtl-sdr.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unistd.h>

#ifndef PX4_CUSED_LIBDIR
#define PX4_CUSED_LIBDIR "/usr/local/lib/px4-cused"
#endif

namespace px4::cuse {
namespace {
// Bulk IN の転送。WinUSB 版の helper が librtlsdr の rtlsdr_read_async に渡す値 (既定の 15 本、256 KiB) と同じ
constexpr int TransferCount = 15;
constexpr std::size_t TransferSize = 262144;
constexpr unsigned char BulkIn = 0x81;
// WinUSB 版の helper と同じ標本化周波数。ワンセグの 1 セグメントの帯域 (約 430 kHz) を含む
constexpr std::uint32_t SampleRate = 1015873;
// 選局から最初の TS までの待機。WinUSB 版の x64 で 4.21〜5.11 秒 (初期窓 1200 シンボルの蓄積と初期の復号) で、
// ほかの機種の 3 秒では足りない。弱電界の再蓄積を含めた余裕を取り、WinUSB 版の TuneTimeout (20 秒) より短くする
constexpr auto FirstTsTimeout = std::chrono::seconds(15);

// helper の置き場所。試験では環境変数 PX4_CUSED_LIBDIR で導入前の場所を指定できる (PX-S1UD と同じ)
std::string LibDir()
{
    const char *dir = std::getenv("PX4_CUSED_LIBDIR");
    return dir && *dir ? dir : PX4_CUSED_LIBDIR;
}

// WinUSB 版の GainTenthsDb と同じ 0.1 dB 単位。既定 297 は WinUSB 版の検証機の 29.7 dB
int GainTenthsDb()
{
    const char *text = std::getenv("PX4_CUSED_RTLSDR_GAIN");
    if (!text || !*text)
        return 297;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (*end || value < -600 || value > 600) {
        msg_warn("RTL-SDR: PX4_CUSED_RTLSDR_GAIN=%s is not -600..600 tenths of a dB; using 297\n", text);
        return 297;
    }
    return static_cast<int>(value);
}
}

bool RtlSdrPortSelected(libusb_device *usb)
{
    return UsbPortListed(usb, "PX4_CUSED_RTLSDR_PORTS", "RTL-SDR");
}

RtlSdrDevice::RtlSdrDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                           std::function<void()> changed)
    : Device(handle, bus, address, "RTL-SDR", "rtlsdrvideo", std::move(changed)),
      helper_path_(LibDir() + "/rtlsdr_frontend"), gain_(GainTenthsDb()),
      session_(static_cast<rtlsdr::HelperPort &>(*this),
               [this](const std::uint8_t *data, std::size_t size, std::uint64_t) { video_.Push(data, size); })
{
}

RtlSdrDevice::~RtlSdrDevice()
{
    StopSessions();
    session_.Stop();
}

int RtlSdrDevice::InitModel()
{
    // librtlsdr と同じく interface 0 の Bulk IN 0x81 から I/Q を読む
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle_), &config))
        return -EIO;
    bool found = false;
    if (config->bNumInterfaces >= 1 && config->interface[0].num_altsetting >= 1) {
        const auto &alt = config->interface[0].altsetting[0];
        for (int e = 0; e < alt.bNumEndpoints; ++e)
            found |= alt.endpoint[e].bEndpointAddress == BulkIn &&
                     (alt.endpoint[e].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
    }
    libusb_free_config_descriptor(config);
    if (!found) {
        msg_err("%s: bulk IN 0x81 was not found\n", label_.c_str());
        return -ENOSYS;
    }
    // チューナーを確かめる。実機で確かめたのは R820T (R820T2 を含む) だけで、ほかのチューナーの利得の一覧と IF の設定は
    // 確かめていないため断る。確かめた後は librtlsdr の閉じる処理で RTL2832U を休止させる
    rtlsdr_dev_t *dev = nullptr;
    if (rtlsdr_open_handle(&dev, handle_) < 0 || !dev) {
        msg_err("%s: RTL2832U initialization failed\n", label_.c_str());
        return -EIO;
    }
    const auto tuner = rtlsdr_get_tuner_type(dev);
    char manufact[256] = {}, product[256] = {}, serial[256] = {};
    rtlsdr_get_usb_strings(dev, manufact, product, serial);
    rtlsdr_close(dev);
    if (tuner != RTLSDR_TUNER_R820T) {
        msg_err("%s: tuner type %d is not R820T; not supported\n", label_.c_str(), static_cast<int>(tuner));
        return -ENODEV;
    }
    msg_info("%s: %s %s serial=%s tuner=R820T gain_tenths_db=%d helper=%s\n", label_.c_str(), manufact, product, serial,
             gain_, helper_path_.c_str());
    return 0;
}

void RtlSdrDevice::OnRemove()
{
    // 起動し直しを止めてから helper と受信を止める。抜去の後の USB の操作は失敗で終わる
    session_.Disconnect();
    session_.Stop();
}

void RtlSdrDevice::OnShutdown()
{
    session_.Stop();
}

int RtlSdrDevice::VideoOpen()
{
    if (leaked_)
        return -EIO;
    if (access(helper_path_.c_str(), X_OK)) {
        msg_err("%s: helper is not installed: %s\n", label_.c_str(), helper_path_.c_str());
        return -ENOENT;
    }
    tuned_ = false;
    return 0;
}

void RtlSdrDevice::VideoClose()
{
    session_.Stop();
    tuned_ = false;
}

bool RtlSdrDevice::AcceptsSystem(std::uint32_t system) const
{
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM;
}

int RtlSdrDevice::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    // 制御転送の失敗も、WinUSB 版と同じく選局の失敗として返し、次の選局で RTL2832U を開き直す
    fatal = false;
    const auto started = Clock::now();
    Tune tune;
    std::uint32_t hz = 0;
    // 復調は UHF 13〜52ch (freq_no 63〜102) の 1/7 MHz のずれを含む格子だけを前提とするため、slot による補正も断る
    int rc = ChannelToTune(system == PTX_UNSPECIFIED_SYSTEM ? PTX_ISDB_T_SYSTEM : system, freq, tune);
    if (!rc && (tune.satellite || freq.freq_no < 63 || freq.freq_no > 102 || freq.slot != 0 ||
                !rtlsdr::HelperFrequency(tune.khz, hz)))
        rc = -EINVAL;
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc) {
        tuned_ = false;
        rc = session_.Start(hz);
    }
    // 最初の TS を待つ。成功はカーネルと同じくロックを意味し、届かなければ EAGAIN で選び直しを求める
    while (!rc && !session_.Locked()) {
        if (session_.Exhausted())
            rc = -EIO;
        else if (Clock::now() - started >= FirstTsTimeout)
            rc = -EAGAIN;
        else if (removal_.Wait(10))
            rc = -ECANCELED;
    }
    tuned_ = !rc || rc == -EAGAIN;
    msg_info("%s: set channel freq_no=%d slot=%d -> kHz=%u result=%d after_ms=%lld\n", label_.c_str(), freq.freq_no,
             freq.slot, tune.khz, rc,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()));
    return rc;
}

int RtlSdrDevice::LockStatus(bool &locked)
{
    locked = tuned_ && session_.Locked();
    return 0;
}

int RtlSdrDevice::ReadCn(std::uint32_t &raw)
{
    // helper の復調器の MER (dB×100) を、recpt1 などの地デジの換算式で同じ dB になる値へ換算する。
    // MER を受け取る前と TS が届いていない間は式の下限を返す (RS の成功割合は dB の量でないため使わない)
    const auto mer = session_.MerX100();
    raw = DbToCndat(tuned_ && session_.Locked() && mer >= 0 ? mer / 100.0 : 0);
    return 0;
}

int RtlSdrDevice::StartStream(bool &fatal)
{
    // helper は選局から閉じるまで動き続け、VideoNode は START の前と STOP の後の TS を捨てる
    fatal = false;
    return 0;
}

int RtlSdrDevice::StopStream(bool &fatal)
{
    fatal = false;
    // 抜去では VideoNode::Remove が受信を止めてから EIO を設定する。0 を返すと、その間に待機中の read と
    // poll の後の read が EOF を受け取るため (helper を止める OnRemove の間に実機で起きた)、抜去の契約の EIO を先に返す
    return removal_.Removed() ? -EIO : 0;
}

int RtlSdrDevice::Configure(std::uint32_t hz)
{
    rtlsdr_dev_t *dev = nullptr;
    if (rtlsdr_open_handle(&dev, handle_) < 0 || !dev) {
        msg_err("%s: RTL2832U initialization failed\n", label_.c_str());
        return removal_.Removed() ? -ECANCELED : -EIO;
    }
    rtl_ = dev;
    const auto fail = [this](const char *what, int value) {
        msg_err("%s: %s failed librtlsdr=%d\n", label_.c_str(), what, value);
        return removal_.Removed() ? -ECANCELED : -EIO;
    };
    if (rtlsdr_get_tuner_type(dev) != RTLSDR_TUNER_R820T)
        return fail("R820T check", static_cast<int>(rtlsdr_get_tuner_type(dev)));
    // WinUSB 版の helper と同じ順に設定する。標本化周波数の設定による再選局が 0 Hz を使わないよう、周波数を先に確定する
    if (const int r = rtlsdr_set_center_freq(dev, hz))
        return fail("tune", r);
    if (const int r = rtlsdr_set_sample_rate(dev, SampleRate))
        return fail("sample rate", r);
    const int n = rtlsdr_get_tuner_gains(dev, nullptr);
    if (n < 1 || n > 256)
        return fail("tuner gain list", n);
    std::vector<int> gains(static_cast<std::size_t>(n));
    if (rtlsdr_get_tuner_gains(dev, gains.data()) != n)
        return fail("tuner gain list", n);
    const int gain = *std::min_element(gains.begin(), gains.end(),
                                       [this](int a, int b) { return std::abs(a - gain_) < std::abs(b - gain_); });
    if (const int r = rtlsdr_set_tuner_gain_mode(dev, 1))
        return fail("manual gain", r);
    if (const int r = rtlsdr_set_tuner_gain(dev, gain))
        return fail("tuner gain", r);
    if (const int r = rtlsdr_reset_buffer(dev))
        return fail("reset buffer", r);
    msg_info("%s: frequency=%u rate=%u gain=%d\n", label_.c_str(), hz, SampleRate, gain);
    return 0;
}

void RtlSdrDevice::CloseTuner()
{
    if (!rtl_)
        return;
    // 受信を止めた後に RTL2832U を休止させる。ハンドルと interface はデーモンが持ち続ける
    rtlsdr_close(rtl_);
    rtl_ = nullptr;
}

int RtlSdrDevice::Launch(std::uint32_t hz, bool)
{
    if (removal_.Removed())
        return -ECANCELED;
    if (leaked_)
        return -EIO;
    usb_failed_ = false;
    int rc = Configure(hz);
    if (!rc)
        rc = helper_.Spawn({helper_path_, "--stdin", "--frequency", std::to_string(hz)});
    if (!rc) {
        rc = StartReceive();
        if (rc)
            helper_.Kill();
    }
    if (rc)
        CloseTuner();
    return rc;
}

int RtlSdrDevice::Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait)
{
    if (usb_failed_)
        return -EIO;
    return helper_.Read(buffer, size, wait);
}

void RtlSdrDevice::RequestStop()
{
    StopReceive();
    helper_.RequestStop();
}

void RtlSdrDevice::WaitStopped(std::chrono::milliseconds limit)
{
    helper_.WaitStopped(limit);
}

std::uint32_t RtlSdrDevice::Kill()
{
    StopReceive();
    const auto code = helper_.Kill();
    CloseTuner();
    return code;
}

void RtlSdrDevice::Release()
{
    helper_.Release();
    CloseTuner();
}

int RtlSdrDevice::StartReceive()
{
    try {
        for (int i = 0; i < TransferCount; ++i) {
            auto t = std::make_unique<Transfer>();
            t->state = receive_;
            t->buffer.resize(TransferSize);
            t->transfer = libusb_alloc_transfer(0);
            if (!t->transfer)
                throw std::bad_alloc();
            libusb_fill_bulk_transfer(t->transfer, handle_, BulkIn, t->buffer.data(), static_cast<int>(TransferSize),
                                      BulkDone, t.get(), 0);
            transfers_.push_back(std::move(t));
        }
    } catch (...) {
        transfers_.clear();
        return -ENOMEM;
    }

    int rc = 0;
    {
        std::lock_guard<std::mutex> lock(receive_->mutex);
        resubmit_ = true;
        for (auto &t : transfers_) {
            rc = libusb_submit_transfer(t->transfer);
            if (rc)
                break;
            t->in_flight = true;
            ++in_flight_;
        }
    }
    if (!rc)
        return 0;
    msg_err("%s: bulk submit failed libusb=%s\n", label_.c_str(), libusb_error_name(rc));
    StopReceive();
    return rc == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO;
}

void RtlSdrDevice::StopReceive()
{
    if (transfers_.empty())
        return;

    // 再投入の判定と投入を、停止の判定とキャンセルの間に挟ませない。
    std::unique_lock<std::mutex> lock(receive_->mutex);
    resubmit_ = false;
    for (auto &t : transfers_)
        if (t->in_flight)
            libusb_cancel_transfer(t->transfer);
    if (!receive_changed_.wait_for(lock, std::chrono::seconds(5), [this] { return in_flight_ == 0; })) {
        msg_err("%s: %d bulk transfers did not complete\n", label_.c_str(), in_flight_);
        leaked_ = true;
        receive_->owner = nullptr;
        // libusb が使い続ける転送は、遅延したコールバックから解放する。
        for (auto &t : transfers_)
            if (t->in_flight)
                t.release();
    }
    transfers_.clear();
}

void LIBUSB_CALL RtlSdrDevice::BulkDone(libusb_transfer *transfer)
{
    auto &t = *static_cast<Transfer *>(transfer->user_data);
    const auto state = t.state;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->owner) {
        delete &t;
        return;
    }

    auto &self = *state->owner;
    bool resubmit = false;
    // 停止後は I/Q の処理を省き、転送の回収だけを行う。
    const auto status = self.resubmit_ ? transfer->status : LIBUSB_TRANSFER_CANCELLED;
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
        // キューが満杯なら helper が失敗を返し、Session が受信ごと起動し直す。I/Q を捨てて続けることはしない
        if (transfer->actual_length > 0)
            self.helper_.Feed(transfer->buffer, static_cast<std::size_t>(transfer->actual_length));
        resubmit = true;
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        // 抜去。デバイスファイルの扱いは Device::Remove が行い、Session は抜去の後に起動し直さない
        self.usb_failed_ = true;
        break;
    default:
        if (!self.usb_failed_.exchange(true))
            msg_err("%s: bulk receive failed status=%d; restarting the helper\n", self.label_.c_str(), transfer->status);
        break;
    }
    if (resubmit) {
        const int rc = libusb_submit_transfer(transfer);
        if (!rc)
            return;
        msg_err("%s: bulk resubmit failed libusb=%s\n", self.label_.c_str(), libusb_error_name(rc));
        self.usb_failed_ = true;
    }
    t.in_flight = false;
    --self.in_flight_;
    self.receive_changed_.notify_all();
}
}
