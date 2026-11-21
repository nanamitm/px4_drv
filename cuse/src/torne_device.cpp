// SPDX-License-Identifier: GPL-2.0-only
#include "torne_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace px4::cuse {
namespace {
// Bulk IN の読み出し。USB の転送の境界は 192 バイトの単位と一致しないため、長さは 192 と 512 の公倍数 (1,536 の倍数)
// にする。地デジ (約 2.16 MB/秒) で 1 本が約 11 ms、16 本で約 180 ms 分を投入しておく。
// 機器内のリング (6 MiB、約 2.9 秒分) が USB の待ちを吸収するため、本数と長さは検証環境の測定で見直す。
constexpr std::size_t BulkCount = 16, BulkSize = 16 * 1536;
// 受信を始めてから最初の TS が届くまでの期限。WinUSB 版の StartReader と同じ 2 秒。
constexpr auto FirstTsTimeout = std::chrono::seconds(2);
// UHF 13〜62ch。WinUSB 版の Receiver::SetFrequency と同じ範囲で、CATV 帯は実機で確かめていないため受け付けない。
constexpr std::uint32_t MinKhz = 473143, MaxKhz = 767143;

std::uint64_t Micros(Device::Clock::duration d)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

// torne は USB のシリアル番号を持たないため、カードリーダーの ID は USB の接続位置 (バス番号とポートの経路) から求める。
// 同じポートへ挿し直すと同じ ID になる。Windows 版の ID (デバイスインスタンスのパスから求める) とは一致しない。
bool PortId(libusb_device_handle *handle, std::string &id)
{
    libusb_device *usb = libusb_get_device(handle);
    std::uint8_t ports[8];
    const int count = libusb_get_port_numbers(usb, ports, sizeof(ports));
    if (count <= 0)
        return false;
    std::string path = "054c:04b2@" + std::to_string(libusb_get_bus_number(usb)) + "-";
    for (int i = 0; i < count; ++i)
        path += (i ? "." : "") + std::to_string(ports[i]);
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : path) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX", static_cast<unsigned long long>(hash));
    id = out;
    return true;
}

// WinUSB 版の ExperimentalDevice::Init() と同じく、interface 0・alternate 0 と、使うエンドポイントの種類を確かめる。
bool CheckEndpoints(libusb_device_handle *handle)
{
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) || !config)
        return false;
    bool stream = false, command = false, answer = false;
    if (config->bNumInterfaces >= 1 && config->interface[0].num_altsetting >= 1) {
        const auto &setting = config->interface[0].altsetting[0];
        for (int i = 0; setting.bInterfaceNumber == 0 && i < setting.bNumEndpoints; ++i) {
            const auto &ep = setting.endpoint[i];
            const int type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
            stream |= ep.bEndpointAddress == 0x81 && type == LIBUSB_TRANSFER_TYPE_BULK && ep.wMaxPacketSize == 512;
            command |= ep.bEndpointAddress == 0x02 && type == LIBUSB_TRANSFER_TYPE_BULK;
            answer |= ep.bEndpointAddress == 0x83 && type == LIBUSB_TRANSFER_TYPE_INTERRUPT;
        }
    }
    libusb_free_config_descriptor(config);
    return stream && command && answer;
}
}

TorneDevice::TorneDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                         std::function<void()> changed)
    : Device(handle, bus, address, "CECH-ZD1J", "tornevideo", std::move(changed))
{
}

TorneDevice::~TorneDevice()
{
    StopSessions();
}

int TorneDevice::InitModel()
{
    if (!CheckEndpoints(handle_)) {
        msg_err("%s: unexpected USB interface or endpoints\n", label_.c_str());
        return -ENODEV;
    }
    if (!PortId(handle_, card_id_))
        msg_warn("%s: USB port path unavailable; card reader disabled\n", label_.c_str());
    return 0;
}

std::shared_ptr<TorneDevice::Shared> TorneDevice::AcquireSession()
{
    // 個体単位のブリッジを共有し、受信とカードがそれぞれ bootstrap しない。
    std::lock_guard<std::mutex> lock(shared_mutex_);
    if (!available_)
        return {};
    if (auto current = shared_.lock())
        return current;
    std::shared_ptr<Shared> created;
    try {
        created = std::make_shared<Shared>(handle_, removal_, label_);
    } catch (...) {
        return {};
    }
    // 配布既定値 (BulkI2c=true) と同じく、Bulk のコマンド経路で I2C を送る。
    const bool ok = created->session.InitializeBridge(true);
    created->port.ReportTiming("bootstrap");
    if (!ok || !available_)
        return {};
    shared_ = created;
    return created;
}

void TorneDevice::SleepFrontend()
{
    if (video_owner_ && available_)
        torne::SleepFrontend(video_owner_->session);
}

void TorneDevice::OnShutdown()
{
    SleepFrontend();
    video_owner_.reset();
}

int TorneDevice::VideoOpen()
{
    auto shared = AcquireSession();
    if (!shared)
        return -EIO;
    // 制御転送の失敗でセッションが無効になっていれば、受信を開くときに bootstrap から作り直す (WinUSB 版と同じ)。
    if (!shared->session.Recover(true)) {
        msg_err("%s: bridge reinitialization failed\n", label_.c_str());
        return -EIO;
    }
    // BulkI2c=true では、対応する書き込みを一括で送る (WinUSB 版の batch_i2c_writes || bulk_i2c)。
    const bool ok = shared->session.InitializeFrontend(false, true);
    shared->port.ReportTiming("frontend");
    video_owner_ = shared;
    if (!ok) {
        msg_err("%s: initialize failed (%s)\n", label_.c_str(), shared->session.FrontendError().c_str());
        SleepFrontend();
        video_owner_.reset();
        return -EIO;
    }
    msg_info("%s: open id=%s\n", label_.c_str(), card_id_.c_str());
    return 0;
}

void TorneDevice::VideoClose()
{
    // 復調器を sleep し、LED を消す。カードが開いていればブリッジは残す (WinUSB 版の Receiver::Close と同じ)。
    SleepFrontend();
    video_owner_.reset();
}

bool TorneDevice::AcceptsSystem(std::uint32_t system) const
{
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM;
}

int TorneDevice::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    Tune tune;
    int rc = ChannelToTune(system, freq, tune);
    if (!rc && (tune.satellite || tune.khz < MinKhz || tune.khz > MaxKhz))
        rc = -EINVAL;
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc && !video_owner_)
        rc = -EIO;
    if (!rc) {
        const auto begin = Clock::now();
        torne::TuneHooks hooks{[this] { return available_.load(); },
                               [this](unsigned ms) { return removal_.Wait(ms) == 0; }};
        rc = torne::TuneFrequency(video_owner_->session, card_id_, tune.khz, begin + torne::TuneBudget, false, false,
                                  hooks);
        video_owner_->port.ReportTiming("tune");
        msg_info("%s: tune kHz=%u result=%d elapsed_ms=%lld\n", label_.c_str(), tune.khz, rc,
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count()));
        // 確定した検索失敗と期限内のロックなしは無信号として、PIX-DT295 と同じ -ENOLINK を返し、次の選局を受け付ける。
        // それ以外 (I2C や USB の失敗) はセッションが無効になっているため、閉じて開き直すまで操作を断る。
        if (rc == -ENODATA || rc == -ETIMEDOUT)
            rc = -ENOLINK;
        else if (rc)
            fatal = true;
    }
    if (rc == -EINVAL)
        msg_info("%s: set channel freq_no=%d slot=%d rejected (UHF 13-62ch only)\n", label_.c_str(), freq.freq_no,
                 freq.slot);
    return rc;
}

int TorneDevice::LockStatus(bool &locked)
{
    std::vector<std::uint8_t> status;
    if (!video_owner_ || !video_owner_->session.ReadStatusWithin(status, std::chrono::seconds(1)))
        return -EIO;
    locked = torne::StatusLocked(status);
    return 0;
}

int TorneDevice::ReadCn(std::uint32_t &raw)
{
    std::vector<std::uint8_t> status;
    if (!video_owner_ || !video_owner_->session.ReadStatusWithin(status, std::chrono::seconds(1)))
        return -EIO;
    std::int32_t milli = 0;
    if (const int rc = torne::ReadStatusValue(status, torne::StatusReading::cnr, milli))
        return rc == -ENOSYS ? -EAGAIN : rc;
    raw = DbToCndat(milli / 1000.0);
    return 0;
}

void LIBUSB_CALL TorneDevice::BulkDone(libusb_transfer *transfer)
{
    auto &self = *static_cast<TorneDevice *>(transfer->user_data);
    auto &s = self.stream_;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        const auto now = Clock::now();
        if (s.previous_read != Clock::time_point{})
            s.max_read_gap_us = std::max(s.max_read_gap_us, Micros(now - s.previous_read));
        s.previous_read = now;
        if (transfer->actual_length && s.running) {
            const auto delivered = s.delivery->Deliver(transfer->buffer, static_cast<std::size_t>(transfer->actual_length),
                                                       [&self](const std::uint8_t *packets, std::size_t size) {
                                                           self.video_.Push(packets, size);
                                                       });
            if (delivered) {
                std::lock_guard<std::mutex> lock(s.mutex);
                if (!s.first) {
                    s.first = true;
                    s.first_at = now;
                    s.changed.notify_all();
                }
            }
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        // 切断では投入済みの転送がまとめて失敗するため、最初の 1 本だけを記録する。
        int expected = 0;
        const int error = transfer->status == LIBUSB_TRANSFER_NO_DEVICE ? -ENODEV : -EIO;
        if (s.error.compare_exchange_strong(expected, error))
            msg_err("%s: bulk status=%d\n", self.label_.c_str(), transfer->status);
    }
    if (s.running && !s.error && transfer->status == LIBUSB_TRANSFER_COMPLETED && !libusb_submit_transfer(transfer))
        return;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        --s.in_flight;
    }
    s.changed.notify_all();
    // Bulk IN の異常は受信の停止を待たずに利用者の read へ伝える。
    if (s.error)
        self.video_.Fail(s.error);
}

int TorneDevice::StartStream(bool &fatal)
{
    auto &s = stream_;
    if (!video_owner_)
        return -EIO;
    auto &session = video_owner_->session;
    try {
        s.delivery = std::make_unique<torne::TsDelivery>(false);
        s.buffers.assign(BulkCount, std::vector<std::uint8_t>(BulkSize));
    } catch (...) {
        return -ENOMEM;
    }
    s.error = 0;
    s.first = s.stop = false;
    s.previous_read = s.first_at = Clock::time_point{};
    s.max_read_gap_us = 0;
    // WinUSB 版の ReadLoop と同じ順序: 出力の切り替え、TS エンジンの準備、EP 0x81 のリセットと読み出しの投入、開始。
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    std::vector<std::uint8_t> response;
    if (!session.Command(0x12, {1}, response, deadline) || session.PrepareStream(deadline, false) != torne::StreamResult::ok) {
        msg_err("%s: stream prepare failed\n", label_.c_str());
        fatal = true;
        return -EIO;
    }
    if (const int rc = libusb_clear_halt(handle_, 0x81)) {
        msg_err("%s: clear halt failed rc=%d\n", label_.c_str(), rc);
        fatal = true;
        return -EIO;
    }
    s.running = true;
    for (auto &buffer : s.buffers) {
        auto *transfer = libusb_alloc_transfer(0);
        if (!transfer) {
            StopTransfers();
            fatal = true;
            return -ENOMEM;
        }
        libusb_fill_bulk_transfer(transfer, handle_, 0x81, buffer.data(), static_cast<int>(buffer.size()), BulkDone,
                                  this, 0);
        s.transfers.push_back(transfer);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.in_flight;
        }
        if (libusb_submit_transfer(transfer)) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                --s.in_flight;
            }
            StopTransfers();
            fatal = true;
            return -EIO;
        }
    }
    s.started = Clock::now();
    if (session.StartStream(deadline, false) != torne::StreamResult::ok) {
        msg_err("%s: stream start failed\n", label_.c_str());
        StopTransfers();
        fatal = true;
        return -EIO;
    }
    // PTX_START_STREAMING はカーネルモジュールと同じく最初の TS を待たずに返す。
    // 2 秒以内に TS が届かなければ、WinUSB 版の StartReader と同じく失敗として利用者の read を終える。
    try {
        s.watchdog = std::thread([this] {
            auto &s = stream_;
            std::unique_lock<std::mutex> lock(s.mutex);
            if (s.changed.wait_until(lock, s.started + FirstTsTimeout, [&s] { return s.first || s.stop; }))
                return;
            lock.unlock();
            msg_err("%s: no TS within %lld ms after the stream started\n", label_.c_str(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(FirstTsTimeout).count()));
            int expected = 0;
            s.error.compare_exchange_strong(expected, -EIO);
            video_.Fail(-EIO);
        });
    } catch (...) {
        StopTransfers();
        fatal = true;
        return -ENOMEM;
    }
    return 0;
}

int TorneDevice::StopTransfers()
{
    auto &s = stream_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stop = true;
    }
    s.changed.notify_all();
    if (s.watchdog.joinable())
        s.watchdog.join();
    s.running = false;
    for (auto *transfer : s.transfers)
        libusb_cancel_transfer(transfer);
    bool all_done = false;
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        all_done = s.changed.wait_for(lock, std::chrono::seconds(5), [&s] { return s.in_flight == 0; });
        if (!all_done)
            msg_err("%s: %d bulk transfers did not complete\n", label_.c_str(), s.in_flight);
    }
    // 完了しなかった転送は libusb が使い続ける可能性があるため、解放せずに残す。
    if (all_done)
        for (auto *transfer : s.transfers)
            libusb_free_transfer(transfer);
    s.transfers.clear();
    return all_done ? 0 : -EIO;
}

int TorneDevice::StopStream(bool &fatal)
{
    auto &s = stream_;
    const int collected = StopTransfers();
    // 読み出しを回収してから TS エンジンを止める。停止を確かめられなければバスを無効にし、
    // 次に開いたときに bootstrap から作り直す (WinUSB 版の StopReader と同じ)。
    bool stop_ok = true;
    if (available_ && video_owner_) {
        auto &session = video_owner_->session;
        stop_ok = !collected && session.StopStream(Clock::now() + std::chrono::seconds(2)) == torne::StreamResult::ok;
        std::vector<std::uint8_t> response;
        if (!stop_ok)
            session.Invalidate();
        else if (!session.Command(0x12, {0}, response, Clock::now() + std::chrono::seconds(1)))
            stop_ok = false;
        if (!stop_ok) {
            msg_warn("%s: stream stop failed; the device must be reopened\n", label_.c_str());
            fatal = true;
        }
    }
    const auto first_ms = s.first_at == Clock::time_point{} ? -1LL
        : static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(s.first_at - s.started).count());
    if (s.delivery) {
        const auto &framer = s.delivery->Framer();
        const auto &quality = s.delivery->Quality();
        msg_info("%s: stream end packets=%llu tei=%llu discarded=%llu cc=%llu malformed=%llu ring_overflow=%llu "
                 "max_read_gap_us=%llu first_ts_ms=%lld stop_ok=%d error=%d\n",
                 label_.c_str(), static_cast<unsigned long long>(framer.Packets()),
                 static_cast<unsigned long long>(framer.TeiPackets()), static_cast<unsigned long long>(framer.DiscardedBytes()),
                 static_cast<unsigned long long>(quality.cc_anomalies), static_cast<unsigned long long>(quality.malformed),
                 static_cast<unsigned long long>(video_.Overflow()), static_cast<unsigned long long>(s.max_read_gap_us),
                 first_ms, stop_ok, s.error.load());
    }
    // 回収できなかった転送の完了処理が使う可能性があるため、その場合は解放しない (受信は fatal で止まる)。
    if (!collected)
        s.delivery.reset();
    return s.error;
}

int TorneDevice::CardOpen()
{
    // カードだけを開いた場合も、ブリッジを bootstrap する。復調器、LED、TS は起動しない (WinUSB 版の OpenCard と同じ)。
    card_owner_ = AcquireSession();
    return card_owner_ ? 0 : -ENODEV;
}

int TorneDevice::CardDetect(bool &present)
{
    return card_owner_ ? card_state_.Detect(card_owner_->session, present) : -ENODEV;
}

int TorneDevice::CardReset()
{
    return card_owner_ ? card_state_.Reset(card_owner_->session) : -ENODEV;
}

int TorneDevice::CardSetBaudrate(std::uint32_t baudrate)
{
    return card_owner_ ? card_state_.SetBaudrate(card_owner_->session, baudrate) : -ENODEV;
}

int TorneDevice::CardDataReady(bool &ready)
{
    ready = false;
    return card_owner_ ? card_state_.DataReady(card_owner_->session, ready) : -ENODEV;
}

int TorneDevice::CardWrite(const std::uint8_t *data, std::size_t size)
{
    return card_owner_ ? card_state_.Write(card_owner_->session, data, size) : -ENODEV;
}

int TorneDevice::CardPowerDown()
{
    return card_owner_ && card_state_.PowerDown(card_owner_->session) ? 0 : -EIO;
}
}
