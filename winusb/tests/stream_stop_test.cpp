#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <windows.h>
#include "receiver_base.hpp"

extern "C" int msg_printf(const char *, ...) { return 0; }

int main()
{
 std::atomic_bool done{false};
 std::thread watchdog([&] {
  for (int i = 0; i < 1500 && !done; ++i)
   Sleep(10);
  if (!done) { std::fprintf(stderr, "stream_stop_test: stalled\n"); ExitProcess(1); }
 });
 auto buffer = std::make_unique<px4::ReceiverBase::StreamBuffer>();
 buffer->Alloc(188 * 16);
 buffer->SetThresholdSize(188);
 auto read = [&] { buffer->HandleRead(188, [](const void *, std::size_t) { return true; }); };
 // スレッド起動より先の停止要求も、受信開始に消されずに残ることを確かめる。
 buffer->PrepareRead();
 buffer->StopRequest();
 buffer->Start();
 read();
 buffer->Stop();
 for (int i = 0; i < 1000; ++i) {
  buffer->PrepareRead();
  if (i & 1) buffer->Start();
  std::thread worker(read);
  if (i % 3 == 0) std::this_thread::yield();
  buffer->StopRequest();
  worker.join();
  buffer->Stop();
 }
 // 送信先が詰まっていても、USB 側の通知と停止要求が戻ることを確かめる。
 buffer->PrepareRead();
 buffer->Start();
 std::promise<void> entered, release;
 auto released = release.get_future();
 std::thread worker([&] {
  buffer->HandleRead(188, [&](const void *, std::size_t) {
   entered.set_value(); released.wait(); return true;
  });
 });
 unsigned char packet[188] = {0x47};
 std::size_t size = sizeof(packet);
 buffer->Write(packet, size);
 buffer->NotifyWrite();
 entered.get_future().wait();
 size = sizeof(packet);
 buffer->Write(packet, size);
 buffer->NotifyWrite();
 buffer->StopRequest();
 release.set_value();
 worker.join();
 done = true;
 watchdog.join();
 std::puts("stream_stop_test: passed (1000 stop races and blocked output)");
}
