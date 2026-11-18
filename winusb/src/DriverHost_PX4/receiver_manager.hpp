// receiver_manager.hpp

#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>

#include "command.hpp"
#include "receiver_base.hpp"
#include "device_identity.hpp"

namespace px4 {

class ReceiverManager final {
public:
	ReceiverManager() {};
	~ReceiverManager() {};

	// cannot copy
	ReceiverManager(const ReceiverManager &) = delete;
	ReceiverManager& operator=(const ReceiverManager &) = delete;

	// cannot move
	ReceiverManager(ReceiverManager &&) = delete;
	ReceiverManager& operator=(ReceiverManager &&) = delete;

	bool Register(px4::command::ReceiverInfo &info, px4::ReceiverBase *receiver,
		const DeviceIdentity &identity = {}, bool open_outside_lock = false);
	bool Unregister(px4::ReceiverBase *receiver);
	px4::ReceiverBase* SearchAndOpen(px4::command::ReceiverInfo &key, px4::command::ReceiverInfo &info, std::uint32_t &data_id);
	std::shared_ptr<px4::ReceiverBase::StreamBuffer> BindStream(std::uint32_t data_id);
	void ClearDataId(px4::ReceiverBase *receiver);
	// 抜去通知を受けた機器の受信機を記録する。機器の破棄が後回しになると登録が残るため、検索の対象から外す
	void MarkRemoved(const std::wstring &path);
	// 抜去の印がある受信機と、機器の破棄で登録から外れた受信機を真とする。開いている接続が持つ受信機だけに使う
	bool IsRemoved(px4::ReceiverBase *receiver);

private:
	struct ReceiverData {
		px4::command::ReceiverInfo info;
		bool valid_data_id;
		DeviceIdentity identity;
		bool open_outside_lock;
		bool opening = false;
		bool unregistering = false;
		bool removed = false;
	};

	bool GenerateDataId(px4::ReceiverBase *receiver, std::uint32_t &data_id);

	std::shared_mutex mtx_;
	std::condition_variable_any open_changed_;
	std::map<px4::ReceiverBase*, ReceiverData> data_;
};

} // namespace px4
