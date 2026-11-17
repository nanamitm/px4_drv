#define msg_prefix	"WinSCard_PX4"

#include "call_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <winscard.h>

#include "msg.h"

namespace px4::winscard {

namespace {

/* DriverHost 側の個別記録と同じ下限にし、両方のログで同じ APDU を対応付けられるようにする */
constexpr std::uint64_t SLOW_CALL_US = 500000;
constexpr auto SUMMARY_INTERVAL = std::chrono::seconds(60);

void ModuleAnchor() {}

std::wstring ModuleDirectory()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module))
		return std::wstring();
	std::vector<wchar_t> path(MAX_PATH);
	while (true) {
		DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		if (!length)
			return std::wstring();
		if (length < path.size()) {
			std::wstring text(path.data(), length);
			auto separator = text.find_last_of(L"\\/");
			return separator == std::wstring::npos ? std::wstring() : text.substr(0, separator + 1);
		}
		if (path.size() >= 32768)
			return std::wstring();
		path.resize((std::min<std::size_t>)(path.size() * 2, 32768));
	}
}

std::wstring ExecutableStem()
{
	std::vector<wchar_t> path(MAX_PATH);
	DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size())
		return L"process";
	std::wstring text(path.data(), length);
	auto separator = text.find_last_of(L"\\/");
	if (separator != std::wstring::npos)
		text.erase(0, separator + 1);
	auto dot = text.find_last_of(L'.');
	if (dot != std::wstring::npos)
		text.erase(dot);
	return text.empty() ? L"process" : text;
}

bool IsTrue(const wchar_t *value) noexcept
{
	return !_wcsicmp(value, L"true") || !_wcsicmp(value, L"yes") || !wcscmp(value, L"1");
}

void Initialize() noexcept try
{
	const std::wstring directory = ModuleDirectory();
	if (directory.empty())
		return;
	const std::wstring ini = directory + L"WinSCard_PX4.ini";
	wchar_t value[MAX_PATH] = {};
	GetPrivateProfileStringW(L"WinSCard_PX4", L"DebugLog", L"false", value,
		static_cast<DWORD>(std::size(value)), ini.c_str());
	if (!IsTrue(value))
		return;

	/* 同じ DLL を TVTest と録画ソフトが同時に読み込んでも、ファイルを奪い合わないようプロセス名で分ける */
	wchar_t log_path[MAX_PATH] = {};
	GetPrivateProfileStringW(L"WinSCard_PX4", L"DebugLogPath", L"", log_path,
		static_cast<DWORD>(std::size(log_path)), ini.c_str());
	std::wstring path = log_path[0] ? log_path : L"WinSCard_PX4_" + ExecutableStem() + L".log";
	if (path.size() < 2 || (path[1] != L':' && path[0] != L'\\' && path[0] != L'/'))
		path = directory + path;

	if (!msg_open_file(path.c_str()))
		return;
	msg_set_mode(static_cast<msg_mode>(MSG_MODE_LOG_FILE | MSG_MODE_TIMESTAMP));
	msg_info("Start pid=%lu exe=%ls\n", GetCurrentProcessId(), ExecutableStem().c_str());
}
catch (...) {
}

struct TransmitStats final {
	std::uint64_t count = 0;
	std::uint64_t failures = 0;
	std::uint64_t slow = 0;
	std::uint64_t total_us = 0;
	std::uint64_t max_us = 0;
	std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};

std::mutex log_mutex;
TransmitStats transmit_stats;
/* 状態照会は 50ms 周期で繰り返されるため、要求とリーダーごとに結果が変わった時だけ記録する */
std::unordered_map<std::wstring, std::string> last_results;
std::atomic<int> last_host_connect{ -1 };

const char* CodeName(card_command::Code code) noexcept
{
	switch (code) {
	case card_command::Code::GET_VERSION: return "GET_VERSION";
	case card_command::Code::LIST_READERS: return "LIST_READERS";
	case card_command::Code::CONNECT: return "CONNECT";
	case card_command::Code::RECONNECT: return "RECONNECT";
	case card_command::Code::DISCONNECT: return "DISCONNECT";
	case card_command::Code::STATUS: return "STATUS";
	case card_command::Code::RESET: return "RESET";
	case card_command::Code::TRANSMIT: return "TRANSMIT";
	case card_command::Code::BEGIN_TRANSACTION: return "BEGIN_TRANSACTION";
	case card_command::Code::END_TRANSACTION: return "END_TRANSACTION";
	default: return "UNKNOWN";
	}
}

std::string ToHex(const std::uint8_t *data, std::size_t length)
{
	std::string text;
	char byte[4];
	for (std::size_t index = 0; index < length; index++) {
		sprintf_s(byte, sizeof(byte), index ? " %02X" : "%02X", data[index]);
		text += byte;
	}
	return text;
}

} // namespace

bool LogEnabled() noexcept
{
	static std::once_flag once;
	try {
		std::call_once(once, Initialize);
	} catch (...) {
		return false;
	}
	return (msg_get_mode() & MSG_MODE_LOG_FILE) != 0;
}

void LogCall(card_command::Code code, const std::wstring &reader, LONG result,
	std::uint64_t elapsed_us, const std::uint8_t header[4], std::size_t send_length,
	const card_command::Command &response) noexcept try
{
	if (!LogEnabled())
		return;

	const unsigned long status = static_cast<unsigned long>(result);
	switch (code) {
	case card_command::Code::TRANSMIT:
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		auto &stats = transmit_stats;
		stats.count++;
		stats.total_us += elapsed_us;
		stats.max_us = (std::max)(stats.max_us, elapsed_us);
		if (result != SCARD_S_SUCCESS)
			stats.failures++;
		const bool slow = elapsed_us >= SLOW_CALL_US;
		if (slow)
			stats.slow++;
		/* APDU の本文には鍵を含む応答があるため、ヘッダー4バイトと長さ、状態語だけを残す */
		if (result != SCARD_S_SUCCESS || slow) {
			unsigned int sw = 0;
			const std::size_t recv_length = (std::min<std::size_t>)(
				response.data_length, card_command::MAX_DATA_SIZE);
			if (result == SCARD_S_SUCCESS && recv_length >= 2)
				sw = (static_cast<unsigned int>(response.data[recv_length - 2]) << 8) |
					response.data[recv_length - 1];
			msg_printf("%s " msg_prefix ": TRANSMIT result=0x%08lX apdu=%02X%02X%02X%02X send=%zu recv=%zu sw=%04X ms=%.1f\n",
				result != SCARD_S_SUCCESS ? "[WARN]" : "[INFO]", status,
				header[0], header[1], header[2], header[3], send_length,
				result == SCARD_S_SUCCESS ? recv_length : 0, sw, elapsed_us / 1000.0);
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - stats.window_start >= SUMMARY_INTERVAL) {
			const double seconds = std::chrono::duration<double>(now - stats.window_start).count();
			msg_info("summary window_s=%.0f transmits=%llu failures=%llu slow=%llu avg_ms=%.1f max_ms=%.1f\n",
				seconds, static_cast<unsigned long long>(stats.count),
				static_cast<unsigned long long>(stats.failures),
				static_cast<unsigned long long>(stats.slow),
				stats.count ? stats.total_us / 1000.0 / stats.count : 0.0,
				stats.max_us / 1000.0);
			stats = TransmitStats();
			stats.window_start = now;
		}
		break;
	}

	case card_command::Code::CONNECT:
	case card_command::Code::RECONNECT:
	case card_command::Code::DISCONNECT:
	case card_command::Code::RESET:
		msg_printf("%s " msg_prefix ": %s reader=%ls result=0x%08lX ms=%.1f atr=%s\n",
			result != SCARD_S_SUCCESS ? "[WARN]" : "[INFO]", CodeName(code),
			reader.c_str(), status, elapsed_us / 1000.0,
			code == card_command::Code::RESET && result == SCARD_S_SUCCESS
				? ToHex(response.atr, (std::min<std::size_t>)(
					response.atr_length, card_command::MAX_ATR_SIZE)).c_str()
				: "");
		break;

	case card_command::Code::BEGIN_TRANSACTION:
	case card_command::Code::END_TRANSACTION:
		/* ECM ごとに繰り返されるため、失敗と遅延だけを記録する */
		if (result != SCARD_S_SUCCESS || elapsed_us >= SLOW_CALL_US)
			msg_printf("%s " msg_prefix ": %s result=0x%08lX ms=%.1f\n",
				result != SCARD_S_SUCCESS ? "[WARN]" : "[INFO]", CodeName(code),
				status, elapsed_us / 1000.0);
		break;

	default:
	{
		/* 他接続のトランザクション中の共有違反は短い周期で交互に現れるため、変化の判定から外す */
		if (result == SCARD_E_SHARING_VIOLATION)
			break;
		std::wstring key = std::to_wstring(static_cast<unsigned int>(code)) + L"|" + reader;
		std::string detail;
		if (code == card_command::Code::STATUS && result == SCARD_S_SUCCESS) {
			char text[64];
			sprintf_s(text, sizeof(text), " present=%d initialized=%d atr=",
				response.card_present ? 1 : 0, response.card_initialized ? 1 : 0);
			detail = text + ToHex(response.atr, (std::min<std::size_t>)(
				response.atr_length, card_command::MAX_ATR_SIZE));
		}
		/* 結果の値だけでなく、カードの在否と ATR の変化も記録の対象にする */
		const std::string signature = std::to_string(status) + detail;
		std::lock_guard<std::mutex> lock(log_mutex);
		auto last = last_results.find(key);
		if (last != last_results.end() && last->second == signature)
			break;
		last_results[key] = signature;
		msg_printf("%s " msg_prefix ": %s reader=%ls result=0x%08lX ms=%.1f%s\n",
			result != SCARD_S_SUCCESS ? "[WARN]" : "[INFO]", CodeName(code),
			reader.c_str(), status, elapsed_us / 1000.0, detail.c_str());
		break;
	}
	}
}
catch (...) {
}

void LogDriverHostConnect(bool connected) noexcept
{
	const DWORD error = GetLastError();
	if (!LogEnabled())
		return;
	const int state = connected ? 1 : 0;
	if (last_host_connect.exchange(state) == state)
		return;
	if (connected)
		msg_info("DriverHost card pipe connected\n");
	else
		msg_warn("DriverHost card pipe connect failed error=%lu\n", error);
}

} // namespace px4::winscard
