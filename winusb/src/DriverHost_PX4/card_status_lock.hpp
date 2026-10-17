#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace px4 {

/* STATUS 用にカード I/O の排他を取得する。
 * 他接続の TRANSMIT や RESET は有限時間で終わるため、終わるまで待って照会する。
 * 他接続のトランザクションは利用者が終了するまで続くため、待たずに false を返し、
 * WinSCard 側が呼び出し全体の期限とキャンセルを確認できるようにする。
 * 自接続のトランザクション中は再帰ミューテックスを同じスレッドで取得できる。 */
template <typename Mutex>
bool LockCardForStatus(std::unique_lock<Mutex> &lock,
		       const std::atomic_bool &transaction_active) noexcept
{
	while (!lock.try_lock()) {
		if (transaction_active)
			return false;
		/* 解放通知を持たない再帰ミューテックスのため、短い間隔で取得を再試行する */
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

} // namespace px4
