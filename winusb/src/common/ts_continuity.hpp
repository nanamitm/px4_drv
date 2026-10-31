// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace px4 {
// TS を変更せず観察する。PID ごとの直前パケットは正当な再送と内容違いの同一 CC を区別するために保持する。
class TsContinuityMonitor final {
public:
    enum class Kind {CcJump,RepeatChanged,DuplicateRun,AdaptationCc,Tei,Sync,Malformed};
    struct Event {Kind kind;std::uint64_t packet;unsigned pid;int previous,expected,actual;};
    struct Counters {
        std::uint64_t packets=0,cc_jump=0,repeat_changed=0,duplicate_run=0,adaptation_cc=0;
        std::uint64_t tei=0,sync=0,resync_bytes=0,malformed=0,duplicates=0;
    };
    void Reset() noexcept {++serial_;pending_size_=0;locked_=true;counts_={};}
    const Counters& Counts() const noexcept {return counts_;}
    template<class Report>
    void Feed(const void* input,std::size_t size,Report report) noexcept
    {
        const auto* p=static_cast<const std::uint8_t*>(input);
        while (size) {
            // 境界を失った後は次の 0x47 までを読み飛ばし、以降の CC 解析を続けられるようにする。
            if (!locked_ && !pending_size_) {
                const auto* s=static_cast<const std::uint8_t*>(std::memchr(p,0x47,size));
                const std::size_t skip=s ? static_cast<std::size_t>(s-p) : size;
                counts_.resync_bytes+=skip;p+=skip;size-=skip;
                if (!size) break;
            }
            auto n=(size<188-pending_size_) ? size : 188-pending_size_;
            std::memcpy(pending_.data()+pending_size_,p,n);pending_size_+=n;p+=n;size-=n;
            if (pending_size_==188) {
                if (pending_[0]==0x47) {locked_=true;Packet(pending_.data(),report);pending_size_=0;}
                else Resync(report);
            }
        }
    }
    static const char* Name(Kind k) noexcept
    {
        switch(k) {
        case Kind::CcJump:return "cc_jump";case Kind::RepeatChanged:return "same_cc_changed";
        case Kind::DuplicateRun:return "duplicate_run";case Kind::AdaptationCc:return "adaptation_cc";
        case Kind::Tei:return "tei";case Kind::Sync:return "sync";default:return "malformed";
        }
    }
private:
    struct Pid {std::uint64_t serial=0;std::uint8_t cc=0,repeats=0;std::array<std::uint8_t,188> packet{};};
    static bool SamePacket(const std::uint8_t* a,const std::uint8_t* b) noexcept
    {
        if (!std::memcmp(a,b,188)) return true;
        // MPEG-TS の再送では PCR の値だけが更新される場合がある。
        return (a[3]&0x20) && (b[3]&0x20) && a[4]>=7 && b[4]>=7 &&
            (a[5]&0x10) && (b[5]&0x10) && !std::memcmp(a,b,6) && !std::memcmp(a+12,b+12,176);
    }
    // 同期の喪失は1回として数え、欠落区間をまたぐ CC 比較を避けるため全 PID の基準を捨てる。
    // 候補の 0x47 がペイロード内の値だった場合も、次の188バイトで再び検証される。
    template<class Report> void Resync(Report report) noexcept
    {
        if (locked_) {
            ++counts_.sync;++serial_;locked_=false;
            report(Event{Kind::Sync,counts_.packets,0x1fff,-1,-1,-1});
        }
        const auto* s=static_cast<const std::uint8_t*>(std::memchr(pending_.data()+1,0x47,187));
        const std::size_t skip=s ? static_cast<std::size_t>(s-pending_.data()) : 188;
        counts_.resync_bytes+=skip;
        std::memmove(pending_.data(),pending_.data()+skip,188-skip);pending_size_=188-skip;
    }
    template<class Report> void Packet(const std::uint8_t* p,Report report) noexcept
    {
        const auto number=counts_.packets++;
        const unsigned pid=((p[1]&31)<<8)|p[2],afc=(p[3]>>4)&3,c=p[3]&15;
        auto& old=pids_[pid];
        auto event=[&](Kind k) {report(Event{k,number,pid,old.serial==serial_ ? old.cc : -1,
            old.serial==serial_ ? ((afc&1) ? ((old.cc+1)&15) : old.cc) : -1,static_cast<int>(c)});};
        if (p[1]&0x80) {++counts_.tei;event(Kind::Tei);old.serial=0;return;}
        if (!afc || ((afc&2) && p[4]>(afc==2 ? 183 : 182))) {
            ++counts_.malformed;event(Kind::Malformed);old.serial=0;return;
        }
        if ((afc&2) && p[4] && (p[5]&0x80)) old.serial=0;
        if (pid==0x1fff) return;
        if (!(afc&1)) {
            if (old.serial==serial_ && c!=old.cc) {++counts_.adaptation_cc;event(Kind::AdaptationCc);old.serial=0;}
            return;
        }
        unsigned repeats=0;
        if (old.serial==serial_) {
            if (c==old.cc) {
                if (!SamePacket(old.packet.data(),p)) {++counts_.repeat_changed;event(Kind::RepeatChanged);}
                else {
                    ++counts_.duplicates;repeats=old.repeats+1;
                    if (repeats>1) {++counts_.duplicate_run;event(Kind::DuplicateRun);repeats=2;}
                }
            } else if (c!=((old.cc+1)&15)) {++counts_.cc_jump;event(Kind::CcJump);}
        }
        old.serial=serial_;old.cc=static_cast<std::uint8_t>(c);old.repeats=static_cast<std::uint8_t>(repeats);
        std::memcpy(old.packet.data(),p,188);
    }
    std::array<Pid,8192> pids_{};
    std::uint64_t serial_=1;
    std::array<std::uint8_t,188> pending_{};std::size_t pending_size_=0;bool locked_=true;
    Counters counts_{};
};
}
