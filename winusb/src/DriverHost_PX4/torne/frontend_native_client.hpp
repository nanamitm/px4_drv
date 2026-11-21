// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "dib8096_native.hpp"
#include "frontend_signal.hpp"
#include "mb86h47_i2c.hpp"

namespace px4::torne {
// 抜去と停止の確認、待機は FrontendSignal を通すため、WinUSB 版と Linux 版で共有する。
class NativeFrontendClient final : private dibnative::Bus {
public:
    NativeFrontendClient(MailboxI2c& bus,FrontendSignal& signal):bus_(bus),signal_(signal),frontend_(*this){}
    bool Command(std::uint8_t kind,const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& response,
        std::chrono::steady_clock::time_point deadline);
    void SetBulkTiming() noexcept { frontend_.SetBulkTiming(true); }
    void SetBatchWrites(bool enabled) noexcept { batch_writes_ = enabled; }
    // true の間、受信機の停止要求で I2C 要求の合間と待機を中断する。
    void SetStoppable(bool stoppable) noexcept {stoppable_=stoppable;}
    bool StoppedCleanly() const noexcept { return stopped_cleanly_ && !io_failed_; }
    const std::string& Error() const noexcept{return error_;}
private:
    std::tuple<dibnative::Slice<std::uint8_t>,dibnative::Error> WriteRead(std::uint8_t address,
        dibnative::Slice<std::uint8_t> write,std::int64_t read_length) override;
    dibnative::Error Wait(std::int64_t duration) override;
    dibnative::Bus::MemoryResult RecallTMCC(std::uint32_t key) override;
    dibnative::Bus::MemoryResult RememberTMCC(std::uint8_t address,std::uint32_t key) override;
    dibnative::Bus::MemoryResult TMCC(std::uint32_t kind,const std::vector<std::uint32_t>& parameters);
    std::tuple<std::int64_t,std::int64_t,bool,dibnative::Error> Captrim(std::uint8_t address,std::uint16_t lo4,
        std::uint32_t key,std::int64_t start,std::int64_t step,std::int64_t target,std::int64_t write,std::int64_t read) override;
    dibnative::Error CheckState();
    dibnative::Error FlushWrites();
    std::array<MailboxI2c::RegisterWrite, MailboxI2c::MaxRegisterWrites> pending_{};
    std::size_t pending_count_ = 0;
    bool batch_writes_ = false;
    MailboxI2c& bus_;
    FrontendSignal& signal_;
    bool stoppable_=false;
    NativeFrontend frontend_;
    std::chrono::steady_clock::time_point deadline_{};
    bool stopped_cleanly_ = false, io_failed_ = false;
    std::string error_;
};
}
