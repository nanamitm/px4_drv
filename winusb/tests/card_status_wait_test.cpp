// 実際の WinSCard 実装へ BUSY 応答を注入し、ホストや実機を起動せず待機制御を検証する。
#include "../src/WinSCard_PX4/winscard.cpp"
#include <cstdio>

namespace px4::winscard {
FARPROC GetNativeFunction(const char *) noexcept { return nullptr; }
}
std::atomic_bool busy{true};
std::atomic<unsigned int> queries{0};
bool px4::CardClient::Connect() noexcept { return true; }
bool px4::CardClient::Call(px4::card_command::Command &cmd) noexcept
{
 using namespace px4::card_command;
 cmd.status = Status::SUCCEEDED;
 cmd.error = Error::NONE;
 if (cmd.code == Code::LIST_READERS) {
  cmd.reader_count = 1;
  wcscpy_s(cmd.readers[0], L"PX4 test reader");
 } else if (cmd.code == Code::STATUS) {
  ++queries;
  if (busy) { cmd.status = Status::FAILED; cmd.error = Error::BUSY; }
  else { cmd.card_present = true; cmd.card_initialized = true; }
 }
 return true;
}
int main()
{
 std::atomic_bool done{false};
 std::thread watchdog([&] {
  for (int i=0; i<1000 && !done; ++i) Sleep(10);
  if (!done) ExitProcess(2);
 });
 SCARDCONTEXT context = 0;
 bool ok = SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &context) == SCARD_S_SUCCESS;
 SCARD_READERSTATEW state = {};
 state.szReader = L"PX4 test reader";
 auto start = std::chrono::steady_clock::now();
 ok = ok && SCardGetStatusChangeW(context, 0, &state, 1) == SCARD_E_TIMEOUT;
 ok = ok && std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
 ok = ok && SCardGetStatusChangeW(context, 100, &state, 1) == SCARD_E_TIMEOUT;
 queries = 0;
 LONG result = 0;
 std::thread waiter([&] { result = SCardGetStatusChangeW(context, INFINITE, &state, 1); });
 while (!queries) std::this_thread::yield();
 ok = ok && SCardCancel(context) == SCARD_S_SUCCESS;
 waiter.join();
 ok = ok && result == SCARD_E_CANCELLED;
 // トランザクションが終われば、通常の状態変化を再び通知する。
 busy = false;
 ok = ok && SCardGetStatusChangeW(context, 0, &state, 1) == SCARD_S_SUCCESS;
 ok = ok && (state.dwEventState & SCARD_STATE_PRESENT);
 // 他接続のトランザクション中の接続は失敗させず、終了を待って成功させる。
 busy = true;
 queries = 0;
 std::atomic_bool connected{false};
 LONG connect_result = SCARD_F_INTERNAL_ERROR;
 SCARDHANDLE card = 0;
 DWORD protocol = 0;
 std::thread connector([&] {
  connect_result = SCardConnectW(context, L"PX4 test reader", SCARD_SHARE_SHARED,
   SCARD_PROTOCOL_T1, &card, &protocol);
  connected = true;
 });
 while (queries < 3) std::this_thread::yield();
 ok = ok && !connected;
 busy = false;
 connector.join();
 ok = ok && connect_result == SCARD_S_SUCCESS && card && protocol == SCARD_PROTOCOL_T1;
 ok = ok && SCardDisconnect(card, SCARD_LEAVE_CARD) == SCARD_S_SUCCESS;
 ok = ok && SCardReleaseContext(context) == SCARD_S_SUCCESS;
 done = true; watchdog.join();
 std::puts(ok ? "card_status_wait_test: passed (zero/finite timeout, cancel, recovery, connect wait)" : "card_status_wait_test: failed");
 return ok ? 0 : 1;
}
