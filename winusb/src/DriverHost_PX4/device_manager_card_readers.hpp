#pragma once

#include "card_reader_directory.hpp"
#include "device_manager.hpp"

namespace px4 {

/*
 * DeviceManager の内蔵カードリーダーの一覧を CardReaderDirectory として渡す
 * macOS 版は device_manager.hpp を共有しつつカードの一覧を実装しないため、DeviceManager 自体には仮想関数を持たせない
 */
class DeviceManagerCardReaders final : public CardReaderDirectory {
public:
	explicit DeviceManagerCardReaders(DeviceManager &device_manager) noexcept
		: device_manager_(device_manager) {}

	std::vector<std::wstring> ListCardReaders() const override
	{
		return device_manager_.ListCardReaders();
	}

	std::shared_ptr<CardDevice> FindCardReader(const std::wstring &name) const override
	{
		return device_manager_.FindCardReader(name);
	}

	std::uint64_t GetCardReaderGeneration() const noexcept override
	{
		return device_manager_.GetCardReaderGeneration();
	}

private:
	DeviceManager &device_manager_;
};

} // namespace px4
