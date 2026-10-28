// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 Till0196; Copyright (C) 2005-9 DiBcom, SA et al.
// C++ adaptation of coax dib8096 at 471328e03942219544e78004e961d8f69d27f46f.
#include "dib8096_native.hpp"
#include <cstring>
#include "dib8096_native_generated.inc"

namespace px4::torne::dibnative {
// Go の任意インターフェース部分だけを手書きで接続する。探索本体と TMCC の
// 展開は生成コードを使い、未対応のバスは上流と同じ通常探索へ戻す。
Error m_Demod_setFrontend(T_Demod* board,std::uint32_t frequency) {
    auto* bus=board->v_demod->v_bus;
    const auto key=frequency/1000;
    auto recall=bus->RecallTMCC(key);
    if(auto error=std::get<3>(recall))return error;
    bool recalled=false;
    if(std::get<1>(recall)) {
        auto words=std::get<0>(recall);
        if(auto error=m_Demod_attempt(board,frequency,&words))return error;
        recalled=board->v_demod->v_status==v_statusDataLocked;
    }
    if(!recalled)if(auto error=m_Demod_attempt(board,frequency,nullptr))return error;
    if(board->v_failure.v_err || !std::get<2>(recall) || board->v_demod->v_status!=v_statusDataLocked ||
        !(m_dib8000_readStatus(board->v_demod)&v_feHasLock))return board->v_failure.v_err;
    auto saved=bus->RememberTMCC(v_Address,key);
    if(auto error=std::get<3>(saved))return error;
    if(recalled && std::get<1>(saved)) {
        auto words=std::get<0>(saved);
        return m_Demod_attempt(board,frequency,&words);
    }
    return nullptr;
}
void m_tuner_captrimOnMaster(T_tuner* state,std::uint16_t lo4) {
    if(state->v_failure->v_err)return;
    auto result=state->v_bus->Captrim(state->v_address,lo4,state->v_channel->v_frequency/1000,
        state->v_captrim,state->v_step,state->v_rest==0?200:400,3000000,4000000);
    if(!std::get<2>(result))return;
    if(auto error=std::get<3>(result)) {
        m_failure_record(state->v_failure,MakeError("DiB0090 capacitor trim search",error));
        return;
    }
    state->v_fcaptrim=static_cast<std::int8_t>(std::get<0>(result));
    state->v_adcDiff=static_cast<std::int16_t>(std::get<1>(result));
    state->v_tuneState=v_tunerStep3;
}
void m_dib8000_modifyWord(T_dib8000* state,std::uint16_t reg,std::uint16_t keep,std::uint16_t set)
{
    // 旧Go helperのproxyBusはModifierを実装しないため、同じread-modify-writeを使う。
    if(state->v_failure->v_err)return;
    const auto value=m_dib8000_readWord(state,reg);
    m_dib8000_writeWord(state,reg,static_cast<std::uint16_t>((value&keep)|set));
}
class GatedBus final: public Bus {
    Bus& parent_;
    T_i2cMaster& master_;
public:
    GatedBus(Bus& parent,T_i2cMaster& master):parent_(parent),master_(master){}
    std::tuple<Slice<std::uint8_t>,Error> WriteRead(std::uint8_t address,Slice<std::uint8_t> write,std::int64_t length) override {
        if(master_.v_gate!=address){
            m_i2cMaster_selectInterface(&master_,v_interfaceTuner);
            m_dib8000_writeWord(master_.v_demod,static_cast<std::uint16_t>(master_.v_baseRegister+1),m_i2cMaster_gateControl(&master_,address>>1,true));
            if(master_.v_demod->v_failure->v_err)return {{},master_.v_demod->v_failure->v_err};
            master_.v_gate=address;
        }
        return parent_.WriteRead(address,write,length);
    }
    Error Wait(std::int64_t duration) override{return parent_.Wait(duration);}
    std::tuple<std::int64_t,std::int64_t,bool,Error> Captrim(std::uint8_t address,std::uint16_t lo4,
        std::uint32_t key,std::int64_t start,std::int64_t step,std::int64_t target,std::int64_t write,std::int64_t read) override {
        if(master_.v_gate!=address) {
            m_i2cMaster_selectInterface(&master_,v_interfaceTuner);
            m_dib8000_writeWord(master_.v_demod,static_cast<std::uint16_t>(master_.v_baseRegister+1),m_i2cMaster_gateControl(&master_,address>>1,true));
            if(master_.v_demod->v_failure->v_err)return {0,0,true,master_.v_demod->v_failure->v_err};
            master_.v_gate=address;
        }
        return parent_.Captrim(address,lo4,key,start,step,target,write,read);
    }
};
}
namespace px4::torne {
using namespace dibnative;
struct NativeFrontend::State {
    T_Demod board{};
    T_dib8000 demod{};
    T_tuner tuner{};
    std::unique_ptr<T_dib0090Config> config;
    GatedBus gated;
    State(Bus& bus):config(v_dib809xDiB0090Config()),gated(bus,demod.v_master){
        board.v_demod=&demod;board.v_tuner=&tuner;
        auto pause=[this,&bus](std::int64_t duration){m_failure_record(&board.v_failure,bus.Wait(duration));};
        demod.v_bus=&bus;demod.v_address=v_Address;demod.v_failure=&board.v_failure;
        demod.v_cfg=v_dib809xDiB8000Config();demod.v_channel=&board.v_channel;
        demod.v_sleep=pause;demod.v_now=Now;demod.v_tuneTimeout=v_defaultTuneTimeout;
        tuner.v_bus=&gated;tuner.v_address=v_tunerAddress;tuner.v_failure=&board.v_failure;
        tuner.v_config=config.get();tuner.v_channel=&board.v_channel;tuner.v_sleep=pause;
        tuner.v_resetLine=[this](std::int64_t on){m_dib8000_setGPIO(&demod,5,0,static_cast<std::uint8_t>(v_boolWord(on==0)));};
        tuner.v_sleepLine=[this](std::int64_t on){m_dib8000_setGPIO(&demod,0,0,static_cast<std::uint8_t>(on));};
        demod.v_agcControl=[this](std::uint8_t restart){m_tuner_dccFreq(&tuner,restart);if(restart==0)m_tuner_setDCServo(&tuner,1);};
        tuner.v_getADCPower=[this](){return m_dib8000_getADCPower(&demod,1);};
        demod.v_tunerSetParams=[this](){return m_Demod_setParamOverride(&board);};
        demod.v_tunerFrequency=[this](){return m_tuner_getFrequency(&tuner);};
    }
};
NativeFrontend::NativeFrontend(Bus& bus):state_(std::make_unique<State>(bus)){}
NativeFrontend::~NativeFrontend()=default;
void NativeFrontend::SetClock(std::function<Time()> now){state_->demod.v_now=std::move(now);}
bool NativeFrontend::Command(std::uint8_t kind,const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& response){
    try{return Execute(kind,body,response);}
    catch(const std::exception& e){error_=e.what();response.clear();initialized_=false;return false;}
}
bool NativeFrontend::Execute(std::uint8_t kind,const std::vector<std::uint8_t>& body,std::vector<std::uint8_t>& response){
    response.clear();error_.clear();auto* board=&state_->board;dibnative::Error err;
    if(!initialized_&&kind!=0x10){error_="frontend is not initialized";return false;}
    switch(kind){
    case 0x10:
        if(!body.empty()){error_="invalid initialize command";return false;}
        err=m_Demod_Initialize(board);if(!err)err=m_Demod_SetGPIO(board,1,true);initialized_=!err;break;
    case 0x11:{
        if(body.size()!=4){error_="invalid tune command";return false;}
        const auto hz=std::uint32_t(body[0])|(std::uint32_t(body[1])<<8)|(std::uint32_t(body[2])<<16)|(std::uint32_t(body[3])<<24);
        err=m_Demod_Tune(board,hz);if(!err&&m_Demod_SearchFailed(board))response={1};break;}
    case 0x12:
        if(body.size()!=1||body[0]>v_OutputFIFO){error_="invalid output command";return false;}
        err=m_Demod_SetOutputMode(board,body[0]);break;
    case 0x13:{
        if(!body.empty()){error_="invalid status command";return false;}
        auto result=m_Demod_Status(board);err=std::get<1>(result);if(err)break;
        const auto& status=std::get<0>(result);response.resize(24);
        response[0]=status.v_Locked;response[1]=status.v_HasCNR;response[2]=status.v_HasStrength;
        // Windows x86/x64はlittle endian。元helperと同じIEEE754バイト列を維持する。
        static_assert(sizeof(double)==8);std::memcpy(response.data()+8,&status.v_CNR,8);std::memcpy(response.data()+16,&status.v_StrengthDBm,8);break;}
    case 0x14:
        if(!body.empty()){error_="invalid sleep command";return false;}
        err=m_Demod_SetGPIO(board,1,false);if(!err)err=m_Demod_Sleep(board);break;
    default:error_="unknown frontend command";return false;
    }
    if(err){error_=*err;response.clear();return false;}return true;
}
}
