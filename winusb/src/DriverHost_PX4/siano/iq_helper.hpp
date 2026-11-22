// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <windows.h>
#include "iq_helper_output.hpp"
#include "session.hpp"
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
class IqHelper final : public IqProcess {
public:
    IqHelper()=default;
    ~IqHelper() override {Stop();}
    IqHelper(const IqHelper&)=delete;
    IqHelper& operator=(const IqHelper&)=delete;
    // 起動する helper の実行ファイルと、標準エラー出力のログ
    void Configure(const std::wstring& exe,const std::wstring& log) {exe_=exe;log_=log;}
    int Start(TsHandler ts) override;
    // 受信スレッドから呼ぶ。待たずに複製してキューへ積み、満杯なら捨てて数える。
    // 捨てたブロックは helper がブロック番号の欠落として検出する
    void Feed(const std::uint8_t* block,std::size_t size) override;
    // 標準入力を閉じて helper の終了を待つ (期限を過ぎたら job ごと止める)
    void Stop() override;
    bool Running() const override {return running_;}
    bool Ready() const override {return output_state_.Ready();}
    bool Failed() const override {return failed_;}
    std::uint64_t LastTs() const override {return output_state_.LastTs();}
    std::uint32_t Quality() const override {return output_state_.Quality();}
    // helper が状態で報告した復調器の MER (dB×100)。まだ報告がない (古い helper を含む) ときは -1
    std::int32_t MerX100() const override {return output_state_.MerX100();}
    std::uint64_t Dropped() const override {return dropped_;}
private:
    void Write() noexcept;
    void Read() noexcept;
    void Fail(const char* reason);
    std::wstring exe_,log_;
    HANDLE process_=nullptr,job_=nullptr,input_=nullptr,output_=nullptr;
    TsHandler ts_;
    HelperOutput output_state_;
    std::thread writer_,reader_;
    std::mutex mutex_;std::condition_variable queued_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::vector<std::vector<std::uint8_t>> spare_;
    bool closing_=false;
    std::atomic_bool running_{false},failed_{false},stopping_{false};
    std::atomic<std::uint64_t> dropped_{0};
};
}
