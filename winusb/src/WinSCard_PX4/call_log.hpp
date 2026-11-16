#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <windows.h>

#include "card_command.hpp"

namespace px4::winscard {

/*
 * DLL と同じフォルダーの WinSCard_PX4.ini の [WinSCard_PX4] で DebugLog=true の場合だけ、
 * DriverHost のカードサーバーとの要求をログへ記録する。
 * TVTest などのフォルダーには別製品の WinSCard.ini が置かれることがあるため、DLL 名とは別の名前にする。
 */
bool LogEnabled() noexcept;
void LogCall(card_command::Code code, const std::wstring &reader, LONG result,
	std::uint64_t elapsed_us, const std::uint8_t header[4], std::size_t send_length,
	const card_command::Command &response) noexcept;
/* DriverHost のカード用パイプへの接続可否は、状態が変わった時だけ記録する */
void LogDriverHostConnect(bool connected) noexcept;

} // namespace px4::winscard
