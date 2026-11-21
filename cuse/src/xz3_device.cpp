// SPDX-License-Identifier: GPL-2.0-only
#include "xz3_device.hpp"
#include "channel.hpp"
#include "torne_port.hpp"
#include "xz3/pump_progress.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// 中継プログラム (winusb/pkg/DriverHost_PX4/firmware/xz3-*.bin)。src/firmware.S が埋め込む。
extern "C" const std::uint8_t px4_xz3_bootstrap[], px4_xz3_bootstrap_end[];
extern "C" const std::uint8_t px4_xz3_runtime[], px4_xz3_runtime_end[];
extern "C" const std::uint8_t px4_xz3_hook[], px4_xz3_hook_end[];

namespace px4::cuse {
namespace {
constexpr std::uint8_t BulkIn = 0x81;

xz3::Image Embedded(const std::uint8_t *begin, const std::uint8_t *end)
{
    return {begin, static_cast<std::size_t>(end - begin)};
}

// WinUSB 版の Device::Init() と同じく、interface 0・alternate 0 に Bulk IN (0x81) があることを確かめる。
bool CheckEndpoints(libusb_device_handle *handle)
{
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) || !config)
        return false;
    bool stream = false;
    if (config->bNumInterfaces >= 1 && config->interface[0].num_altsetting >= 1) {
        const auto &setting = config->interface[0].altsetting[0];
        for (int i = 0; setting.bInterfaceNumber == 0 && i < setting.bNumEndpoints; ++i) {
            const auto &ep = setting.endpoint[i];
            stream |= ep.bEndpointAddress == BulkIn &&
                      (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
        }
    }
    libusb_free_config_descriptor(config);
    return stream;
}

/*
 * 操作12 の送出量と同じ大きさの Bulk IN の読み出し 1 本 (WinUSB 版の BulkRead と同じ契約)。
 * 完了の通知は libusb のイベントのスレッドから届く。取り消しが期限内に完了しなければ、
 * libusb がバッファと転送を使い続ける可能性があるため、解放せずに残す。
 */
class LibusbRead final : public xz3::BulkReader {
public:
    LibusbRead(libusb_device_handle *handle, std::string label) : handle_(handle), label_(std::move(label))
    {
        slot_ = new Slot;
        slot_->buffer.resize(xz3::PumpSize(0));
        slot_->transfer = libusb_alloc_transfer(0);
    }
    LibusbRead(const LibusbRead &) = delete;
    ~LibusbRead() override
    {
        if (!slot_->transfer) {
            delete slot_;
            return;
        }
        bool done = true;
        {
            std::unique_lock<std::mutex> lock(slot_->mutex);
            if (slot_->pending) {
                libusb_cancel_transfer(slot_->transfer);
                done = slot_->changed.wait_for(lock, std::chrono::seconds(5), [this] { return !slot_->pending; });
            }
        }
        if (!done) {
            msg_err("%s: bulk read did not complete after cancel; leaking its buffer\n", label_.c_str());
            return;
        }
        libusb_free_transfer(slot_->transfer);
        delete slot_;
    }

    bool Submit(unsigned stage) override
    {
        if (!slot_->transfer)
            return false;
        stage_ = stage;
        std::lock_guard<std::mutex> lock(slot_->mutex);
        slot_->complete = false;
        libusb_fill_bulk_transfer(slot_->transfer, handle_, BulkIn, slot_->buffer.data(),
                                  static_cast<int>(xz3::PumpSize(stage)), Done, slot_, 0);
        slot_->pending = libusb_submit_transfer(slot_->transfer) == 0;
        return slot_->pending;
    }

    // WinUSB 版と同じく、完了を最長 2 秒待つ。切断では libusb が転送を失敗で完了させる。
    bool Collect() override
    {
        std::unique_lock<std::mutex> lock(slot_->mutex);
        if (!slot_->changed.wait_for(lock, std::chrono::seconds(2), [this] { return !slot_->pending; }))
            return false;
        return slot_->complete && slot_->status == LIBUSB_TRANSFER_COMPLETED &&
               static_cast<unsigned>(slot_->actual) == xz3::PumpSize(stage_);
    }

    unsigned Stage() const override { return stage_; }
    const std::uint8_t *Data() const override { return slot_->buffer.data(); }

private:
    struct Slot {
        libusb_transfer *transfer = nullptr;
        std::vector<std::uint8_t> buffer;
        std::mutex mutex;
        std::condition_variable changed;
        bool pending = false, complete = false;
        int status = 0, actual = 0;
    };

    static void LIBUSB_CALL Done(libusb_transfer *transfer)
    {
        auto &slot = *static_cast<Slot *>(transfer->user_data);
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.status = transfer->status;
            slot.actual = transfer->actual_length;
            slot.complete = true;
            slot.pending = false;
        }
        slot.changed.notify_all();
    }

    libusb_device_handle *handle_;
    const std::string label_;
    Slot *slot_;
    unsigned stage_ = xz3::PumpStages - 1;
};
}

// 共有の xz3::Session へ渡す libusb の制御転送、Bulk IN、待機。制御転送と期限は CECH-ZD1J の TornePort を使う。
class Xz3Device::Port final : public xz3::SessionPort {
public:
    Port(libusb_device_handle *handle, RemovalSignal &removal, const std::atomic_bool &available, const std::string &label)
        : handle_(handle), removal_(removal), available_(available), label_(label), control_(handle, removal, label) {}

    void Begin(xz3::Clock::time_point deadline) noexcept override { control_.Begin(deadline, false); }
    int Transfer(const torne::MemoryRequest &request, std::uint8_t *data, std::size_t length,
                 std::size_t &transferred) override
    {
        return control_.Transport().Transfer(request, data, length, transferred);
    }
    int Pause(unsigned milliseconds) override { return control_.Wait().Pause(milliseconds); }
    bool Available() const noexcept override { return available_ && !removal_.Removed(); }
    int WaitRemoved(unsigned milliseconds) override { return removal_.Wait(milliseconds); }
    // WinUSB 版の Sleep(1) と同じく約 1 ms。
    void Nap() noexcept override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    // WinUSB 版の WinUsb_FlushPipe は WinUSB がホストに溜めたデータを捨てる。libusb はホストにデータを溜めず、
    // 発行中の読み出しは BulkReader の破棄で取り消すため、捨てるものがない。
    // libusb_clear_halt は機器のエンドポイントの状態も変えるため、WinUSB 版にない操作として使わない。
    int FlushStream() override { return 0; }
    std::unique_ptr<xz3::BulkReader> CreateReader() override
    {
        try {
            return std::make_unique<LibusbRead>(handle_, label_);
        } catch (...) {
            return {};
        }
    }
    void ReportTiming(const char *stage) { control_.ReportTiming(stage); }

private:
    libusb_device_handle *handle_;
    RemovalSignal &removal_;
    const std::atomic_bool &available_;
    const std::string label_;
    TornePort control_;
};

Xz3Device::Xz3Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                     std::function<void()> changed)
    : Device(handle, bus, address, "GV-MVP/XZ3", "xz3video", std::move(changed))
{
}

Xz3Device::~Xz3Device()
{
    StopSessions();
    card_reader_.Detach();
}

int Xz3Device::InitModel()
{
    if (!CheckEndpoints(handle_)) {
        msg_err("%s: unexpected USB interface or endpoints\n", label_.c_str());
        return -ENODEV;
    }
    const xz3::Images images{Embedded(px4_xz3_bootstrap, px4_xz3_bootstrap_end),
                             Embedded(px4_xz3_runtime, px4_xz3_runtime_end), Embedded(px4_xz3_hook, px4_xz3_hook_end)};
    try {
        port_ = std::make_unique<Port>(handle_, removal_, available_, label_);
        session_ = std::make_unique<xz3::Session>(*port_, images);
        batch_.reserve(xz3::PumpSize(0) / 192 * 188);
    } catch (...) {
        return -ENOMEM;
    }
    if (!session_->Valid()) {
        msg_err("%s: embedded relay images are invalid\n", label_.c_str());
        return -EINVAL;
    }
    card_reader_.Attach(*session_);
    // GV-MVP/XZ3 は USB のシリアル番号を持たないため、カードリーダーの ID は USB の接続位置から求める。
    if (!PortCardId(handle_, "04bb:0530", card_id_))
        msg_warn("%s: USB port path unavailable; card reader disabled\n", label_.c_str());
    return 0;
}

// 受信を止めた後、中継プログラムを外して boot0 の RAM を元へ戻す (WinUSB 版の Receiver::Close と同じ)。
// DMA の停止を確かめられなかった場合は RAM を戻さず、USB を挿し直すまで受信を断る。
void Xz3Device::Restore()
{
    tuned_ = false;
    if (!available_ || !session_)
        return;
    const bool initialized = session_->Initialized();
    restore_failed_ = session_->StreamStarted();
    if (!restore_failed_)
        restore_failed_ = !session_->Restore();
    if (restore_failed_)
        msg_err("%s: RAM restoration failed; reconnect USB\n", label_.c_str());
    else if (initialized)
        msg_info("%s: boot RAM restored\n", label_.c_str());
}

void Xz3Device::OnShutdown()
{
    // デーモンを止めても、次に起動したデーモンが同じ RAM の照合で初期化できるよう、元へ戻す。
    Restore();
}

int Xz3Device::VideoOpen()
{
    if (!available_ || !session_)
        return -ENODEV;
    if (restore_failed_) {
        msg_err("%s: boot RAM was not restored; reconnect USB\n", label_.c_str());
        return -ENODEV;
    }
    tuned_ = false;
    session_->ClearWorkerError();
    return 0;
}

void Xz3Device::VideoClose()
{
    Restore();
}

bool Xz3Device::AcceptsSystem(std::uint32_t system) const
{
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM || system == PTX_ISDB_S_SYSTEM;
}

int Xz3Device::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    Tune tune;
    int rc = ChannelToTune(system, freq, tune);
    if (!rc && streaming)
        rc = -EBUSY;
    if (!rc) {
        tuned_ = false;
        session_->ClearWorkerError();
        bool init_failed = false;
        const auto begin = Clock::now();
        rc = session_->Tune(tune.satellite, tune.khz, tune.tsid, init_failed, tune.relative);
        port_->ReportTiming(init_failed ? "init" : "tune");
        // 引数不正、無信号 (-ENOLINK)、存在しない相対 TS 番号 (-EAGAIN) は機器の状態を壊さないため、次の選局を受け付ける。
        // 中継プログラムの導入の失敗と転送の異常は、閉じて RAM を戻すまで操作を断る (WinUSB 版の failed_ と同じ)。
        if (init_failed || (rc && rc != -EINVAL && rc != -ENOLINK && rc != -EAGAIN))
            fatal = true;
        if (!rc) {
            tuned_ = true;
            satellite_ = tune.satellite;
        }
        msg_info("%s: set channel freq_no=%d slot=%d -> %s%s kHz=%u tsid=%u result=%d elapsed_ms=%lld\n",
                 label_.c_str(), freq.freq_no, freq.slot, tune.satellite ? "ISDB-S" : "ISDB-T",
                 tune.relative ? " relative" : "", tune.khz, tune.tsid, rc,
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count()));
    }
    return rc;
}

int Xz3Device::LockStatus(bool &locked)
{
    // TS の転送中の制御転送は worker が担当するため、WinUSB 版と同じく最後の選局の結果を返す。
    locked = tuned_;
    return session_->WorkerError() ? -EIO : 0;
}

int Xz3Device::ReadCn(std::uint32_t &raw)
{
    if (!tuned_)
        return -EAGAIN;
    raw = session_->ReadCndat();
    return 0;
}

int Xz3Device::StartStream(bool &fatal)
{
    if (!tuned_)
        return -EIO;
    packets_ = 0;
    bool stop_failed = false;
    const int rc = session_->StartStream(satellite_, *this, stop_failed);
    if (rc) {
        msg_err("%s: stream start failed result=%d%s\n", label_.c_str(), rc, stop_failed ? " (stop failed)" : "");
        fatal = true;
    }
    return rc;
}

int Xz3Device::StopStream(bool &fatal)
{
    const int rc = session_->StopStream();
    if (rc) {
        msg_warn("%s: stream stop failed result=%d; the device must be reconnected\n", label_.c_str(), rc);
        fatal = true;
    }
    const int error = session_->WorkerError();
    msg_info("%s: stream end packets=%llu ring_overflow=%llu stop_ok=%d error=%d\n", label_.c_str(),
             static_cast<unsigned long long>(packets_.load()), static_cast<unsigned long long>(video_.Overflow()),
             rc == 0, error);
    return error ? -EIO : 0;
}

void Xz3Device::Started()
{
    batch_.clear();
}

void Xz3Device::PutPacket(const std::uint8_t *packet)
{
    batch_.insert(batch_.end(), packet, packet + 188);
}

void Xz3Device::Flush()
{
    if (batch_.empty())
        return;
    video_.Push(batch_.data(), batch_.size());
    packets_ += batch_.size() / 188;
    batch_.clear();
}

void Xz3Device::Failed(int error)
{
    // 受信の停止を待たずに利用者の read へ伝える。
    video_.Fail(error);
}

std::size_t Xz3Device::CardRead(std::uint8_t *data, std::size_t capacity)
{
    std::uint8_t length = static_cast<std::uint8_t>((std::min)(capacity, std::size_t{255}));
    return card_reader_.Read(data, length) ? 0 : length;
}

int Xz3Device::CardWrite(const std::uint8_t *data, std::size_t size)
{
    if (size > 255)
        return -EINVAL;
    return card_reader_.Write(data, static_cast<std::uint8_t>(size));
}
}
