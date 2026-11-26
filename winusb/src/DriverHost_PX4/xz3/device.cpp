// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "card_uart.hpp"
#include "pump_progress.hpp"
#include "session.hpp"

#include "../resource.h"
#include "../torne/winusb_control.hpp"
#include "../stream_overflow.hpp"
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
constexpr UCHAR BulkIn = 0x81;

Image Resource(int id)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto res = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res)
        return {};
    return {static_cast<const std::uint8_t*>(LockResource(LoadResource(module, res))),
            SizeofResource(module, res)};
}

Images LoadImages()
{
    return {Resource(IDR_XZ3_BOOTSTRAP), Resource(IDR_XZ3_RUNTIME), Resource(IDR_XZ3_HOOK)};
}

class BulkRead final : public BulkReader {
public:
    BulkRead(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed)
        : data_(PumpSize(0)), file_(file), usb_(usb), removed_(removed)
    {
        op_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    BulkRead(const BulkRead&) = delete;
    ~BulkRead() override
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
    bool Submit(unsigned stage) override
    {
        if (!op_.hEvent)
            return false;
        stage_ = stage;
        const auto event = op_.hEvent;
        op_ = {};
        op_.hEvent = event;
        ResetEvent(event);
        pending_ = WinUsb_ReadPipe(usb_, BulkIn, data_.data(), PumpSize(stage), nullptr, &op_) ||
                   GetLastError() == ERROR_IO_PENDING;
        return pending_;
    }

    bool Collect() override
    {
        HANDLE events[]{removed_, op_.hEvent};
        if (WaitForMultipleObjects(2, events, FALSE, 2000) != WAIT_OBJECT_0 + 1)
            return false;
        ULONG size = 0;
        const auto ok = WinUsb_GetOverlappedResult(usb_, &op_, &size, FALSE);
        pending_ = !ok && GetLastError() == ERROR_IO_INCOMPLETE;
        return ok && size == PumpSize(stage_);
    }

    unsigned Stage() const override { return stage_; }
    const std::uint8_t* Data() const override { return data_.data(); }

private:
    std::vector<std::uint8_t> data_;
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    HANDLE removed_;
    OVERLAPPED op_{};
    bool pending_ = false;
    unsigned stage_ = PumpStages - 1;
};

// 共有の Session へ渡す WinUSB の制御転送、Bulk IN、待機。
class WinUsbPort final : public SessionPort {
public:
    WinUsbPort(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed, const std::atomic_bool& available)
        : file_(file), usb_(usb), removed_(removed), available_(available), control_(file, usb, removed, io_) {}

    void Begin(Clock::time_point deadline) noexcept override { control_.Begin(deadline); }
    int Transfer(const px4::torne::MemoryRequest& request, std::uint8_t* data, std::size_t length,
                 std::size_t& transferred) override
    {
        return control_.Transfer(request, data, length, transferred);
    }
    int Pause(unsigned milliseconds) override { return control_.Pause(milliseconds); }
    bool Available() const noexcept override { return available_; }
    int WaitRemoved(unsigned milliseconds) override
    {
        return WaitForSingleObject(removed_, milliseconds) == WAIT_TIMEOUT ? 0 : -ENODEV;
    }
    void Nap() noexcept override { Sleep(1); }
    int FlushStream() override
    {
        if (WinUsb_FlushPipe(usb_, BulkIn))
            return 0;
        const auto error = GetLastError();
        return error ? static_cast<int>(error) : -EIO;
    }
    int ReadStream(std::uint8_t* data, std::size_t size, unsigned timeout_ms, std::size_t& transferred) override
    {
        transferred = 0;
        OVERLAPPED op{};
        op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!op.hEvent)
            return -ENOMEM;
        int result = 0;
        if (!WinUsb_ReadPipe(usb_, BulkIn, data, static_cast<ULONG>(size), nullptr, &op) &&
            GetLastError() != ERROR_IO_PENDING) {
            result = -EIO;
        } else {
            HANDLE events[]{removed_, op.hEvent};
            const auto wait = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
            if (wait != WAIT_OBJECT_0 + 1)
                CancelIoEx(file_, &op);
            // 取り消した場合も、完了を待ってから OVERLAPPED とバッファを手放す。
            ULONG done = 0;
            const bool ok = WinUsb_GetOverlappedResult(usb_, &op, &done, TRUE) != FALSE;
            const auto error = ok ? ERROR_SUCCESS : GetLastError();
            if (wait == WAIT_OBJECT_0)
                result = -ENODEV;
            else if (ok || error == ERROR_OPERATION_ABORTED)
                transferred = done;
            else
                result = -EIO;
        }
        CloseHandle(op.hEvent);
        return result;
    }
    std::unique_ptr<BulkReader> CreateReader() override
    {
        return std::make_unique<BulkRead>(file_, usb_, removed_);
    }

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    HANDLE removed_;
    const std::atomic_bool& available_;
    px4::torne::NativeControlIo io_;
    WinUsbControl control_;
};
}

class Device::Receiver final : public ReceiverBase, private StreamSink {
public:
    explicit Receiver(Device& parent)
        : ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE), parent_(parent),
          port_(parent.usb_dev_.dev, parent.usb_dev_.winusb, parent.removed_, parent.available_),
          session_(port_, LoadImages()) {}
    ~Receiver() override { Close(); }

    bool Valid() const { return session_.Valid(); }

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
        failed_ = capture_ = tuned_ = false;
        session_.ClearWorkerError();
        return 0;
    }

    void Close() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!open_)
            return;
        StopReader();
        if (parent_.available_) {
            const bool initialized = session_.Initialized();
            // DMA停止に失敗した場合はフックを残し、復元せずに電源リセットを要求する。
            restore_failed_ = session_.StreamStarted();
            if (!restore_failed_)
                restore_failed_ = !session_.Restore();
            if (restore_failed_)
                msg_err("GV-MVP/XZ3: RAM restoration failed; reconnect USB\n");
            else if (initialized || failed_)
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
        if (failed_ || session_.WorkerError())
            return -EIO;
        return tuned_ ? StartReader() : 0;
    }

    int CheckLock(bool& locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked = false;
        if (!open_ || !parent_.available_ || failed_ || session_.WorkerError())
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
        if (!open_ || !parent_.available_ || failed_ || session_.WorkerError())
            return -ECANCELED;
        // 選局後に取得し、TS workerが転送応答から更新した値を返す。
        value = session_.Cnr();
        return 0;
    }

    Session& GetSession() { return session_; }

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
        session_.ClearWorkerError();
        const auto tsid = satellite ? params_.stream_id : 0;
        bool init_failed = false;
        result = session_.Tune(satellite, params_.freq, tsid, init_failed);
        if (init_failed) {
            failed_ = true;
            return result;
        }
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
    int StartReader()
    {
        if (session_.Running())
            return session_.WorkerError() ? -EIO : 0;
        if (!stream_buf_->AdvanceEpoch())
            return -EIO;
        bool stop_failed = false;
        const int result = session_.StartStream(satellite_, *this, stop_failed);
        if (stop_failed)
            failed_ = true;
        return result;
    }

    int StopReader()
    {
        const int result = session_.StopStream();
        if (result)
            failed_ = true;
        return result;
    }

    void Started() override
    {
        // 選局ごとに作り直し、前のチャンネルで満杯のまま捨てたバイト数を次のチャンネルの記録へ持ち越さない。
        overflow_ = px4::StreamOverflowLog("GV-MVP/XZ3");
        stream_buf_->Start();
    }

    void PutPacket(const std::uint8_t* packet) override
    {
        std::size_t size = 188;
        const bool written = stream_buf_->Write(packet, size);
        overflow_.Record(written, 188, size);
    }

    void Flush() override { stream_buf_->NotifyWrite(); }

    Device& parent_;
    WinUsbPort port_;
    Session session_;
    // mutex_は受信機の状態を守る。Sessionの制御ロックより先に取得する。
    std::mutex mutex_;
    std::condition_variable closed_;
    px4::StreamOverflowLog overflow_{"GV-MVP/XZ3"};
    bool satellite_ = false;
    bool open_ = false, capture_ = false, tuned_ = false;
    bool failed_ = false, restore_failed_ = false;
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
    card_.Attach(receiver_->GetSession());
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
    // 実行中のカード操作がreceiver_を使い終わるまで待ち、受信途中のカード応答を破棄する。
    card_.Detach();
    if (receiver_) {
        receiver_->WaitClosed();
        receiver_.reset();
    }
}

int Device::OpenCard() { return card_.Open(); }
void Device::CloseCard() { card_.Close(); }
int Device::DetectCard(bool& detected) { return card_.Detect(detected); }
int Device::ResetCard() { return card_.Reset(); }
int Device::SetCardBaudrate(std::uint32_t baudrate) { return card_.SetBaudrate(baudrate); }
int Device::IsCardDataReady(bool& ready) { return card_.IsDataReady(ready); }
int Device::ReadCardData(std::uint8_t* buf, std::uint8_t& len) { return card_.Read(buf, len); }
int Device::WriteCardData(const std::uint8_t* buf, std::uint8_t len) { return card_.Write(buf, len); }

ReceiverBase* Device::GetReceiver(int id) const
{
    if (id)
        throw std::out_of_range("GV-MVP/XZ3 receiver index");
    return receiver_.get();
}

}
