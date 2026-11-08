// SPDX-License-Identifier: GPL-2.0-only
#include "iq_helper.hpp"
#include "../rtlsdr/protocol.hpp"
#include "msg.h"
#include <algorithm>
#include <array>
#include <cerrno>

namespace px4::siano {
namespace {
// 標準入力のキューのブロック数 (256 ブロックは約 0.16 秒、約 3.8 MB)。helper の起動直後の FFT の計画や
// 長い欠落をつなぐ試行で一時的に読み取りが遅れても捨てずに済む長さで、helper 側にも約 2.5 秒のキューがある
constexpr std::size_t QueueBlocks=256;
// 標準入力のパイプの容量 (約 0.6 秒分)。書き込みのスレッドが helper の一時的な遅れで止まらないようにする
constexpr DWORD InputPipeSize=16u<<20;
// 停止要求から helper の終了までの期限。helper はキューに残ったブロックを処理してから終わる
constexpr DWORD StopDeadlineMs=5000;
std::wstring Quote(const std::wstring& input)
{
    std::wstring out=L"\"";
    std::size_t slashes=0;
    for (wchar_t c:input) {
        if (c==L'\\') {++slashes;continue;}
        if (c==L'"') {out.append(slashes*2+1,L'\\');out+=c;}
        else {out.append(slashes,L'\\');out+=c;}
        slashes=0;
    }
    out.append(slashes*2,L'\\');return out+L'"';
}
void CloseIf(HANDLE& h) {if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);h=nullptr;}
}

int IqHelper::Start(const std::wstring& exe,const std::wstring& log,TsHandler ts,FailHandler fail)
{
    Stop();
    ts_=std::move(ts);fail_=std::move(fail);
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE child_input=nullptr,child_output=nullptr,child_error=nullptr;
    auto cleanup=[&]{CloseIf(child_input);CloseIf(child_output);CloseIf(child_error);};
    if (!CreatePipe(&child_input,&input_,&sa,InputPipeSize) || !SetHandleInformation(input_,HANDLE_FLAG_INHERIT,0) ||
        !CreatePipe(&output_,&child_output,&sa,1u<<20) || !SetHandleInformation(output_,HANDLE_FLAG_INHERIT,0)) {
        cleanup();Stop();return -EIO;
    }
    child_error=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (child_error==INVALID_HANDLE_VALUE) {child_error=nullptr;cleanup();Stop();return -EIO;}
    job_=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {cleanup();Stop();return -EIO;}
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=child_input;startup.StartupInfo.hStdOutput=child_output;startup.StartupInfo.hStdError=child_error;
    SIZE_T attribute_size=0;
    InitializeProcThreadAttributeList(nullptr,1,0,&attribute_size);
    std::vector<std::uint8_t> attributes(attribute_size);
    startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size)) {cleanup();Stop();return -EIO;}
    HANDLE inherited[]={child_input,child_output,child_error};
    const bool attr_ok=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
    std::wstring command=Quote(exe)+L" serve-s1ud";
    PROCESS_INFORMATION process{};
    const BOOL created=attr_ok && CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    // 子への書込端を親が残すと、子の終了を EOF として検出できなくなる
    cleanup();
    if (!created) {msg_err("PX-S1UD: J.83B helper CreateProcess failed win32=%lu\n",GetLastError());Stop();return -EIO;}
    process_=process.hProcess;
    if (!AssignProcessToJobObject(job_,process_)) {
        TerminateProcess(process_,ERROR_ACCESS_DENIED);WaitForSingleObject(process_,1000);CloseHandle(process.hThread);Stop();return -EIO;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_=false;queue_.clear();
    }
    ready_=false;failed_=false;stopping_=false;last_ts_=0;quality_=0;dropped_=0;
    const bool resumed=ResumeThread(process.hThread)!=static_cast<DWORD>(-1);
    CloseHandle(process.hThread);
    if (!resumed) {Stop();return -EIO;}
    try {
        writer_=std::thread([this]{Write();});
        reader_=std::thread([this]{Read();});
    } catch (...) {Stop();return -ENOMEM;}
    running_=true;
    return 0;
}

void IqHelper::Feed(const std::uint8_t* block,std::size_t size)
{
    if (!running_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closing_) return;
    if (queue_.size()>=QueueBlocks) {++dropped_;return;}
    // ブロックごとの確保を避けるため、書き終えたバッファを使い回す
    std::vector<std::uint8_t> buffer;
    if (!spare_.empty()) {buffer=std::move(spare_.back());spare_.pop_back();}
    buffer.assign(block,block+size);
    queue_.push_back(std::move(buffer));
    lock.unlock();
    queued_.notify_one();
}

void IqHelper::Write() noexcept
{
    for (;;) {
        std::vector<std::uint8_t> buffer;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock,[this]{return closing_ || !queue_.empty();});
            // 停止要求では残りを送らずに標準入力を閉じる (選局の切り替えを待たせない)
            if (closing_) break;
            buffer=std::move(queue_.front());queue_.pop_front();
        }
        const auto size=static_cast<DWORD>(buffer.size());
        DWORD written=0;
        const BOOL ok=WriteFile(input_,buffer.data(),size,&written,nullptr);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (spare_.size()<QueueBlocks) spare_.push_back(std::move(buffer));
        }
        // helper が終わった (パイプが切れた) 場合。原因は読み取りのスレッドが EOF として扱う
        if (!ok || written!=size) break;
    }
    // 標準入力の終わりが helper への停止要求になる
    CloseIf(input_);
}

void IqHelper::Read() noexcept
{
    try {
        rtlsdr::Frames frames;
        std::array<std::uint8_t,65536> bytes{};
        for (;;) {
            DWORD got=0;
            if (!ReadFile(output_,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr) || !got) break;
            bool valid=true;
            // Frames::Feed は 1 回の入力を MaxTs までに制限するため、分けて渡す
            for (DWORD offset=0;valid && offset<got;) {
                const DWORD part=(std::min)(got-offset,static_cast<DWORD>(rtlsdr::MaxTs));
                valid=frames.Feed(bytes.data()+offset,part,[&](const rtlsdr::Frame& frame,const std::uint8_t* data) {
                    if (frame.kind==1) ready_=true;
                    if (frame.kind==4) {
                        msg_err("PX-S1UD: J.83B helper error: %.*s\n",static_cast<int>(frame.size),reinterpret_cast<const char*>(data));
                        return false;
                    }
                    if (frame.kind==3) quality_=frame.quality;
                    if (frame.kind==2) {
                        quality_=frame.quality;last_ts_=GetTickCount64();
                        if (!stopping_ && !ts_(data,frame.size)) return false;
                    }
                    return true;
                });
                offset+=part;
            }
            if (!valid) {Fail("invalid or failed helper frame");return;}
        }
    } catch (const std::exception& e) {
        msg_err("PX-S1UD: J.83B helper reader failed: %s\n",e.what());
    }
    if (!stopping_) Fail("helper stopped unexpectedly");
}

void IqHelper::Fail(const char* reason)
{
    if (stopping_ || failed_.exchange(true)) return;
    msg_err("PX-S1UD: J.83B %s\n",reason);
    quality_=0;
    // 書き込み中の WriteFile を終わらせるため、helper を止める
    if (job_) TerminateJobObject(job_,ERROR_INVALID_DATA);
    if (fail_) fail_();
}

void IqHelper::Stop()
{
    stopping_=true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_=true;queue_.clear();
    }
    queued_.notify_all();
    // 書き込みのスレッドが標準入力を閉じると helper は終わる。helper が応答しない場合は
    // 書き込み中の WriteFile も戻らないため、期限を過ぎたら job ごと止めてパイプを切る
    if (process_ && WaitForSingleObject(process_,StopDeadlineMs)==WAIT_TIMEOUT) {
        msg_warn("PX-S1UD: J.83B helper stop deadline expired; terminating owned job\n");
        TerminateJobObject(job_,ERROR_TIMEOUT);WaitForSingleObject(process_,1000);
    }
    if (writer_.joinable()) writer_.join();
    if (reader_.joinable()) reader_.join();
    CloseIf(input_);CloseIf(output_);CloseIf(process_);CloseIf(job_);
    running_=false;ready_=false;last_ts_=0;quality_=0;
}
}
