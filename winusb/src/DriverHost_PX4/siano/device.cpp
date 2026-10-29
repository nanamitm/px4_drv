// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"
#include "capture_gate.hpp"
#include "msg.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>

namespace px4::siano {
namespace {
using Clock=std::chrono::steady_clock;
// 上流 Windows と同じ 32 x 16 KiB。制御応答と TS は同じ bulk IN を通る。
constexpr std::size_t TransferSize=16384, TransferCount=32;
int UsbError(DWORD e)
{
    if (e==ERROR_DEVICE_NOT_CONNECTED || e==ERROR_OPERATION_ABORTED || e==ERROR_INVALID_HANDLE) return -ECANCELED;
    if (e==ERROR_TIMEOUT || e==ERROR_SEM_TIMEOUT) return -ETIMEDOUT;
    return -EIO;
}
std::filesystem::path HostDirectory()
{
    std::array<wchar_t,32768> path{};
    auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!n || n==path.size()) throw DeviceError("PX-S1UD: cannot resolve DriverHost directory");
    return std::filesystem::path(path.data()).parent_path();
}
}
class Receiver final : public ReceiverBase, private Io {
public:
    Receiver(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed,UCHAR in,std::size_t alignment,
             const std::filesystem::path& firmware)
        : ReceiverBase(0),file_(file),usb_(usb),removed_(removed),in_(in),alignment_(alignment),
          firmware_(firmware),frontend_(*this) {}
    ~Receiver() override
    {
        // ReceiverManager が貸し出した生ポインターの Close 完了まで寿命を保持する。
        std::unique_lock<std::mutex> lock(mutex_);
        closed_.wait(lock,[this]{return !open_;});
        StopWorker();
    }
    void Disconnected()
    {
        stream_buf_->StopRequest();
        responses_changed_.notify_all();
    }
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return -EBUSY;
        if (Removed()) return -ECANCELED;
        std::vector<std::uint8_t> firmware;
        std::ifstream f(firmware_,std::ios::binary|std::ios::ate);
        if (f) {
            auto n=f.tellg();
            if (n<=0 || n>4*1024*1024) return -EBADMSG;
            firmware.resize(static_cast<std::size_t>(n)); f.seekg(0);
            if (!f.read(reinterpret_cast<char*>(firmware.data()),n)) return -EIO;
        } else msg_warn("PX-S1UD: firmware not found: %ls; only an already initialized device can open\n",firmware_.c_str());
        int ret=StartWorker();
        if (!ret) ret=frontend_.Initialize(firmware);
        if (ret) {
            msg_err("PX-S1UD: initialization failed ret=%d firmware=%ls\n",ret,firmware_.c_str());
            StopWorker(); return ret;
        }
        open_=true; tuned_=false; capture_=false; locked_=false; cnr_=0;
        msg_info("PX-S1UD: opened firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
        return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        capture_=false; tuned_=false; DisableOutput();
        stream_buf_->Stop(); StopWorker(); open_=false; closed_.notify_all();
    }
    int SetLnbVoltage(std::int32_t voltage) override { return voltage ? -ENOSYS : 0; }
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (capture==capture_) return capture ? -EALREADY : 0;
        if (capture) {
            if (!stream_buf_->Alloc(188*8192)) return -ENOMEM;
            stream_buf_->SetThresholdSize(188*32);
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->Start(); capture_=true;
            if (tuned_ && locked_) EnableOutput();
        } else {
            capture_=false; DisableOutput(); stream_buf_->Stop();
        }
        return error_.load();
    }
    int CheckLock(bool& locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked=false;
        if (!open_) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        if (!tuned_) return 0;
        const auto now=Clock::now();
        if (now-last_stats_>=std::chrono::milliseconds(100)) {
            std::vector<std::uint8_t> p;
            int ret=frontend_.RequestStats(p);
            if (!ret) ret=frontend_.DecodeStats(p,locked_,cnr_);
            if (ret) {
                error_=ret;stream_buf_->StopRequest();
                msg_err("PX-S1UD: statistics failed ret=%d\n",ret);
                return ret;
            }
            last_stats_=Clock::now();
        }
        locked=locked_;
        if (locked && capture_ && !output_) EnableOutput();
        return 0;
    }
    int ReadStat(command::StatType type,std::int32_t& value) override
    {
        bool locked=false;
        int ret=CheckLock(locked);
        if (ret) return ret;
        std::lock_guard<std::mutex> lock(mutex_);
        value=0;
        if (type!=command::StatType::CNR) return -ENOSYS;
        value=locked ? cnr_ : 0; return 0;
    }
protected:
    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || params_.system!=SystemType::ISDB_T ||
            (params_.bandwidth && params_.bandwidth!=6)) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        DisableOutput(); tuned_=false; locked_=false; cnr_=0;
        // Siano の制御応答と TS は同じ bulk IN を通る。選局ごとの取消・flush は
        // メッセージ途中の USB パケットを残し、再開後に TS の端数をヘッダーとして
        // 読むことがある。受信要求を継続し、Gate の世代で選局前とロック前の TS を除く。
        if (!stream_buf_->AdvanceEpoch()) return -EIO;
        const int ret=frontend_.Tune(params_.freq);
        if (ret) {
            msg_err("PX-S1UD: tune failed frequency_khz=%u ret=%d\n",params_.freq,ret);
            error_=ret;stream_buf_->StopRequest();
            return ret;
        }
        tuned_=true; last_stats_=Clock::time_point{};
        return 0;
    }
    int SetStreamId() override { return 0; }
private:
    bool Removed() const { return WaitForSingleObject(removed_,0)==WAIT_OBJECT_0; }
    void DisableOutput()
    {
        // Feed/Write と同じロックで閉じ、呼び出しから戻った後の旧 TS 書込みを防ぐ。
        gate_.SetEnabled(false); output_=false;
    }
    void EnableOutput()
    {
        gate_.SetEnabled(true); output_=true;
    }
    int Delay(unsigned ms) override
    {
        return WaitForSingleObject(removed_,ms)==WAIT_TIMEOUT ? 0 : -ECANCELED;
    }
    int Exchange(const std::vector<std::uint8_t>& message,std::uint16_t response,
                 std::vector<std::uint8_t>& payload,unsigned ms) override
    {
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        const auto deadline=Clock::now()+std::chrono::milliseconds(ms);
        {
            std::lock_guard<std::mutex> lock(response_mutex_);
            expected_=response; received_=false; response_.clear();
        }
        OVERLAPPED op{}; op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!op.hEvent) return -ENOMEM;
        ULONG bytes=0;
        BOOL ok=WinUsb_WritePipe(usb_,2,const_cast<PUCHAR>(message.data()),static_cast<ULONG>(message.size()),nullptr,&op);
        DWORD e=ok ? ERROR_SUCCESS : GetLastError();
        if (ok || e==ERROR_IO_PENDING) {
            HANDLE events[]={removed_,op.hEvent};
            auto result=ok ? WAIT_OBJECT_0+1 : WaitForMultipleObjects(2,events,FALSE,ms);
            if (result!=WAIT_OBJECT_0+1) {
                CancelIoEx(file_,&op); WinUsb_GetOverlappedResult(usb_,&op,&bytes,TRUE);
                ok=FALSE; e=result==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
            } else {
                ok=WinUsb_GetOverlappedResult(usb_,&op,&bytes,FALSE);
                e=ok ? ERROR_SUCCESS : GetLastError();
            }
        }
        CloseHandle(op.hEvent);
        int ret=ok ? (bytes==message.size() ? 0 : -EIO) : UsbError(e);
        if (!ret && response) {
            std::unique_lock<std::mutex> lock(response_mutex_);
            if (!responses_changed_.wait_until(lock,deadline,[&]{return received_ || error_ || Removed();})) ret=-ETIMEDOUT;
            else if (Removed()) ret=-ECANCELED;
            else if (error_) ret=error_;
            else payload=response_;
            expected_=0;
        }
        if (ret) {
            // 通し番号のない応答を次の要求へ誤対応させないため、この open を失敗状態にする。
            error_=ret; stream_buf_->StopRequest();
            msg_err("PX-S1UD: command=%u response=%u failed ret=%d win32=%lu\n",sms_get_le16(message.data()),response,ret,e);
        }
        return ret;
    }
    int StartWorker()
    {
        if (Removed()) return -ECANCELED;
        if (worker_.joinable()) return -EALREADY;
        error_=0; stop_=false;
        if (!WinUsb_ResetPipe(usb_,in_) || !WinUsb_ResetPipe(usb_,2) || !WinUsb_FlushPipe(usb_,in_)) {
            msg_err("PX-S1UD: pipe reset/flush failed in=0x%02x win32=%lu\n",in_,GetLastError());
            return -EIO;
        }
        try { worker_=std::thread([this]{Run();}); }
        catch (...) {stop_=true;return -ENOMEM;}
        return 0;
    }
    void StopWorker()
    {
        stop_=true;
        if (worker_.joinable()) { WinUsb_AbortPipe(usb_,in_); worker_.join(); }
    }
    void Run() noexcept
    {
        try { RunTransfers(); }
        catch (const std::bad_alloc&) { error_=-ENOMEM;msg_err("PX-S1UD: receive allocation failed\n"); }
        catch (const std::exception& e) { error_=-EIO;msg_err("PX-S1UD: receive exception: %s\n",e.what()); }
        catch (...) { error_=-EIO;msg_err("PX-S1UD: unknown receive exception\n"); }
        if (error_) {stream_buf_->StopRequest();responses_changed_.notify_all();}
    }
    void RunTransfers()
    {
        struct Read {
            OVERLAPPED op{};
            std::array<std::uint8_t,TransferSize> data{};
            bool pending=false;
            std::uint64_t generation=0;
        };
        // 32要求は x86 の既定スタックを圧迫するためヒープへ確保する。
        auto reads=std::make_unique<std::array<Read,TransferCount>>();
        // 例外経路でも取消完了まで OVERLAPPED とバッファの寿命を保つ。
        struct Cleanup {
            Receiver& owner; std::array<Read,TransferCount>& reads;
            ~Cleanup() {
                for (auto& r:reads) {
                    if (r.pending) {CancelIoEx(owner.file_,&r.op);ULONG n=0;WinUsb_GetOverlappedResult(owner.usb_,&r.op,&n,TRUE);}
                    if (r.op.hEvent) CloseHandle(r.op.hEvent);
                }
            }
        } cleanup{*this,*reads};
        auto submit=[&](Read& r) {
            if (stop_ || Removed()) return false;
            ResetEvent(r.op.hEvent); r.generation=gate_.Ticket();
            BOOL ok=WinUsb_ReadPipe(usb_,in_,r.data.data(),static_cast<ULONG>(r.data.size()),nullptr,&r.op);
            if (!ok && GetLastError()!=ERROR_IO_PENDING) {
                const auto e=GetLastError();error_=UsbError(e);
                msg_err("PX-S1UD: USB read submission failed endpoint=0x%02x win32=%lu\n",in_,e);
                return false;
            }
            r.pending=true;return true;
        };
        for (auto& r:*reads) {
            r.op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!r.op.hEvent) {error_=-ENOMEM;return;}
            if (!submit(r)) return;
        }
        std::uint64_t malformed=0;
        std::size_t next=0;
        while (!stop_ && !Removed() && !error_) {
            auto& r=(*reads)[next];
            HANDLE events[]={removed_,r.op.hEvent};
            auto result=WaitForMultipleObjects(2,events,FALSE,100);
            if (result==WAIT_TIMEOUT) continue;
            if (result!=WAIT_OBJECT_0+1) {
                if (!stop_) {
                    error_=result==WAIT_OBJECT_0 ? -ECANCELED : -EIO;
                    msg_err("PX-S1UD: USB receive wait failed result=%lu win32=%lu\n",result,GetLastError());
                }
                break;
            }
            ULONG n=0;
            if (!WinUsb_GetOverlappedResult(usb_,&r.op,&n,FALSE)) {
                if (!stop_) {
                    const auto e=GetLastError();error_=UsbError(e);
                    msg_err("PX-S1UD: USB read completion failed endpoint=0x%02x win32=%lu\n",in_,e);
                }
                break;
            }
            r.pending=false;
            if (n) {
                sms_frame frame{};
                const auto parse=sms_frame_message(r.data.data(),n,alignment_,&frame);
                if (parse) {
                    ++malformed;error_=-EBADMSG;
                    msg_err("PX-S1UD: invalid USB frame bytes=%lu alignment=%zu ret=%d\n",n,alignment_,parse);
                    break;
                }
                const auto* p=r.data.data()+frame.payload_offset;
                if (frame.type==MSG_SMS_DVBT_BDA_DATA) {
                    if (!gate_.Feed(r.generation,p,frame.payload_length,[this](const std::uint8_t* data,std::size_t size) {
                        std::size_t bytes=size;
                        const bool written=stream_buf_->Write(data,bytes);
                        stream_buf_->NotifyWrite();
                        // Purge 中は true と短い長さで意図した破棄を返す。満杯の false と区別する。
                        if (!written) {
                            msg_err("PX-S1UD: TS buffer overflow bytes=%zu accepted=%zu\n",size,bytes);
                            return false;
                        }
                        return true;
                    })) error_=-ENOBUFS;
                } else {
                    std::lock_guard<std::mutex> lock(response_mutex_);
                    if (frame.type==expected_) {
                        response_.assign(p,p+frame.payload_length);received_=true;responses_changed_.notify_all();
                    }
                }
            }
            if (!submit(r)) break;
            next=(next+1)%TransferCount;
        }
        const auto& framer=gate_.Diagnostics();
        msg_info("PX-S1UD: RX stopped packets=%llu tei=%llu discarded=%llu malformed=%llu ret=%d\n",
            static_cast<unsigned long long>(framer.packets),static_cast<unsigned long long>(framer.tei),
            static_cast<unsigned long long>(framer.discarded),static_cast<unsigned long long>(malformed),error_.load());
    }
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_; UCHAR in_; std::size_t alignment_;
    std::filesystem::path firmware_; Frontend frontend_;
    std::mutex mutex_,response_mutex_; std::condition_variable closed_,responses_changed_;
    bool open_=false,capture_=false,tuned_=false,locked_=false;
    std::int32_t cnr_=0; Clock::time_point last_stats_{};
    std::atomic_bool stop_{true},output_{false}; std::atomic_int error_{0};
    CaptureGate gate_;
    std::uint16_t expected_=0; bool received_=false; std::vector<std::uint8_t> response_;
    std::thread worker_;
};

Device::Device(const std::wstring& path,const DeviceDefinition& definition,std::uintptr_t index,ReceiverManager& manager)
    : DeviceBase(path,definition,index,manager,true)
{
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!removed_) throw DeviceError("PX-S1UD: CreateEventW failed");
}
Device::~Device() {SetAvailability(false);Term();if (removed_) CloseHandle(removed_);}
int Device::Init()
{
    if (usb_dev_.descriptor.idVendor!=0x3275 || usb_dev_.descriptor.idProduct!=0x0080) return -ENODEV;
    if (device_def_.receivers.size()!=1 || device_def_.receivers[0].systems!=SystemType::ISDB_T) return -EINVAL;
    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (!WinUsb_QueryInterfaceSettings(usb_dev_.winusb,0,&descriptor)) return -EIO;
    UCHAR in=0; USHORT packet=0; bool out=false;
    for (UCHAR i=0;i<descriptor.bNumEndpoints;++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb_dev_.winusb,0,i,&pipe)) return -EIO;
        if (pipe.PipeType!=UsbdPipeTypeBulk) continue;
        if (pipe.PipeId&0x80) {if (in) return -ENOSYS;in=pipe.PipeId;packet=pipe.MaximumPacketSize;}
        else if (pipe.PipeId==2) out=true;
    }
    if (!in || !out || packet<=SMS_HEADER_SIZE) return -ENOSYS;
    // RAW_IO は使わず、短い応答で要求を完了させ、過剰分を次要求へ残す。
    BOOL disabled=FALSE;
    if (!WinUsb_SetPipePolicy(usb_dev_.winusb,in,RAW_IO,sizeof(disabled),&disabled) ||
        !WinUsb_SetPipePolicy(usb_dev_.winusb,in,IGNORE_SHORT_PACKETS,sizeof(disabled),&disabled)) return -EIO;
    auto firmware=HostDirectory()/L"firmware"/L"isdbt_rio.inp";
    if (device_def_.configs.Exists(L"FirmwarePath")) {
        firmware=device_def_.configs.Get(L"FirmwarePath");
        if (firmware.is_relative()) firmware=HostDirectory()/firmware;
    }
    receiver_=std::make_unique<Receiver>(usb_dev_.dev,usb_dev_.winusb,removed_,in,packet-SMS_HEADER_SIZE,firmware);
    const auto& def=device_def_.receivers[0];command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,def.name.c_str());info.receiver_guid=def.guid;
    info.systems=SystemType::ISDB_T;info.index=def.index;
    if (!receiver_manager_.Register(info,receiver_.get())) {receiver_.reset();return -EIO;}
    return 0;
}
void Device::Term()
{
    SetAvailability(false);
    if (receiver_) {receiver_manager_.Unregister(receiver_.get());receiver_.reset();}
}
void Device::SetAvailability(bool available)
{
    if (!available) {SetEvent(removed_);if (receiver_) receiver_->Disconnected();}
}
ReceiverBase* Device::GetReceiver(int id) const
{
    if (id!=0) throw std::out_of_range("PX-S1UD receiver index");return receiver_.get();
}
}
