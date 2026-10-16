// ctrl_server.cpp - macOS version
// Buffer enlarged to 4096 because wchar_t is 4 bytes on macOS and
// ReceiverInfo (with wchar_t[96] fields) is ~820 bytes vs ~428 on Windows.

#include "ctrl_server.hpp"
#include "command_reader.hpp"

namespace px4 {

CtrlServer::CtrlServer(px4::ReceiverManager &receiver_manager)
	: ServerBase(L"px4_ctrl_pipe", receiver_manager)
{
	pipe_config_.in_buffer_size  = 4096;
	pipe_config_.out_buffer_size = 4096;
	pipe_config_.stream_pipe     = false;
	pipe_config_.stream_read     = false;
	pipe_config_.default_timeout = 2000;
}

px4::ServerBase::Connection *
CtrlServer::CreateConnection(std::unique_ptr<px4::PipeServer> &pipe)
{
	return new CtrlConnection(*this, pipe);
}

CtrlServer::CtrlConnection::CtrlConnection(ServerBase &parent,
					    std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: Connection(parent, pipe)
{
}

bool CtrlServer::CtrlConnection::CheckCommandLength(const std::uint8_t *buf,
						    px4::command::CtrlCmdCode cmd,
						    std::size_t length) noexcept
{
	/*
	 * ParameterSet と StatSet は要素数1の配列を可変長として使うため、
	 * 実際に届いた長さから収容できる要素数を求め、num がそれを超えないことを確かめる
	 */
	auto check_variable = [buf, length](std::size_t fixed_size, std::size_t element_size,
					    std::size_t num_offset) {
		/* 要素数そのものが届いていない段階で読まないよう、先に長さを確かめる */
		if (length < fixed_size)
			return false;

		std::uint32_t num;

		memcpy(&num, buf + num_offset, sizeof(num));
		if (!num)
			return true;

		return (num - 1) <= ((length - fixed_size) / element_size);
	};

	switch (cmd) {
	case px4::command::CtrlCmdCode::GET_VERSION:
		return length >= sizeof(px4::command::CtrlVersionCmd);

	case px4::command::CtrlCmdCode::OPEN:
		return length >= sizeof(px4::command::CtrlOpenCmd);

	case px4::command::CtrlCmdCode::GET_INFO:
		return length >= sizeof(px4::command::CtrlReceiverInfoCmd);

	case px4::command::CtrlCmdCode::SET_CAPTURE:
		return length >= sizeof(px4::command::CtrlCaptureCmd);

	case px4::command::CtrlCmdCode::GET_PARAMS:
	case px4::command::CtrlCmdCode::SET_PARAMS:
		return check_variable(sizeof(px4::command::CtrlParamsCmd),
				      sizeof(px4::command::Parameter),
				      offsetof(px4::command::CtrlParamsCmd, param_set.num));

	case px4::command::CtrlCmdCode::TUNE:
		return length >= sizeof(px4::command::CtrlTuneCmd);

	case px4::command::CtrlCmdCode::CHECK_LOCK:
		return length >= sizeof(px4::command::CtrlCheckLockCmd);

	case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE:
		return length >= sizeof(px4::command::CtrlLnbVoltageCmd);

	case px4::command::CtrlCmdCode::READ_STATS:
		return check_variable(sizeof(px4::command::CtrlStatsCmd),
				      sizeof(px4::command::Stat),
				      offsetof(px4::command::CtrlStatsCmd, stat_set.num));

	default:
		/* ヘッダーだけで完結するコマンドは冒頭の検査で足りる */
		return true;
	}
}

void CtrlServer::CtrlConnection::Worker() noexcept
{
	std::size_t size = config_.in_buffer_size;
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[size]);
	px4::command::ReceiverInfo info = { 0 };
	px4::ReceiverBase *receiver = nullptr;

	while (true) {
		bool ret = true;
		std::size_t read;

		if (!px4::ReadControlCommand(*conn_, buf.get(), size, read, quit_event_))
			break;

		/* 短いメッセージで未受信領域を解釈しないよう、ヘッダーを先に検査する */
		if (read < sizeof(px4::command::CtrlCmdHeader))
			break;

		px4::command::CtrlCmdHeader *hdr =
			reinterpret_cast<px4::command::CtrlCmdHeader *>(buf.get());

		if (!CheckCommandLength(buf.get(), hdr->cmd, read)) {
			std::size_t written;

			hdr->status = px4::command::CtrlStatusCode::FAILED;
			if (!conn_->Write(buf.get(), read, written, quit_event_) || read != written)
				break;

			continue;
		}

		switch (hdr->cmd) {
		case px4::command::CtrlCmdCode::GET_VERSION:
		{
			auto *version = reinterpret_cast<px4::command::CtrlVersionCmd *>(
				buf.get());
			version->status         = px4::command::CtrlStatusCode::SUCCEEDED;
			version->driver_version = 0x00040000;
			version->cmd_version    = px4::command::VERSION;
			break;
		}
		case px4::command::CtrlCmdCode::OPEN:
		{
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
			}

			auto *open = reinterpret_cast<px4::command::CtrlOpenCmd *>(
				buf.get());
			std::uint32_t data_id;

			receiver = receiver_manager_.SearchAndOpen(
				open->receiver_info, info, data_id);
			if (receiver) {
				open->receiver_info         = info;
				open->receiver_info.data_id = data_id;
			}

			open->status = receiver
				? px4::command::CtrlStatusCode::SUCCEEDED
				: px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::CLOSE:
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
				info     = { 0 };
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}
			break;

		case px4::command::CtrlCmdCode::GET_INFO:
		{
			auto *receiver_info =
				reinterpret_cast<px4::command::CtrlReceiverInfoCmd *>(
					buf.get());
			if (receiver) {
				receiver_info->receiver_info = info;
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}
			break;
		}
		case px4::command::CtrlCmdCode::SET_CAPTURE:
		{
			auto *capture = reinterpret_cast<px4::command::CtrlCaptureCmd *>(
				buf.get());
			if (receiver && !receiver->SetCapture(capture->capture ? true : false))
				capture->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				capture->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::GET_PARAMS:
		{
			auto *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(
				buf.get());
			if (receiver && receiver->GetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::SET_PARAMS:
		{
			auto *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(
				buf.get());
			if (receiver && receiver->SetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::CLEAR_PARAMS:
			if (receiver && (receiver->ClearParameters(), true))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			break;

		case px4::command::CtrlCmdCode::TUNE:
		{
			auto *tune = reinterpret_cast<px4::command::CtrlTuneCmd *>(
				buf.get());
			if (receiver && receiver->Tune(tune->timeout))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::CHECK_LOCK:
		{
			auto *check_lock =
				reinterpret_cast<px4::command::CtrlCheckLockCmd *>(
					buf.get());
			if (receiver && !receiver->CheckLock(check_lock->locked))
				check_lock->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				check_lock->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE:
		{
			auto *lnb = reinterpret_cast<px4::command::CtrlLnbVoltageCmd *>(
				buf.get());
			if (receiver && !receiver->SetLnbVoltage(lnb->voltage))
				lnb->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				lnb->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		case px4::command::CtrlCmdCode::READ_STATS:
		{
			auto *stats = reinterpret_cast<px4::command::CtrlStatsCmd *>(
				buf.get());
			if (receiver && receiver->ReadStats(stats->stat_set))
				stats->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				stats->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}
		default:
			hdr->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}

		if (!ret)
			break;

		std::size_t written;
		if (!conn_->Write(buf.get(), read, written, quit_event_) || read != written)
			break;
	}

	if (receiver) {
		receiver->Close();
		receiver_manager_.ClearDataId(receiver);
	}

	delete this;
}

} // namespace px4
