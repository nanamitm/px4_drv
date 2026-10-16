// DriverHost の STATUS が使う排他取得を、カードや DriverHost を起動せずに検証する。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <windows.h>
#include "card_status_lock.hpp"

using clock_type = std::chrono::steady_clock;

int main()
{
 std::atomic_bool done{false};
 std::thread watchdog([&] {
  for (int i = 0; i < 1000 && !done; ++i) Sleep(10);
  if (!done) { std::fprintf(stderr, "card_status_lock_test: stalled\n"); ExitProcess(2); }
 });
 bool ok = true;
 std::recursive_mutex io;
 std::atomic_bool transaction{false};

 // 他接続の TRANSMIT 中は BUSY にせず、I/O の終了を待って照会する。
 {
  std::promise<void> held;
  std::thread transmit([&] {
   std::lock_guard<std::recursive_mutex> lock(io);
   held.set_value();
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  held.get_future().wait();
  auto start = clock_type::now();
  std::unique_lock<std::recursive_mutex> lock(io, std::defer_lock);
  ok = ok && px4::LockCardForStatus(lock, transaction) && lock.owns_lock();
  ok = ok && clock_type::now() - start >= std::chrono::milliseconds(50);
  lock.unlock();
  transmit.join();
 }

 // 他接続のトランザクション中は待たずに BUSY を返す。
 {
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner([&] {
   std::lock_guard<std::recursive_mutex> lock(io);
   transaction = true;
   held.set_value();
   released.wait();
   transaction = false;
  });
  held.get_future().wait();
  auto start = clock_type::now();
  std::unique_lock<std::recursive_mutex> lock(io, std::defer_lock);
  ok = ok && !px4::LockCardForStatus(lock, transaction) && !lock.owns_lock();
  ok = ok && clock_type::now() - start < std::chrono::milliseconds(500);
  release.set_value();
  owner.join();
 }

 // 待機中に他接続のトランザクションが始まった場合も、終了を待たずに BUSY を返す。
 {
  std::promise<void> held, waiting, release;
  auto released = release.get_future();
  auto waiting_future = waiting.get_future();
  std::thread owner([&] {
   std::lock_guard<std::recursive_mutex> lock(io);
   held.set_value();
   waiting_future.wait();
   std::this_thread::sleep_for(std::chrono::milliseconds(20));
   transaction = true;
   released.wait();
   transaction = false;
  });
  held.get_future().wait();
  std::unique_lock<std::recursive_mutex> lock(io, std::defer_lock);
  waiting.set_value();
  ok = ok && !px4::LockCardForStatus(lock, transaction) && !lock.owns_lock();
  release.set_value();
  owner.join();
 }

 // 自接続のトランザクション中は、同じスレッドが保持する排他を再取得して照会できる。
 {
  std::lock_guard<std::recursive_mutex> own(io);
  transaction = true;
  std::unique_lock<std::recursive_mutex> lock(io, std::defer_lock);
  ok = ok && px4::LockCardForStatus(lock, transaction) && lock.owns_lock();
  transaction = false;
 }

 done = true;
 watchdog.join();
 std::puts(ok ? "card_status_lock_test: passed (transmit wait, foreign transaction, own transaction)"
  : "card_status_lock_test: failed");
 return ok ? 0 : 1;
}
