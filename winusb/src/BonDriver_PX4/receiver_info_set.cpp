// receiver_info_set.cpp

#include "receiver_info_set.hpp"

#include <algorithm>
#include <cwchar>
#include <utility>

#include "util.hpp"

namespace px4 {

void ReceiverInfoSet::Load(const px4::ConfigSet &configs) noexcept
{
	/* OpenPriority の大きい定義から順に Open を試す。同じ値の定義は ini の記載順を保つため、
	 * OpenPriority を書かない既存の ini では従来どおり記載順になる。 */
	std::vector<std::pair<long, px4::command::ReceiverInfo>> defs;

	for (int i = 0; i <= 512; i++) {
		wchar_t sct[32];

		swprintf_s(sct, L"ReceiverDefinition%d", i);

		if (!configs.Exists(sct))
			break;

		const px4::Config &c = configs.Get(sct);
		const std::wstring &device_name = c.Get(L"DeviceName", L"");
		const std::wstring &device_guid_str = c.Get(L"DeviceGUID", L"");
		const std::wstring &receiver_name = c.Get(L"ReceiverName", L"");
		const std::wstring &receiver_guid_str = c.Get(L"ReceiverGUID", L"");
		const std::wstring &system_str = c.Get(L"System", L"");
		const std::wstring &index_str = c.Get(L"Index", L"-1");
		const std::wstring &priority_str = c.Get(L"OpenPriority", L"0");

		px4::command::ReceiverInfo receiver_info = { 0 };

		wcscpy_s(receiver_info.device_name, device_name.c_str());
		px4::util::ParseGuidStr(device_guid_str, receiver_info.device_guid);
		wcscpy_s(receiver_info.receiver_name, receiver_name.c_str());
		px4::util::ParseGuidStr(receiver_guid_str, receiver_info.receiver_guid);
		px4::util::ParseSystemStr(system_str, receiver_info.systems);
		receiver_info.index = static_cast<std::int32_t>(std::stol(index_str));
		receiver_info.data_id = 0;

		/* 数値として読めない値は 0 として扱い、例外で Load を中断しない */
		defs.emplace_back(std::wcstol(priority_str.c_str(), nullptr, 10), receiver_info);
	}

	std::stable_sort(defs.begin(), defs.end(), [](const auto &a, const auto &b) {
		return a.first > b.first;
	});

	for (const auto &d : defs)
		receivers_.push_back(d.second);
}

const px4::command::ReceiverInfo& ReceiverInfoSet::Get(std::size_t i) const
{
	return receivers_.at(i);
}

} // namespace px4
