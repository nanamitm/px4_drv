#pragma once
#include <cstdint>
#include <cwctype>
#include <string>

namespace px4 {
struct DeviceIdentity final {
	std::wstring path;
	std::wstring serial;
	std::wstring PathId() const
	{
		if (path.empty()) return {};
		std::uint64_t hash = 14695981039346656037ULL;
		for (wchar_t c : path) { hash ^= static_cast<std::uint16_t>(c); hash *= 1099511628211ULL; }
		wchar_t result[17] = {};
		for (unsigned int i = 0; i < 16; ++i)
			result[15 - i] = L"0123456789ABCDEF"[(hash >> (i * 4)) & 15];
		return result;
	}
};

inline bool IsDeviceSelector(const std::wstring &selector)
{ return selector.compare(0, 4, L"@id:") == 0 || selector.compare(0, 8, L"@serial:") == 0; }

inline bool MatchesDeviceSelector(const std::wstring &selector, const DeviceIdentity &identity)
{
	if (selector.compare(0, 8, L"@serial:") == 0)
		return !identity.serial.empty() && selector.substr(8) == identity.serial;
	if (selector.compare(0, 4, L"@id:") != 0 || selector.size() != 20) return false;
	std::wstring id = selector.substr(4);
	for (auto &c : id) c = static_cast<wchar_t>(std::towupper(c));
	return !identity.path.empty() && id == identity.PathId();
}
}
