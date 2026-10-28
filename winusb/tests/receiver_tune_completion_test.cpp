// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cerrno>
#include <stdexcept>
#include "receiver_base.hpp"
extern "C" int msg_printf(const char *, ...) { return 0; }
namespace {
void Require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
class Hardware:public px4::ReceiverBase {
public:
    Hardware():ReceiverBase(0){params_.system=px4::SystemType::ISDB_T;params_.freq=473143;}
    bool physical_lock=false;
    int frequency_error=0;
    unsigned checks=0;
    int Open() override{return 0;}
    void Close() override{}
    int CheckLock(bool &locked) override{++checks;locked=physical_lock;return 0;}
    int SetLnbVoltage(std::int32_t) override{return 0;}
    int SetCapture(bool) override{return 0;}
    int ReadStat(px4::command::StatType,std::int32_t &value) override{value=physical_lock?1000:0;return 0;}
protected:
    int SetFrequency() override{return frequency_error;}
    int SetStreamId() override{return 0;}
};
class Software:public Hardware {
public:
    bool accepted=false;
    int request_error=0;
protected:
    int CheckTuneComplete(bool &complete) override{complete=accepted;return request_error;}
};
}
int main(){
    try{
        Hardware hardware;
        Require(!hardware.Tune(0),"Existing hardware accepted a tune without RF lock");
        Require(hardware.checks==1,"Default completion did not call CheckLock");
        hardware.physical_lock=true;
        Require(hardware.Tune(0),"Existing hardware failed with RF lock");
        Software software;
        Require(!software.Tune(0),"Unaccepted software request succeeded");
        software.accepted=true;
        Require(software.Tune(0),"Accepted software request failed");
        bool locked=true;
        Require(!software.CheckLock(locked)&&!locked,"Request acceptance falsely reported RF lock");
        std::int32_t quality=-1;
        Require(!software.ReadStat(px4::command::StatType::CNR,quality)&&quality==0,"Quality before TS was nonzero");
        software.frequency_error=-EIO;
        Require(!software.Tune(0),"Frequency setup failure was hidden");
        software.frequency_error=0;software.request_error=-ECANCELED;
        Require(!software.Tune(0),"Rejected request was hidden");
        std::puts("receiver_tune_completion_test: passed");return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"receiver_tune_completion_test: %s\n",e.what());return 1;}
}
