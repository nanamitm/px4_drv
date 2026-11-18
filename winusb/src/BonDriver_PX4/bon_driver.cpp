// bon_driver.cpp

#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include <stdexcept>

#include <windows.h>
#include <aclapi.h>
#include <shlwapi.h>

#include "bon_driver.hpp"
#include "ipc_names.hpp"
#include "config.hpp"
#include "chset.hpp"
#include "security_attributes.hpp"
#include "pipe_client.hpp"
#include "command.hpp"
#include "util.hpp"
#include "msg.h"
#include "../../../driver/isdbc_channel.h"

namespace px4 {

BonDriver::BonDriver() noexcept
	: mtx_(),
	driver_host_path_(),
	configs_(),
	name_(),
	systems_(px4::SystemType::UNSPECIFIED),
	chset_(),
	receivers_(),
	lnb_power_(false),
	lnb_power_state_(false),
	pipe_timeout_(2000),
	tune_timeout_(5000),
	ctrl_client_(nullptr),
	data_pipe_(nullptr),
	open_(FALSE),
	space_(0),
	ch_(0),
	display_error_message_(false),
	stream_mtx_(),
	ioq_(nullptr),
	iorp_(*this),
	current_ofs_(0),
	quit_event_(nullptr)
{

}

BonDriver::~BonDriver()
{
	CloseTuner();
	Term();
}

bool BonDriver::Init()
{
	try {
		const std::wstring &dir_path = px4::util::path::GetDir();

		if (!configs_.Load(px4::util::path::GetFileBase() + L".ini"))
			return false;

		std::size_t num_packets = 1024;
		std::uintptr_t max_buffers = 64, min_buffers = 4;
		int data_ignore_count = 1;

		if (configs_.Exists(L"BonDriver")) {
			const px4::Config &bon_config = configs_.Get(L"BonDriver");
			framed_stream_=px4::util::wtob(bon_config.Get(L"FramedStream",L"false"));
			const std::wstring &mode = bon_config.Get(L"System", L"ISDB-T");
			WCHAR path[MAX_PATH];

			/* 選局ごとの判断を残す診断ログ。ログ出力のない従来の動作を既定とし、
			 * DriverHost_PX4.ini と同じ DebugLog / DebugLogPath で有効にする。
			 * ファイルは開くたびに作り直すため、前回の内容は残らない。
			 * DLL は TVTest や dantto4k など任意の作業フォルダーから読み込まれるため、
			 * 相対パスは DLL のフォルダーを基準に解決する。 */
			if (px4::util::wtob(bon_config.Get(L"DebugLog", L"false"))) {
				/* GetFileBase() already contains the module's full path.  Prefixing
				 * dir_path once more produced an invalid path when this DLL was loaded
				 * as dantto4k's inner BonDriver, hiding the useful Init diagnostics. */
				std::wstring log_path = bon_config.Get(L"DebugLogPath", px4::util::path::GetFileBase() + L".log");

				if (PathIsRelativeW(log_path.c_str()) && PathCanonicalizeW(path, (dir_path + log_path).c_str()))
					log_path = path;

				if (msg_open_file(log_path.c_str()))
					msg_set_mode(static_cast<msg_mode>(MSG_MODE_LOG_FILE | MSG_MODE_TIMESTAMP));
			}

			ts_diagnostics_enabled_=(msg_get_mode() & MSG_MODE_LOG_FILE) != 0;
			ts_diagnostics_.Enable(ts_diagnostics_enabled_);
			if (!px4::util::ParseSystemStr(mode, systems_))
				return false;

			driver_host_path_ = bon_config.Get(L"DriverHostPath", dir_path + L"DriverHost_PX4.exe");
			if (PathIsRelativeW(driver_host_path_.c_str()) && PathCanonicalizeW(path, (dir_path + driver_host_path_).c_str()))
				driver_host_path_ = path;

			name_ = bon_config.Get(L"Name", L"PX4");

			try {
				pipe_timeout_ = px4::util::wtoui32(bon_config.Get(L"PipeConnectTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				tune_timeout_ = px4::util::wtoui32(bon_config.Get(L"TuneTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				num_packets = px4::util::wtoui(bon_config.Get(L"NumberOfPacketsPerBuffer"));
				if (num_packets <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				max_buffers = px4::util::wtoui(bon_config.Get(L"MaximumNumberOfBuffers"));
				if (max_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				min_buffers = px4::util::wtoui(bon_config.Get(L"MinimumNumberOfBuffers"));
				if (min_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			if (max_buffers < min_buffers)
				return false;

			try {
				data_ignore_count = px4::util::wtoi(bon_config.Get(L"NumberOfBuffersToIgnoreAfterPurge"));
				if (data_ignore_count < 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				int display_error_message = px4::util::wtoi(bon_config.Get(L"DisplayErrorMessage"));
				if (display_error_message == 1) {
					display_error_message_ = true;
				}
			} catch (const std::out_of_range &) {}
		} else {
			systems_ = px4::SystemType::ISDB_T;
			driver_host_path_ = dir_path + L"DriverHost_PX4.exe";
			name_ = L"PX4";
		}

		if ((systems_ & px4::SystemType::ISDB_T) == px4::SystemType::ISDB_T) {
			px4::ChannelSet chset_t;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.ISDB-T").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_t.Load(chset_path, px4::SystemType::ISDB_T);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_t.Clear();
				result = chset_t.Load(dir_path + L"BonDriver_PX4-T.ChSet.txt", px4::SystemType::ISDB_T);
			}

			if (result)
				chset_.Merge(chset_t);
		}

		if ((systems_ & px4::SystemType::ISDB_S) == px4::SystemType::ISDB_S) {
			px4::ChannelSet chset_s;
			bool result;

			try {
				auto &config = configs_.Get(L"BonDriver.ISDB-S");

				try {
					lnb_power_ = !!px4::util::wtoi(config.Get(L"LNBPower"));
				} catch (const std::out_of_range &) {}

				std::wstring chset_path = config.Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_s.Load(chset_path, px4::SystemType::ISDB_S);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_s.Clear();
				result = chset_s.Load(dir_path + L"BonDriver_PX4-S.ChSet.txt", px4::SystemType::ISDB_S);
			}

			if (result)
				chset_.Merge(chset_s);
		}

		if ((systems_ & px4::SystemType::ISDB_C) == px4::SystemType::ISDB_C) {
			px4::ChannelSet chset_c;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.ISDB-C").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_c.Load(chset_path, px4::SystemType::ISDB_C);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_c.Clear();
				result = chset_c.Load(dir_path + L"BonDriver_PX4-C.ChSet.txt", px4::SystemType::ISDB_C);
			}

			if (result)
				chset_.Merge(chset_c);
		}

		if ((systems_ & px4::SystemType::J83B) == px4::SystemType::J83B) {
			px4::ChannelSet chset_j;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.J.83B").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_j.Load(chset_path, px4::SystemType::J83B);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_j.Clear();
				result = chset_j.Load(dir_path + L"BonDriver_PX4-J83B.ChSet.txt", px4::SystemType::J83B);
			}

			if (result)
				chset_.Merge(chset_j);
		}

		receivers_.Load(configs_);

		quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!quit_event_)
			return false;

		ioq_.reset(new px4::IoQueue(px4::IoQueue::IoOperation::READ, iorp_, 188 * num_packets, max_buffers, min_buffers, data_ignore_count));
	} catch (const std::runtime_error &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	}

	return true;
}

void BonDriver::Term() noexcept
{
	ioq_.reset();

	if (quit_event_) {
		CloseHandle(quit_event_);
		quit_event_ = nullptr;
	}
}

const BOOL BonDriver::OpenTuner()
{
	BOOL ret = TRUE;
	std::lock_guard<std::mutex> lock(mtx_);

	if (open_)
		return TRUE;

	try {
		// CloseTuner() が残した終了通知を消し、新しい読み出しワーカーだけを動かす
		if (!ResetEvent(quit_event_))
			throw BonDriverError("BonDriver::OpenTuner: ResetEvent() failed.");

		ConnectReceiver(ctrl_client_, data_pipe_);

		if (!ctrl_client_.SetCapture(true))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");

		ioq_->Start();

		open_ = TRUE;
	} catch (const std::exception &e) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	} catch (...) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	}

	if (!open_) {
		if (data_pipe_) {
			SetEvent(quit_event_);
			ioq_->Stop();
			data_pipe_.reset();
		}

		ctrl_client_.Close();
		ctrl_client_.ClearPipe();
	}

	return ret;
}

/*
 * DriverHost を起動して受信機を開き、データのパイプを受信機へ結び付ける
 * 受信の開始 (SET_CAPTURE) は呼び出し元が行う。失敗は BonDriverError で通知し、
 * 開いた受信機とパイプの後始末は呼び出し元が ctrl_client と data_pipe から行う
 */
void BonDriver::ConnectReceiver(px4::CtrlCmdClient &ctrl_client, std::unique_ptr<px4::PipeClient> &data_pipe)
{
	{
		HANDLE startup_event;
		DWORD st;

		{
			SecurityAttributes sa(EVENT_ALL_ACCESS);
			startup_event = CreateEventW(sa.Get(), TRUE, FALSE, px4::ipc::Startup);
		}

		if (!startup_event) {
			throw BonDriverError("BonDriver::OpenTuner: CreateEventW(\"DriverHost_PX4_StartupEvent\") failed.");
		} else if (GetLastError() != ERROR_ALREADY_EXISTS) {
			STARTUPINFO si;
			PROCESS_INFORMATION pi;

			memset(&si, 0, sizeof(si));
			si.cb = sizeof(si);
			memset(&pi, 0, sizeof(pi));

			if (!CreateProcessW(driver_host_path_.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
				CloseHandle(startup_event);
				throw BonDriverError("BonDriver::OpenTuner: CreateProcessW() failed.");
			}

			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}

		st = WaitForSingleObject(startup_event, 10000);
		CloseHandle(startup_event);

		if (st != WAIT_OBJECT_0)
			throw BonDriverError("BonDriver::OpenTuner: WaitForSingleObject() failed.");

		std::unique_ptr<px4::PipeClient> ctrl_pipe(new px4::PipeClient());
		px4::PipeClient::PipeClientConfig ctrl_pipe_config;
		px4::command::ReceiverInfo ri_res;

		ctrl_pipe_config.stream_read = false;
		ctrl_pipe_config.timeout = pipe_timeout_;

		if (!ctrl_pipe->Connect(px4::ipc::Control, ctrl_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: control pipe: cannot connect.");

		ctrl_client.SetPipe(ctrl_pipe);
		if(framed_stream_){
			std::uint32_t version=0;
			if(!ctrl_client.GetVersion(version) || (version>>16)!=(px4::command::VERSION>>16) || version<0x00040004){
				msg_err("OpenTuner: FramedStream requires DriverHost command protocol >= 0x00040004; actual=0x%08x\n",version);
				throw BonDriverError("FramedStream requires an updated DriverHost_PX4.exe (protocol 0x00040004 or later).");
			}
		}

		try {
			for (std::size_t i = 0; ; i++) {
				const px4::command::ReceiverInfo &ri = receivers_.Get(i);

				if (ctrl_client.Open(ri, systems_, &ri_res))
					break;
			}
		} catch (const std::out_of_range &) {
			throw BonDriverError("BonDriver::OpenTuner: no receivers available.");
		}

		px4::PipeClient::PipeClientConfig data_pipe_config;

		data_pipe_config.stream_read = true;
		data_pipe_config.timeout = pipe_timeout_;

		data_pipe.reset(new px4::PipeClient());

		if (!data_pipe->Connect(px4::ipc::Data, data_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: data pipe: cannot connect.");

		px4::command::DataCmd data_cmd;
		std::size_t ret_size;

		data_cmd.cmd = framed_stream_?px4::command::DataCmdCode::SET_DATA_ID_FRAMED:px4::command::DataCmdCode::SET_DATA_ID;
		data_cmd.data_id = ri_res.data_id;

		if (!data_pipe->Write(&data_cmd, sizeof(data_cmd), ret_size))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");
	}
}

void BonDriver::CloseTuner()
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (!open_)
		return;

	if (data_pipe_) {
		SetEvent(quit_event_);
		ioq_->Stop();
		data_pipe_.reset();
	}

	if (lnb_power_state_) {
		ctrl_client_.SetLnbVoltage(0);
		lnb_power_state_ = false;
	}

	ctrl_client_.Close();
	ctrl_client_.ClearPipe();

	if (ts_diagnostics_enabled_) {
		std::lock_guard<std::mutex> diagnostic_lock(diagnostic_mtx_);
		ts_diagnostics_.Reset();
	}
	space_ = 0;
	ch_ = 0;
	tuned_ = false;
	reconnect_needed_ = false;
	next_reconnect_tick_ = 0;

	open_ = FALSE;

	return;
}

const BOOL BonDriver::SetChannel(const BYTE bCh)
{
	return SetChannel(0, bCh);
}

const float BonDriver::GetSignalLevel(void)
{
	std::uint32_t epoch = 0;

	{
		std::lock_guard<std::mutex> lock(mtx_);

		if (!open_)
			return 0.0f;

		if (!reconnect_needed_) {
			px4::command::StatSet stat_set;

			stat_set.num = 1;
			stat_set.data[0].type = px4::command::StatType::CNR;
			stat_set.data[0].value = 0;

			if (ctrl_client_.ReadStats(stat_set))
				return static_cast<float>(stat_set.data[0].value) / 1000.0f;

			if (!ctrl_client_.LastDeviceRemoved())
				return 0.0f;

			reconnect_needed_ = true;
		}

		/*
		 * 利用側は受信が止まってもチャンネルを変えない限り SetChannel() を呼ばないため、
		 * 定期的に呼ばれる信号レベルの取得で抜去を検出し、挿し直した機器へつなぎ直して同じチャンネルを選局し直す
		 * 機器がまだ戻っていない間は ReconnectLocked() が間隔を空けて失敗する
		 */
		if (!ReconnectLocked())
			return 0.0f;

		if (tuned_) {
			std::unique_ptr<std::uint8_t[]> param_buf = MakeParameterSet(space_, ch_);
			bool removed = false;

			if (param_buf && !TuneLocked(*reinterpret_cast<px4::command::ParameterSet*>(param_buf.get()), removed) && removed)
				reconnect_needed_ = true;
		}

		epoch = PurgeStreamLocked();
	}

	ioq_->PurgeDataBuffer(epoch);
	if (ts_diagnostics_enabled_) {
		std::lock_guard<std::mutex> diagnostic_lock(diagnostic_mtx_);
		ts_diagnostics_.Reset();
	}

	return 0.0f;
}

/*
 * 抜去済みの機器の受信機から、挿し直した機器の受信機へ接続を移す
 * 新しい受信機で受信を開始できるまで古い接続を残し、失敗した場合は次の機会に再試行する
 */
bool BonDriver::ReconnectLocked() noexcept
{
	/*
	 * 試行ごとに DriverHost への接続と受信機の検索を行うため、機器が戻るまでの間は試行の間隔を空ける
	 * 抜き差しの実機の記録では、挿し直した直後に PX-W3U4 が約7秒の間に接続と切断を繰り返したため、
	 * 1秒ごとの信号レベルの取得のたびではなく 2 秒おきに試し、つながった後に消えた機器は次の抜去として扱う
	 */
	static const ULONGLONG reconnect_interval_ms = 2000;
	const ULONGLONG now = GetTickCount64();

	if (now < next_reconnect_tick_)
		return false;

	next_reconnect_tick_ = now + reconnect_interval_ms;

	px4::CtrlCmdClient ctrl_client;
	std::unique_ptr<px4::PipeClient> data_pipe;

	try {
		ConnectReceiver(ctrl_client, data_pipe);

		if (!ctrl_client.SetCapture(true))
			throw BonDriverError("BonDriver::ReconnectLocked: command failed.");
	} catch (const std::exception &e) {
		msg_err("Reconnect after device removal failed: %s\n", e.what());
		data_pipe.reset();
		ctrl_client.Close();
		ctrl_client.ClearPipe();
		return false;
	} catch (...) {
		data_pipe.reset();
		ctrl_client.Close();
		ctrl_client.ClearPipe();
		return false;
	}

	/* 古い受信機の読み出しを止めてから閉じ、新しい受信機のパイプへ差し替える */
	SetEvent(quit_event_);
	ioq_->Stop();
	data_pipe_ = std::move(data_pipe);

	ctrl_client_.Close();
	std::unique_ptr<px4::PipeClient> ctrl_pipe = ctrl_client.TakePipe();
	ctrl_client_.SetPipe(ctrl_pipe);

	/* 古い受信機の LNB 給電は閉じた時点で解放されるため、選局し直すときに新しい受信機で入れ直す */
	lnb_power_state_ = false;
	reconnect_needed_ = false;

	ResetEvent(quit_event_);
	ioq_->Start();

	msg_info("Reconnected to the receiver after device removal.\n");
	return true;
}

const DWORD BonDriver::WaitTsStream(const DWORD dwTimeOut)
{
	return (ioq_->WaitDataBuffer(std::chrono::milliseconds((dwTimeOut == INFINITE) ? 5000 : dwTimeOut))) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

const DWORD BonDriver::GetReadyCount(void)
{
	return static_cast<::DWORD>(ioq_->GetDataBufferCount() + (ioq_->HaveReadingBuffer() ? 1 : 0));
}

const BOOL BonDriver::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *src;

	if (!pDst)
		return FALSE;

	// コピー型はコピー先の容量を受け取らないため、他の BonDriver と同じくポインター型で取り出した1つのバッファー分をコピーする
	if (!GetTsStream(&src, pdwSize, pdwRemain))
		return FALSE;

	std::memcpy(pDst, src, *pdwSize);

	return TRUE;
}

const BOOL BonDriver::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!ppDst || !pdwSize)
		return FALSE;

	std::size_t size, remain;
	std::uint32_t epoch=0;

	if (!ioq_->ReadBuffer(reinterpret_cast<void **>(ppDst), size, remain, false, &epoch))
		return FALSE;
	if (ts_diagnostics_enabled_) {
		std::lock_guard<std::mutex> diagnostic_lock(diagnostic_mtx_);
		ts_diagnostics_.Feed(*ppDst,size,epoch);
	}

	*pdwSize = static_cast<::DWORD>(size);
	if (pdwRemain)
		*pdwRemain = static_cast<::DWORD>(remain);

	return TRUE;
}

std::uint32_t BonDriver::PurgeStreamLocked()
{
	std::uint32_t epoch = 0;

	/*
	 * データのパイプの PURGE は世代を変えず、応答も待たないため、DriverHost がパージを処理する前に送った TS が
	 * パイプと受信中のバッファーに残り、パージ後の TS の前に渡っていた (J.83B の選局直後の CC の飛び)。
	 * 世代付きの接続では制御のパイプでパージを頼み、返った世代より前のデータを IoQueue で捨てる。
	 * このコマンドを知らない古い DriverHost では、従来のデータのパイプの PURGE に戻す
	 */
	if (!framed_stream_ || !ctrl_client_.PurgeStream(epoch)) {
		px4::command::DataCmd data_cmd;
		std::size_t ret_size;

		epoch = 0;
		data_cmd.cmd = px4::command::DataCmdCode::PURGE;
		data_pipe_->Write(&data_cmd, sizeof(data_cmd), ret_size);
	}

	return epoch;
}

void BonDriver::PurgeTsStream(void)
{
	std::uint32_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(mtx_);

		epoch = PurgeStreamLocked();
	}

	ioq_->PurgeDataBuffer(epoch);
	if (ts_diagnostics_enabled_) {
		std::lock_guard<std::mutex> diagnostic_lock(diagnostic_mtx_);
		ts_diagnostics_.Reset();
	}
	return;
}

void BonDriver::Release(void)
{
	delete this;
}

LPCTSTR BonDriver::GetTunerName(void)
{
	return name_.c_str();
}

const BOOL BonDriver::IsTunerOpening(void)
{
	return open_;
}

LPCTSTR BonDriver::EnumTuningSpace(const DWORD dwSpace)
{
	try {
		return chset_.GetSpaceName(dwSpace).c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

LPCTSTR BonDriver::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	try {
		return chset_.GetChannel(dwSpace, dwChannel).name.c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

const BOOL BonDriver::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!open_)
		return FALSE;

	if (!chset_.ExistsChannel(dwSpace, dwChannel))
		return FALSE;

	std::unique_ptr<std::uint8_t[]> param_buf = MakeParameterSet(dwSpace, dwChannel);

	if (!param_buf)
		return FALSE;

	const px4::command::ParameterSet &param_set = *reinterpret_cast<px4::command::ParameterSet*>(param_buf.get());
	px4::SystemType system = param_set.system;
	bool ret = false;

	try {
		std::lock_guard<std::mutex> lock(mtx_);
		bool removed = false;

		ret = TuneLocked(param_set, removed);
		if (!ret && removed)
			reconnect_needed_ = true;

		/*
		 * 抜去済みの機器では選局が失敗し続けるため、挿し直した機器へつなぎ直して同じ要求を1回だけ選局し直す
		 * 利用者の操作による選局なので、信号レベルの取得で空けている試行の間隔を待たない
		 */
		if (!ret && reconnect_needed_) {
			next_reconnect_tick_ = 0;

			if (ReconnectLocked()) {
				ret = TuneLocked(param_set, removed);
				if (!ret && removed)
					reconnect_needed_ = true;
			}
		}
	} catch (const std::exception &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	}

	// TSMF で選んだ TS は毎秒数十 KB のことがあり、バッファーが埋まるまで待つと最初のデータまで数秒かかるため、ISDB-C だけ 200ms で途中まででも渡す
	// 地デジや BS/CS は 192KB を 0.1 秒前後で埋めるので、従来どおり埋まるまで待つ
	ioq_->SetReadFlushInterval(std::chrono::milliseconds((system == px4::SystemType::ISDB_C) ? 200 : 0));
	PurgeTsStream();

	if (ret) {
		// succeeded
		space_ = dwSpace;
		ch_ = dwChannel;
		tuned_ = true;
	}

	return !!ret;
}

/*
 * チャンネルを DriverHost へ渡す ParameterSet に変換する。周波数へ変換できないチャンネルは nullptr を返す
 * つなぎ直した後に同じチャンネルを選局し直すため、SetChannel() から分けている
 */
std::unique_ptr<std::uint8_t[]> BonDriver::MakeParameterSet(DWORD space, DWORD ch)
{
	const px4::ChannelSet::ChannelInfo &channel = chset_.GetChannel(space, ch);
	px4::SystemType system = chset_.GetSpaceSystem(space);
	std::uint32_t real_freq, num_param = 0;

	switch (system) {
	case px4::SystemType::ISDB_T:
		if ((channel.ptx_ch >= 3 && channel.ptx_ch <= 12) || (channel.ptx_ch >= 22 && channel.ptx_ch <= 62)) {
			// CATV C13-22ch, C23-63ch
			real_freq = 93143 + (channel.ptx_ch * 6000);

			if (channel.ptx_ch == 12)
				real_freq += 2000;
		} else if (channel.ptx_ch >= 63 && channel.ptx_ch <= 112) {
			// UHF 13-62ch
			real_freq = 95143 + (channel.ptx_ch * 6000);
		} else {
			// unknown channel
			return nullptr;
		}

		break;

	case px4::SystemType::ISDB_S:
		if (channel.ptx_ch <= 11) {
			// BS
			real_freq = 1049480 + (channel.ptx_ch * 38360);
		} else if (channel.ptx_ch <= 23) {
			// CS 110
			real_freq = 1613000 + ((channel.ptx_ch - 12) * 40000);
		} else {
			// unknown channel
			return nullptr;
		}

		num_param = 1;

		break;

	case px4::SystemType::ISDB_C:
	{
		// 物理チャンネル番号から中心周波数への変換は、Linux 版と isdbc_channel.h で共有する
		int khz = isdbc_channel_to_khz(static_cast<int>(channel.ptx_ch));

		if (khz < 0) {
			// unknown channel
			return nullptr;
		}

		real_freq = static_cast<std::uint32_t>(khz);
		num_param = 1;

		break;
	}

	case px4::SystemType::J83B:
		// J.83 Annex B (スカパー！プレミアムサービス光) は、ChSet に中心周波数 (kHz) を書く
		real_freq = channel.freq;

		break;

	default:
		// unknown system
		return nullptr;
	}

	/*
	 * CtrlCmdClient::SetParams() は固定長の ParameterSet を値でコピーするため、
	 * params を使わない地上波でも構造体1個分を確保しないと末尾を範囲外参照する
	 * 使わない領域もそのまま送るので、確保した時点で0で埋めておく
	 */
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[std::max<std::size_t>(sizeof(px4::command::ParameterSet), sizeof(px4::command::ParameterSet) - sizeof(px4::command::ParameterSet::params) + (sizeof(px4::command::ParameterSet::params[0]) * num_param))]());
	px4::command::ParameterSet *param_set = reinterpret_cast<px4::command::ParameterSet*>(buf.get());

	param_set->system = system;
	param_set->freq = real_freq;
	param_set->num = num_param;

	if (system == px4::SystemType::ISDB_S && num_param) {
		param_set->params[0].type = px4::command::ParameterType::STREAM_ID;
		param_set->params[0].value = channel.tsid;
	}

	if (system == px4::SystemType::ISDB_C) {
		param_set->params[0].type = px4::command::ParameterType::TSMF_STREAM;
		param_set->params[0].value = channel.tsmf_stream;
	}

	return buf;
}

/*
 * mtx_ を保持した状態で選局する。失敗したときは、抜去済みの機器の受信機での失敗かを removed で返す
 */
bool BonDriver::TuneLocked(const px4::command::ParameterSet &param_set, bool &removed)
{
	const px4::SystemType system = param_set.system;
	bool ret = true;

	removed = false;

	if (lnb_power_) {
		if (system == px4::SystemType::ISDB_S) {
			if (!lnb_power_state_) {
				ret = ctrl_client_.SetLnbVoltage(15);
				lnb_power_state_ = true;
			}
		} else if (lnb_power_state_) {
			ret = ctrl_client_.SetLnbVoltage(0);
			lnb_power_state_ = false;
		}
	}

	if (ret) {
		ret = ctrl_client_.SetParams(param_set);
		if (ret)
			ret = ctrl_client_.Tune(tune_timeout_);
	}

	if (!ret)
		removed = ctrl_client_.LastDeviceRemoved();

	return ret;
}

const DWORD BonDriver::GetCurSpace(void)
{
	return space_;
}

const DWORD BonDriver::GetCurChannel(void)
{
	return ch_;
}

bool BonDriver::ReadProvider::Start()
{
	frames_=px4::StreamFrameReader{};
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	return true;
}

void BonDriver::ReadProvider::Stop()
{
	return;
}

bool BonDriver::ReadProvider::Do(void *buf, std::size_t &size)
{
	if(parent_.framed_stream_) {
		try {return frames_.ReadChunk(buf,size,[this](void* target,std::size_t& count){
			return parent_.data_pipe_->Read(target,count,count,parent_.quit_event_);
		});}catch(const std::exception& error){msg_err("FramedStream: %s\n",error.what());return false;}
	}
	return parent_.data_pipe_->Read(buf, size, size, parent_.quit_event_);
}

} // namespace px4

#pragma warning(disable: 4273)
extern "C" IBonDriver* CreateBonDriver()
{
	try {
		auto bon = new px4::BonDriver();

		if (!bon)
			return nullptr;

		if (!bon->Init()) {
			delete bon;
			return nullptr;
		}

		return bon;
	} catch (const std::exception &e) {
		MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	} catch (...) {
		MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	}
}
#pragma warning(default: 4273)
