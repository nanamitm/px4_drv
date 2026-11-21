// SPDX-License-Identifier: GPL-2.0-only
#include "experimental_device.hpp"
#include "device_identity.hpp"
#include "session.hpp"
#include "winusb_session_port.hpp"
#include "reception_policy.hpp"
#include "../stream_overflow.hpp"
#include "msg.h"
#include "util.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace px4::torne {
namespace {
struct Event {
	HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~Event() { if (value) CloseHandle(value); }
	Event() = default;
	Event(const Event &) = delete;
	Event &operator=(const Event &) = delete;
};

struct PendingRead {
	PendingRead(HANDLE file_handle, WINUSB_INTERFACE_HANDLE usb_handle, HANDLE event)
		: file(file_handle), usb(usb_handle) { operation.hEvent = event; }
	~PendingRead()
	{
		if (active) {
			CancelIoEx(file, &operation);
			ULONG ignored = 0;
			WinUsb_GetOverlappedResult(usb, &operation, &ignored, TRUE);
		}
		}
	HANDLE file;
	WINUSB_INTERFACE_HANDLE usb;
	OVERLAPPED operation{};
	bool active = false;
};

} // namespace

// 個体単位の共有セッション。制御の順序と期限は WinUSB 版と Linux 版で共有する Session が持ち、
// ここでは WinUSB のハンドルとイベントを結び付けるだけにする。
struct ExperimentalSession {
	ExperimentalSession(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed, HANDLE stop)
		: port(file, usb, removed, stop), session(port) {}
	WinUsbSessionPort port;
	Session session;
};

struct ExperimentalDevice::CardState {
	explicit CardState(std::shared_ptr<ExperimentalSession> owner) : session(std::move(owner)) {}
	std::shared_ptr<ExperimentalSession> session;
	CardSession card;
};

int ExperimentalDevice::OpenCard() try
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (card_) return -EBUSY;
	auto session = AcquireSession();
	if (!session) return -ENODEV;
	card_ = std::make_unique<CardState>(std::move(session));
	return 0;
} catch (...) { return -ENOMEM; }

void ExperimentalDevice::CloseCard()
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_) return;
	if (available_ && !card_->card.PowerDown(card_->session->session))
		msg_warn("torne: card power down failed\n");
	card_.reset();
		}

int ExperimentalDevice::DetectCard(bool &detected)
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_ || !available_) return -ENODEV;
	return card_->card.Detect(card_->session->session, detected);
		}

int ExperimentalDevice::ResetCard()
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_ || !available_) return -ENODEV;
	return card_->card.Reset(card_->session->session);
		}

int ExperimentalDevice::SetCardBaudrate(std::uint32_t baudrate)
	{
	// ATRで指定されるB-CASの19200bpsとA-CASの38400bpsを受け付ける。
	if (baudrate != 19200 && baudrate != 38400) return -ENOTSUP;
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_ || !available_) return -ENODEV;
	return card_->card.SetBaudrate(card_->session->session, baudrate);
		}

int ExperimentalDevice::IsCardDataReady(bool &ready) try
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	ready = false;
	if (!card_ || !available_) return -ENODEV;
	return card_->card.DataReady(card_->session->session, ready);
} catch (...) { return -ENOMEM; }

int ExperimentalDevice::ReadCardData(std::uint8_t *buf, std::uint8_t &len)
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_ || !available_) return -ENODEV;
	if (!buf || !len) return -EINVAL;
	len = static_cast<std::uint8_t>(card_->card.Read(buf, len));
	return 0;
		}

int ExperimentalDevice::WriteCardData(const std::uint8_t *buf, std::uint8_t len)
	{
	std::lock_guard<std::mutex> lock(card_mutex_);
	if (!card_ || !available_) return -ENODEV;
	return card_->card.Write(card_->session->session, buf, len);
		}

std::shared_ptr<ExperimentalSession> ExperimentalDevice::AcquireSession()
	{
	// 個体単位のbridgeを共有し、受信とカードがそれぞれbootstrapしない。
	std::lock_guard<std::mutex> lock(session_mutex_);
	if (!available_) return {};
	if (auto current = shared_session_.lock()) return current;
	auto created = std::make_shared<ExperimentalSession>(usb_dev_.dev, usb_dev_.winusb, cancel_, receive_stop_);
	if (!created->session.InitializeBridge(bulk_i2c_) || !available_) return {};
	shared_session_ = created;
	return created;
		}

class ExperimentalDevice::Receiver final : public px4::ReceiverBase {
public:
	explicit Receiver(ExperimentalDevice &parent) : ReceiverBase(0), parent_(parent) {}
	~Receiver() override { Close(); }
	int Open() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (open_) return -EBUSY;
		if (!parent_.available_ || !Stop()) return -ENODEV;
		if (!parent_.available_) { SetEvent(parent_.cancel_); return -ENODEV; }
		try {
			session_ = parent_.AcquireSession();
			if (!session_) return -EIO;
			if (!Control().Recover(parent_.bulk_i2c_)) {
				msg_err("torne: bridge reinitialization failed.\n");
				session_.reset();
				return -EIO;
		}
			if (!Control().InitializeFrontend(parent_.output_error_packets_, (parent_.batch_i2c_writes_ || parent_.bulk_i2c_))) {
				msg_err("torne: initialize failed (%s).\n", Control().FrontendError().c_str());
				SleepFrontend();
				session_.reset();
				return -EIO;
		}
			if (!stream_buf_->Alloc(188u * 65536u)) { SleepFrontend(); session_.reset(); return -ENOMEM; }
		} catch (...) { session_.reset(); return -ENOMEM; }
		stream_buf_->SetThresholdSize(188u * 16u);
		stream_buf_->Start();
		capture_ = tuned_ = failed_ = false;
		open_ = true;
		msg_info("torne: open id=%s\n", parent_.id_.c_str());
	return 0;
		}
	void Close() override
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (!open_) return;
		StopReader();
				SleepFrontend();
				session_.reset();
		stream_buf_->Stop();
		capture_ = tuned_ = failed_ = open_ = false;
		lock.unlock();
		closed_.notify_all();
		}
	void WaitClosed()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		closed_.wait(lock, [this] { return !open_; });
		}
	int SetCapture(bool capture) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_ || !parent_.available_) return -ENODEV;
		capture_ = capture;
		if (!capture) return StopReader();
		if (failed_) return -EIO;
		return tuned_ ? StartReader(Clock::now() + std::chrono::seconds(2)) : 0;
		}
	int SetLnbVoltage(std::int32_t voltage) override { return voltage ? -EINVAL : 0; }
	int CheckLock(bool &locked) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		locked = false;
		if (!open_ || !parent_.available_) return -ECANCELED;
		if (failed_) return -ECANCELED;
		std::vector<std::uint8_t> response;
		if (!Control().ReadStatusWithin(response, std::chrono::seconds(1))) return -ECANCELED;
		locked = tuned_ && StatusLocked(response);
		if (capture_) {
			std::lock_guard<std::mutex> state(state_mutex_);
			locked = locked && first_ && !finished_;
		}
	return 0;
		}
	int ReadStat(px4::command::StatType type, std::int32_t &value) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		value = 0;
		if (!open_ || !parent_.available_) return -ENODEV;
		std::vector<std::uint8_t> response;
		if (!Control().ReadStatusWithin(response, std::chrono::seconds(1))) return -EIO;
		if (type == px4::command::StatType::CNR)
			return ReadStatusValue(response, StatusReading::cnr, value);
		if (type == px4::command::StatType::SIGNAL_STRENGTH)
			return ReadStatusValue(response, StatusReading::signal_strength, value);
		return -ENOSYS;
		}
protected:
	int SetStreamId() override { return 0; }
	int SetFrequency() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_ || !parent_.available_) return -ENODEV;
		if (params_.system != px4::SystemType::ISDB_T || params_.freq < 473143 || params_.freq > 767143 ||
			(params_.bandwidth && params_.bandwidth != 6 && params_.bandwidth != 6000 && params_.bandwidth != 6000000)) return -EINVAL;
		tuned_ = false;
		if (StopReader()) { failed_ = true; return -EIO; }
		if (!stream_buf_->AdvanceEpoch()) return -EIO;
		stream_buf_->ResetInputPacketCount();
		failed_ = false;
        const auto deadline=Clock::now()+TuneBudget;
		const int tuned = TuneFrequency(Control(), parent_.id_, params_.freq, deadline, false, parent_.initial_lock_retry_, Hooks(false));
        if(tuned){failed_=true;return tuned;}
		tuned_ = true;
		const int result = capture_ ? StartReader(deadline) : 0;
		if (result) failed_ = true;
		msg_info("torne: tune id=%s freq_khz=%u result=%d\n", parent_.id_.c_str(), params_.freq, result);
		return result;
		}
private:
	Session &Control() const { return session_->session; }
	HANDLE Stop() const { return parent_.receive_stop_; }
    bool WaitStop(unsigned milliseconds) const {
		const HANDLE events[]{Stop(), parent_.cancel_};
        return WaitForMultipleObjects(2,events,FALSE,milliseconds)==WAIT_TIMEOUT && parent_.available_;
		}
	// 停止要求を受け付ける選局 (受信中の自動復帰) だけが stoppable を true にする。
	TuneHooks Hooks(bool stoppable) const
	{
		return { [this] { return WaitStop(0); }, [this, stoppable](unsigned ms) {
			const HANDLE events[]{parent_.cancel_, Stop()};
			return WaitForMultipleObjects(stoppable ? 2 : 1, events, FALSE, ms) == WAIT_TIMEOUT;
		} };
		}
	void SleepFrontend()
	{
		if (!session_ || !parent_.available_) return;
		px4::torne::SleepFrontend(Control());
		}
	int StartReader(Clock::time_point deadline)
	{
		if (worker_.joinable()) { std::lock_guard<std::mutex> state(state_mutex_); return finished_ ? -EIO : 0; }
		if (!ResetEvent(Stop())) return -EIO;
		{ std::lock_guard<std::mutex> state(state_mutex_); first_ = finished_ = false; }
		worker_error_ = 0;
		worker_stop_ok_ = false;
		try { worker_ = std::thread([this, deadline, frequency=params_.freq] { ReadLoop(deadline,frequency); }); }
		catch (...) { failed_ = true; return -ENOMEM; }
		std::unique_lock<std::mutex> state(state_mutex_);
		const auto first_deadline = (std::min)(deadline, Clock::now() + std::chrono::seconds(2));
		const bool ready = changed_.wait_until(state, first_deadline, [this] { return first_ || finished_; });
		const bool good = ready && first_ && !finished_;
		state.unlock();
		if (!good) { StopReader(); failed_ = true; return -ETIMEDOUT; }
	return 0;
		}
    void ReadLoop(Clock::time_point start_deadline,std::uint32_t frequency) noexcept
	{
        Event complete;
        bool seen_stream=false;
        std::uint64_t attempt=0;
        bool exceptional=false;
		try {
            for(;;) {
				TsDelivery delivery(parent_.output_error_packets_);
                px4::StreamOverflowLog overflow("torne");
                ReceptionWatch watch(Clock::now());
                auto next_status=Clock::now()+std::chrono::seconds(1);
                std::array<std::uint8_t,6144> raw{};
                bool prepared=false,running=false;
                worker_error_=0;worker_stop_ok_=false;
		std::vector<std::uint8_t> response;
				if (!Control().Command(0x12, {1}, response, start_deadline, true)) worker_error_ = ERROR_GEN_FAILURE;
				prepared = !worker_error_ && Control().PrepareStream(start_deadline, true) == StreamResult::ok;
                if(!prepared || !complete.value || !WinUsb_ResetPipe(parent_.usb_dev_.winusb,0x81))worker_error_=ERROR_GEN_FAILURE;
                ReceptionFault fault=ReceptionFault::none;
                auto monitor=[&] {
                    if(!parent_.auto_recovery_ || !seen_stream)return;
                    const auto now=Clock::now();
                    if(now>=next_status) {
		std::vector<std::uint8_t> response;
						if (!Control().ReadStatusWithin(response, std::chrono::seconds(1), true)) { worker_error_ = ERROR_GEN_FAILURE; return; }
						watch.Lock(StatusLocked(response), Clock::now());
                        next_status=Clock::now()+std::chrono::seconds(1);
		}
                    fault=watch.Fault(Clock::now());
                    if(fault!=ReceptionFault::none)worker_error_=ERROR_TIMEOUT;
};
                while(!worker_error_) {
                    if(!WaitStop(0))break;
                    monitor();if(worker_error_)break;
                    if(!ResetEvent(complete.value)){worker_error_=GetLastError();break;}
                    PendingRead read(parent_.usb_dev_.dev,parent_.usb_dev_.winusb,complete.value);
                    const BOOL submitted=WinUsb_ReadPipe(parent_.usb_dev_.winusb,0x81,raw.data(),static_cast<ULONG>(raw.size()),nullptr,&read.operation);
                    const DWORD error=submitted?0:GetLastError();
                    if(!submitted && error!=ERROR_IO_PENDING){worker_error_=error;break;}
                    read.active=true;
                    if(!running) {
						running = Control().StartStream(start_deadline, true) == StreamResult::ok;
                        if(!running)worker_error_=ERROR_GEN_FAILURE;
		}
                    const auto read_deadline=Clock::now()+std::chrono::seconds(2);
                    while(!submitted && !worker_error_) {
						const HANDLE events[]{Stop(), parent_.cancel_, complete.value};
                        const auto waited=WaitForMultipleObjects(3,events,FALSE,parent_.auto_recovery_?200:2000);
                        if(waited==WAIT_OBJECT_0+2)break;
                        if(waited==WAIT_TIMEOUT) {
                            monitor();
                            if(!parent_.auto_recovery_ && Clock::now()>=read_deadline)worker_error_=ERROR_TIMEOUT;
                            // 初回 TS 待ちは StartReader の絶対期限と停止イベントで回収される。
                            continue;
		}
                        worker_error_=waited==WAIT_FAILED?GetLastError():ERROR_OPERATION_ABORTED;
		}
                    if(worker_error_)CancelIoEx(parent_.usb_dev_.dev,&read.operation);
                    ULONG transferred=0;
                    const BOOL collected=WinUsb_GetOverlappedResult(parent_.usb_dev_.winusb,&read.operation,&transferred,TRUE);
                    read.active=false;
                    if(!collected && !worker_error_)worker_error_=GetLastError();
                    if(worker_error_)break;
                    if(transferred>raw.size()){worker_error_=ERROR_INVALID_DATA;break;}
					const auto delivered = delivery.Deliver(raw.data(), transferred, [&](const std::uint8_t *packets, std::size_t size) {
						for (std::size_t offset = 0; offset < size; offset += 188) {
							std::size_t count = 188;
                        // 引数の評価順は決まっていないため、Write が count を書き換えてから記録へ渡す。
							const bool written = stream_buf_->Write(packets + offset, count);
                        overflow.Record(written,188,count);
		}
        });
                    stream_buf_->NotifyWrite();
					if (delivered && !worker_error_) {
                        watch.Received(Clock::now());seen_stream=true;
                        bool resumed=false;
                        {std::lock_guard<std::mutex> state(state_mutex_);resumed=!first_;first_=true;}
                        changed_.notify_all();
                        if(resumed && attempt)msg_info("torne: recovery succeeded id=%s freq_khz=%u attempt=%llu\n",parent_.id_.c_str(),frequency,static_cast<unsigned long long>(attempt));
		}
		}
                // OVERLAPPED 回収後に停止する。カードの共有セッション・USB 電源・
                // ReceiverManager の所有権は保持したまま、受信経路だけを再構築する。
				worker_stop_ok_ = Control().StopStream(Clock::now() + std::chrono::seconds(2)) == StreamResult::ok;
				const auto &framer = delivery.Framer();
				const auto &quality = delivery.Quality();
                msg_info("torne: reader end id=%s freq_khz=%u error=%lu stop_ok=%d fault=%u packets=%llu tei=%llu discarded=%llu raw_cc=%llu raw_malformed=%llu\n",
                    parent_.id_.c_str(),frequency,worker_error_,worker_stop_ok_,static_cast<unsigned>(fault),
                    static_cast<unsigned long long>(framer.Packets()),static_cast<unsigned long long>(framer.TeiPackets()),
                    static_cast<unsigned long long>(framer.DiscardedBytes()),static_cast<unsigned long long>(quality.cc_anomalies),static_cast<unsigned long long>(quality.malformed));
                if(!parent_.auto_recovery_ || !seen_stream || !worker_stop_ok_ || !WaitStop(0) ||
                    (worker_error_!=ERROR_TIMEOUT && worker_error_!=ERROR_SEM_TIMEOUT))break;
                {std::lock_guard<std::mutex> state(state_mutex_);first_=false;}
                if(!stream_buf_->AdvanceEpoch()){worker_error_=ERROR_BUFFER_OVERFLOW;break;}
                const auto recovered=RetryReception([&] {
                    ++attempt;
                    msg_warn("torne: recovery attempt id=%s freq_khz=%u attempt=%llu cause=%u error=%lu\n",parent_.id_.c_str(),frequency,static_cast<unsigned long long>(attempt),static_cast<unsigned>(fault),worker_error_);
		std::vector<std::uint8_t> response;
        const auto deadline=Clock::now()+TuneBudget;
					if (!Control().Command(0x12, {0}, response, deadline, true)) return RecoveryResult::fatal;
					const int result = TuneFrequency(Control(), parent_.id_, frequency, deadline, true, parent_.initial_lock_retry_, Hooks(true));
                    if(!WaitStop(0))return RecoveryResult::stopped;
                    if(!result){start_deadline=Clock::now()+std::chrono::seconds(2);return RecoveryResult::ready;}
					msg_warn("torne: recovery tune failed id=%s freq_khz=%u attempt=%llu result=%d detail=%s\n", parent_.id_.c_str(), frequency, static_cast<unsigned long long>(attempt), result, Control().FrontendError().c_str());
                    return result==-ETIMEDOUT || result==-ENODATA?RecoveryResult::no_lock:RecoveryResult::fatal;
                },[&](unsigned ms){return WaitStop(ms);});
                if(recovered!=RecoveryResult::ready){
                    worker_error_=recovered==RecoveryResult::stopped?ERROR_OPERATION_ABORTED:ERROR_GEN_FAILURE;
                    break;
		}
		}
        }catch(...){worker_error_=ERROR_NOT_ENOUGH_MEMORY;worker_stop_ok_=false;exceptional=true;}
        if(exceptional) {
		try {
				worker_stop_ok_ = Control().StopStream(Clock::now() + std::chrono::seconds(2)) == StreamResult::ok;
            }catch(...){worker_stop_ok_=false;}
		}
        {std::lock_guard<std::mutex> state(state_mutex_);finished_=true;}
                        changed_.notify_all();
		}
	int StopReader()
	{
		if (!worker_.joinable()) return 0;
		SetEvent(Stop());
		worker_.join();
		std::vector<std::uint8_t> response;
		if (!worker_stop_ok_) Control().Invalidate();
		const bool parked = !parent_.available_ || (worker_stop_ok_ && Control().Command(0x12, {0}, response, Clock::now() + std::chrono::seconds(1)));
		// 受信の成否と停止状態を分ける。停止確認後なら次の明示的選局で復帰を試せる。
		const bool good = parked && worker_stop_ok_;
		if (!good || (worker_error_ && worker_error_ != ERROR_OPERATION_ABORTED)) msg_warn("torne: receive stopped with error=%lu known_state=%d\n", worker_error_, good);
		return good ? 0 : -EIO;
		}
	ExperimentalDevice &parent_;
	std::mutex mutex_, state_mutex_;
	std::condition_variable changed_, closed_;
	bool open_ = false, capture_ = false, tuned_ = false, failed_ = false;
	bool first_ = false, finished_ = false;
	std::shared_ptr<ExperimentalSession> session_;
	std::thread worker_;
	DWORD worker_error_ = 0; // workerだけが書き、join後に制御側が読む
	bool worker_stop_ok_ = false;
};

ExperimentalDevice::ExperimentalDevice(const std::wstring &path, const px4::DeviceDefinition &definition,
	std::uintptr_t index, px4::ReceiverManager &manager)
	: DeviceBase(path, definition, index, manager), id_(InterfaceId(path))
	{
	cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!cancel_) throw DeviceError("torne: CreateEvent failed");
	receive_stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!receive_stop_) { CloseHandle(cancel_); cancel_ = nullptr; throw DeviceError("torne: CreateEvent failed"); }
		}
ExperimentalDevice::~ExperimentalDevice()
	{
	Term();
	if (receive_stop_) CloseHandle(receive_stop_);
	if (cancel_) CloseHandle(cancel_);
		}
int ExperimentalDevice::Init()
	{
    const auto backend=device_def_.configs.Get(L"I2cBackend",L"native");
    if (backend == L"source-fr") {
        msg_err("torne: I2cBackend=source-fr has been removed. Set I2cBackend=native or remove the setting.\n");
        return -EINVAL;
		}
    if (backend != L"native") {
        msg_err("torne: invalid I2cBackend (expected native).\n");
        return -EINVAL;
		}
	output_error_packets_=px4::util::wtob(device_def_.configs.Get(L"OutputErrorPackets",L"false"));
	batch_i2c_writes_=px4::util::wtob(device_def_.configs.Get(L"BatchI2cWrites",L"false"));
	bulk_i2c_=px4::util::wtob(device_def_.configs.Get(L"BulkI2c",L"false"));
    auto_recovery_=px4::util::wtob(device_def_.configs.Get(L"AutoRecovery",L"false"));
    initial_lock_retry_=px4::util::wtob(device_def_.configs.Get(L"InitialLockRetry",L"false"));
	// autoでは個体ごとに登録し、ReceiverManagerがOpen可能な受信機を選ぶ。
	// 固定が必要な環境だけ、同じUSBポートのpath表示IDで絞り込む。
	auto configured = device_def_.configs.Get(L"DeviceId", L"auto");
	for (auto &character : configured)
		if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character + (L'a' - L'A'));
	if (!configured.empty() && configured != L"auto") {
		std::string selected;
		if (!NormalizeInterfaceId(std::string(configured.begin(), configured.end()), selected) || selected != id_) return -ENODEV;
		}
	if (usb_dev_.descriptor.idVendor != 0x054c || usb_dev_.descriptor.idProduct != 0x04b2 || device_def_.receivers.size() != 1) return -EINVAL;
	UCHAR alternate = 0xff;
	USB_INTERFACE_DESCRIPTOR descriptor{};
	if (!WinUsb_GetCurrentAlternateSetting(usb_dev_.winusb, &alternate) || alternate != 0 ||
		!WinUsb_QueryInterfaceSettings(usb_dev_.winusb, 0, &descriptor) || descriptor.bInterfaceNumber != 0) return -EINVAL;
	bool stream = false, command = false, answer = false;
	for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
		WINUSB_PIPE_INFORMATION pipe{};
		if (!WinUsb_QueryPipe(usb_dev_.winusb, 0, i, &pipe)) return -EIO;
		stream |= pipe.PipeId == 0x81 && pipe.PipeType == UsbdPipeTypeBulk && pipe.MaximumPacketSize == 512;
		command |= pipe.PipeId == 0x02 && pipe.PipeType == UsbdPipeTypeBulk;
		answer |= pipe.PipeId == 0x83 && pipe.PipeType == UsbdPipeTypeInterrupt;
		}
	if (!stream || !command || !answer) return -EINVAL;
	receiver_ = std::make_unique<Receiver>(*this);
	px4::command::ReceiverInfo info{};
	const auto &definition = device_def_.receivers.front();
	wcscpy_s(info.device_name, device_def_.name.c_str());
	info.device_guid = device_def_.guid;
	const auto name = definition.name + L" [" + std::wstring(id_.begin(), id_.end()) + L"]";
	wcscpy_s(info.receiver_name, name.c_str());
	info.receiver_guid = definition.guid;
	info.systems = px4::SystemType::ISDB_T;
	info.index = definition.index;
	registered_ = receiver_manager_.Register(info, receiver_.get(), GetDeviceIdentity(), true);
	return registered_ ? 0 : -EIO;
		}
void ExperimentalDevice::Term()
	{
	SetAvailability(false);
	if (registered_) { receiver_manager_.Unregister(receiver_.get()); registered_ = false; }
	if (receiver_) { receiver_->WaitClosed(); receiver_.reset(); }
		}
void ExperimentalDevice::SetAvailability(bool available)
	{
	available_ = available;
	if (!available && cancel_) SetEvent(cancel_);
		}
px4::ReceiverBase *ExperimentalDevice::GetReceiver(int id) const
	{
	if (id != 0) throw std::out_of_range("torne receiver index");
	return receiver_.get();
		}
} // namespace px4::torne
