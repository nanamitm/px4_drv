// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"
#include "plain_framer.hpp"
#include "pat_gate.hpp"
#include "msg.h"
#include <chrono>
#include <cmath>
#include <thread>
#include <condition_variable>
#include <array>
#include <algorithm>

namespace px4::pxbcud {
namespace {
using Clock=std::chrono::steady_clock;
class UsbIo final : public Io {
public:
    UsbIo(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed)
        : file_(file),usb_(usb),removed_(removed) {}
    void Begin(unsigned milliseconds) { deadline_=Clock::now()+std::chrono::milliseconds(milliseconds); }
    int Wait(unsigned milliseconds) override
    {
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-Clock::now()).count();
        if (remaining<=0) return -ETIMEDOUT;
        DWORD result=WaitForSingleObject(removed_,static_cast<DWORD>((std::min)(remaining,static_cast<long long>(milliseconds))));
        if (result==WAIT_OBJECT_0) return -ECANCELED;
        if (result!=WAIT_TIMEOUT) return -EIO;
        return Clock::now()<deadline_ ? 0 : -ETIMEDOUT;
    }
    int Transfer(bool input, std::uint8_t request, std::uint16_t index,
                 std::uint8_t *data, std::size_t size) override
    {
        if (WaitForSingleObject(removed_,0)==WAIT_OBJECT_0) return -ECANCELED;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-Clock::now()).count();
        if (remaining<=0) return -ETIMEDOUT;
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType=input?0xc0:0x40; setup.Request=request;
        setup.Index=index; setup.Length=static_cast<USHORT>(size);
        OVERLAPPED operation{};
        operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!operation.hEvent) return -ENOMEM;
        ULONG received=0;
        BOOL ok=WinUsb_ControlTransfer(usb_,setup,data,static_cast<ULONG>(size),nullptr,&operation);
        DWORD error=ok?ERROR_SUCCESS:GetLastError();
        if (ok || error==ERROR_IO_PENDING) {
            HANDLE events[]={removed_,operation.hEvent};
            DWORD result=ok?WAIT_OBJECT_0+1:WaitForMultipleObjects(2,events,FALSE,
                static_cast<DWORD>((std::min)(remaining,1000LL)));
            if (result!=WAIT_OBJECT_0+1) {
                // 取消後も OVERLAPPED と呼び出し側バッファの寿命を完了まで保つ。
                CancelIoEx(file_,&operation);
                WinUsb_GetOverlappedResult(usb_,&operation,&received,TRUE);
                error=result==WAIT_OBJECT_0?ERROR_OPERATION_ABORTED:
                    result==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_GEN_FAILURE;
                ok=FALSE;
            } else {
                ok=WinUsb_GetOverlappedResult(usb_,&operation,&received,FALSE);
                error=ok?ERROR_SUCCESS:GetLastError();
            }
        }
        CloseHandle(operation.hEvent);
        if (!ok) {
            msg_err("PX-BCUD: USB control failed input=%d request=0x%02x index=0x%04x length=%zu win32=%lu\n",
                    input,request,index,size,error);
            if (error==ERROR_OPERATION_ABORTED || error==ERROR_DEVICE_NOT_CONNECTED) return -ECANCELED;
            if (error==ERROR_TIMEOUT || error==ERROR_SEM_TIMEOUT) return -ETIMEDOUT;
            return -EIO;
        }
        return static_cast<int>(received);
    }
private:
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_;
    Clock::time_point deadline_{};
};
}

class Receiver final : public ReceiverBase {
public:
    Receiver(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed)
        : ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_AFTER_TUNE),
          file_(file),usb_(usb),removed_(removed),io_(file,usb,removed),frontend_(io_) {}
    ~Receiver() override
    {
        // ReceiverManager は生ポインターを貸し出すため、既存機種と同様 Close を待つ。
        std::unique_lock<std::mutex> lock(mutex_);
        closed_.wait(lock,[&]{return !open_;});
        if (worker_.joinable()) Stop();
    }
    void Disconnected() {stream_buf_->StopRequest();}
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return -EBUSY;
        if (WaitForSingleObject(removed_,0)==WAIT_OBJECT_0) return -ECANCELED;
        io_.Begin(8000);
        const int ret=frontend_.Initialize();
        if (ret) {
            msg_err("PX-BCUD: frontend initialization failed ret=%d tuner_id=0x%02x\n",ret,frontend_.TunerId());
            io_.Begin(1000); frontend_.Capture(false);
            return ret;
        }
        open_=true;
        msg_info("PX-BCUD: tuner ID=0x%02x\n",frontend_.TunerId());
        worker_error_=0;
        return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        Stop();
        io_.Begin(2000);
        const int ret=frontend_.Sleep();
        if (ret) msg_err("PX-BCUD: sleep failed ret=%d\n",ret);
        open_=false; closed_.notify_all();
    }
    int CheckLock(bool &locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked=false;
        if (!open_) return -EINVAL;
        if (worker_error_.load()) return -EIO;
        io_.Begin(1000);
        return frontend_.Locked(locked);
    }
    int SetLnbVoltage(std::int32_t voltage) override
    {
        // メーカー公式FAQでアンテナ給電非対応と明記されているため、外部給電を前提とする。
        if (voltage) { msg_err("PX-BCUD: antenna power supply is not supported; use external LNB power; requested=%d\n",voltage); return -ENOSYS; }
        return 0;
    }
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return -EINVAL;
        if (!capture) return Stop();
        if (capture_requested_) return -EALREADY;
        capture_requested_=true;
        const int ret=Start();
        if (ret) {capture_requested_=false;stream_buf_->Stop();}
        return ret;
    }
    int ReadStat(command::StatType type,std::int32_t &value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value=0;
        if (!open_) return -EINVAL;
        if (worker_error_.load()) return -EIO;
        if (type!=command::StatType::CNR) return -ENOSYS;
        std::uint16_t cn=0;
        io_.Begin(1000);
        int ret=frontend_.ReadCn(cn);
        if (!ret && cn>=3000) {
            const double p=std::sqrt(cn-3000)/64;
            const double cnr=((((-1634.6*p+14341)*p-50259)*p+88977)*p-89565)*p+58857;
            value=static_cast<std::int32_t>((std::max)(0.0,cnr));
        }
        return ret;
    }
protected:
    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || params_.system!=SystemType::ISDB_S) return -EINVAL;
        resume_capture_=capture_requested_;
        int ret=Stop(false);
        if (ret) return ret;
        worker_error_=0;
        // 選局中の旧 TS と端数は worker の終了で破棄し、新しい epoch へ進める。
        if (!stream_buf_->AdvanceEpoch()) return -EIO;
        io_.Begin(5000);
        ret=frontend_.Tune(params_.freq);
        if (ret) msg_err("PX-BCUD: tune failed frequency_khz=%u ret=%d\n",params_.freq,ret);
        return ret;
    }
    int SetStreamId() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return -EINVAL;
        io_.Begin(2000);
        int ret=frontend_.SelectStream(params_.stream_id);
        if (ret) msg_err("PX-BCUD: stream selection failed stream_id=%u ret=%d\n",params_.stream_id,ret);
        if (!ret && resume_capture_) ret=Start(false);
        resume_capture_=false;
        return ret;
    }
private:
    int Start(bool initialize_buffer=true)
    {
        if (initialize_buffer) {
            if (!stream_buf_->Alloc(188*8192)) return -ENOMEM;
            stream_buf_->SetThresholdSize(188*32);
            // 生 TS を維持し、PID 0x1fff を含む全パケットを配信する。
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->Start();
        }
        // BonDriver は Open で capture を要求し、その後に初回選局する。
        // 配信接続は準備するが、未選局中の USB 受信とタイムアウトは開始しない。
        if (!frontend_.SelectedStream()) return 0;
        // 実測 PX-BCUD rev0205 は alt1 / EP84 / 940byte / interval1 の isoch。
        // 記述子を照合してから帯域を予約し、停止後は alt0 に戻す。
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (!WinUsb_QueryInterfaceSettings(usb_,1,&descriptor)) return -EIO;
        bool found=false;
        for (UCHAR i=0;i<descriptor.bNumEndpoints;++i) {
            WINUSB_PIPE_INFORMATION_EX pipe{};
            if (!WinUsb_QueryPipeEx(usb_,1,i,&pipe)) return -EIO;
            if (pipe.PipeId==0x84 && pipe.PipeType==UsbdPipeTypeIsochronous &&
                pipe.Interval==1 && pipe.MaximumBytesPerInterval==940) found=true;
        }
        if (!found) {
            msg_err("PX-BCUD: expected alt1 EP84 isoch interval1 940-byte pipe was not found\n");
            return -ENOSYS;
        }
        // 選局による一時停止では配信中のリングを再確保せず、読取接続を保持する。
        if (!WinUsb_SetCurrentAlternateSetting(usb_,1)) return -EIO;
        stop_=false; worker_error_=0;
        io_.Begin(1000);
        int ret=frontend_.Capture(true);
        Snapshot();
        if (ret) {
            if (initialize_buffer) stream_buf_->Stop();
            io_.Begin(1000); frontend_.Capture(false);
            WinUsb_SetCurrentAlternateSetting(usb_,0);
            return ret;
        }
        try { worker_=std::thread([this]{Run();}); }
        catch (...) {
            stop_=true;
            if (initialize_buffer) stream_buf_->Stop();
            io_.Begin(1000); frontend_.Capture(false);
            WinUsb_SetCurrentAlternateSetting(usb_,0);
            return -ENOMEM;
        }
        return 0;
    }
    int Stop(bool stop_buffer=true)
    {
        stop_=true;
        if (worker_.joinable()) {
            // 未完了 read の投入との競合も worker の短い待機周期で回収する。
            WinUsb_AbortPipe(usb_,0x84);
            worker_.join();
        }
        // 選局中の停止は USB に限定する。Stop() で配信側を起こすと読取接続が
        // 終了し、再開後は誰も消費しないリングが満杯になる。
        if (stop_buffer) {stream_buf_->Stop();capture_requested_=false;}
        io_.Begin(1000);
        if (open_) Snapshot();
        const int ret=frontend_.Capture(false);
        if (!WinUsb_SetCurrentAlternateSetting(usb_,0) && !ret) return -EIO;
        return ret;
    }
    void Run() noexcept
    {
        try { RunStream(); }
        catch (...) { worker_error_=ERROR_NOT_ENOUGH_MEMORY; }
        if (worker_error_) {
            msg_err("PX-BCUD: TS worker failed win32=%lu\n",worker_error_.load());
            // USB 受信の異常終了は CheckLock/ReadStat で失敗として返すが、
            // データ接続は次の明示的な選局まで待機させる。StopRequest で
            // 配信スレッドまで終了すると、Start(false) では復帰できない。
            // 抜去時の停止要求は Disconnected、接続終了時は StreamServer が行う。
        }
    }
    void Snapshot()
    {
        std::uint8_t values[9]{};
        int ret=0;
        const std::uint8_t bridge[]={0x0c,0x0f,0x5f,0x80};
        const std::uint8_t demod[]={0xc3,0xe6,0xe7,0x1c,0x1f};
        for (unsigned i=0;i<4 && !ret;++i) ret=frontend_.ReadRegister(bridge[i],values[i]);
        for (unsigned i=0;i<5 && !ret;++i) ret=frontend_.ReadDemodRegister(demod[i],values[i+4]);
        msg_info("PX-BCUD: state ret=%d bridge[0c,0f,5f,80]=%02x,%02x,%02x,%02x demod[c3,e6,e7,1c,1f]=%02x,%02x,%02x,%02x,%02x\n",
            ret,values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7],values[8]);
    }
    void RunStream()
    {
        // Linux em28xx と同じ 5 URB x 64 microframe。WinUSB はフレーム境界の
        // 倍数を要求するため、interval1 の 8 microframe ごとに区切る。
        struct Read {
            HANDLE file=nullptr;
            WINUSB_INTERFACE_HANDLE usb=nullptr;
            OVERLAPPED operation{};
            std::vector<std::uint8_t> data=std::vector<std::uint8_t>(940*64);
            std::array<USBD_ISO_PACKET_DESCRIPTOR,64> packets{};
            WINUSB_ISOCH_BUFFER_HANDLE buffer=nullptr;
            bool pending=false;
            ~Read() {
                // Feed のメモリー確保が例外になっても未完了 USB I/O を回収する。
                if (pending) {
                    CancelIoEx(file,&operation);
                    ULONG ignored=0;
                    WinUsb_GetOverlappedResult(usb,&operation,&ignored,TRUE);
                }
                if (buffer) WinUsb_UnregisterIsochBuffer(buffer);
                if (operation.hEvent) CloseHandle(operation.hEvent);
            }
        };
        std::array<Read,5> reads;
        PlainFramer framer;
        PatGate gate(frontend_.SelectedStream());
        std::uint64_t startup_discarded=0;
        std::uint64_t iso_errors=0, empty_packets=0;
        std::vector<std::uint8_t> output;
        bool first=true;
        const auto started=Clock::now();
        std::uint64_t submitted=0, completed=0;
        // 同じ Win32 エラーでも投入失敗と完了失敗では原因が異なるため、
        // エラー取得直後に処理段階とキューの状態を保存して記録する。
        auto fail=[&](const char *stage, std::size_t index, DWORD error) {
            worker_error_=error;
            unsigned pending=0;
            for (const auto &read:reads) if (read.pending) ++pending;
            msg_err("PX-BCUD: USB stream failure stage=%s read=%u win32=%lu elapsed_ms=%llu submitted=%llu completed=%llu pending=%u continue_stream=%u\n",
                stage,static_cast<unsigned>(index),error,
                static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()),
                static_cast<unsigned long long>(submitted),static_cast<unsigned long long>(completed),pending,first?0u:1u);
        };
        auto submit=[&](Read &r) {
            ResetEvent(r.operation.hEvent);
            r.packets={};
            BOOL ok=WinUsb_ReadIsochPipeAsap(r.buffer,0,static_cast<ULONG>(r.data.size()),
                first?FALSE:TRUE,static_cast<ULONG>(r.packets.size()),r.packets.data(),&r.operation);
            const DWORD error=ok?ERROR_SUCCESS:GetLastError();
            if (!ok && error!=ERROR_IO_PENDING) {fail("submit",&r-reads.data(),error);return false;}
            ++submitted;
            first=false;r.pending=true; return true;
        };
        for (auto &r:reads) {
            r.file=file_; r.usb=usb_;
            r.operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!r.operation.hEvent) {fail("create_event",&r-reads.data(),GetLastError());break;}
            if (!WinUsb_RegisterIsochBuffer(usb_,0x84,r.data.data(),static_cast<ULONG>(r.data.size()),&r.buffer)) {
                fail("register_buffer",&r-reads.data(),GetLastError());break;
            }
            if (stop_ || !submit(r)) break;
        }
        std::size_t next=0;
        auto last_data=Clock::now();
        while (!stop_ && !worker_error_) {
            if (Clock::now()-last_data>std::chrono::seconds(5)) {fail("no_data",next,ERROR_TIMEOUT);break;}
            if (!gate.Ready() && Clock::now()-started>std::chrono::seconds(5)) {
                fail("pat_timeout",next,ERROR_INVALID_DATA);
                msg_err("PX-BCUD: selected TSID PAT was not received tsid=0x%04x\n",frontend_.SelectedStream());
                break;
            }
            Read &r=reads[next];
            HANDLE events[]={removed_,r.operation.hEvent};
            const DWORD wait=WaitForMultipleObjects(2,events,FALSE,50);
            if (wait==WAIT_OBJECT_0) {fail("device_removed",next,ERROR_DEVICE_NOT_CONNECTED);break;}
            if (wait==WAIT_TIMEOUT) {
                if (Clock::now()-last_data>std::chrono::seconds(5)) fail("no_data",next,ERROR_TIMEOUT);
                continue;
            }
            if (wait!=WAIT_OBJECT_0+1) {fail("wait",next,GetLastError());break;}
            ULONG size=0;
            if (!WinUsb_GetOverlappedResult(usb_,&r.operation,&size,FALSE)) {
                const DWORD error=GetLastError();
                if (!stop_) fail("complete",next,error);
                break;
            }
            r.pending=false;
            ++completed;
            output.clear();
            for (const auto &packet:r.packets) {
                if (packet.Status) {++iso_errors;framer.Gap();continue;}
                if (!packet.Length) {++empty_packets;continue;}
                if (packet.Offset>r.data.size() || packet.Length>r.data.size()-packet.Offset || packet.Length>940) {
                    msg_err("PX-BCUD: invalid isoch descriptor packet=%u status=0x%08lx offset=%lu length=%lu buffer_bytes=%u\n",
                        static_cast<unsigned>(&packet-r.packets.data()),packet.Status,packet.Offset,packet.Length,static_cast<unsigned>(r.data.size()));
                    fail("packet_descriptor",next,ERROR_INVALID_DATA);break;
                }
                last_data=Clock::now();
                framer.Feed(r.data.data()+packet.Offset,packet.Length,output);
            }
            if (worker_error_) break;
            if (!output.empty()) {
                for (std::size_t offset=0;offset<output.size();offset+=188) {
                    if(!gate.Accept(output.data()+offset)){++startup_discarded;continue;}
                    std::size_t written=188;
                    if (!stream_buf_->Write(output.data()+offset,written) || written!=188) {
                        fail("stream_buffer_write",next,ERROR_BUFFER_OVERFLOW);break;
                    }
                }
                stream_buf_->NotifyWrite();
            }
            if (stop_ || !submit(r)) break;
            next=(next+1)%reads.size();
        }
        for (auto &r:reads) {
            if (r.pending) {
                CancelIoEx(file_,&r.operation);
                ULONG ignored=0;
                WinUsb_GetOverlappedResult(usb_,&r.operation,&ignored,TRUE);
                r.pending=false;
            }
        }
        msg_info("PX-BCUD: TS stopped packets=%llu tei=%llu discarded_bytes=%llu win32=%lu iso_errors=%llu empty_iso_packets=%llu startup_discarded_packets=%llu\n",
            static_cast<unsigned long long>(framer.packets),static_cast<unsigned long long>(framer.tei),
            static_cast<unsigned long long>(framer.discarded),worker_error_.load(),
            static_cast<unsigned long long>(iso_errors),static_cast<unsigned long long>(empty_packets),
            static_cast<unsigned long long>(startup_discarded));
    }
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_;
    UsbIo io_; Frontend frontend_;
    std::mutex mutex_; std::condition_variable closed_;
    bool open_=false, resume_capture_=false,capture_requested_=false;
    std::atomic_bool stop_{true};
    std::atomic<DWORD> worker_error_{0};
    std::thread worker_;
};

Device::Device(const std::wstring &path,const DeviceDefinition &definition,std::uintptr_t index,ReceiverManager &manager)
    : DeviceBase(path,definition,index,manager)
{
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!removed_) throw DeviceError("PX-BCUD: CreateEventW failed");
}
Device::~Device() { SetAvailability(false); Term(); if (removed_) CloseHandle(removed_); }
int Device::Init()
{
    if (!available_) return -ECANCELED;
    if (receiver_) return -EALREADY;
    if (usb_dev_.descriptor.idVendor!=0x3275 || usb_dev_.descriptor.idProduct!=0x0085) return -ENODEV;
    if (device_def_.receivers.size()!=1 || device_def_.receivers[0].systems!=SystemType::ISDB_S) return -EINVAL;
    receiver_=std::make_unique<Receiver>(usb_dev_.dev,usb_dev_.winusb,removed_);
    const auto &definition=device_def_.receivers[0];
    command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str()); info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,definition.name.c_str()); info.receiver_guid=definition.guid;
    info.systems=SystemType::ISDB_S; info.index=definition.index;
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
    available_=available;
    if (!available) SetEvent(removed_);
    if (!available && receiver_) receiver_->Disconnected();
}
ReceiverBase *Device::GetReceiver(int id) const
{
    if (id!=0) throw std::out_of_range("PX-BCUD receiver index");
    return receiver_.get();
}
}
