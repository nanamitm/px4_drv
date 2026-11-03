#include "device_wait.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>

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
	SetEnvironmentVariableW(L"PX4_PRECISE_DEVICE_WAIT", L"0");
	std::puts(ok ? "device_wait_test: PASS" : "device_wait_test: FAILED");
	return ok ? 0 : 1;
}
