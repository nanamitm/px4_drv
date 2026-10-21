// msg.c

#include "msg.h"

#include <stdarg.h>
#include <stdio.h>

#include <windows.h>

static enum msg_mode mode = MSG_MODE_NONE;
static HANDLE file_handle = INVALID_HANDLE_VALUE;
/* 改行で終わらない出力の続きへ時刻を挟まないよう、直前の出力が行末で
 * 終わったかを残す。複数スレッドの競合で時刻が欠けても本文は失われない。 */
static volatile LONG at_line_start = 1;

void msg_set_mode(enum msg_mode new_mode)
{
	mode = new_mode;
}

bool msg_open_file(const wchar_t *path)
{
	if (file_handle != INVALID_HANDLE_VALUE)
		CloseHandle(file_handle);

	/* A diagnostic run must not retain stale data from the previous run. */
	file_handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	return (file_handle != INVALID_HANDLE_VALUE) ? true : false;
}

void msg_close_file()
{
	if (file_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(file_handle);
		file_handle = INVALID_HANDLE_VALUE;
	}

	return;
}

int msg_printf(const char *format, ...)
{
	va_list args;
	char buf[1024];
	int c, prefix = 0;

	if (mode == MSG_MODE_NONE)
		return 0;

	if ((mode & MSG_MODE_TIMESTAMP) && at_line_start) {
		SYSTEMTIME st;

		/* .err と同じ「年/月/日 時:分:秒」に、照合用のミリ秒を加える。 */
		GetLocalTime(&st);
		prefix = sprintf_s(buf, sizeof(buf), "%04u/%02u/%02u %02u:%02u:%02u.%03u ",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		if (prefix < 0)
			prefix = 0;
	}

	va_start(args, format);
	c = vsprintf_s(buf + prefix, sizeof(buf) - prefix, format, args);
	va_end(args);

	if (c < 0)
		return c;
	if (c > 0)
		at_line_start = (buf[prefix + c - 1] == '\n') ? 1 : 0;
	c += prefix;

	if (mode & MSG_MODE_CONSOLE)
		printf("%s", buf);

	if (mode & MSG_MODE_DEBUGGER)
		OutputDebugStringA(buf);

	if ((mode & MSG_MODE_LOG_FILE) && (file_handle != INVALID_HANDLE_VALUE)) {
		DWORD wb;

		WriteFile(file_handle, buf, c, &wb, NULL);
	}

	return c;
}
