#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include "command_reader.hpp"
#include "misc_posix.h"

static void Check(bool ok) { if (!ok) throw std::runtime_error("IPC check failed"); }
struct Pair {
 int fds[2];
 Pair() { Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }
 ~Pair() { for (int fd : fds) if (fd >= 0) close(fd); }
 px4::Pipe TakePipe() { int fd = fds[0]; fds[0] = -1; return px4::Pipe(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd))); }
};
static void Send(int fd, const void *buf, std::size_t size)
{
 auto *p = static_cast<const char *>(buf);
 while (size) { auto n = send(fd, p, size, MSG_NOSIGNAL); Check(n > 0); p += n; size -= n; }
}
int main() try
{
 using namespace px4::command;
 std::array<std::uint8_t, 4096> buf{};
 std::size_t length = 0;
 // OPEN を1バイトずつ送り、構造体全体が揃うまで保持する。
 {
  Pair pair; auto pipe = pair.TakePipe();
  CtrlOpenCmd cmd{}; cmd.cmd = CtrlCmdCode::OPEN; cmd.receiver_info.index = 42;
  std::thread sender([&] {
   for (std::size_t i = 0; i < sizeof(cmd); ++i) {
    Send(pair.fds[1], reinterpret_cast<char *>(&cmd) + i, 1);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
   }
  });
  bool ok = px4::ReadControlCommand(pipe, buf.data(), buf.size(), length, nullptr);
  sender.join(); Check(ok && length == sizeof(cmd) && !std::memcmp(buf.data(), &cmd, length));
 }
 // 連結された要求を1件ずつ消費し、次の要求を捨てない。
 {
  Pair pair; auto pipe = pair.TakePipe();
  CtrlVersionCmd cmds[2]{}; cmds[0].cmd = cmds[1].cmd = CtrlCmdCode::GET_VERSION;
  Send(pair.fds[1], cmds, sizeof(cmds));
  for (int i = 0; i < 2; ++i) Check(px4::ReadControlCommand(pipe, buf.data(), buf.size(), length, nullptr) && length == sizeof(cmds[0]));
 }
 for (unsigned int count : {0u, 1u, 3u, 0xffffffffu}) {
  Pair pair; auto pipe = pair.TakePipe();
  std::vector<std::uint8_t> data(sizeof(CtrlParamsCmd) + (count == 3 ? 2 * sizeof(Parameter) : 0));
  auto *cmd = reinterpret_cast<CtrlParamsCmd *>(data.data());
  cmd->cmd = CtrlCmdCode::SET_PARAMS; cmd->param_set.num = count;
  Send(pair.fds[1], data.data(), data.size());
  bool ok = px4::ReadControlCommand(pipe, buf.data(), buf.size(), length, nullptr);
  Check(count == 0xffffffffu ? !ok : ok && length == data.size());
 }
 // データ接続の分割受信と、途中切断。
 {
  Pair pair; auto pipe = pair.TakePipe();
  DataCmd cmd{}; cmd.cmd = DataCmdCode::SET_DATA_ID; cmd.data_id = 123;
  std::thread sender([&] {
   Send(pair.fds[1], &cmd, 1);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   Send(pair.fds[1], reinterpret_cast<char *>(&cmd) + 1, sizeof(cmd) - 1);
  });
  DataCmd result{}; bool ok = px4::ReadExact(pipe, &result, sizeof(result), nullptr);
  sender.join(); Check(ok && result.data_id == 123);
  Send(pair.fds[1], &cmd, 1); close(pair.fds[1]); pair.fds[1] = -1;
  Check(!px4::ReadExact(pipe, &result, sizeof(result), nullptr));
 }
 // 読み出さない相手への送信は、送信バッファが埋まってもキャンセル可能。
 {
  Pair pair; auto pipe = pair.TakePipe();
  HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool ok = true;
  std::thread writer([&] {
   std::vector<char> data(16 * 1024 * 1024);
   std::size_t written = 0; ok = pipe.Write(data.data(), data.size(), written, cancel);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  SetEvent(cancel); writer.join();
  Check(!ok && pipe.GetError() == std::errc::operation_canceled);
  CloseHandle(cancel);
 }
 std::puts("ipc_test: passed (fragmented/coalesced/variable commands, disconnect, cancelled write)");
 return 0;
} catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
