// SPDX-License-Identifier: GPL-2.0-only
// siano::Session (WinUSB 版と Linux 版の共有の制御) を模擬の USB で試す。
// firmware の切り替えの順序、前のホストの残りの読み飛ばし、応答の対応、ロック前の TS の除外、失敗の扱いを確かめる。
#include "siano/session.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

using namespace px4::siano;
namespace {
void Require(bool ok, const char* what) {if (!ok) {std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}

constexpr std::uint8_t ModeRom=255, ModeIsdbt=6, ModeJ83b=7;

std::vector<std::uint8_t> Firmware(std::size_t body)
{
    std::vector<std::uint8_t> f(12+body,0);
    sms_put_le32(f.data()+4,static_cast<std::uint32_t>(body));
    sms_put_le32(f.data()+8,0x40260);
    return f;
}

struct FakeHelper final : IqProcess {
    int start_error=0; unsigned starts=0,stops=0,fed=0; std::size_t started_after=0;
    bool running=false; std::int32_t mer=-1; std::uint64_t last=0;
    std::size_t* sent_count=nullptr;
    int Start(TsHandler) override {++starts;if (sent_count) started_after=*sent_count;if (start_error) return start_error;running=true;return 0;}
    void Feed(const std::uint8_t*,std::size_t) override {++fed;}
    void Stop() override {++stops;running=false;}
    bool Running() const override {return running;}
    bool Ready() const override {return running;}
    bool Failed() const override {return false;}
    std::uint64_t LastTs() const override {return last;}
    std::uint32_t Quality() const override {return 0;}
    std::int32_t MerX100() const override {return mer;}
    std::uint64_t Dropped() const override {return 0;}
};

struct FakePort final : SessionPort {
    Session* session=nullptr;
    std::vector<std::uint16_t> sent; std::size_t sent_count=0;
    std::uint8_t mode=ModeIsdbt; std::size_t downloaded=0;
    std::vector<std::uint8_t> isdbt=Firmware(64),j83b=Firmware(96);
    int send_error=0,failed=0; bool stale=false,recovery_during_reload=false;
    std::vector<std::uint8_t> ts;
    unsigned started=0,stopped=0;

    void Respond(std::uint16_t type,const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> b(SMS_HEADER_SIZE+payload.size());
        sms_pack_header(b.data(),type,0,0,static_cast<std::uint16_t>(b.size()),0);
        std::copy(payload.begin(),payload.end(),b.begin()+SMS_HEADER_SIZE);
        Deliver(b);
    }
    void Deliver(const std::vector<std::uint8_t>& b)
    {
        session->Receive(session->Ticket(),b.data(),b.size(),504,[this](const std::uint8_t* p,std::size_t n) {
            ts.insert(ts.end(),p,p+n);return true;
        });
    }
    static std::vector<std::uint8_t> Words(std::initializer_list<std::uint32_t> words)
    {
        std::vector<std::uint8_t> p(words.size()*4);std::size_t i=0;
        for (auto w:words) {sms_put_le32(p.data()+i,w);i+=4;}
        return p;
    }
    int Send(const std::uint8_t* data,std::size_t size,Clock::time_point) override
    {
        const auto type=sms_get_le16(data);
        sent.push_back(type);++sent_count;
        if (send_error) return send_error;
        switch (type) {
        case MSG_SMS_GET_VERSION_EX_REQ: {
            std::vector<std::uint8_t> p(12,0);
            p[4]=mode;p[5]=mode==ModeRom ? 0 : static_cast<std::uint8_t>(1U<<ModeIsdbt);p[10]=8;p[11]=1;
            Respond(MSG_SMS_GET_VERSION_EX_RES,p);
            break;
        }
        case MSG_SW_RELOAD_START_REQ:
            // 実機の J.83B 用の firmware と同じく、応答せずに ROM へ戻る
            recovery_during_reload=session->ReloadRecovery();
            mode=ModeRom;downloaded=0;
            break;
        case MSG_SMS_DATA_DOWNLOAD_REQ:
            downloaded+=size-SMS_HEADER_SIZE-4;Respond(MSG_SMS_DATA_DOWNLOAD_RES,{});
            break;
        case MSG_SMS_DATA_VALIDITY_REQ: Respond(MSG_SMS_DATA_VALIDITY_RES,{}); break;
        case MSG_SMS_SWDOWNLOAD_TRIGGER_REQ:
            // 書いた大きさ (ヘッダーを含むファイル長) で、どちらの firmware かを見分ける
            mode=downloaded==j83b.size() ? ModeJ83b : ModeIsdbt;
            Respond(MSG_SMS_SWDOWNLOAD_TRIGGER_RES,{});
            break;
        case MSG_SMS_INIT_DEVICE_REQ: Respond(MSG_SMS_INIT_DEVICE_RES,{}); break;
        case MSG_SMS_ISDBT_TUNE_REQ: Respond(MSG_SMS_ISDBT_TUNE_RES,{}); break;
        case MSG_IQ_STREAM_REQ: Respond(MSG_IQ_STREAM_RES,Words({0,10,10,0,0,0,0})); break;
        case MSG_SMS_GET_STATISTICS_EX_REQ: {
            // sms_isdbt_stats_ex: result、is_rf_locked など。階層の segments は 255 (なし)
            std::vector<std::uint8_t> p(4+76+48*3,0);
            sms_put_le32(p.data()+4,1);sms_put_le32(p.data()+4+12,1);sms_put_le32(p.data()+4+20,25);
            for (int i=0;i<3;++i) sms_put_le32(p.data()+80+48*i+40,255);
            Respond(MSG_SMS_GET_STATISTICS_EX_RES,p);
            break;
        }
        default: break;
        }
        return 0;
    }
    bool Removed() const override {return false;}
    int Delay(unsigned) override {return 0;}
    int StartReceive() override
    {
        ++started;
        // 前のホストが残した、メッセージの途中から始まる転送 (ヘッダーとして長さが不正)
        if (stale) {std::vector<std::uint8_t> b(64,0x47);b[4]=b[5]=0;Deliver(b);}
        return 0;
    }
    void StopReceive() override {++stopped;}
    void Failed(int error) override {failed=error;}
    bool AdvanceEpoch() override {return true;}
    int ReadFirmware(FirmwareKind kind,std::vector<std::uint8_t>& f) override {f=kind==FirmwareKind::J83b ? j83b : isdbt;return 0;}
    std::function<void(const std::uint8_t*,std::size_t)> HelperWriter() override {return [](const std::uint8_t*,std::size_t){};}
    unsigned PendingReads() override {return 32;}
    bool SignalLogEnabled() override {return true;}
};

std::vector<std::uint8_t> TsFrame(unsigned packets)
{
    std::vector<std::uint8_t> b(SMS_HEADER_SIZE+188*packets,0);
    sms_pack_header(b.data(),MSG_SMS_DVBT_BDA_DATA,0,0,static_cast<std::uint16_t>(b.size()),0);
    for (unsigned i=0;i<packets;++i) {b[SMS_HEADER_SIZE+i*188]=0x47;b[SMS_HEADER_SIZE+i*188+3]=0x10;}
    return b;
}

void IsdbtAndJ83b()
{
    FakePort port;FakeHelper helper;port.stale=true;helper.sent_count=&port.sent_count;
    Session session(port,&helper,32);port.session=&session;
    Require(session.Open()==0 && session.IsOpen(),"open an ISDB-T device");
    Require(port.sent==std::vector<std::uint16_t>{MSG_SMS_GET_VERSION_EX_REQ},"running ISDB-T is not initialized again");
    Require(session.Malformed()==0,"stale transfer before the first message is skipped");

    port.sent.clear();
    Require(session.Tune(Session::System::Isdbt,473143)==0,"ISDB-T tune");
    Require(port.sent==std::vector<std::uint16_t>({MSG_SMS_ISDBT_TUNE_REQ,MSG_SMS_ADD_PID_FILTER_REQ}),"tune and all-PID filter");
    Require(session.SetCapture(true)==0,"capture");
    // PlainFramer は 3 パケットの同期で TS の境界を決めるため、4 パケットずつ渡す
    port.Deliver(TsFrame(4));
    Require(port.ts.empty(),"TS before the lock is not delivered");
    bool locked=false;std::int32_t cnr=0;
    Require(session.CheckLock(locked,cnr)==0 && locked && cnr==25000,"lock and SNR from EX statistics");
    port.Deliver(TsFrame(4));
    Require(port.ts.size()==188*4,"TS after the lock is delivered");

    Require(session.SetCapture(false)==0,"stop capture");
    port.sent.clear();port.ts.clear();
    Require(!session.ReloadRecovery(),"no receive recovery outside a firmware reload");
    const auto before=port.sent_count;
    Require(session.Tune(Session::System::J83b,138750)==0,"J.83B tune");
    Require(helper.starts==1 && helper.started_after==before,"helper starts before the firmware switch");
    Require(port.recovery_during_reload,"receive recovery is allowed during the reload");
    Require(!session.ReloadRecovery(),"receive recovery ends with the reload");
    Require(port.mode==ModeJ83b && port.sent.back()==MSG_IQ_STREAM_REQ,"J.83B firmware loaded and IQ stream started");

    std::vector<std::uint8_t> iq(SMS_HEADER_SIZE+IqBlockBytes,0);
    sms_pack_header(iq.data(),MSG_IQ_DATA,0,0,static_cast<std::uint16_t>(iq.size()),0);
    port.Deliver(iq);
    Require(helper.fed==1,"IQ block goes to the helper");
    helper.last=MonotonicMs();helper.mer=3150;
    Require(session.CheckLock(locked,cnr)==0 && locked && cnr==31500,"J.83B lock and MER");

    port.sent.clear();
    session.Close();
    Require(!session.IsOpen() && helper.stops==1,"close stops the helper");
    Require(port.sent.front()==MSG_IQ_STREAM_REQ && port.mode==ModeIsdbt,"close stops IQ and reloads the ISDB-T firmware");
    Require(port.failed==0 && session.Error()==0,"no failure");

    // 地デジの firmware のまま開き直し、J.83B の後に地デジへ戻る選局
    Require(session.Open()==0,"reopen");
    Require(session.Tune(Session::System::J83b,138750)==0 && port.mode==ModeJ83b,"J.83B again");
    port.sent.clear();
    Require(session.Tune(Session::System::Isdbt,479143)==0 && port.mode==ModeIsdbt,"ISDB-T after J.83B reloads the firmware");
    Require(port.sent.back()==MSG_SMS_ADD_PID_FILTER_REQ,"ISDB-T tune after the reload");
    session.Close();
}

void Failures()
{
    FakePort port;FakeHelper helper;
    Session session(port,&helper,32);port.session=&session;
    Require(session.Open()==0,"open");
    helper.start_error=-ENOENT;
    Require(session.Tune(Session::System::J83b,138750)==-ENOENT && session.Error()==0 && port.mode==ModeIsdbt,
        "helper start failure keeps the open usable and the firmware unchanged");
    Require(session.Tune(Session::System::J83b,80000)==-EINVAL,"J.83B frequency range");
    Require(session.Tune(Session::System::Isdbt,473143)==0,"initial ISDB-T tune");
    const auto sent=port.sent_count;
    Require(session.Tune(Session::System::Isdbt,467143)==-EINVAL && session.Error()==0 &&
        session.Tuned() && port.sent_count==sent,"invalid ISDB-T frequency keeps the current tune");
    Require(session.Tune(Session::System::Isdbt,479143)==0,"valid ISDB-T tune after invalid frequency");
    helper.start_error=0;
    port.send_error=-EIO;
    Require(session.Tune(Session::System::Isdbt,473143)==-EIO && session.Error()==-EIO && port.failed==-EIO,
        "control failure fails the open");
    bool locked=false;std::int32_t cnr=0;
    Require(session.CheckLock(locked,cnr)==-EIO,"later operations report the failure");
    port.send_error=0;
    session.Close();
    Require(!session.IsOpen() && port.mode==ModeIsdbt,"close after a failure");

    // 最初のメッセージを解釈した後の形式の異常は、受信を止める失敗
    FakePort p2;Session s2(p2,nullptr,32);p2.session=&s2;
    Require(s2.Open()==0 && !s2.J83bSupported(),"open without J.83B");
    Require(s2.Tune(Session::System::J83b,138750)==-EINVAL,"J.83B is rejected without a helper");
    std::vector<std::uint8_t> bad(64,0);
    const auto r=s2.Receive(s2.Ticket(),bad.data(),bad.size(),504,[](const std::uint8_t*,std::size_t){return true;});
    Require(r.error==-EBADMSG && s2.Malformed()==1 && s2.Error()==-EBADMSG,"malformed frame after synchronization");
}
}

int main()
{
    IsdbtAndJ83b();
    Failures();
    std::puts("PASS: Siano session firmware switching, stale skip, lock gating, J.83B helper order and failures");
}
