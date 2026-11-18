// receiver_manager.cpp

#include "receiver_manager.hpp"

#include <random>
#include "msg.h"

namespace px4 {

bool ReceiverManager::Register(px4::command::ReceiverInfo &info, px4::ReceiverBase *receiver,
	const DeviceIdentity &identity, bool open_outside_lock)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	if (data_.count(receiver))
		return false;

	data_.emplace(receiver, ReceiverData{ info, false, identity, open_outside_lock });
	return true;
}

bool ReceiverManager::Unregister(px4::ReceiverBase *receiver)
{
	std::unique_lock<std::shared_mutex> lock(mtx_);
	const auto it = data_.find(receiver);
	if (it == data_.end() || it->second.unregistering)
		return false;

	// Openが参照している間は、登録解除の呼び出し元に受信機を破棄させない。
	it->second.unregistering = true;
	open_changed_.wait(lock, [&] { return !it->second.opening; });
	data_.erase(it);
	return true;
}

static const GUID empty_guid = { 0 };

px4::ReceiverBase* ReceiverManager::SearchAndOpen(px4::command::ReceiverInfo &key, px4::command::ReceiverInfo &info, std::uint32_t &data_id)
{
	std::unique_lock<std::shared_mutex> lock(mtx_);

	if (!wmemchr(key.device_name, L'\0', sizeof(key.device_name) / sizeof(wchar_t)) ||
		!wmemchr(key.receiver_name, L'\0', sizeof(key.receiver_name) / sizeof(wchar_t)))
		return nullptr;
	const std::wstring selector = key.device_name;
	const bool explicit_device = IsDeviceSelector(selector);
	std::wstring matched_path;
	if (explicit_device) {
		/* 空き受信機だけを調べると、片方が使用中の重複serialを見落とす。 */
		for (const auto &entry : data_) {
			if (entry.second.unregistering || !MatchesDeviceSelector(selector, entry.second.identity)) continue;
			if (!matched_path.empty() && matched_path != entry.second.identity.path) {
				msg_err("Ambiguous device selector: %ls; use @id:<path ID>\n", selector.c_str());
				return nullptr;
			}
			matched_path = entry.second.identity.path;
		}
		if (matched_path.empty()) {
			msg_err("Device selector not found: %ls\n", selector.c_str());
			return nullptr;
		}
	}

	for (auto it = data_.begin(); it != data_.end(); ++it) {
		auto &entry = it->second;
		const auto &k = entry.info;

		if (k.data_id || entry.unregistering)
			continue;

		if (explicit_device && (entry.identity.path != matched_path || !MatchesDeviceSelector(selector, entry.identity)))
			continue;
		if (!explicit_device && key.device_name[0] && wcscmp(key.device_name, k.device_name))
			continue;

		if (memcmp(&key.device_guid, &empty_guid, sizeof(key.device_guid)) &&
		    memcmp(&key.device_guid, &k.device_guid, sizeof(key.device_guid)))
			continue;

		if (key.receiver_name[0] && wcscmp(key.receiver_name, k.receiver_name))
			continue;

		if (memcmp(&key.receiver_guid, &empty_guid, sizeof(key.receiver_guid)) &&
		    memcmp(&key.receiver_guid, &k.receiver_guid, sizeof(key.receiver_guid)))
			continue;

		if ((key.systems & k.systems) != key.systems)
			continue;

		if ((key.index >= 0) && (key.index != k.index))
			continue;

		px4::ReceiverBase *r = it->first;

		if (entry.open_outside_lock) {
			// IDは予約だけ行い、Openが成功するまでストリーム接続に公開しない。
			std::uint32_t reserved_id = 0;
			if (!GenerateDataId(r, reserved_id))
				continue;
			entry.valid_data_id = false;
			entry.opening = true;
			lock.unlock();
			bool opened = false;
			try {
				opened = r->Open() == 0;
				lock.lock();
				if (opened && entry.unregistering) {
					lock.unlock();
					r->Close();
					lock.lock();
					opened = false;
				}
			} catch (...) {
				if (!lock.owns_lock())
					lock.lock();
				entry.opening = false;
				entry.info.data_id = 0;
				open_changed_.notify_all();
				throw;
			}
			entry.opening = false;
			open_changed_.notify_all();
			if (!opened) {
				entry.info.data_id = 0;
				continue;
			}
			entry.valid_data_id = true;
			info = k;
			data_id = reserved_id;
			return r;
		}
		if (r->Open())
			continue;

		if (!GenerateDataId(r, data_id)) {
			r->Close();
			continue;
		}

		info = k;

		return r;
	}

	return nullptr;
}

px4::ReceiverBase* ReceiverManager::SearchByDataId(std::uint32_t data_id)
{
	std::unique_lock<std::shared_mutex> lock(mtx_);

	for (auto it = data_.begin(); it != data_.end(); ++it) {
		if (it->second.info.data_id != data_id || !it->second.valid_data_id || it->second.unregistering)
			continue;

		it->second.valid_data_id = false;
		return it->first;
	}

	return nullptr;
}

bool ReceiverManager::GenerateDataId(px4::ReceiverBase *receiver, std::uint32_t &data_id)
{
	if (!data_.count(receiver))
		return false;

	auto& v = data_.at(receiver);

	if (v.info.data_id)
		return false;

	while (true) {
		std::uint32_t tmp = std::random_device()();

		if (!tmp)
			continue;

		auto it = data_.cbegin();
		for (; it != data_.cend(); ++it) {
			if (it->second.info.data_id == tmp)
				break;
		}

		if (it == data_.cend()) {
			v.info.data_id = data_id = tmp;
			v.valid_data_id = true;
			break;
		}
	}

	return true;
}

void ReceiverManager::ClearDataId(px4::ReceiverBase *receiver)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	if (!data_.count(receiver))
		return;

	auto& data = data_.at(receiver);

	data.info.data_id = 0;
	data.valid_data_id = false;

	return;
}

} // namespace px4
