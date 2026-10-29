// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"
#include "plain_framer.hpp"
#include "pat_gate.hpp"
#include "recovery_budget.hpp"
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
// isoch 要求数の既定と範囲。1要求は 64マイクロフレーム = 8ms のため、
// 要求数がそのまま受信スレッドの遅れを吸収できる時間になる。
// Linux em28xx と同じ5要求 (40ms) では、完了待ちの実測が 33.2ms・39.4ms・41.9ms と
// 余裕と同じ桁に並び、超えた時点で isoch が枯れていた。カーネルは完了コールバックで
// 再投入するため5で足りるが、ユーザー空間のスレッドが再投入するこちらでは足りない。
// 10要求 (80ms) は実測の停滞 30〜42ms に対して倍以上の余裕があり、
// 追加で確保するバッファは 60,160バイト x 5 = 約300KB。
constexpr unsigned PXBCUD_DEFAULT_REQUESTS=10;
constexpr unsigned PXBCUD_MIN_REQUESTS=2;
constexpr unsigned PXBCUD_MAX_REQUESTS=32;
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
    Receiver(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed,bool auto_recovery,std::size_t requests)
        : ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_AFTER_TUNE),
          file_(file),usb_(usb),removed_(removed),io_(file,usb,removed),frontend_(io_),
          auto_recovery_(auto_recovery),requests_(requests) {}
    ~Receiver() override
    {
        // ReceiverManager は生ポインターを貸し出すため、既存機種と同様 Close を待つ。
        std::unique_lock<std::mutex> lock(mutex_);
        closed_.wait(lock,[&]{return !open_;});
        if (worker_.joinable()) Stop();
    }
    void Disconnected() {stream_buf_->StopRequest();}
    DWORD LastStreamError() const noexcept {return terminal_error_.load();}
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
        worker_error_=0;terminal_error_=0;
        recovery_count_=0;
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
        if (worker_error_.load() || recovering_.load()) return -EIO;
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
        if (worker_error_.load() || recovering_.load()) return -EIO;
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
        stop_=false; worker_error_=0; terminal_error_=0; recovering_=false;
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
        if (ret) msg_err("PX-BCUD: capture stop failed ret=%d\n",ret);
        if (!WinUsb_SetCurrentAlternateSetting(usb_,0)) {
            const DWORD error=GetLastError();
            msg_err("PX-BCUD: stop alternate setting failed win32=%lu capture_ret=%d\n",error,ret);
            if (!ret) return -EIO;
        }
        return ret;
    }
    void Run() noexcept
    {
        // アイソクロナス転送はフレーム番号を追い越されると転送全体が
        // USBD_STATUS_ISO_NOT_ACCESSED_BY_HW で失敗し、再投入では取り返せない。
        // 5要求 x 64マイクロフレーム = 40ms より長く受信スレッドが実行されないと
        // この状態に入るため、計算負荷と同じ優先度では高負荷時に維持できない。
        // 実測 (3Dスライサーで全論理プロセッサを占有、各4回) では base 8 で失敗4回と
        // 復旧予算切れによる停止、base 10 で失敗1回、base 13 で0回だった。
        // NORMAL_PRIORITY_CLASS のスレッドで 13 を超える値は TIME_CRITICAL になる。
        // 投入の遅れが欠落にならないバルク転送の他機種は、この引き上げの対象外。
        if (!SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL))
            msg_warn("PX-BCUD: TS worker priority raise failed win32=%lu\n",GetLastError());
        RecoveryBudget budget;
        Clock::time_point outage{};
        bool have_outage=false;
        while (!stop_) {
            const auto stream_begin=Clock::now();
            try { RunStream(have_outage?&outage:nullptr); }
            catch (...) { failure_time_=Clock::now();worker_error_=ERROR_NOT_ENOUGH_MEMORY; }
            const DWORD error=worker_error_.load();
            if (!error || stop_ || WaitForSingleObject(removed_,0)==WAIT_OBJECT_0) break;
            // 復旧が実って長く受信できていれば、それまでの連続失敗を忘れる。
            // 高負荷で時々失敗する状態と、復旧しても受信が続かない状態を分ける。
            budget.NoteStream(Clock::now()-stream_begin);
            if (!have_outage || !recovering_) {outage=failure_time_;have_outage=true;}
            const bool retryable=error==ERROR_GEN_FAILURE || error==ERROR_INVALID_PARAMETER ||
                error==ERROR_TIMEOUT || error==ERROR_SEM_TIMEOUT;
            if (!auto_recovery_ || !retryable) break;
            if (!budget.Take()) {
                msg_err("PX-BCUD: auto recovery exhausted limit=%u healthy_run_seconds=%lld last_win32=%lu recovery_count=%llu\n",
                    static_cast<unsigned>(RecoveryBudget::Limit),
                    static_cast<long long>(RecoveryBudget::HealthyRun.count()),
                    error,static_cast<unsigned long long>(recovery_count_));
                break;
            }
            // 実らない再試行を詰めて繰り返すと、同じブリッジの制御転送を占有し続ける。
            // ロックを取る前に待ち、停止要求と抜去の監視を止めない。
            const auto backoff=budget.Backoff();
            if (backoff.count()) {
                if (WaitForSingleObject(removed_,static_cast<DWORD>(backoff.count()))==WAIT_OBJECT_0) break;
                if (stop_) break;
            }
            recovering_=true;
            // Stop は mutex_ を持って join する。ここで無条件に lock すると
            // 相互待機になるため、競合中も stop_ を監視して終了要求を優先する。
            std::unique_lock<std::mutex> lock(mutex_,std::defer_lock);
            while (!lock.try_lock() && !stop_ && WaitForSingleObject(removed_,10)==WAIT_TIMEOUT) {}
            if (!lock.owns_lock() || stop_ || WaitForSingleObject(removed_,0)==WAIT_OBJECT_0) break;
            ++recovery_count_;
            msg_warn("PX-BCUD: auto recovery begin count=%llu win32=%lu consecutive=%u backoff_ms=%lld possible_ts_loss=1\n",
                static_cast<unsigned long long>(recovery_count_),error,
                static_cast<unsigned>(budget.Consecutive()),static_cast<long long>(backoff.count()));
            // RunStream の終了時点で全 isoch I/O と登録バッファを回収済み。
            // 選局は変えず、配信リングと epoch/CC を維持して欠落を隠さない。
            io_.Begin(1000);
            int ret=frontend_.Capture(false);
            if (!WinUsb_SetCurrentAlternateSetting(usb_,0) && !ret) ret=-EIO;
            if (!ret && !stop_ && WaitForSingleObject(removed_,0)==WAIT_TIMEOUT) {
                if (!WinUsb_SetCurrentAlternateSetting(usb_,1)) ret=-EIO;
                else {io_.Begin(1000);ret=frontend_.Capture(true);}
            } else if (!ret) ret=-ECANCELED;
            if (ret) {
                msg_err("PX-BCUD: auto recovery restart failed ret=%d original_win32=%lu count=%llu\n",
                    ret,error,static_cast<unsigned long long>(recovery_count_));
                io_.Begin(1000);frontend_.Capture(false);
                WinUsb_SetCurrentAlternateSetting(usb_,0);
                break;
            }
            worker_error_=0;
        }
        recovering_=false;
        if (worker_error_) {
            msg_err("PX-BCUD: TS worker failed win32=%lu\n",worker_error_.load());
            // USB 受信の異常終了は CheckLock/ReadStat で失敗として返すが、
            // データ接続は次の明示的な選局まで待機させる。StopRequest で
            // 配信スレッドまで終了すると、Start(false) では復帰できない。
            // 抜去時の停止要求は Disconnected、接続終了時は StreamServer が行う。
        }
        // 調査ツールは診断出力とI/O回収が終わった後にだけ停止処理へ進む。
        terminal_error_=worker_error_.load();
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
    void RunStream(const Clock::time_point *outage)
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
            std::size_t trace_slot=0;
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
        // 要求数は実測で決める。5要求 (40ms) では、優先度引き上げと書込みまとめの後も
        // 完了待ちの最大が40msへ張り付いて isoch が枯れたため、設定で増やせるようにする。
        const std::size_t request_count=requests_;
        const auto reads_storage=std::make_unique<Read[]>(request_count);
        Read *const reads=reads_storage.get();
        PlainFramer framer;
        PatGate gate(frontend_.SelectedStream());
        std::uint64_t startup_discarded=0;
        std::uint64_t iso_errors=0, empty_packets=0;
        std::vector<std::uint8_t> output;
        bool first=true;
        bool reported_recovery=false;
        const auto started=Clock::now();
        msg_info("PX-BCUD: timing start pid=%lu tid=%lu priority=%d requests=%u microframes_per_request=64 nominal_queue_us=%u\n",
            GetCurrentProcessId(),GetCurrentThreadId(),GetThreadPriority(GetCurrentThread()),
            static_cast<unsigned>(request_count),static_cast<unsigned>(request_count*64*125));
        std::uint64_t submitted=0, completed=0;
        struct Trace {
            std::uint64_t sequence=0, submitted_us=0, completed_us=0;
            std::uint64_t submit_begin_us=0, wait_begin_us=0, wake_us=0;
            std::uint64_t framing_end_us=0, write_end_us=0, requeue_gap_us=0;
            std::uint64_t framing_cycles=0, write_cycles=0;
            unsigned index=0;
            DWORD error=0;
        };
        std::uint64_t max_wait_us=0,max_framing_us=0,max_write_us=0,max_submit_us=0,max_requeue_gap_us=0;
        // 正常受信時は直近32要求の時刻だけを保持し、ディスクへは出力しない。
        std::array<Trace,32> trace{};
        auto elapsed_us=[&] {return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started).count());};
        // 同じ Win32 エラーでも投入失敗と完了失敗では原因が異なるため、
        // エラー取得直後に処理段階とキューの状態を保存して記録する。
        auto fail=[&](const char *stage, std::size_t index, DWORD error) {
            failure_time_=Clock::now();
            worker_error_=error;
            unsigned pending=0;
            for (std::size_t i=0;i<request_count;++i) if (reads[i].pending) ++pending;
            msg_err("PX-BCUD: USB stream failure stage=%s read=%u win32=%lu elapsed_ms=%llu submitted=%llu completed=%llu pending=%u continue_stream=%u\n",
                stage,static_cast<unsigned>(index),error,
                static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()),
                static_cast<unsigned long long>(submitted),static_cast<unsigned long long>(completed),pending,first?0u:1u);
            for (std::size_t i=0;i<request_count;++i) {
                const auto &read=reads[i];
                msg_err("PX-BCUD: USB request read=%u pending=%u internal=0x%llx internal_high=%llu offset=%lu offset_high=%lu event_set=%u\n",
                    static_cast<unsigned>(i),read.pending?1u:0u,
                    static_cast<unsigned long long>(read.operation.Internal),static_cast<unsigned long long>(read.operation.InternalHigh),
                    read.operation.Offset,read.operation.OffsetHigh,
                    read.operation.hEvent && WaitForSingleObject(read.operation.hEvent,0)==WAIT_OBJECT_0?1u:0u);
            }
            const auto count=(std::min)(submitted,static_cast<std::uint64_t>(trace.size()));
            for (std::uint64_t sequence=submitted-count;sequence<submitted;++sequence) {
                const auto &entry=trace[sequence%trace.size()];
                msg_err("PX-BCUD: USB history sequence=%llu read=%u submitted_us=%llu completed_us=%llu win32=%lu submit_begin_us=%llu wait_begin_us=%llu wake_us=%llu framing_end_us=%llu write_end_us=%llu requeue_gap_us=%llu framing_cycles=%llu write_cycles=%llu\n",
                    static_cast<unsigned long long>(entry.sequence),entry.index,
                    static_cast<unsigned long long>(entry.submitted_us),static_cast<unsigned long long>(entry.completed_us),entry.error,
                    static_cast<unsigned long long>(entry.submit_begin_us),static_cast<unsigned long long>(entry.wait_begin_us),
                    static_cast<unsigned long long>(entry.wake_us),static_cast<unsigned long long>(entry.framing_end_us),
                    static_cast<unsigned long long>(entry.write_end_us),static_cast<unsigned long long>(entry.requeue_gap_us),
                    static_cast<unsigned long long>(entry.framing_cycles),static_cast<unsigned long long>(entry.write_cycles));
            }
            // 未完了要求のパケット配列はUSB側が更新中のため参照しない。
            const auto &failed=reads[index];
            if (failed.operation.hEvent && HasOverlappedIoCompleted(&failed.operation)) {
                unsigned errors=0,nonempty=0;
                for (std::size_t i=0;i<failed.packets.size();++i) {
                    const auto &packet=failed.packets[i];
                    if (packet.Length) ++nonempty;
                    if (packet.Status) {
                        ++errors;
                        if (errors<=8) msg_err("PX-BCUD: failed transfer packet=%u usbd_status=0x%08lx offset=%lu length=%lu\n",
                            static_cast<unsigned>(i),packet.Status,packet.Offset,packet.Length);
                    }
                }
                msg_err("PX-BCUD: failed transfer descriptor_errors=%u nonempty_packets=%u\n",errors,nonempty);
            }
        };
        auto submit=[&](Read &r) {
            const auto submit_begin=elapsed_us();
            const auto previous_write=trace[r.trace_slot].write_end_us;
            const auto requeue_gap=previous_write && !r.pending?submit_begin-previous_write:0;
            ResetEvent(r.operation.hEvent);
            r.packets={};
            BOOL ok=WinUsb_ReadIsochPipeAsap(r.buffer,0,static_cast<ULONG>(r.data.size()),
                first?FALSE:TRUE,static_cast<ULONG>(r.packets.size()),r.packets.data(),&r.operation);
            const DWORD error=ok?ERROR_SUCCESS:GetLastError();
            const auto submitted_at=elapsed_us();
            max_submit_us=(std::max)(max_submit_us,submitted_at-submit_begin);
            max_requeue_gap_us=(std::max)(max_requeue_gap_us,requeue_gap);
            if (!ok && error!=ERROR_IO_PENDING) {fail("submit",&r-reads,error);return false;}
            r.trace_slot=static_cast<std::size_t>(submitted%trace.size());
            trace[r.trace_slot]={};
            auto &entry=trace[r.trace_slot];
            entry.sequence=submitted+1;entry.index=static_cast<unsigned>(&r-reads);
            entry.submitted_us=submitted_at;entry.submit_begin_us=submit_begin;entry.requeue_gap_us=requeue_gap;
            ++submitted;
            first=false;r.pending=true; return true;
        };
        for (std::size_t i=0;i<request_count;++i) {
            auto &r=reads[i];
            r.file=file_; r.usb=usb_;
            r.operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!r.operation.hEvent) {fail("create_event",&r-reads,GetLastError());break;}
            if (!WinUsb_RegisterIsochBuffer(usb_,0x84,r.data.data(),static_cast<ULONG>(r.data.size()),&r.buffer)) {
                fail("register_buffer",&r-reads,GetLastError());break;
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
            auto &entry=trace[r.trace_slot];
            HANDLE events[]={removed_,r.operation.hEvent};
            if (!entry.wait_begin_us) entry.wait_begin_us=elapsed_us();
            const DWORD wait=WaitForMultipleObjects(2,events,FALSE,50);
            const DWORD wait_error=wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE;
            entry.wake_us=elapsed_us();
            max_wait_us=(std::max)(max_wait_us,entry.wake_us-entry.wait_begin_us);
            if (wait==WAIT_OBJECT_0) {fail("device_removed",next,ERROR_DEVICE_NOT_CONNECTED);break;}
            if (wait==WAIT_TIMEOUT) {
                if (Clock::now()-last_data>std::chrono::seconds(5)) fail("no_data",next,ERROR_TIMEOUT);
                continue;
            }
            if (wait!=WAIT_OBJECT_0+1) {fail("wait",next,wait_error);break;}
            ULONG size=0;
            if (!WinUsb_GetOverlappedResult(usb_,&r.operation,&size,FALSE)) {
                const DWORD error=GetLastError();
                trace[r.trace_slot].completed_us=elapsed_us();
                trace[r.trace_slot].error=error;
                if (!stop_) fail("complete",next,error);
                break;
            }
            r.pending=false;
            trace[r.trace_slot].completed_us=elapsed_us();
            ++completed;
            ULONG64 frame_begin_cycles=0,frame_end_cycles=0,write_end_cycles=0;
            QueryThreadCycleTime(GetCurrentThread(),&frame_begin_cycles);
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
            entry.framing_end_us=elapsed_us();
            QueryThreadCycleTime(GetCurrentThread(),&frame_end_cycles);
            if (frame_end_cycles>=frame_begin_cycles) entry.framing_cycles=frame_end_cycles-frame_begin_cycles;
            max_framing_us=(std::max)(max_framing_us,entry.framing_end_us-entry.completed_us);
            if (worker_error_) break;
            if (!output.empty()) {
                // PatGate が開くまではパケット単位で捨てるが、開いた後の Accept は
                // 無条件に true を返すため、残り全体を1回の Write で渡す。
                // 188バイトごとに呼ぶと1要求 (最大320パケット) あたり StreamBuffer の
                // ロック取得が320回になり、この区間で実行を奪われる機会が増える。
                // 実測では優先度を base 10 まで上げても write 区間 31.9ms に対し
                // 消費CPUは 47us で、処理量ではなく区間の長さが投入遅れにつながった。
                std::size_t begin=0;
                while (begin<output.size() && !gate.Accept(output.data()+begin)) {
                    ++startup_discarded;begin+=188;
                }
                if (begin<output.size()) {
                    const std::size_t requested=output.size()-begin;
                    std::size_t written=requested;
                    // 部分書き込みはリング満杯を示す。書けた分は配信済みとして残し、
                    // パケット単位で書いていたときと同じく失敗として扱う。
                    if (!stream_buf_->Write(output.data()+begin,written) || written!=requested) {
                        fail("stream_buffer_write",next,ERROR_BUFFER_OVERFLOW);
                    } else if (outage && !reported_recovery) {
                        reported_recovery=true;recovering_=false;
                        msg_warn("PX-BCUD: auto recovery resumed count=%llu outage_ms=%llu possible_ts_loss=1\n",
                            static_cast<unsigned long long>(recovery_count_),
                            static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-*outage).count()));
                    }
                }
                stream_buf_->NotifyWrite();
            }
            entry.write_end_us=elapsed_us();
            QueryThreadCycleTime(GetCurrentThread(),&write_end_cycles);
            if (write_end_cycles>=frame_end_cycles) entry.write_cycles=write_end_cycles-frame_end_cycles;
            max_write_us=(std::max)(max_write_us,entry.write_end_us-entry.framing_end_us);
            if (stop_ || !submit(r)) break;
            next=(next+1)%request_count;
        }
        for (std::size_t i=0;i<request_count;++i) {
            auto &r=reads[i];
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
        msg_info("PX-BCUD: timing maxima tid=%lu wait_us=%llu framing_us=%llu write_us=%llu submit_us=%llu requeue_gap_us=%llu\n",
            GetCurrentThreadId(),static_cast<unsigned long long>(max_wait_us),static_cast<unsigned long long>(max_framing_us),
            static_cast<unsigned long long>(max_write_us),static_cast<unsigned long long>(max_submit_us),
            static_cast<unsigned long long>(max_requeue_gap_us));
    }
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_;
    UsbIo io_; Frontend frontend_;
    const bool auto_recovery_;
    const std::size_t requests_;
    std::uint64_t recovery_count_=0;
    Clock::time_point failure_time_{};
    std::atomic_bool recovering_{false};
    std::mutex mutex_; std::condition_variable closed_;
    bool open_=false, resume_capture_=false,capture_requested_=false;
    std::atomic_bool stop_{true};
    std::atomic<DWORD> worker_error_{0};
    std::atomic<DWORD> terminal_error_{0};
    std::thread worker_;
};

Device::Device(const std::wstring &path,const DeviceDefinition &definition,std::uintptr_t index,ReceiverManager &manager,bool auto_recovery)
    : DeviceBase(path,definition,index,manager),auto_recovery_(auto_recovery)
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
    // isoch のキュー深度。1要求 64マイクロフレーム = 8ms のため、要求数がそのまま
    // 受信スレッドが遅れても欠落しない時間になる。既定の5は Linux em28xx と同じだが、
    // 完了コールバックで再投入するカーネルと違い、ここはユーザー空間のスレッドが
    // 再投入するため余裕が要る。他機種と同じ名前で設定できるようにする。
    std::size_t requests=PXBCUD_DEFAULT_REQUESTS;
    if (device_def_.configs.Exists(L"MaxUrbs")) {
        const auto value=px4::util::wtoui(device_def_.configs.Get(L"MaxUrbs"));
        if (value<PXBCUD_MIN_REQUESTS || value>PXBCUD_MAX_REQUESTS)
            msg_warn("PX-BCUD: MaxUrbs=%u is out of range %u-%u; using %u\n",
                value,PXBCUD_MIN_REQUESTS,PXBCUD_MAX_REQUESTS,static_cast<unsigned>(requests));
        else
            requests=value;
    }
    receiver_=std::make_unique<Receiver>(usb_dev_.dev,usb_dev_.winusb,removed_,auto_recovery_,requests);
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
DWORD Device::LastStreamError() const noexcept
{
    return receiver_?receiver_->LastStreamError():ERROR_INVALID_HANDLE;
}
}
