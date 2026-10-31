// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ts_continuity.hpp"
#include "msg.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace px4 {
// USB 転送1回分のメタデータ。TS の中身は先頭と末尾のパケットの PID・CC だけを残す。
struct TsTransferTrace {
    std::uint64_t sequence=0,completed_at=0; // completed_at は GetSystemTimePreciseAsFileTime の値
    unsigned slot=0,bytes=0,frame_type=0,frame_length=0,frame_flags=0,packets=0,skipped=0;
    int first_pid=-1,first_cc=-1,last_pid=-1,last_cc=-1;
    const char* tag="";
};
// 受信スレッドでは固定長レコードを非待機で積むだけとし、ファイル I/O は専用スレッドへ逃がす。
class TsDiagnosticLog final {
public:
    explicit TsDiagnosticLog(const char* stage) noexcept : stage_(stage) {}
    ~TsDiagnosticLog() {Stop();}
    void Enable(bool enabled) noexcept
    {
        if (!enabled) {Stop();monitor_.reset();return;}
        if (monitor_) return;
        try {
            monitor_.reset(new(std::nothrow) TsContinuityMonitor);
            queue_.reset(new(std::nothrow) Queue);
            if (!monitor_ || !queue_ || !queue_->ready) throw std::bad_alloc();
            writer_=std::thread([this]{WriteLoop();});
        } catch (...) {
            monitor_.reset();queue_.reset();
            msg_warn("TS diagnostics stage=%s disabled: allocation/thread creation failed\n",stage_);
        }
    }
    void Reset() noexcept
    {
        if (monitor_ && started_) Summary("reset");
        if (monitor_) monitor_->Reset();
        started_=false;details_=0;suppressed_=0;max_gap_=0;gap_=0;
    }
    void Context(std::uint64_t gap_us) noexcept {gap_=gap_us;if (gap_us>max_gap_) max_gap_=gap_us;}
    // 観測点より前の段階で捨てた累積バイト数。ログには世代の開始時点からの差分を記録する。
    void Source(std::uint64_t discarded) noexcept {source_=discarded;source_valid_=true;}
    void SetReadyProbe(int(*probe)(void*) noexcept,void* context) noexcept {probe_=probe;probe_context_=context;}
    TsContinuityMonitor::Counters Counts() const noexcept {return monitor_ ? monitor_->Counts() : TsContinuityMonitor::Counters{};}
    void Feed(const void* data,std::size_t size,std::uint64_t epoch) noexcept
    {
        if (!monitor_) return;
        const auto now=Milliseconds();
        if (!started_ || epoch!=epoch_) {
            if (started_) Summary("epoch_end");
            monitor_->Reset();epoch_=epoch;started_=true;last_summary_=now;window_=now;details_=0;suppressed_=0;max_gap_=gap_;
            source_base_=source_;
            Record r{};r.type=Type::Start;r.epoch=epoch_;Emit(r);
        }
        if (now-window_>=1000) {
            if (suppressed_) {Record r{};r.type=Type::Suppressed;r.epoch=epoch_;r.suppressed=suppressed_;Emit(r);}
            details_=0;suppressed_=0;window_=now;
        }
        monitor_->Feed(data,size,[&](const TsContinuityMonitor::Event& e) {
            ++anomalies_;
            if (details_++>=32) {++suppressed_;return;}
            Record r{};r.type=Type::Anomaly;r.epoch=epoch_;r.event=e;r.gap=gap_;r.max_gap=max_gap_;r.discarded=SourceDiscarded();
            r.ready=probe_ ? probe_(probe_context_) : -1;Emit(r);
        });
        if (now-last_summary_>=10000) {Summary("periodic");last_summary_=now;max_gap_=gap_;}
    }
    // 詳細行の省略前に数えた異常の累積数。転送記録の出力契機に使う。
    std::uint64_t AnomalyCount() const noexcept {return anomalies_;}
    // 転送記録はまとめて積み、空きが足りない場合は前後関係の一部だけが残らないよう全体を欠落として数える。
    void Traces(const TsTransferTrace* traces,std::size_t count) noexcept
    {
        if (!monitor_ || !count) return;
        const auto tail=queue_->tail.load(std::memory_order_relaxed);
        if (queue_->records.size()-(tail-queue_->head.load(std::memory_order_acquire))<count) {queue_->dropped+=count;return;}
        for (std::size_t i=0;i<count;++i) {
            auto& r=queue_->records[(tail+i)%queue_->records.size()];
            r=Record{};r.type=Type::Trace;r.epoch=epoch_;r.trace=traces[i];
        }
        Publish(tail+count);
    }
    // 終了時・試験用。受信／配信の Feed と Purge では待機しない。
    void Flush()
    {
        if (!queue_) return;
        while (queue_->head.load(std::memory_order_acquire)!=queue_->tail.load(std::memory_order_acquire)) Sleep(1);
    }
private:
    enum class Type {Start,Anomaly,Suppressed,Summary,Trace};
    struct Record {
        Type type=Type::Start;SYSTEMTIME time{};std::uint64_t epoch=0;
        TsContinuityMonitor::Event event{};TsContinuityMonitor::Counters counts{};TsTransferTrace trace{};
        std::uint64_t gap=0,max_gap=0,suppressed=0;std::int64_t discarded=-1;int ready=-1;const char* reason="";
    };
    // 書込み側は1スレッドに限る。Siano は受信スレッドだけが積み、BonDriver は diagnostic_mtx_ で直列化する。
    // 待機もロックもない単一生産者・単一消費者のリングとし、欠落は満杯の場合だけに限る。
    // head は書込みスレッドが1行書き終えてから進めるため、head==tail は出力済みを意味する。
    struct Queue {
        Queue() noexcept : wake(CreateEventW(nullptr,FALSE,FALSE,nullptr)),ready(wake!=nullptr) {}
        ~Queue() {if (wake) CloseHandle(wake);}
        HANDLE wake;bool ready;
        std::array<Record,256> records{};std::atomic<std::size_t> head{0},tail{0};
        std::atomic_bool stop{false};std::atomic_uint64_t dropped{0};
    };
    static std::uint64_t Milliseconds() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void Emit(Record& r) noexcept
    {
        GetLocalTime(&r.time);
        const auto tail=queue_->tail.load(std::memory_order_relaxed);
        if (tail-queue_->head.load(std::memory_order_acquire)==queue_->records.size()) {++queue_->dropped;return;}
        queue_->records[tail%queue_->records.size()]=r;
        Publish(tail+1);
    }
    void Publish(std::size_t tail) noexcept
    {
        queue_->tail.store(tail,std::memory_order_release);
        SetEvent(queue_->wake);
    }
    // Source() を呼ばない段階は測定対象外として -1 を記録する。
    std::int64_t SourceDiscarded() const noexcept
    {
        return source_valid_ ? static_cast<std::int64_t>(source_-source_base_) : -1;
    }
    void Summary(const char* reason) noexcept
    {
        Record r{};r.type=Type::Summary;r.epoch=epoch_;r.reason=reason;r.counts=monitor_->Counts();
        r.suppressed=suppressed_;r.max_gap=max_gap_;r.discarded=SourceDiscarded();Emit(r);
    }
    void Write(const Record& r) noexcept
    {
        char at[40];sprintf_s(at,"%04u/%02u/%02u %02u:%02u:%02u.%03u",r.time.wYear,r.time.wMonth,r.time.wDay,
            r.time.wHour,r.time.wMinute,r.time.wSecond,r.time.wMilliseconds);
        if (r.type==Type::Start) msg_info("TS diagnostics stage=%s epoch=%llu observed_at=%s start\n",stage_,static_cast<unsigned long long>(r.epoch),at);
        else if (r.type==Type::Suppressed) msg_warn("TS diagnostics stage=%s epoch=%llu observed_at=%s suppressed=%llu (detail rate limit)\n",
            stage_,static_cast<unsigned long long>(r.epoch),at,static_cast<unsigned long long>(r.suppressed));
        else if (r.type==Type::Trace) {
            const auto& t=r.trace;
            // 完了時刻は異常行の observed_at と照合できるよう、ローカル時刻のマイクロ秒で書く。
            FILETIME utc{static_cast<DWORD>(t.completed_at),static_cast<DWORD>(t.completed_at>>32)};
            SYSTEMTIME s{},local{};char done[40]="-";
            if (FileTimeToSystemTime(&utc,&s) && SystemTimeToTzSpecificLocalTime(nullptr,&s,&local))
                sprintf_s(done,"%02u:%02u:%02u.%06llu",local.wHour,local.wMinute,local.wSecond,
                    static_cast<unsigned long long>((t.completed_at/10)%1000000));
            msg_info("TS transfer stage=%s tag=%s seq=%llu completed_at=%s slot=%u bytes=%u frame_type=%u frame_length=%u frame_flags=0x%04x packets=%u first=0x%04x/%d last=0x%04x/%d skipped=%u\n",
                stage_,t.tag,static_cast<unsigned long long>(t.sequence),done,t.slot,t.bytes,t.frame_type,t.frame_length,
                t.frame_flags,t.packets,t.first_pid<0 ? 0 : t.first_pid,t.first_cc,t.last_pid<0 ? 0 : t.last_pid,t.last_cc,t.skipped);
        } else if (r.type==Type::Anomaly) {
            const auto& e=r.event;
            msg_warn("TS anomaly stage=%s epoch=%llu observed_at=%s kind=%s packet=%llu pid=0x%04x prev_cc=%d expected_cc=%d cc=%d gap_us=%llu max_gap_us=%llu ready_reads=%d framer_discarded=%lld\n",
                stage_,static_cast<unsigned long long>(r.epoch),at,TsContinuityMonitor::Name(e.kind),
                static_cast<unsigned long long>(e.packet),e.pid,e.previous,e.expected,e.actual,
                static_cast<unsigned long long>(r.gap),static_cast<unsigned long long>(r.max_gap),r.ready,
                static_cast<long long>(r.discarded));
        } else {
            const auto& c=r.counts;
            msg_info("TS diagnostics stage=%s epoch=%llu observed_at=%s reason=%s packets=%llu cc_jump=%llu same_cc_changed=%llu duplicate_run=%llu adaptation_cc=%llu tei=%llu sync=%llu resync_bytes=%llu malformed=%llu repeated_packets=%llu suppressed=%llu max_gap_us=%llu framer_discarded=%lld\n",
                stage_,static_cast<unsigned long long>(r.epoch),at,r.reason,
                static_cast<unsigned long long>(c.packets),static_cast<unsigned long long>(c.cc_jump),
                static_cast<unsigned long long>(c.repeat_changed),static_cast<unsigned long long>(c.duplicate_run),
                static_cast<unsigned long long>(c.adaptation_cc),static_cast<unsigned long long>(c.tei),
                static_cast<unsigned long long>(c.sync),static_cast<unsigned long long>(c.resync_bytes),
                static_cast<unsigned long long>(c.malformed),static_cast<unsigned long long>(c.duplicates),
                static_cast<unsigned long long>(r.suppressed),static_cast<unsigned long long>(r.max_gap),
                static_cast<long long>(r.discarded));
        }
    }
    void WriteLoop() noexcept
    {
        for (;;) {
            auto head=queue_->head.load(std::memory_order_relaxed);
            if (head==queue_->tail.load(std::memory_order_acquire)) {
                if (queue_->stop.load(std::memory_order_acquire) && head==queue_->tail.load(std::memory_order_acquire)) break;
                WaitForSingleObject(queue_->wake,INFINITE);
                continue;
            }
            const auto r=queue_->records[head%queue_->records.size()];
            Write(r);
            const auto dropped=queue_->dropped.exchange(0);
            if (dropped) msg_warn("TS diagnostics stage=%s queue_dropped=%llu (nonblocking diagnostic queue)\n",stage_,static_cast<unsigned long long>(dropped));
            queue_->head.store(head+1,std::memory_order_release);
        }
        const auto dropped=queue_->dropped.exchange(0);
        if (dropped) msg_warn("TS diagnostics stage=%s queue_dropped=%llu (nonblocking diagnostic queue)\n",stage_,static_cast<unsigned long long>(dropped));
    }
    void Stop() noexcept
    {
        if (!writer_.joinable()) return;
        queue_->stop.store(true,std::memory_order_release);SetEvent(queue_->wake);
        writer_.join();queue_.reset();
    }
    const char* stage_;std::unique_ptr<TsContinuityMonitor> monitor_;std::unique_ptr<Queue> queue_;std::thread writer_;
    bool started_=false;std::uint64_t epoch_=0,last_summary_=0,window_=0,suppressed_=0,gap_=0,max_gap_=0,details_=0;
    std::uint64_t source_=0,source_base_=0,anomalies_=0;bool source_valid_=false;
    int(*probe_)(void*) noexcept=nullptr;void* probe_context_=nullptr;
};
// 異常を含む転送の直前 History 件と、その後 After 件の転送メタデータを残す。
// 受信スレッドでは固定長配列を更新するだけで、出力は1秒あたり Budget 件までに抑える。
class TsTransferTracer final {
public:
    static constexpr std::size_t History=16,After=8,Budget=64;
    // 転送完了ごとに呼び、返した参照へ TS 出力の情報を後から書き足す。
    TsTransferTrace& Begin(const TsTransferTrace& t) noexcept
    {
        auto& slot=ring_[next_%History];slot=t;slot.sequence=next_++;return slot;
    }
    void End(TsDiagnosticLog& log,bool anomaly,std::uint64_t now_ms) noexcept
    {
        if (!next_) return;
        if (now_ms-window_>=1000) {window_=now_ms;budget_=Budget;}
        std::array<TsTransferTrace,History> out{};std::size_t n=0;
        if (anomaly && !after_) {
            const auto first=next_>History ? next_-History : 0;
            for (auto i=first;i<next_;++i) {out[n]=ring_[i%History];out[n++].tag=i+1==next_ ? "trigger" : "before";}
        } else if (after_) {
            out[n]=ring_[(next_-1)%History];out[n++].tag=anomaly ? "trigger" : "after";--after_;
        }
        if (anomaly) after_=After;
        if (!n) return;
        if (budget_<n) {skipped_+=static_cast<unsigned>(n);return;}
        budget_-=n;out[0].skipped=skipped_;skipped_=0;
        log.Traces(out.data(),n);
    }
private:
    std::array<TsTransferTrace,History> ring_{};std::uint64_t next_=0,window_=0;
    std::size_t after_=0,budget_=Budget;unsigned skipped_=0;
};
}
