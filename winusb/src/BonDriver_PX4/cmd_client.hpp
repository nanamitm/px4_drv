// cmd_client.hpp

#pragma once

#include <memory>

#include "command.hpp"
#include "pipe.hpp"
#include "pipe_client.hpp"

namespace px4 {

class CtrlCmdClient final {
public:
	explicit CtrlCmdClient(px4::PipeClient *pipe = nullptr) noexcept : pipe_(pipe) {}
	~CtrlCmdClient() {}

	// cannot copy
	CtrlCmdClient(const CtrlCmdClient &) = delete;
	CtrlCmdClient& operator=(const CtrlCmdClient &) = delete;

	// cannot move
	CtrlCmdClient(CtrlCmdClient &&) = delete;
	CtrlCmdClient& operator=(CtrlCmdClient &&) = delete;

	void SetPipe(px4::PipeClient *pipe) noexcept { pipe_.reset(pipe); }
	void SetPipe(std::unique_ptr<px4::PipeClient> &pipe) noexcept { pipe_ = std::move(pipe); }
	void ClearPipe() noexcept { pipe_.reset(); }
	std::unique_ptr<px4::PipeClient> TakePipe() noexcept { return std::move(pipe_); }
	// 直前のコマンドが、抜去済みの機器の受信機で失敗したか
	bool LastDeviceRemoved() const noexcept { return last_status_ == px4::command::CtrlStatusCode::DEVICE_REMOVED; }

	bool Open(const px4::command::ReceiverInfo &in, px4::SystemType systems, px4::command::ReceiverInfo *out) noexcept;
	bool GetVersion(std::uint32_t &version) noexcept;
	bool Close() noexcept;
	bool GetInfo(px4::command::ReceiverInfo &receiver_info) noexcept;
	bool SetCapture(bool capture) noexcept;
	bool GetParams(px4::command::ParameterSet &param_set) noexcept;
	bool SetParams(const px4::command::ParameterSet &param_set) noexcept;
	bool ClearParams() noexcept;
	bool Tune(std::uint32_t timeout) noexcept;
	bool CheckLock(bool &locked) noexcept;
	bool SetLnbVoltage(std::int32_t voltage) noexcept;
	bool ReadStats(px4::command::StatSet &stat_set) noexcept;
	// 受信機の TS バッファーを空にして世代を進め、進めた後の世代を受け取る。古い DriverHost では false
	bool PurgeStream(std::uint32_t &epoch) noexcept;

private:
	template <typename T> bool Call(T &cmd) noexcept;

	std::unique_ptr<px4::PipeClient> pipe_;
	px4::command::CtrlStatusCode last_status_ = px4::command::CtrlStatusCode::NONE;
};

} // namespace px4
