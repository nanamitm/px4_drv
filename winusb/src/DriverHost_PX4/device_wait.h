#pragma once

#include <windows.h>

/* 既存の16ms単位の余裕は既定で維持する。実機で測定する場合だけ opt-in する。 */
static inline DWORD px4_legacy_wait_duration(DWORD milliseconds)
{
	DWORD padding = 16 - milliseconds % 16;
	return milliseconds > MAXDWORD - padding ? MAXDWORD - 1 : milliseconds + padding;
}

static inline void px4_device_wait(DWORD milliseconds)
{
	wchar_t enabled[2] = {0};
	if (GetEnvironmentVariableW(L"PX4_PRECISE_DEVICE_WAIT", enabled, 2) != 1 || enabled[0] != L'1') {
		Sleep(px4_legacy_wait_duration(milliseconds));
		return;
	}
	if (!milliseconds) return;
	/* 各呼出しが timer を所有する。全プロセスの timer resolution は変更しない。 */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
		TIMER_MODIFY_STATE | SYNCHRONIZE);
	if (timer) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)milliseconds * 10000;
		if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
			WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
			CloseHandle(timer);
			return;
		}
		CloseHandle(timer);
	}
	/* 高精度timerが使えない場合も要求した最小待機を守る。 */
	Sleep(milliseconds);
}
