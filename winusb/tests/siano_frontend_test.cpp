// SPDX-License-Identifier: GPL-2.0-only
#include "siano/frontend.hpp"
#include "siano/capture_gate.hpp"
#include "siano/firmware_path.hpp"
#include "pxbcud/plain_framer.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <string>

using namespace px4::siano;
namespace {
void Require(bool ok,const char* what) {if (!ok) {std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
struct Mock final : Io {
    std::vector<std::vector<std::uint8_t>> messages;
    std::vector<std::uint16_t> responses;
    int mode=255, protocols=0, version=0x800, fail_type=0;
    bool short_version=false, old_mode_after_init=false;
    int Delay(unsigned ms) override {Require(ms==400,"firmware settle");return 0;}
    int Exchange(const std::vector<std::uint8_t>& m,std::uint16_t response,
                 std::vector<std::uint8_t>& p,unsigned timeout) override
    {
        messages.push_back(m);responses.push_back(response);p.clear();
        Require(m.size()>=8 && sms_get_le16(m.data()+4)==m.size(),"header length");
        auto type=sms_get_le16(m.data());
        Require(timeout==(type==MSG_SMS_GET_STATISTICS_EX_REQ || type==MSG_SMS_GET_STATISTICS_REQ ? 500U:10000U),"bounded timeout");
        if (type==fail_type) return -ETIMEDOUT;
        if (type==MSG_SMS_GET_VERSION_EX_REQ) {
            p.resize(short_version?11:12);p[4]=static_cast<std::uint8_t>(mode);p[5]=static_cast<std::uint8_t>(protocols);
            p[10]=static_cast<std::uint8_t>(version>>8);if (p.size()>11) p[11]=static_cast<std::uint8_t>(version);
        }
        if (type==MSG_SMS_SWDOWNLOAD_TRIGGER_REQ || type==MSG_SW_RELOAD_EXEC_REQ ||
            (type==MSG_SMS_INIT_DEVICE_REQ && !old_mode_after_init)) mode=6;
        if (type==MSG_SMS_ISDBT_TUNE_REQ) p.resize(4);
        return 0;
    }
};
std::vector<std::uint8_t> Firmware(unsigned n=256)
{
    std::vector<std::uint8_t> f(12+n);sms_put_le32(f.data()+4,n);sms_put_le32(f.data()+8,0x400000);
    for (unsigned i=0;i<n;++i) f[12+i]=static_cast<std::uint8_t>(i);
    return f;
}
void Download()
{
    Mock m;Frontend f(m);auto fw=Firmware();Require(f.Initialize(fw)==0,"cold firmware boot");
    Require(m.messages.size()==6,"cold command count");
    Require(sms_get_le16(m.messages[1].data())==MSG_SMS_DATA_DOWNLOAD_REQ,"download command");
    Require(m.messages[1].size()==252,"240-byte chunk");
    Require(sms_get_le32(m.messages[2].data()+8)==0x400000+240,"second address");
    Require(m.messages[2].size()==40,"file length includes 12 zero bytes");
    Require(std::all_of(m.messages[2].end()-12,m.messages[2].end(),[](auto x){return x==0;}),"deterministic padding");
    Mock warm;warm.mode=6;Frontend w(warm);Require(w.Initialize({})==0 && warm.messages.size()==1,"warm does not resend INIT");
    Mock reload;reload.mode=4;Frontend r(reload);Require(r.Initialize(fw)==0,"reload mode");
    Require(sms_get_le16(reload.messages[1].data())==MSG_SW_RELOAD_START_REQ,"reload start");
    Require(sms_get_le32(reload.messages[2].data()+8)==sms_get_le32(fw.data()+32),"reload payload[20] address");
    Require(reload.responses[reload.responses.size()-2]==0,"reload exec has no awaited ACK");
    Mock bad;Frontend b(bad);auto truncated=fw;truncated.pop_back();Require(b.Initialize(truncated)==-EBADMSG,"truncated firmware");
    Require(bad.messages.size()==1,"invalid firmware never sent");
    Mock missing;Frontend absent(missing);Require(absent.Initialize({})==-ENOENT,"missing cold firmware is distinct");
    Mock fail;fail.fail_type=MSG_SMS_DATA_DOWNLOAD_REQ;Frontend x(fail);Require(x.Initialize(fw)==-ETIMEDOUT,"download error propagates");
    Require(fail.messages.size()==2,"download failure stops sequence");
    Mock shortv;shortv.short_version=true;Frontend sv(shortv);Require(sv.Initialize(fw)==-EBADMSG,"short version");
    Mock init;init.mode=4;init.protocols=1<<6;Frontend i(init);Require(i.Initialize({})==0,"supported mode INIT");
    Require(init.messages.size()==3,"INIT followed by mode confirmation");
    Mock wrong;wrong.mode=4;wrong.protocols=1<<6;wrong.old_mode_after_init=true;Frontend e(wrong);
    Require(e.Initialize({})==-EIO,"wrong mode rejected");
}
void TuneAndStats()
{
    Mock m;m.mode=6;Frontend f(m);Require(f.Initialize({})==0,"warm start");
    Require(f.Tune(557143)==0,"tune");
    auto& tune=m.messages[1];Require(tune[2]==201 && sms_get_le32(tune.data()+8)==557142857,"kHz to exact Japan Hz");
    Require(sms_get_le32(tune.data()+12)==8 && sms_get_le32(tune.data()+16)==12000000,"13seg bandwidth and crystal");
    Require(m.responses.back()==0 && sms_get_le32(m.messages.back().data()+8)==0x2000,"catch-all never waits for ACK");
    Require(f.Tune(557142)==0 && sms_get_le32(m.messages[m.messages.size()-2].data()+8)==557142857,"truncated kHz precision");
    auto before=m.messages.size();Require(f.Tune(100)==-EINVAL && m.messages.size()==before,"invalid frequency no command");
    std::vector<std::uint8_t> p;Require(f.RequestStats(p)==0 && m.responses.back()==MSG_SMS_GET_STATISTICS_EX_RES,"EX statistics");
    bool locked=true;std::int32_t cnr=999;p.resize(28);sms_put_le32(p.data()+4,6);sms_put_le32(p.data()+16,1);sms_put_le32(p.data()+24,31);
    Require(f.DecodeStats(p,locked,cnr)==0 && locked && cnr==31000,"statistics offsets and milli-dB");
    sms_put_le32(p.data()+16,0);Require(f.DecodeStats(p,locked,cnr)==0 && !locked && !cnr,"lock loss clears CNR");
    p.resize(27);Require(f.DecodeStats(p,locked,cnr)==-EBADMSG,"short stats");
    px4::siano::SignalStats s{};p.assign(4+76+48*3,0);
    Require(f.DecodeSignal(std::vector<std::uint8_t>(p.begin(),p.end()-1),s)==-EBADMSG,"short signal stats rejected");
    sms_put_le32(p.data()+4+12,1);sms_put_le32(p.data()+4+20,static_cast<std::uint32_t>(-3));
    sms_put_le32(p.data()+4+24,static_cast<std::uint32_t>(-62));sms_put_le32(p.data()+4+36,557142857);sms_put_le32(p.data()+4+64,2);
    const auto layer=[&](unsigned i,std::uint32_t seg,std::uint32_t ber,std::uint32_t err,std::uint32_t total,std::uint32_t tmcc) {
        auto* o=p.data()+4+76+48*i;sms_put_le32(o+8,ber);sms_put_le32(o+28,err);sms_put_le32(o+32,total);
        sms_put_le32(o+40,seg);sms_put_le32(o+44,tmcc);
    };
    layer(0,1,0xffffffff,0,100,0);layer(1,12,25,7,4000,1);layer(2,255,0,0,0,0);
    Require(f.DecodeSignal(p,s)==0 && s.demod_locked && !s.rf_locked && s.snr==-3 && s.rssi==-62 &&
        s.frequency==557142857 && s.layers==2,"signal common fields");
    Require(s.layer[0].segments==1 && s.layer[0].ber==0xffffffff && s.layer[0].total_ts==100 &&
        s.layer[1].segments==12 && s.layer[1].ber==25 && s.layer[1].error_ts==7 && s.layer[1].total_ts==4000 &&
        s.layer[1].tmcc_errors==1 && s.layer[2].segments==255,"per-layer stats offsets");
    sms_put_le32(p.data(),1);Require(f.DecodeSignal(p,s)==-EBADMSG,"failed EX result rejected");
    Mock old;old.mode=6;old.version=0x700;Frontend o(old);Require(o.Initialize({})==0,"legacy warm");
    p.assign(24,0);sms_put_le32(p.data()+12,1);sms_put_le32(p.data()+20,99);
    Require(o.DecodeStats(p,locked,cnr)==0 && locked && !cnr,"legacy unknown signal field is not CNR");
    Require(o.DecodeSignal(p,s)==-ENOSYS,"legacy layout is not decoded as EX");
}
void Frames()
{
    std::vector<std::uint8_t> b(1024);sms_frame fr{};
    sms_pack_header(b.data(),MSG_SMS_DVBT_BDA_DATA,0,0,196,0);
    Require(sms_frame_message(b.data(),196,504,&fr)==0 && fr.payload_length==188 && fr.payload_offset==8,"plain frame");
    sms_put_le16(b.data()+6,4|0x200);
    Require(sms_frame_message(b.data(),702,504,&fr)==0 && fr.payload_offset==514,"split frame alignment");
    Require(sms_frame_message(b.data(),701,504,&fr)==-EMSGSIZE,"truncated split frame");
    sms_put_le16(b.data()+4,7);Require(sms_frame_message(b.data(),b.size(),504,&fr)==-EBADMSG,"invalid wire length");
    Require(sms_frame_message(b.data(),7,504,&fr)==-EINVAL,"short header");
    px4::pxbcud::PlainFramer ts;std::vector<std::uint8_t> packet(188*4),out;
    for (unsigned n=0;n<4;++n) {packet[n*188]=0x47;packet[n*188+3]=0x10;}
    ts.Feed(packet.data(),17,out);ts.Feed(packet.data()+17,packet.size()-17,out);
    Require(out==packet,"TS partial transfer preserves all packets");
    ts.Feed(packet.data(),17,out);ts.Gap();out.clear();ts.Feed(packet.data(),packet.size(),out);
    Require(out==packet,"retune discards partial packet");
}
void Generations()
{
    CaptureGate gate;std::vector<std::uint8_t> packet(188*4),out;
    for (unsigned n=0;n<4;++n) {packet[n*188]=0x47;packet[n*188+3]=0x10;}
    auto write=[&](const std::uint8_t* p,std::size_t n) {out.insert(out.end(),p,p+n);return true;};
    const auto disabled=gate.Ticket();
    gate.SetEnabled(true);auto current=gate.Ticket();
    Require(gate.Feed(disabled,packet.data(),packet.size(),write) && out.empty(),"pre-lock submitted read discarded");
    Require(gate.Feed(current,packet.data(),17,write) && out.empty(),"partial TS held");
    gate.SetEnabled(false);gate.SetEnabled(true);
    Require(gate.Feed(current,packet.data(),packet.size(),write) && out.empty(),"late previous-channel completion discarded");
    Require(gate.Feed(gate.Ticket(),packet.data(),packet.size(),write) && out==packet,"new channel excludes old partial tail");
    Require(!gate.Feed(gate.Ticket(),packet.data(),packet.size(),[](auto,auto){return false;}),"writer failure propagates");
    out.clear();current=gate.Ticket();gate.SetEnabled(false);
    Require(gate.Feed(current,packet.data(),packet.size(),write) && out.empty(),"stop prevents late output");
}
void FirmwarePaths()
{
    namespace fs=std::filesystem;
    const auto original=fs::current_path();
    const auto build=(original/L"build").lexically_normal();fs::create_directories(build);
    const auto root=build/(L"siano-firmware-path-test-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(root),"new firmware test directory");
    const auto host=root/L"host 日本語";fs::create_directories(host/L"firmware");
    const auto flat=host/L"isdbt_rio.inp",sub=host/L"firmware"/L"isdbt_rio.inp";
    const auto custom=root/L"custom.inp";
    auto touch=[](const fs::path& p) {std::ofstream f(p,std::ios::binary);f<<"test";Require(static_cast<bool>(f),"create firmware fixture");};
    touch(flat);
    Require(ResolveFirmwarePath(host,{})==flat,"EXE directory firmware found");
    touch(sub);
    Require(ResolveFirmwarePath(host,{})==flat,"EXE directory preferred over default subfolder");
    Require(ResolveFirmwarePath(host,L"firmware/isdbt_rio.inp")==sub,"explicit relative path keeps priority");
    touch(custom);
    Require(ResolveFirmwarePath(host,custom)==custom,"explicit absolute path keeps priority");
    Require(ResolveFirmwarePath(host,L"missing.inp")==flat,"missing configured path falls back to EXE directory");
    auto candidates=FirmwareCandidates(host,L"./firmware/isdbt_rio.inp");
    Require(candidates.size()==2 && candidates[0]==sub && candidates[1]==flat,"duplicate paths removed");
    fs::remove(flat);
    Require(ResolveFirmwarePath(host,{})==sub,"existing distribution subfolder remains supported");
    Require(ResolveFirmwarePath(host,L"firmware/isdbt_rio.inp")==sub,"configured distribution path remains supported");
    fs::create_directory(flat);
    Require(ResolveFirmwarePath(host,{})==sub,"same-name directory is skipped");
    fs::remove(flat);touch(flat);
    fs::create_directory(root/L"other cwd");fs::current_path(root/L"other cwd");
    Require(ResolveFirmwarePath(host,{})==flat,"current directory cannot redirect firmware search");
    fs::current_path(original);
    fs::remove(flat);fs::remove(sub);
    Require(ResolveFirmwarePath(host,L"missing.inp")==host/L"missing.inp","missing firmware preserves diagnostic candidate");
    touch(flat);
    Require(ResolveFirmwarePath(host,{})==flat,"files added after initialization are found on re-open");
    Require(ResolveFirmwarePath(host,L"firmware/isdbt_rio.inp")==flat,"default INI works with firmware placed beside EXE");
    Require(root.parent_path()==build && root.filename().wstring().find(L"siano-firmware-path-test-")==0,"cleanup is confined to test directory");
    fs::remove_all(root);
}
}
int main() {Download();TuneAndStats();Frames();Generations();FirmwarePaths();std::puts("PASS: Siano boot/reload, warm mode, tune, PID, errors, stats, split frames, capture generations and firmware paths");}
