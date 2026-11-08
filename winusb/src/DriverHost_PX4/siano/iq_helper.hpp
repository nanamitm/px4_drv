// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace px4::siano {
// J.83B の IQ を復調する helper (msi2500_frontend.exe serve-s1ud) の起動、IQ のブロックの受け渡し、TS の受け取り。
// USB は DriverHost が持ち続け、helper は標準入力で IQ のブロックを受けて、RTL-SDR の helper と同じ形式で
// 標準出力へ TS と状態を返す。DSP を別プロセスに置く理由は MSi2500 と同じ (ライセンスと障害の分離)。
class IqHelper final {
public:
    // helper の TS を受け取る。false を返すと helper を失敗として止める
    using TsHandler=std::function<bool(const std::uint8_t*,std::size_t)>;
    // helper が停止要求なしに終わったとき、または形式不正で止めたときに一度だけ呼ばれる (読み取りのスレッドから)
    using FailHandler=std::function<void()>;
    IqHelper()=default;
    ~IqHelper() {Stop();}
    IqHelper(const IqHelper&)=delete;
    IqHelper& operator=(const IqHelper&)=delete;
    int Start(const std::wstring& exe,const std::wstring& log,TsHandler ts,FailHandler fail);
    // 受信スレッドから呼ぶ。待たずに複製してキューへ積み、満杯なら捨てて数える。
    // 捨てたブロックは helper がブロック番号の欠落として検出する
    void Feed(const std::uint8_t* block,std::size_t size);
    // 標準入力を閉じて helper の終了を待つ (期限を過ぎたら job ごと止める)
    void Stop();
    bool Running() const {return running_;}
    bool Ready() const {return ready_;}
    bool Failed() const {return failed_;}
    ULONGLONG LastTs() const {return last_ts_;}
    std::uint32_t Quality() const {return quality_;}
    std::uint64_t Dropped() const {return dropped_;}
private:
    void Write() noexcept;
    void Read() noexcept;
    void Fail(const char* reason);
    HANDLE process_=nullptr,job_=nullptr,input_=nullptr,output_=nullptr;
    TsHandler ts_;
    FailHandler fail_;
    std::thread writer_,reader_;
    std::mutex mutex_;std::condition_variable queued_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::vector<std::vector<std::uint8_t>> spare_;
    bool closing_=false;
    std::atomic_bool running_{false},ready_{false},failed_{false},stopping_{false};
    std::atomic<ULONGLONG> last_ts_{0};
    std::atomic<std::uint32_t> quality_{0};
    std::atomic<std::uint64_t> dropped_{0};
};
}
