// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cerrno>
#include <cwchar>
#include <string>
#include "config.hpp"
#include "device_base.hpp"
#include "msg.h"

namespace px4 {

// DeviceDefinition の Config にある整数を読む。key がなければ fallback を返す。
// px4::util::wtoi は数値として読めない値を 0 として返すため、利得などの誤記がエラーなく 0 として使われ、
// 受信できないまま動き続ける。前後の空白とタブだけを許し、数値でない値、空、末尾の余分な文字、桁あふれ、
// [min, max] の範囲外は、キーと範囲をログへ出して DeviceError で受信機の作成を失敗させる。
// name はログと例外の先頭に付ける機種名 (例: "MSi2500")。
inline int ConfigInt(const Config &config, const wchar_t *key, int fallback, int min, int max, const char *name)
{
	if (!config.Exists(key))
		return fallback;
	const auto text = config.Get(key);
	const wchar_t *begin = text.c_str();
	wchar_t *end = nullptr;
	errno = 0;
	const long value = std::wcstol(begin, &end, 10);
	const bool parsed = end != begin && errno != ERANGE;
	while (*end == L' ' || *end == L'\t')
		++end;
	if (!parsed || *end || value < min || value > max) {
		msg_err("%s: %ls=%ls is not an integer in %d..%d\n", name, key, text.c_str(), min, max);
		throw DeviceError(std::string(name) + ": invalid configuration value");
	}
	return static_cast<int>(value);
}

} // namespace px4
