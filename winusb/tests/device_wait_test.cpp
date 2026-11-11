#include <windows.h>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>

/* 高精度timerの作成失敗と Sleep の引数を観測するため、device_wait.h の呼出しを差し替える。 */
static std::atomic_bool fail_timer{false};
static std::atomic<DWORD> last_sleep{0};
static HANDLE WINAPI mock_create_timer(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access)
{
	if (fail_timer) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	return CreateWaitableTimerExW(attributes, name, flags, access);
}
static void WINAPI mock_sleep(DWORD milliseconds)
{
	last_sleep = milliseconds;
	Sleep(milliseconds);
}
#define CreateWaitableTimerExW mock_create_timer
#define Sleep mock_sleep
#include "device_wait.h"

int main()
{
	if (px4_legacy_wait_duration(0) != 16 || px4_legacy_wait_duration(16) != 32 ||
		px4_legacy_wait_duration(1) != 16 || px4_legacy_wait_duration(MAXDWORD) != MAXDWORD - 1)
		return 1;
	SetEnvironmentVariableW(L"PX4_PRECISE_DEVICE_WAIT", L"1");
	std::atomic_bool ok{true};
	std::vector<std::thread> waiters;
	for (int i = 0; i < 8; ++i) waiters.emplace_back([&] {
		LARGE_INTEGER frequency, before, after;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&before);
		px4_device_wait(5);
		QueryPerformanceCounter(&after);
		if ((after.QuadPart - before.QuadPart) * 1000 < frequency.QuadPart * 5) ok = false;
	});
	for (auto &waiter : waiters) waiter.join();
	/* timer を作れない環境の Sleep は、分解能未満の要求でも従来の余裕を付ける。 */
	fail_timer = true;
	px4_device_wait(5);
	if (last_sleep != px4_legacy_wait_duration(5)) ok = false;
	fail_timer = false;
	SetEnvironmentVariableW(L"PX4_PRECISE_DEVICE_WAIT", L"0");
	std::puts(ok ? "device_wait_test: PASS" : "device_wait_test: FAILED");
	return ok ? 0 : 1;
}
