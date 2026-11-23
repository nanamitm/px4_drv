// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"
#include "capture_gate.hpp"
#include "firmware_path.hpp"
#include "iq_helper.hpp"
#include "session.hpp"
#include "ts_diagnostic_log.hpp"
#include "../stream_overflow.hpp"
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
int ReadFirmwareFile(const std::filesystem::path& path,std::vector<std::uint8_t>& firmware)
{
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f) return -ENOENT;
    auto n=f.tellg();
    if (n<=0 || n>4*1024*1024) return -EBADMSG;
    firmware.resize(static_cast<std::size_t>(n)); f.seekg(0);
    return f.read(reinterpret_cast<char*>(firmware.data()),n) ? 0 : -EIO;
}
}
// USB の転送と待機、配信のバッファー、firmware のファイルの探索を受け持ち、制御の順序は Session (session.cpp) が持つ。
class Receiver final : public ReceiverBase, private SessionPort {
public:
    // J.83B は、利用者が用意した IQ を送出する firmware と helper を指定したときだけ受け付ける
    struct J83bConfig {std::filesystem::path firmware,helper,log;};
    Receiver(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed,UCHAR in,std::size_t alignment,
             const std::filesystem::path& directory,const std::filesystem::path& firmware,const J83bConfig* j83b)
        : ReceiverBase(0),file_(file),usb_(usb),removed_(removed),in_(in),alignment_(alignment),
          host_directory_(directory),firmware_(firmware),
          session_(*this,j83b ? &iq_helper_ : nullptr,static_cast<unsigned>(TransferCount))
    {
        if (j83b) {j83b_=*j83b;iq_helper_.Configure(j83b_.helper.wstring(),j83b_.log.wstring());}
    }
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
        session_.Interrupt();
    }
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return -EBUSY;
        const int ret=session_.Open();
        if (ret) return ret;
        open_=true; capturing_=false; cnr_=0;
        return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        session_.Close();
        stream_buf_->Stop(); StopWorker(); open_=false; closed_.notify_all();
    }
    int SetLnbVoltage(std::int32_t voltage) override { return voltage ? -ENOSYS : 0; }
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (capture==capturing_) return capture ? -EALREADY : 0;
        if (capture) {
            if (!stream_buf_->Alloc(188*8192)) return -ENOMEM;
            stream_buf_->SetThresholdSize(188*32);
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->Start(); capturing_=true;
            return session_.SetCapture(true);
        }
        capturing_=false;
        const int ret=session_.SetCapture(false);
        stream_buf_->Stop();
        return ret;
    }
    int CheckLock(bool& locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked=false;
        if (!open_) return -EINVAL;
        return session_.CheckLock(locked,cnr_);
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
        const bool j83b=params_.system==SystemType::J83B;
        if (!open_) return -EINVAL;
        if (!j83b && (params_.system!=SystemType::ISDB_T || (params_.bandwidth && params_.bandwidth!=6))) return -EINVAL;
        return session_.Tune(j83b ? Session::System::J83b : Session::System::Isdbt,params_.freq);
    }
    int SetStreamId() override { return 0; }
    // J.83B は helper がブロック間の欠落の長さを推定できるまで復調を始めず、その後に等化器を収束させるため、
    // ロックまでの時間が既定の 1 秒を超えることがある。138.75 MHz と 132.25 MHz を交互に選局した 25 回
    // (firmware を読み込み済みの選局) では SetChannel が 141〜1,125 ms で、うち 3 回は SetFrequency の後に約 1 秒を
    // 要し、1 秒の上限では選局に失敗した。最長の約 2 倍を上限にする。搬送波のない行は、チャンネルスキャンで
    // この時間だけ待ってから失敗する
    std::uint32_t IsdbCLockWaitLimit() const override
    {
        return params_.system==SystemType::J83B ? 2000 : ReceiverBase::IsdbCLockWaitLimit();
    }
private:
    struct Read {
        OVERLAPPED op{};
        std::array<std::uint8_t,TransferSize> data{};
        bool pending=false;
        std::uint64_t generation=0;
    };
    using Reads=std::array<Read,TransferCount>;
    bool Removed() const override { return WaitForSingleObject(removed_,0)==WAIT_OBJECT_0; }
    int Delay(unsigned ms) override
    {
        return WaitForSingleObject(removed_,ms)==WAIT_TIMEOUT ? 0 : -ECANCELED;
    }
    int Send(const std::uint8_t* data,std::size_t size,Clock::time_point deadline) override
    {
        if (Removed()) return -ECANCELED;
        const auto left=deadline-Clock::now();
        const auto ms=left<=Clock::duration::zero() ? DWORD{0} :
            static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count());
        OVERLAPPED op{}; op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!op.hEvent) return -ENOMEM;
        ULONG bytes=0;
        BOOL ok=WinUsb_WritePipe(usb_,2,const_cast<PUCHAR>(data),static_cast<ULONG>(size),nullptr,&op);
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
        const int ret=ok ? (bytes==size ? 0 : -EIO) : UsbError(e);
        if (ret) msg_err("PX-S1UD: USB write failed bytes=%zu ret=%d win32=%lu\n",size,ret,e);
        return ret;
    }
    void Failed(int) override {stream_buf_->StopRequest();}
    bool AdvanceEpoch() override {return stream_buf_->AdvanceEpoch();}
    int ReadFirmware(FirmwareKind kind,std::vector<std::uint8_t>& firmware) override
    {
        if (kind==FirmwareKind::J83b) {
            // J.83B 用の firmware は J83BFirmwarePath で指定する
            const int ret=ReadFirmwareFile(j83b_.firmware,firmware);
            if (ret) msg_err("PX-S1UD: J.83B firmware read failed ret=%d firmware=%ls\n",ret,j83b_.firmware.c_str());
            return ret;
        }
        // 配置変更後の再オープンでも、INI 指定先・EXE 直下・配布用サブフォルダーを再探索する。
        const auto path=ResolveFirmwarePath(host_directory_,firmware_);
        const int ret=ReadFirmwareFile(path,firmware);
        if (ret==-ENOENT) {
            for (const auto& candidate:FirmwareCandidates(host_directory_,firmware_))
                msg_warn("PX-S1UD: firmware candidate: %ls\n",candidate.c_str());
        } else if (ret) {
            msg_err("PX-S1UD: firmware read failed ret=%d firmware=%ls\n",ret,path.c_str());
        }
        return ret;
    }
    std::function<void(const std::uint8_t*,std::size_t)> HelperWriter() override
    {
        // 記録は helper の起動ごとに作り、その起動の間だけ helper の読み取りスレッドが使う
        auto overflow=std::make_shared<px4::StreamOverflowLog>("PX-S1UD J.83B");
        return [this,overflow](const std::uint8_t* data,std::size_t size) {
            std::size_t bytes=size;
            const bool written=stream_buf_->Write(data,bytes);
            overflow->Record(written,size,bytes);
            stream_buf_->NotifyWrite();
        };
    }
    // 受信のスレッドの中 (Session::Receive の IQ の処理) からだけ呼ばれる
    unsigned PendingReads() override
    {
        unsigned outstanding=0;
        if (const auto* reads=reads_.load())
            for (const auto& q:*reads) if (q.pending && WaitForSingleObject(q.op.hEvent,0)!=WAIT_OBJECT_0) ++outstanding;
        return outstanding;
    }
    bool SignalLogEnabled() override {return (msg_get_mode() & (MSG_MODE_LOG_FILE | MSG_MODE_CONSOLE))!=0;}
    int StartReceive() override
    {
        if (Removed()) return -ECANCELED;
        if (worker_.joinable()) return -EALREADY;
        stop_=false;
        if (!WinUsb_ResetPipe(usb_,in_) || !WinUsb_ResetPipe(usb_,2) || !WinUsb_FlushPipe(usb_,in_)) {
            msg_err("PX-S1UD: pipe reset/flush failed in=0x%02x win32=%lu\n",in_,GetLastError());
            return -EIO;
        }
        try { worker_=std::thread([this]{Run();}); }
        catch (...) {stop_=true;return -ENOMEM;}
        return 0;
    }
    void StopReceive() override {StopWorker();}
    void StopWorker()
    {
        stop_=true;
        if (worker_.joinable()) { WinUsb_AbortPipe(usb_,in_); worker_.join(); }
    }
    void Run() noexcept
    {
        // J.83B では同じ PC で IQ の復調が CPU を使い続け、通常の優先度では受信要求の出し直しが最大 64 ms 遅れた。
        // チップの送信枠は約 7 ms 分しかないため、TS の配信スレッドと同じく優先度を上げて読み取りを途切れさせない
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        try { RunTransfers(); }
        catch (const std::bad_alloc&) { session_.Fail(-ENOMEM);msg_err("PX-S1UD: receive allocation failed\n"); }
        catch (const std::exception& e) { session_.Fail(-EIO);msg_err("PX-S1UD: receive exception: %s\n",e.what()); }
        catch (...) { session_.Fail(-EIO);msg_err("PX-S1UD: unknown receive exception\n"); }
    }
    void RunTransfers()
    {
        // 32要求は x86 の既定スタックを圧迫するためヒープへ確保する。
        auto reads=std::make_unique<Reads>();
        // 例外経路でも取消完了まで OVERLAPPED とバッファの寿命を保つ。
        struct Cleanup {
            Receiver& owner; Reads& reads;
            ~Cleanup() {
                owner.reads_=nullptr;
                for (auto& r:reads) {
                    if (r.pending) {CancelIoEx(owner.file_,&r.op);ULONG n=0;WinUsb_GetOverlappedResult(owner.usb_,&r.op,&n,TRUE);}
                    if (r.op.hEvent) CloseHandle(r.op.hEvent);
                }
            }
        } cleanup{*this,*reads};
        TsDiagnosticLog diagnostics("siano_usb_input");
        px4::StreamOverflowLog overflow("PX-S1UD");
        const bool diagnostic_enabled=(msg_get_mode() & (MSG_MODE_LOG_FILE | MSG_MODE_CONSOLE))!=0;
        diagnostics.Enable(diagnostic_enabled);
        diagnostics.SetReadyProbe([](void* context) noexcept {
            const auto& requests=*static_cast<Reads*>(context);
            int ready=0;
            for (const auto& request:requests)
                if (request.pending && WaitForSingleObject(request.op.hEvent,0)==WAIT_OBJECT_0) ++ready;
            return ready;
        },reads.get());
        TsTransferTracer tracer;
        auto last_submit=Clock::now();
        std::uint64_t last_gap_us=0;
        auto submit=[&](Read& r) {
            if (stop_ || Removed()) return false;
            ResetEvent(r.op.hEvent); r.generation=session_.Ticket();
            BOOL ok=WinUsb_ReadPipe(usb_,in_,r.data.data(),static_cast<ULONG>(r.data.size()),nullptr,&r.op);
            if (!ok && GetLastError()!=ERROR_IO_PENDING) {
                const auto e=GetLastError();session_.Fail(UsbError(e));
                msg_err("PX-S1UD: USB read submission failed endpoint=0x%02x win32=%lu\n",in_,e);
                return false;
            }
            if (diagnostic_enabled) {
                const auto now=Clock::now();
                last_gap_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-last_submit).count());
                last_submit=now;
            }
            r.pending=true;return true;
        };
        for (auto& r:*reads) {
            r.op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!r.op.hEvent) {session_.Fail(-ENOMEM);return;}
        }
        reads_=reads.get();
        for (auto& r:*reads) if (!submit(r)) return;
        std::size_t next=0;
        while (!stop_ && !Removed() && !session_.Error()) {
            auto& r=(*reads)[next];
            HANDLE events[]={removed_,r.op.hEvent};
            auto result=WaitForMultipleObjects(2,events,FALSE,100);
            if (result==WAIT_TIMEOUT) continue;
            if (result!=WAIT_OBJECT_0+1) {
                if (!stop_) {
                    session_.Fail(result==WAIT_OBJECT_0 ? -ECANCELED : -EIO);
                    msg_err("PX-S1UD: USB receive wait failed result=%lu win32=%lu\n",result,GetLastError());
                }
                break;
            }
            ULONG n=0;
            if (!WinUsb_GetOverlappedResult(usb_,&r.op,&n,FALSE)) {
                const auto e=GetLastError();
                // firmware の読み込み直しで J.83B 用の firmware が ROM へ戻るとき、受信が ERROR_GEN_FAILURE で失敗し、
                // パイプのリセットで残りの要求も取り消される。BonDriver_S1UD.dll と同じく IN をリセットして要求を出し直す。
                // 読み込み直しの間だけ、要求の数の 4 倍までに限る (Session::ReloadRecovery)
                if (!stop_ && (e==ERROR_GEN_FAILURE || e==ERROR_OPERATION_ABORTED) && session_.ReloadRecovery()) {
                    r.pending=false;
                    if (e==ERROR_GEN_FAILURE) {
                        msg_warn("PX-S1UD: USB read failed during firmware reload win32=%lu; resetting the IN pipe\n",e);
                        WinUsb_ResetPipe(usb_,in_);
                    }
                    if (!submit(r)) break;
                    next=(next+1)%TransferCount;
                    continue;
                }
                if (!stop_) {
                    session_.Fail(UsbError(e));
                    msg_err("PX-S1UD: USB read completion failed endpoint=0x%02x win32=%lu\n",in_,e);
                }
                break;
            }
            r.pending=false;
            // 異常の前後で転送単位の欠落・入替えを判別するため、完了順にメタデータを残す。
            TsTransferTrace* trace=nullptr;
            const auto anomalies=diagnostics.AnomalyCount();
            if (diagnostic_enabled) {
                TsTransferTrace t{};FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
                t.completed_at=(static_cast<std::uint64_t>(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
                t.slot=static_cast<unsigned>(next);t.bytes=n;trace=&tracer.Begin(t);
            }
            const auto received=session_.Receive(r.generation,r.data.data(),n,alignment_,
                [this,&diagnostics,&overflow,&r,&last_submit,&last_gap_us,diagnostic_enabled,trace](const std::uint8_t* data,std::size_t size) {
                    // 異常検出とメタデータだけを記録し、TS 本体と転送条件には触れない。
                    if (diagnostic_enabled) {
                        const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-last_submit).count());
                        diagnostics.Context((std::max)(elapsed,last_gap_us));
                        // framer の再同期で捨てたバイトは TS に残らないため、CC 異常と並べて記録する。
                        diagnostics.Source(session_.DiagnosticsInWrite().discarded);
                        diagnostics.Feed(data,size,r.generation);
                        if (trace && size>=188) {
                            const auto* last=data+(size/188-1)*188;
                            trace->packets=static_cast<unsigned>(size/188);
                            trace->first_pid=((data[1]&31)<<8)|data[2];trace->first_cc=data[3]&15;
                            trace->last_pid=((last[1]&31)<<8)|last[2];trace->last_cc=last[3]&15;
                        }
                    }
                    std::size_t bytes=size;
                    const bool written=stream_buf_->Write(data,bytes);
                    stream_buf_->NotifyWrite();
                    overflow.Record(written,size,bytes);
                    return true;
                });
            if (trace && received.parsed) {
                trace->frame_type=received.frame.type;trace->frame_length=received.frame.length;trace->frame_flags=received.frame.flags;
            }
            if (received.error) break;
            if (trace) tracer.End(diagnostics,diagnostics.AnomalyCount()!=anomalies,GetTickCount64());
            if (!submit(r)) break;
            next=(next+1)%TransferCount;
        }
        diagnostics.Reset();
        const auto framer=session_.Diagnostics();
        msg_info("PX-S1UD: RX stopped packets=%llu tei=%llu discarded=%llu malformed=%llu iq_malformed=%llu ret=%d\n",
            static_cast<unsigned long long>(framer.packets),static_cast<unsigned long long>(framer.tei),
            static_cast<unsigned long long>(framer.discarded),static_cast<unsigned long long>(session_.Malformed()),
            static_cast<unsigned long long>(session_.IqMalformed()),session_.Error());
    }
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_; UCHAR in_; std::size_t alignment_;
    std::filesystem::path host_directory_,firmware_;
    J83bConfig j83b_;
    // session_ より先に作り、後に壊す (session_ が helper を参照する)
    IqHelper iq_helper_;
    Session session_;
    std::mutex mutex_; std::condition_variable closed_;
    bool open_=false,capturing_=false;
    std::int32_t cnr_=0;
    std::atomic_bool stop_{true};
    // 受信のスレッドが持つ受信要求。IQ の欠落の切り分けで、受信のスレッドの中から数える
    std::atomic<Reads*> reads_{nullptr};
    std::thread worker_;
};

Device::Device(const std::wstring& path,const DeviceDefinition& definition,std::uintptr_t index,ReceiverManager& manager)
    : DeviceBase(path,definition,index,manager,true),index_(index)
{
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!removed_) throw DeviceError("PX-S1UD: CreateEventW failed");
}
Device::~Device() {SetAvailability(false);Term();if (removed_) CloseHandle(removed_);}
int Device::Init()
{
    if (usb_dev_.descriptor.idVendor!=0x3275 || usb_dev_.descriptor.idProduct!=0x0080) return -ENODEV;
    // J.83B は INI の System に書き、J83BFirmwarePath を指定した場合だけ受け付ける
    if (device_def_.receivers.size()!=1) return -EINVAL;
    const auto systems=device_def_.receivers[0].systems;
    const bool j83b=(systems&SystemType::J83B)==SystemType::J83B;
    if (systems!=SystemType::ISDB_T && systems!=(SystemType::ISDB_T|SystemType::J83B)) return -EINVAL;
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
    const auto directory=HostDirectory();
    const std::filesystem::path firmware=device_def_.configs.Get(L"FirmwarePath",L"");
    Receiver::J83bConfig j83b_config{};
    if (j83b) {
        const auto absolute=[&](std::filesystem::path p) {return (p.is_relative() ? directory/p : p).lexically_normal();};
        const std::filesystem::path j83b_firmware=device_def_.configs.Get(L"J83BFirmwarePath",L"");
        if (j83b_firmware.empty()) {msg_err("PX-S1UD: System includes J.83B but J83BFirmwarePath is not set\n");return -EINVAL;}
        j83b_config.firmware=absolute(j83b_firmware);
        j83b_config.helper=absolute(device_def_.configs.Get(L"HelperPath",L"msi2500\\msi2500_frontend.exe"));
        // ログ名は DriverHost 内の機器番号で区別する (MSi2500 と同じ)
        j83b_config.log=directory/(L"px-s1ud_j83b-"+std::to_wstring(index_)+L".log");
    }
    receiver_=std::make_unique<Receiver>(usb_dev_.dev,usb_dev_.winusb,removed_,in,packet-SMS_HEADER_SIZE,directory,firmware,
        j83b ? &j83b_config : nullptr);
    const auto& def=device_def_.receivers[0];command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,def.name.c_str());info.receiver_guid=def.guid;
    info.systems=systems;info.index=def.index;
    // USB初期化とファームウェア探索の待機中も、他の受信機を開けるようにする。
    if (!receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity(),true)) {receiver_.reset();return -EIO;}
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
