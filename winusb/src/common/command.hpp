// command.hpp

#pragma once

#include <cstdint>
#include <cwchar>
#include <guiddef.h>

#include "type.hpp"

namespace px4 {

#pragma pack(push, 8)

namespace command {
	static const std::uint32_t VERSION = 0x00040005U;

	enum class CtrlCmdCode : std::uint32_t {
		UNDEFINED = 0,
		GET_VERSION = 1,
		OPEN = 8,
		CLOSE,
		GET_INFO,
		SET_CAPTURE,
		GET_PARAMS = 16,
		SET_PARAMS,
		CLEAR_PARAMS,
		TUNE,
		CHECK_LOCK,
		SET_LNB_VOLTAGE = 24,
		READ_STATS = 32,
		SET_PID_FILTER = 40,
	};

	enum class CtrlStatusCode : std::uint32_t {
		NONE = 0,	// request
		SUCCEEDED,	// response
		FAILED,		// response
	};

	struct CtrlCmdHeader {
		CtrlCmdCode cmd;
		CtrlStatusCode status;
	};

	struct CtrlVersionCmd : CtrlCmdHeader {
		std::uint32_t driver_version;
		std::uint32_t cmd_version;
	};

	struct ReceiverInfo {
		wchar_t device_name[96];
		GUID device_guid;		// not 'DeviceInterfaceGUID'
		wchar_t receiver_name[96];
		GUID receiver_guid;
		px4::SystemType systems;
		std::int32_t index;
		std::uint32_t data_id;
	};

	struct CtrlOpenCmd : CtrlCmdHeader {
		ReceiverInfo receiver_info;
	};

	struct CtrlCloseCmd : CtrlCmdHeader {
		// header only
	};

	struct CtrlReceiverInfoCmd : CtrlCmdHeader {
		ReceiverInfo receiver_info;
	};

	struct CtrlCaptureCmd : CtrlCmdHeader {
		bool capture;
	};

	enum class ParameterType : std::uint32_t {
		UNDEFINED = 0,
		BANDWIDTH = 1,
		TSMF_STREAM,
		STREAM_ID = 16,
	};

	struct Parameter {
		ParameterType type;
		std::uint32_t value;
	};

	struct ParameterSet {
		px4::SystemType system;
		std::uint32_t freq;
		std::uint32_t num;
		Parameter params[1];
	};

	struct CtrlParamsCmd : CtrlCmdHeader {
		ParameterSet param_set;
	};

	struct CtrlClearParamsCmd : CtrlCmdHeader {
		// header only
	};

	struct CtrlTuneCmd : CtrlCmdHeader {
		std::uint32_t timeout;
	};

	struct CtrlCheckLockCmd : CtrlCmdHeader {
		bool locked;
	};

	struct CtrlLnbVoltageCmd : CtrlCmdHeader {
		std::int32_t voltage;
	};

	enum class StatType : std::uint32_t {
		UNDEFINED = 0,
		SIGNAL_STRENGTH,
		CNR,
	};

	struct Stat {
		StatType type;
		std::int32_t value;
	};

	struct StatSet {
		std::uint32_t num;
		Stat data[1];
	};

	struct CtrlStatsCmd : CtrlCmdHeader {
		StatSet stat_set;
	};

	// 既存コマンドの配置を保つ追加コマンド。ポインターを含まない固定長のwire形式。
	enum class PidFilterMode : std::uint32_t { Restore = 0, Pass = 1, Block = 2 };
	struct PidFilter {
		PidFilterMode mode;
		std::uint32_t count;
		std::uint16_t pids[64];
	};
	inline bool ValidPidFilter(const PidFilter& filter) noexcept {
		if (filter.count > 64) return false;
		if (filter.mode == PidFilterMode::Restore) return filter.count == 0;
		if (filter.mode != PidFilterMode::Pass && filter.mode != PidFilterMode::Block) return false;
		if (filter.mode == PidFilterMode::Pass && !filter.count) return false;
		for (std::uint32_t i=0; i<filter.count; ++i) if (filter.pids[i] > 0x1fff) return false;
		return true;
	}
	struct CtrlPidFilterCmd : CtrlCmdHeader {
		PidFilter filter;
		std::int32_t result; // 0、または負のerrno。未対応とI/O失敗を区別する。
	};
	static_assert(sizeof(CtrlPidFilterCmd) == 148, "PID filter IPC layout");

	enum class DataCmdCode : std::uint32_t {
		UNDEFINED = 0,
		SET_DATA_ID = 1,
		SET_DATA_ID_FRAMED = 2,
		PURGE = 8,
	};

	struct DataCmd {
		DataCmdCode cmd;
		union {
			std::uint32_t data_id;
		};
	};

} // namespace command

#pragma pack(pop)

} // namespace px4

