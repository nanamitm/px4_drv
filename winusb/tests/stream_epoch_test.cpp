// SPDX-License-Identifier: MIT
#include "../src/common/stream_frame.hpp"
#include "../src/BonDriver_PX4/io_queue.hpp"
#include "receiver_base.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <windows.h>
extern "C" int msg_printf(const char*,...) {return 0;}
static void Check(bool ok,const char* reason){if(!ok){std::cerr<<reason<<"\n";ExitProcess(1);}}
template<class F> static void Until(F ready){for(unsigned i=0;i<2000;++i){if(ready())return;Sleep(1);}Check(false,"timed out");}

static void FrameTests(){
    for(unsigned fragment:{1u,7u,188u,4096u}) {
        std::vector<std::uint8_t> wire;
        for(unsigned epoch:{1u,2u}) {
            px4::StreamFrameHeader h{px4::StreamFrameMagic,epoch,376};
            const auto* p=reinterpret_cast<const std::uint8_t*>(&h);
            wire.insert(wire.end(),p,p+sizeof(h));wire.insert(wire.end(),376,static_cast<std::uint8_t>(epoch));
        }
        std::size_t offset=0;px4::StreamFrameReader reader;
        auto read=[&](void* p,std::size_t& count){count=(std::min)({count,std::size_t(fragment),wire.size()-offset});if(!count)return false;std::memcpy(p,wire.data()+offset,count);offset+=count;return true;};
        for(unsigned expected:{1u,1u,2u,2u}){
            std::uint8_t output[188]{};std::size_t size=sizeof(output);
            Check(reader.ReadChunk(output,size,read)&&size==188&&reader.Epoch()==expected,"fragmented frame/epoch");
            for(auto byte:output)Check(byte==expected,"header excluded from TS");
        }
    }
    for(unsigned scenario=0;scenario<6;++scenario){
        px4::StreamFrameHeader h{px4::StreamFrameMagic,1,188};
        if(scenario==0)h.magic=0;if(scenario==1)h.epoch=0;if(scenario==2)h.length=0;
        if(scenario==3)h.length=px4::StreamFrameMax+188;
        std::vector<std::uint8_t> wire(sizeof(h)+188);std::memcpy(wire.data(),&h,sizeof(h));
        if(scenario==4)wire.resize(sizeof(h)-1);if(scenario==5)wire.pop_back();
        std::size_t offset=0;px4::StreamFrameReader reader;std::uint8_t output[188];std::size_t size=188;
        Check(!reader.ReadChunk(output,size,[&](void* target,std::size_t& count){count=(std::min)(count,wire.size()-offset);std::memcpy(target,wire.data()+offset,count);offset+=count;return count!=0;}),"malformed/truncated frame accepted");
    }
}

// raw TLV と通常 TS を同じ世代付き通信で運び、ヘッダーや隣の世代が混ざらないことを確認する。
static void ByteFrameTests() {
    for (unsigned fragment : {1u, 7u, 188u, 4096u}) {
        for (unsigned length : {1u, 187u, 189u, 65537u, px4::StreamFrameMax}) {
            std::vector<std::uint8_t> wire;
            for (unsigned epoch : {1u, 2u}) {
                px4::StreamFrameHeader h{px4::StreamFrameMagic, epoch, length};
                const auto *p = reinterpret_cast<const std::uint8_t *>(&h);
                wire.insert(wire.end(), p, p + sizeof(h));
                for (unsigned i = 0; i < length; ++i) wire.push_back(std::uint8_t(i + epoch));
            }
            std::size_t offset = 0;
            px4::StreamFrameReader reader;
            auto read = [&](void *p, std::size_t &count) {
                count = (std::min)({count, std::size_t(fragment), wire.size() - offset});
                if (!count) return false;
                std::memcpy(p, wire.data() + offset, count); offset += count; return true;
            };
            for (unsigned epoch : {1u, 2u}) {
                std::size_t received = 0;
                while (received < length) {
                    std::uint8_t output[113]; std::size_t size = sizeof(output);
                    Check(reader.ReadChunk(output, size, read), "byte frame read");
                    Check(size == (std::min)(sizeof(output), length - received), "byte frame tail");
                    Check(reader.Epoch() == epoch, "byte frame epoch");
                    for (std::size_t i = 0; i < size; ++i)
                        Check(output[i] == std::uint8_t(received + i + epoch), "byte frame content");
                    received += size;
                }
            }
            Check(offset == wire.size(), "byte frames fully consumed");
        }
    }
}

class Provider final:public px4::IoQueue::IoProvider {
public:
    struct Part{std::uint32_t epoch;std::vector<std::uint8_t> bytes;};
    std::mutex mutex;std::condition_variable changed;std::deque<Part> parts;
    std::atomic_uint calls{0};std::uint32_t epoch=0;bool ended=false;
    bool Start() override{return true;}void Stop() override{}
    std::uint32_t Epoch() const noexcept override{return epoch;}
    bool Do(void* output,std::size_t& size) override{
        std::unique_lock<std::mutex> lock(mutex);++calls;
        changed.wait(lock,[&]{return ended||!parts.empty();});if(parts.empty())return false;
        auto part=std::move(parts.front());parts.pop_front();Check(part.bytes.size()<=size,"test provider capacity");
        epoch=part.epoch;size=part.bytes.size();std::memcpy(output,part.bytes.data(),size);return true;
    }
    void Push(unsigned e,unsigned count){std::lock_guard<std::mutex> lock(mutex);parts.push_back({e,std::vector<std::uint8_t>(count,static_cast<std::uint8_t>(e))});changed.notify_all();}
    void End(){std::lock_guard<std::mutex> lock(mutex);ended=true;changed.notify_all();}
};
static void QueueTests(){
    Provider source;px4::IoQueue queue(px4::IoQueue::IoOperation::READ,source,8,8,2,0);
    Check(queue.Start(),"queue start");source.Push(1,8);Until([&]{return queue.GetDataBufferCount()==1;});
    void* current=nullptr;std::size_t size=0,remain=0;
    Check(queue.ReadBuffer(&current,size,remain,false)&&size==8,"first buffer");
    auto* held=static_cast<std::uint8_t*>(current);
    source.Push(1,8);source.Push(1,4);source.Push(2,4);source.Push(2,4);
    Until([&]{return source.calls>=6;});
    for(unsigned i=0;i<8;++i)Check(held[i]==1,"returned pointer reused by recovery worker");
    Check(queue.GetDataBufferCount()==1,"old queued buffer retained");
    Check(queue.ReadBuffer(&current,size,remain,false)&&size==8,"new epoch buffer");
    for(unsigned i=0;i<8;++i)Check(static_cast<std::uint8_t*>(current)[i]==2,"mixed epochs in queue");
    source.End();queue.Stop();
}
static void HostTests(){
    px4::ReceiverBase::StreamBuffer buffer;Check(buffer.Alloc(188*16),"host allocation");
    buffer.SetThresholdSize(188);buffer.PrepareRead();buffer.Start();
    std::promise<void> entered,release;auto released=release.get_future();
    std::uint32_t first=0,second=0;unsigned calls=0;
    std::thread sender([&]{buffer.HandleReadWithEpoch(188,[&](const void* p,std::size_t n,std::uint32_t e){
        Check(n==188,"TS length");if(!calls++){first=e;entered.set_value();released.wait();return true;}
        second=e;Check(static_cast<const std::uint8_t*>(p)[1]==2,"stale host payload after epoch");return false;
    });});
    std::uint8_t packet[188]={0x47,1};std::size_t size=188;Check(buffer.Write(packet,size),"first host write");buffer.NotifyWrite();
    Check(entered.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready,"host read start");
    size=188;Check(buffer.Write(packet,size),"queued old host write");
    Check(buffer.AdvanceEpoch(),"epoch change blocked by pipe send");
    packet[1]=2;size=188;Check(buffer.Write(packet,size),"new host write");buffer.NotifyWrite();
    release.set_value();sender.join();buffer.Stop();Check(second==first+1,"host epoch/payload snapshot");
}

// TSMF の1フレームから1パケットだけを選ぶ低レートのストリームを作る。
// 世代変更前に蓄積したフレームを捨て、新しい出力は通常の通知しきい値未満でも届ける。
static void TsmfHostTests() {
    px4::ReceiverBase::StreamBuffer buffer;
    Check(buffer.Alloc(188 * 64), "TSMF host allocation");
    buffer.SetThresholdSize(188 * 32);
    buffer.PrepareRead();
    buffer.Start();
    Check(buffer.StartTsmf(0, false, 0), "TSMF selector allocation");
    std::uint8_t header[188];
    std::memset(header, 0xff, sizeof(header));
    header[0] = 0x47; header[1] = 0; header[2] = 0x2f; header[3] = 0x10;
    header[4] = 0x1a; header[5] = 0x86; header[6] = 1;
    header[7] = 0x80; header[8] = 0; // 相対TS0だけを有効にする。
    std::memset(header + 73, 0, 26);
    header[73] = 0x10; // 最初のスロットだけがTS0、残りは空き。
    std::uint32_t crc = 0xffffffff;
    for (unsigned i = 4; i < 184; ++i) {
        crc ^= std::uint32_t(header[i]) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
    for (unsigned i = 0; i < 4; ++i) header[184 + i] = std::uint8_t(crc >> (24 - i * 8));
    auto feed = [&](const std::uint8_t *packet) {
        std::size_t size = 188;
        Check(buffer.Write(packet, size), "TSMF host write");
        buffer.NotifyWrite();
    };
    auto frame = [&](std::uint8_t fill) {
        std::uint8_t packet[188]; std::memset(packet, fill, sizeof(packet));
        packet[0] = 0x47; packet[1] = 1; packet[2] = 0; packet[3] = 0x10;
        for (unsigned i = 0; i < TSMF_SLOT_NUM; ++i) feed(packet);
    };
    feed(header); frame(0xa1);
    Check(buffer.GetTsmfState() == TSMF_FILTER_FOUND, "TSMF TS found");
    Check(buffer.AdvanceEpoch(), "TSMF epoch purge");
    Check(buffer.GetTsmfState() == TSMF_FILTER_FOUND, "purge preserves selected TS");
    feed(header); frame(0xb2);
    std::promise<void> entered;
    auto reader = std::async(std::launch::async, [&] {
        entered.set_value();
        buffer.HandleReadWithEpoch(188, [&](const void *data, std::size_t size, std::uint32_t epoch) {
            Check(size == 188 && epoch == 2, "TSMF new epoch payload");
            Check(static_cast<const std::uint8_t *>(data)[4] == 0xb2, "TSMF old staged frame discarded");
            return false;
        });
    });
    entered.get_future().wait();
    feed(header); // 次のヘッダーでフレームが確定し、1パケットだけが通知される。
    Check(reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "low-rate TSMF notification");
    reader.get();
    buffer.Stop();
    buffer.PrepareRead(); buffer.StopRequest(); buffer.Start();
    auto cancelled = std::async(std::launch::async, [&] {
        buffer.HandleRead(188, [](const void *, std::size_t) { return true; });
    });
    Check(cancelled.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "TSMF pre-start stop retained");
    cancelled.get(); buffer.Stop();
}

int main(){FrameTests();ByteFrameTests();QueueTests();HostTests();TsmfHostTests();std::cout<<"PASS: stream epochs, fragmented frames, stale queue discard and pointer lifetime (no hardware)\n";}
