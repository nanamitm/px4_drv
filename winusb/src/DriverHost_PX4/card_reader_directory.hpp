#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "card_device.hpp"

namespace px4 {

/*
 * カードサーバーが使う内蔵カードリーダーの一覧
 * DeviceManager が実装し、試験では USB を使わない模擬の一覧へ差し替えて、抜き差しの経路を検証する
 */
class CardReaderDirectory {
public:
	virtual ~CardReaderDirectory() = default;

	virtual std::vector<std::wstring> ListCardReaders() const = 0;
	/* 同じ名前の機器を挿し直すと、別のオブジェクトが返る */
	virtual std::shared_ptr<CardDevice> FindCardReader(const std::wstring &name) const = 0;
	/* 機器の追加と削除で増える世代番号 */
	virtual std::uint64_t GetCardReaderGeneration() const noexcept = 0;
};

} // namespace px4
