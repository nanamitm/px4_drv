// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "dib8096_native_runtime.hpp"

namespace px4::torne {
class NativeFrontend final {
public:
    static constexpr std::uint8_t ReadOnlyStatus = 0x15;
    explicit NativeFrontend(dibnative::Bus& bus);
    ~NativeFrontend();
    NativeFrontend(const NativeFrontend&)=delete;
    NativeFrontend& operator=(const NativeFrontend&)=delete;
    // 模擬バスでは時刻だけを置き換え、実機と同じ状態遷移を検証できる。
    void SetBulkTiming(bool enabled) noexcept;
    void SetClock(std::function<dibnative::Time()> now);
    bool Command(std::uint8_t kind,const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& response);
    const std::string& Error() const noexcept {return error_;}
private:
    bool Execute(std::uint8_t kind,const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& response);
    struct State;
    dibnative::Bus& bus_;
    std::unique_ptr<State> state_;
    // 初期化のたびに作り直すStateへ、構築後に設定された値を引き継ぐ。
    bool bulk_timing_=false;
    std::function<dibnative::Time()> now_;
    std::string error_;
    bool initialized_=false;
};
}
