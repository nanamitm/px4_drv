#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include <windows.h>
#include "pipe_server.hpp"

int main()
{
 std::atomic_bool done{false};
 std::thread watchdog([&] {
  for (int i = 0; i < 1000 && !done; ++i) Sleep(10);
  if (!done) { std::fprintf(stderr, "pipe_shutdown_test: stalled\n"); ExitProcess(1); }
 });
 bool ok = true;
 for (int test = 0; test < 2; ++test) {
  const std::wstring name = L"px4_review_shutdown_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(test);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic_bool accepted{false}, write_ok{false};
  std::thread server([&] {
   px4::PipeServer pipe;
   px4::PipeServer::PipeServerConfig config = {false, false, 512, 512, 2000};
   if (!pipe.Accept(name, config, ready, cancel)) return;
   accepted = true;
   std::vector<unsigned char> data(test ? 1024 * 1024 : 8, 0x47);
   std::size_t written = 0;
   write_ok = pipe.Write(data.data(), data.size(), written, cancel);
   // 接続先が未読のままでも、デストラクタが同期フラッシュで停止しないことを確かめる。
  });
  WaitForSingleObject(ready, INFINITE);
  HANDLE client = CreateFileW((L"\\\\.\\pipe\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE,
   0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (client == INVALID_HANDLE_VALUE) ExitProcess(2);
  if (test) { Sleep(50); SetEvent(cancel); }
  server.join();
  ok = ok && accepted && (test ? !write_ok : write_ok.load());
  CloseHandle(client); CloseHandle(ready); CloseHandle(cancel);
 }
 done = true; watchdog.join();
 std::puts(ok ? "pipe_shutdown_test: passed (unread output and cancelled write)" : "pipe_shutdown_test: failed");
 return ok ? 0 : 1;
}
