// xit_sqr100_device.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>

#include "device_definition_set.hpp"
#include "device_base.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"
#include "backend_power_state.hpp"

#include "winusb_compat.h"

#include "i2c_comm.h"
#include "it930x.h"
#include "itedtv_bus.h"
#include "cxd2856er.h"
#include "cxd6866.h"

namespace px4 {

#define XITSQR100_DEVICE_TS_SYNC_COUNT	4U
#define XITSQR100_DEVICE_TS_SYNC_SIZE	(188U * XITSQR100_DEVICE_TS_SYNC_COUNT)

struct XitSqr100DeviceConfig final {
	XitSqr100DeviceConfig()
		: usb{ 816, 816, 5, false },
		device{ 2048, 2000, true }
	{}
	struct {
		unsigned int xfer_packets;
		unsigned int urb_max_packets;
		unsigned int max_urbs;
		bool no_raw_io;
	} usb;
	struct {
		unsigned int receiver_max_packets;
		int psb_purge_timeout;
		bool discard_null_packets;
	} device;
};

// PIXELA XIT-SQR100: 地デジ・BS/CS・J.83B を1台の受信機で受信し、内蔵カードリーダーと基板電源を共有する
class XitSqr100Device final : public px4::DeviceBase {
public:
	XitSqr100Device() = delete;
	XitSqr100Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~XitSqr100Device();

	// cannot copy
	XitSqr100Device(const XitSqr100Device &) = delete;
	XitSqr100Device& operator=(const XitSqr100Device &) = delete;

	// cannot move
	XitSqr100Device(XitSqr100Device &&) = delete;
	XitSqr100Device& operator=(XitSqr100Device &&) = delete;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	px4::ReceiverBase * GetReceiver(int id) const override;
	bool HasCardReader() const noexcept override { return true; }
	int OpenCard() override;
	void CloseCard() override;
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(std::uint32_t baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;

private:
	struct StreamContext final {
		std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf;
		/* 最初のコールバック前も未処理データなしの状態として初期化 */
		std::uint8_t remain_buf[XITSQR100_DEVICE_TS_SYNC_SIZE] = {};
		std::size_t remain_len = 0;
	};

	class XitSqr100Receiver final : public px4::ReceiverBase {
	public:
		XitSqr100Receiver() = delete;
		XitSqr100Receiver(XitSqr100Device &parent, std::uintptr_t index);
		~XitSqr100Receiver();

		// cannot copy
		XitSqr100Receiver(const XitSqr100Receiver &) = delete;
		XitSqr100Receiver& operator=(const XitSqr100Receiver &) = delete;

		// cannot move
		XitSqr100Receiver(XitSqr100Receiver &&) = delete;
		XitSqr100Receiver& operator=(XitSqr100Receiver &&) = delete;

		int Open() override;
		void Close() override;
		int CheckLock(bool &locked) override;
		int SetLnbVoltage(std::int32_t voltage) override;
		int SetCapture(bool capture) override;
		int ReadStat(px4::command::StatType type, std::int32_t &value) override;

	protected:
		int SetFrequency() override;
		int SetStreamId() override;

	private:
		XitSqr100Device &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		cxd2856er_demod cxd2856er_;
		cxd6866_tuner cxd6866_;
		px4::SystemType current_system_;
		std::atomic_bool streaming_;
	};

	void LoadConfig();

	int SetBackendPower(bool state);

	int PrepareCapture();
	int StartCapture();
	int StopCapture();

	static void StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len);
	static int StreamHandler(void *context, void *buf, std::uint32_t len);

	XitSqr100DeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	std::atomic_bool init_;
	unsigned int streaming_count_;
	/* 受信機を開いているか (受信機は1台) */
	bool receiver_open_;
	bool card_open_;
	/* 受信機と内蔵カードリーダーで共有する基板電源 (GPIO H5・H2) の状態 */
	px4::BackendPowerState backend_power_;
	std::unique_ptr<XitSqr100Receiver> receiver_;
	it930x_bridge it930x_;
	StreamContext stream_ctx_;
};

} // namespace px4
