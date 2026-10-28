// SPDX-License-Identifier: GPL-2.0-only
#include "frontend_native_client.hpp"
#include "frontend_wait.hpp"

namespace px4::torne {
dibnative::Bus::MemoryResult NativeFrontendClient::TMCC(std::uint32_t kind,const std::vector<std::uint32_t>& parameters) {
    if(auto error=CheckState())return {{},false,true,error};
    std::array<std::uint32_t,3> results{};
    const auto result=bus_.RunJob(kind,parameters,0,results,deadline_);
    if(result!=I2cResult::ok)return {{},false,true,dibnative::MakeError("TMCC job failed",static_cast<unsigned>(result),static_cast<unsigned>(bus_.LastMemoryResult()),bus_.LastSystemError())};
    if(auto error=CheckState())return {{},false,true,error};
    return {{results[1],results[2]},results[0]!=0,true,nullptr};
}
dibnative::Bus::MemoryResult NativeFrontendClient::RecallTMCC(std::uint32_t key) {return TMCC(3,{key});}
dibnative::Bus::MemoryResult NativeFrontendClient::RememberTMCC(std::uint8_t address,std::uint32_t key) {return TMCC(4,{address,key});}
std::tuple<std::int64_t,std::int64_t,bool,dibnative::Error> NativeFrontendClient::Captrim(std::uint8_t address,std::uint16_t lo4,
    std::uint32_t key,std::int64_t start,std::int64_t step,std::int64_t target,std::int64_t write,std::int64_t read) {
    if(auto error=CheckState())return {0,0,true,error};
    int best=0,diff=0;
    const auto result=bus_.Captrim(address,lo4,key,static_cast<int>(start),static_cast<int>(step),static_cast<int>(target),write,read,best,diff,deadline_);
    if(result!=I2cResult::ok)return {0,0,true,dibnative::MakeError("Captrim job failed",static_cast<unsigned>(result),static_cast<unsigned>(bus_.LastMemoryResult()),bus_.LastSystemError())};
    if(auto error=CheckState())return {0,0,true,error};
    return {best,diff,true,nullptr};
}
dibnative::Error NativeFrontendClient::CheckState() const {
    const HANDLE events[]{cancel_,stop_};
    const auto state=WaitForMultipleObjects(stop_?2:1,events,FALSE,0);
    if(state!=WAIT_TIMEOUT)return dibnative::MakeError("native frontend cancelled/wait error",state==WAIT_FAILED?GetLastError():ERROR_OPERATION_ABORTED);
    if(std::chrono::steady_clock::now()>=deadline_)return dibnative::MakeError("native frontend deadline expired");
    return nullptr;
}
std::tuple<dibnative::Slice<std::uint8_t>,dibnative::Error> NativeFrontendClient::WriteRead(
    std::uint8_t address,dibnative::Slice<std::uint8_t> write,std::int64_t length){
    if(auto err=CheckState())return {{},err};
    if(address&1||write.size()>32||length<0||length>32||(!write.size()&&!length))return {{},dibnative::MakeError("invalid native I2C request")};
    std::array<std::uint8_t,32> output{},input{};
    for(std::size_t i=0;i<write.size();++i)output[i]=write[i];
    const auto result=bus_.Transfer(address,output.data(),write.size(),input.data(),static_cast<std::size_t>(length),deadline_);
    if(result!=I2cResult::ok)return {{},dibnative::MakeError("native I2C failure",static_cast<unsigned>(result),static_cast<unsigned>(bus_.LastMemoryResult()),bus_.LastSystemError())};
    if(auto err=CheckState())return {{},err};
    dibnative::Slice<std::uint8_t> response(static_cast<std::size_t>(length));
    for(std::int64_t i=0;i<length;++i)response[i]=input[static_cast<std::size_t>(i)];
    return {response,nullptr};
}
dibnative::Error NativeFrontendClient::Wait(std::int64_t duration){
    // 直前の書込みは同期I2C完了時に届いている。待機だけを切り詰め、
    // 操作全体の期限を延長せず、抜去時には同じイベントで中断する。
    const auto result=WaitFrontendDelay(cancel_,deadline_,duration,stop_);
    return result?dibnative::MakeError("native frontend wait failed",result):nullptr;
}
bool NativeFrontendClient::Command(std::uint8_t kind,const std::vector<std::uint8_t>& body,
    std::vector<std::uint8_t>& response,std::chrono::steady_clock::time_point deadline){
    deadline_=deadline;error_.clear();response.clear();
    if(auto err=CheckState()){error_=*err;return false;}
    const bool ok=frontend_.Command(kind,body,response);
    if(!ok){error_=frontend_.Error();return false;}
    if(auto err=CheckState()){error_=*err;response.clear();return false;}
    return true;
}
}
