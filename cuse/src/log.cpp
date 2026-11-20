// SPDX-License-Identifier: GPL-2.0-only
// 共有コードが使う msg_printf の Linux 版。systemd のもとでは標準エラー出力が journald に記録され、
// 時刻は journald が付けるため、ここでは書式だけを出力する。
#include "msg.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex lock;
}

extern "C" int msg_printf(const char *format, ...)
{
    std::lock_guard<std::mutex> guard(lock);
    va_list ap;
    va_start(ap, format);
    const int n = std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fflush(stderr);
    return n;
}
