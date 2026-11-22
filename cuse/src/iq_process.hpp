// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "siano/iq_helper_output.hpp"
#include "siano/session.hpp"
#include <sys/types.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace px4::cuse {
// PX-S1UD の J.83B の helper (msi2500_frontend serve-s1ud) を子プロセスとして動かす。
// WinUSB 版の IqHelper (siano/iq_helper.cpp) の Linux 版で、標準入力へ IQ のブロックを渡し、標準出力の TS と状態を
// HelperOutput で解釈する。標準エラー出力はデーモンの標準エラー出力 (journal) をそのまま使う
// (px4-cused.service は ProtectSystem=strict で、ログのファイルを書く場所を持たないため)。
class PosixIqProcess final : public siano::IqProcess {
public:
    explicit PosixIqProcess(std::string executable) : executable_(std::move(executable)) {}
    ~PosixIqProcess() override { Stop(); }
    PosixIqProcess(const PosixIqProcess &) = delete;
    PosixIqProcess &operator=(const PosixIqProcess &) = delete;

    const std::string &Executable() const { return executable_; }
    int Start(TsHandler ts) override;
    void Feed(const std::uint8_t *block, std::size_t size) override;
    void Stop() override;
    bool Running() const override { return running_; }
    bool Ready() const override { return output_state_.Ready(); }
    bool Failed() const override { return failed_; }
    std::uint64_t LastTs() const override { return output_state_.LastTs(); }
    std::uint32_t Quality() const override { return output_state_.Quality(); }
    std::int32_t MerX100() const override { return output_state_.MerX100(); }
    std::uint64_t Dropped() const override { return dropped_; }

private:
    void Write() noexcept;
    void Read() noexcept;
    void Fail(const char *reason);
    void CloseInput();

    const std::string executable_;
    pid_t pid_ = -1;
    int input_ = -1, output_ = -1;
    TsHandler ts_;
    siano::HelperOutput output_state_;
    std::thread writer_, reader_;
    std::mutex mutex_;
    std::condition_variable queued_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::vector<std::vector<std::uint8_t>> spare_;
    bool closing_ = false;
    std::atomic_bool running_{false}, failed_{false}, stopping_{false};
    std::atomic<std::uint64_t> dropped_{0};
};
}
