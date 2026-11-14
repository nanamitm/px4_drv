// xit_sqr100_device.cpp
//
// yyya-nico/px4_drv の feat/support-sqr100 (2026年10月) の XIT-SQR100 対応を、このリポジトリの受信機・カード・電源の共通処理へ合わせて移植した
// 移植元では Windows の実機で地デジ・BS/CS の受信と内蔵カードの APDU を短時間確認している
// J.83B (5.6Msps) は PX-MLT 系と同じ復調器 (CXD2856ER) の手順と、チューナー (CXD6866AER) の Sony の J.83B の設定で受信し、信号での受信は確認していない

#include "xit_sqr100_device.hpp"

#include <cassert>
#include <cmath>

#include "type.hpp"
#include "command.hpp"
#include "misc_win.h"
#include "ts_sync.h"
#include "cxd2856er_cn_table.hpp"

namespace px4 {

XitSqr100Device::XitSqr100Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager)
	: DeviceBase(path, device_def, index, receiver_manager),
	config_(),
	lock_(),
	available_(true),
	init_(false),
	streaming_count_(0),
	receiver_open_(false),
	card_open_(false),
	backend_power_()
{
	if (usb_dev_.descriptor.idVendor != 0x06b8 || usb_dev_.descriptor.idProduct != 0x106b)
		throw DeviceError("px4::XitSqr100Device::XitSqr100Device: unsupported device. (unknown vendor id or product id)");

	LoadConfig();

	memset(&it930x_, 0, sizeof(it930x_));
}

XitSqr100Device::~XitSqr100Device()
{
	Term();
}

void XitSqr100Device::LoadConfig()
{
	auto &configs = device_def_.configs;

	if (configs.Exists(L"XferPackets"))
		config_.usb.xfer_packets = px4::util::wtoui(configs.Get(L"XferPackets"));

	if (configs.Exists(L"UrbMaxPackets"))
		config_.usb.urb_max_packets = px4::util::wtoui(configs.Get(L"UrbMaxPackets"));

	if (configs.Exists(L"MaxUrbs"))
		config_.usb.max_urbs = px4::util::wtoui(configs.Get(L"MaxUrbs"));

	if (configs.Exists(L"NoRawIo"))
		config_.usb.no_raw_io = px4::util::wtob(configs.Get(L"NoRawIo"));

	if (configs.Exists(L"ReceiverMaxPackets"))
		config_.device.receiver_max_packets = px4::util::wtoui(configs.Get(L"ReceiverMaxPackets"));

	if (configs.Exists(L"PsbPurgeTimeout"))
		config_.device.psb_purge_timeout = px4::util::wtoi(configs.Get(L"PsbPurgeTimeout"));

	if (configs.Exists(L"DiscardNullPackets"))
		config_.device.discard_null_packets = px4::util::wtob(configs.Get(L"DiscardNullPackets"));

	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: xfer_packets: %u\n", config_.usb.xfer_packets);
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: urb_max_packets: %u\n", config_.usb.urb_max_packets);
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: max_urbs: %u\n", config_.usb.max_urbs);
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: no_raw_io: %s\n", (config_.usb.no_raw_io) ? "true" : "false");
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: receiver_max_packets: %u\n", config_.device.receiver_max_packets);
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: psb_purge_timeout: %i\n", config_.device.psb_purge_timeout);
	dev_dbg(&dev_, "px4::XitSqr100Device::LoadConfig: discard_null_packets: %s\n", (config_.device.discard_null_packets) ? "true" : "false");

	return;
}

int XitSqr100Device::Init()
{
	dev_dbg(&dev_, "px4::XitSqr100Device::Init: init_: %s\n", (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (init_)
		return -EALREADY;

	int ret = 0;
	itedtv_bus &bus = it930x_.bus;
	it930x_stream_input& input = it930x_.config.input[0];

	bus.dev = &dev_;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb_dev_;
	bus.usb.ctrl_timeout = 3000;

	it930x_.dev = &dev_;
	it930x_.config.xfer_size = 188 * config_.usb.xfer_packets;
	it930x_.config.i2c_speed = 0x07;
	/* XIT-SQR100 の内蔵カードリーダーは、カード検出 (GPIO H15) とリセット (GPIO H1) が通常と異なる拡張配線 */
	it930x_.bcas_is_extend = true;

	ret = itedtv_bus_init(&bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(&it930x_);
	if (ret)
		goto fail_bridge;

	/*
	 * Windows の USB キャプチャでは、TS 入力はシリアルの port 0
	 * 使わない入力にも別の port を割り当てる (ゼロのままだと、後の設定で port 0 が無効になる)
	 */
	for (int i = 0; i < 5; i++) {
		it930x_.config.input[i].enable = false;
		it930x_.config.input[i].port_number = i;
	}

	input.enable = true;
	input.is_parallel = false;
	input.port_number = 0;
	input.slave_number = 0;
	input.i2c_bus = 3;
	input.i2c_addr = 0x64;
	input.packet_len = 188;
	input.sync_byte = 0x47;

	receiver_.reset(new XitSqr100Receiver(*this, 0));
	stream_ctx_.stream_buf = receiver_->GetStreamBuffer();

	ret = it930x_raise(&it930x_);
	if (ret)
		goto fail_device;

	ret = it930x_load_firmware(&it930x_, "it930x-firmware.bin");
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(&it930x_);
	if (ret)
		goto fail_device;

	/*
	 * USB 転送用の TS の暗号化を IT9300_setTsEncrypt(false) と同じ順で無効にする (Linux 版と同じ)
	 * 放送の B-CAS のスクランブルとは別の機能で、有効のままだと TS の一部が化けて adaptation field や continuity counter が壊れる
	 */
	ret = it930x_write_reg(&it930x_, 0xdab0, 1);
	if (ret)
		goto fail_device;

	ret = it930x_write_reg(&it930x_, 0xdaae, 0);
	if (ret)
		goto fail_device;

	/* GPIO (基板電源は切った状態から始め、H7 は Windows ドライバーの初期化と同じく High にする) */
	ret = it930x_set_gpio_mode(&it930x_, 5, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 5, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(&it930x_, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 2, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(&it930x_, 7, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 7, true);
	if (ret)
		goto fail_device;

	if (config_.device.discard_null_packets) {
		it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(&it930x_, 0, &filter);
		if (ret)
			goto fail_device;
	}

	backend_power_.SetOff();
	init_ = true;

	for (auto it = device_def_.receivers.cbegin(); it != device_def_.receivers.cend(); ++it) {
		px4::command::ReceiverInfo ri;

		if (it->index != 0)
			continue;

		wcscpy_s(ri.device_name, device_def_.name.c_str());
		ri.device_guid = device_def_.guid;
		wcscpy_s(ri.receiver_name, it->name.c_str());
		ri.receiver_guid = it->guid;
		ri.systems = it->systems;
		// J.83B は地上波と同じ RF 入力を使うため、PX-MLT 系と同じく地上波を受信できる受信機では ini に書かなくても受け付ける
		// TSMF の ISDB-C は、この機種では選局の手順を確かめていないため受け付けない
		if ((it->systems & px4::SystemType::ISDB_T) == px4::SystemType::ISDB_T)
			ri.systems |= px4::SystemType::J83B;
		ri.index = it->index;
		ri.data_id = 0;

		receiver_manager_.Register(ri, receiver_.get(), GetDeviceIdentity());
	}

	return 0;

fail_device:
	stream_ctx_.stream_buf = nullptr;
	receiver_.reset();

	it930x_term(&it930x_);

fail_bridge:
	itedtv_bus_term(&bus);

fail_bus:
	return ret;
}

void XitSqr100Device::Term()
{
	dev_dbg(&dev_, "px4::XitSqr100Device::Term: init_: %s\n", (init_) ? "true" : "false");

	std::unique_lock<std::recursive_mutex> lock(lock_);

	if (!init_)
		return;

	receiver_manager_.Unregister(receiver_.get());

	lock.unlock();

	stream_ctx_.stream_buf = nullptr;
	receiver_.reset();

	lock.lock();

	it930x_term(&it930x_);
	itedtv_bus_term(&it930x_.bus);

	init_ = false;
	return;
}

void XitSqr100Device::SetAvailability(bool available)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	/* 切断後の古いカードセッションから USB 制御転送を発行せず、共有電源の状態も切断済みにする */
	if (available_ && !available) {
		card_open_ = false;
		backend_power_.Disconnect();
	}
	available_ = available;
}

ReceiverBase* XitSqr100Device::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("receiver id out of range");

	return receiver_.get();
}

int XitSqr100Device::OpenCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!available_)
		return -ENODEV;
	if (card_open_)
		return -EBUSY;

	/* カードの検出と通信には基板の電源が必要で、チューナー未使用時だけ電源を投入し、受信中の電源には触れない */
	if (!receiver_open_) {
		int ret = SetBackendPower(true);
		if (ret)
			return ret;
	}

	int ret = it930x_bcas_init(&it930x_);
	if (!ret) {
		/* 移植元の実機の確認では、GPIO H7 を Low にしないとカードの ATR を受信できない */
		ret = it930x_write_gpio(&it930x_, 7, false);
	}
	if (ret) {
		if (!receiver_open_)
			SetBackendPower(false);
		return ret;
	}

	card_open_ = true;
	return 0;
}

void XitSqr100Device::CloseCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!card_open_)
		return;

	card_open_ = false;
	if (!receiver_open_)
		SetBackendPower(false);
}

int XitSqr100Device::DetectCard(bool &detected)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ ? it930x_bcas_detect_card(&it930x_, &detected) : -ENODEV;
}

int XitSqr100Device::ResetCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_reset_card(&it930x_) : -ENODEV;
}

int XitSqr100Device::SetCardBaudrate(std::uint32_t baudrate)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_set_baudrate(&it930x_, baudrate) : -ENODEV;
}

int XitSqr100Device::IsCardDataReady(bool &ready)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_check_ready(&it930x_, &ready) : -ENODEV;
}

int XitSqr100Device::ReadCardData(std::uint8_t *buf, std::uint8_t &len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_get_data(&it930x_, buf, &len) : -ENODEV;
}

int XitSqr100Device::WriteCardData(const std::uint8_t *buf, std::uint8_t len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_send_data(&it930x_, buf, len) : -ENODEV;
}

int XitSqr100Device::SetBackendPower(bool state)
{
	dev_dbg(&dev_, "px4::XitSqr100Device::SetBackendPower: state: %s\n", (state) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> lock(lock_);
	return backend_power_.Request(state, available_, (receiver_open_ ? 1U : 0U) + (card_open_ ? 1U : 0U),
		[this](bool on) {
		int ret = 0;
		/*
		 * Windows ドライバーの CXD2856+CXD6866 の初期化 (GPIO H2 を Low にして 100ms 後に High) を基に、移植元が実機で使っている手順
		 * GPIO H5 と H2 が基板のどの信号かは確かめられていないため、待機時間を含めて移植元の値を変えない
		 */
		if (on) {
			ret = it930x_write_gpio(&it930x_, 5, true);
			if (ret)
				return ret;

			Sleep(50);

			ret = it930x_write_gpio(&it930x_, 2, false);
			if (ret)
				return ret;

			Sleep(100);

			ret = it930x_write_gpio(&it930x_, 2, true);
			if (ret)
				return ret;

			Sleep(20);
		} else {
			/* 片方に失敗してももう一方を停止し、最初のエラーを保持する */
			ret = it930x_write_gpio(&it930x_, 5, false);
			int second = it930x_write_gpio(&it930x_, 2, false);
			if (!ret)
				ret = second;
		}
		if (ret)
			dev_err(&dev_, "px4::XitSqr100Device::SetBackendPower: %s failed. (ret: %d)\n", on ? "on" : "off", ret);
		return ret;
	});
}

int XitSqr100Device::PrepareCapture()
{
	dev_dbg(&dev_, "px4::XitSqr100Device::PrepareCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (streaming_count_)
		return 0;

	ret = it930x_purge_psb(&it930x_, config_.device.psb_purge_timeout);
	if (ret)
		dev_err(&dev_, "px4::XitSqr100Device::PrepareCapture: it930x_purge_psb() failed. (ret: %d)\n", ret);

	return (ret && ret != -ETIMEDOUT) ? ret : 0;
}

int XitSqr100Device::StartCapture()
{
	dev_dbg(&dev_, "px4::XitSqr100Device::StartCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!streaming_count_) {
		it930x_.bus.usb.streaming.urb_buffer_size = 188 * config_.usb.urb_max_packets;
		it930x_.bus.usb.streaming.urb_num = config_.usb.max_urbs;
		it930x_.bus.usb.streaming.no_dma = true;
		it930x_.bus.usb.streaming.no_raw_io = config_.usb.no_raw_io;

		/* 前の受信の端数を次の受信の TS へつなげない */
		stream_ctx_.remain_len = 0;

		ret = itedtv_bus_start_streaming(&it930x_.bus, StreamHandler, this);
		if (ret) {
			dev_err(&dev_, "px4::XitSqr100Device::StartCapture: itedtv_bus_start_streaming() failed. (ret: %d)\n", ret);
			return ret;
		}
		streaming_count_++;
	}

	dev_dbg(&dev_, "px4::XitSqr100Device::StartCapture: streaming_count_: %u\n", streaming_count_);

	return 0;
}

int XitSqr100Device::StopCapture()
{
	dev_dbg(&dev_, "px4::XitSqr100Device::StopCapture\n");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (streaming_count_) {
		dev_dbg(&dev_, "px4::XitSqr100Device::StopCapture: stopping...\n");
		itedtv_bus_stop_streaming(&it930x_.bus);
		streaming_count_ = 0;
	}

	return 0;
}

void XitSqr100Device::StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len)
{
	std::uint8_t *p = *buf;
	std::size_t remain = len;

	while (remain) {
		std::size_t i = 0;
		bool sync_remain = false;

		while (true) {
			if (((i + 1) * 188) <= remain) {
				/* 受信機番号のない通常の TS (同期バイト 0x47) の連続する同期バイトを数え、崩れた位置の直前までを出力する */
				if (!px4_ts_has_plain_sync(p[i * 188]))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (i < XITSQR100_DEVICE_TS_SYNC_COUNT) {
			p++;
			remain--;
			continue;
		}

		std::size_t pkt_len = 188 * i;
		stream_buf->Write(p, pkt_len);

		p += 188 * i;
		remain -= 188 * i;

		if (sync_remain)
			break;
	}

	stream_buf->NotifyWrite();

	*buf = p;
	len = remain;

	return;
}

int XitSqr100Device::StreamHandler(void *context, void *buf, std::uint32_t len)
{
	XitSqr100Device &obj = *static_cast<XitSqr100Device*>(context);
	StreamContext &stream_ctx = obj.stream_ctx_;
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = len;

	if (stream_ctx.remain_len) {
		if ((stream_ctx.remain_len + len) >= XITSQR100_DEVICE_TS_SYNC_SIZE) {
			std::uint8_t * remain_buf = stream_ctx.remain_buf;
			std::size_t t = XITSQR100_DEVICE_TS_SYNC_SIZE - stream_ctx.remain_len;

			memcpy(remain_buf + stream_ctx.remain_len, p, t);
			stream_ctx.remain_len = XITSQR100_DEVICE_TS_SYNC_SIZE;

			StreamProcess(stream_ctx.stream_buf, &remain_buf, stream_ctx.remain_len);
			if (!stream_ctx.remain_len) {
				p += t;
				remain -= t;
			}

			stream_ctx.remain_len = 0;
		} else {
			memcpy(stream_ctx.remain_buf + stream_ctx.remain_len, p, len);
			stream_ctx.remain_len += len;

			return 0;
		}
	}

	StreamProcess(stream_ctx.stream_buf, &p, remain);

	if (remain) {
		memcpy(stream_ctx.remain_buf, p, remain);
		stream_ctx.remain_len = remain;
	}

	return 0;
}

XitSqr100Device::XitSqr100Receiver::XitSqr100Receiver(XitSqr100Device &parent, std::uintptr_t index)
	: ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE | RECEIVER_WAIT_AFTER_LOCK_TC_T),
	parent_(parent),
	index_(index),
	lock_(),
	open_(false),
	current_system_(px4::SystemType::UNSPECIFIED),
	streaming_(false)
{
	/* 受信機のメンバーはゼロ初期化されないため、設定しない項目も 0 にしておく */
	memset(&cxd2856er_, 0, sizeof(cxd2856er_));
	memset(&cxd6866_, 0, sizeof(cxd6866_));

	const device *dev = &parent_.GetDevice();
	const it930x_stream_input &input = parent_.it930x_.config.input[0];

	/* 復調器の I2C アドレスは Windows ドライバーの 8 ビット表記 0xC8 に当たり、PX-MLT 系と同じく SLV-X は SLV-T + 2 */
	cxd2856er_.dev = dev;
	cxd2856er_.i2c = &parent_.it930x_.i2c_master[input.i2c_bus - 1];
	cxd2856er_.i2c_addr.slvx = input.i2c_addr + 2;
	cxd2856er_.i2c_addr.slvt = input.i2c_addr;
	cxd2856er_.config.xtal = 24000;
	cxd2856er_.config.tuner_i2c = true;
	cxd2856er_.config.serial_ts_clock = true;

	/* Windows ドライバーが FREIA に渡す構成フラグ 0x10004000 (内部ループフィルター・REFOUT 800mVpp) と 24MHz の水晶 */
	cxd6866_.dev = dev;
	cxd6866_.i2c = &cxd2856er_.i2c_master;
	cxd6866_.i2c_addr = 0x60;
	cxd6866_.config.xtal = 24000;
	cxd6866_.config.loop_filter_internal = true;
	cxd6866_.config.outlmt_dtv_1_2vpp = false;
	cxd6866_.config.refout_enable = true;
}

XitSqr100Device::XitSqr100Receiver::~XitSqr100Receiver()
{
	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::~XitSqr100Receiver(%u)\n", index_);

	std::unique_lock<std::mutex> lock(lock_);

	close_cond_.wait(lock, [this] { return !open_; });

	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::~XitSqr100Receiver(%u): exit\n", index_);
}

int XitSqr100Device::XitSqr100Receiver::Open()
{
	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): open_: %s\n", index_, (open_) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (open_)
		return -EALREADY;

	if (!parent_.available_)
		return -ENODEV;

	if (!parent_.card_open_) {
		ret = parent_.SetBackendPower(true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): SetBackendPower(true) failed. (ret: %d)\n", index_, ret);
			return ret;
		}
	}

	ret = cxd2856er_init(&cxd2856er_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): cxd2856er_init() failed. (ret: %d)\n", index_, ret);
		goto fail_demod_init;
	}

	/* 移植元と Windows ドライバーと同じく、Sony の初期化のコールバックで SLV-X の 0x08 に待機状態を書き、チューナーの初期化の間は復調器を待機させる */
	ret = cxd2856er_write_slvx_reg(&cxd2856er_, 0x08, 0x01);
	if (ret) {
		dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): CXD2856ER init sleep failed. (ret: %d)\n", index_, ret);
		goto fail_tuner_init;
	}

	ret = cxd6866_init(&cxd6866_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): cxd6866_init() failed. (ret: %d)\n", index_, ret);
		goto fail_tuner_init;
	}

	ret = cxd2856er_write_slvx_reg(&cxd2856er_, 0x08, 0x00);
	if (ret) {
		dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Open(%u): CXD2856ER init wakeup failed. (ret: %d)\n", index_, ret);
		goto fail_backend;
	}

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc4, 0x80, 0x88);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0xa0);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xb9, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	current_system_ = px4::SystemType::UNSPECIFIED;

	parent_.receiver_open_ = true;
	open_ = true;

	return 0;

fail_backend:
	cxd6866_term(&cxd6866_);

fail_tuner_init:
	cxd2856er_term(&cxd2856er_);

fail_demod_init:
	/* カードを使っていなければ、初期化前に入れた電源を切る */
	if (!parent_.card_open_)
		parent_.SetBackendPower(false);

	return ret;
}

void XitSqr100Device::XitSqr100Receiver::Close()
{
	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::Close(%u): open_: %s\n", index_, (open_) ? "true" : "false");

	if (!open_)
		return;

	SetCapture(false);

	std::unique_lock<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	/* 切断後は USB の制御転送を発行せず、受信機の状態だけを戻す */
	if (parent_.available_) {
		cxd6866_term(&cxd6866_);
		cxd2856er_term(&cxd2856er_);
	}

	parent_.receiver_open_ = false;
	if (!parent_.card_open_)
		parent_.SetBackendPower(false);

	current_system_ = px4::SystemType::UNSPECIFIED;
	open_ = false;

	lock.unlock();
	close_cond_.notify_all();

	return;
}

int XitSqr100Device::XitSqr100Receiver::SetFrequency()
{
	if (!open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): system: %d freq: %u\n", index_, params_.system, params_.freq);

	int ret = 0;
	cxd2856er_system_params demod_params = { 0 };
	std::lock_guard<std::mutex> lock(lock_);

	current_system_ = px4::SystemType::UNSPECIFIED;

	switch (params_.system) {
	case px4::SystemType::ISDB_T:
		demod_params.bandwidth = (params_.bandwidth) ? params_.bandwidth : 6;

		ret = cxd2856er_wakeup(&cxd2856er_, CXD2856ER_ISDB_T_SYSTEM, &demod_params);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd2856er_wakeup(CXD2856ER_ISDB_T_SYSTEM) failed. (ret: %d)\n", index_, ret);

		break;

	case px4::SystemType::ISDB_S:
		ret = cxd2856er_wakeup(&cxd2856er_, CXD2856ER_ISDB_S_SYSTEM, &demod_params);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd2856er_wakeup(CXD2856ER_ISDB_S_SYSTEM) failed. (ret: %d)\n", index_, ret);

		break;

	// J.83 Annex B は TSMF を使わないため、地デジと同じくブリッジで null パケットを除く (初期化時の設定をそのまま使う)
	// 復調器の J.83B の手順と IF (3.75MHz) は PX-MLT 系と共通で、Sony のコードでもチューナーの種類によらない
	case px4::SystemType::J83B:
		ret = cxd2856er_wakeup(&cxd2856er_, CXD2856ER_J83B_SYSTEM, &demod_params);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd2856er_wakeup(CXD2856ER_J83B_SYSTEM) failed. (ret: %d)\n", index_, ret);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	switch (params_.system) {
	case px4::SystemType::ISDB_T:
		ret = cxd6866_set_params_t(&cxd6866_, CXD6866_ISDB_T_SYSTEM, params_.freq, 6);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd6866_set_params_t(%u, 6) failed. (ret: %d)\n", index_, params_.freq, ret);

		break;

	case px4::SystemType::ISDB_S:
		ret = cxd6866_set_params_s(&cxd6866_, CXD6866_ISDB_S_SYSTEM, params_.freq, 28860);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd6866_set_params_s(%u, 28860) failed. (ret: %d)\n", index_, params_.freq, ret);

		break;

	case px4::SystemType::J83B:
		ret = cxd6866_set_params_j83b(&cxd6866_, params_.freq);
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd6866_set_params_j83b(%u) failed. (ret: %d)\n", index_, params_.freq, ret);

		break;

	default:
		break;
	}

	if (ret)
		return ret;

	ret = cxd2856er_post_tune(&cxd2856er_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetFrequency(%u): cxd2856er_post_tune() failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	current_system_ = params_.system;
	return ret;
}

int XitSqr100Device::XitSqr100Receiver::CheckLock(bool &locked)
{
	if (!open_)
		return -EINVAL;

	int ret = 0;
	std::unique_lock<std::mutex> lock(lock_);

	locked = false;

	switch (current_system_) {
	case px4::SystemType::ISDB_T:
	{
		bool unlocked = false;

		ret = cxd2856er_is_ts_locked_isdbt(&cxd2856er_, &locked, &unlocked);
		if (!ret && unlocked)
			ret = -ECANCELED;

		break;
	}

	case px4::SystemType::ISDB_S:
		ret = cxd2856er_is_ts_locked_isdbs(&cxd2856er_, &locked);
		break;

	// J.83B も ISDB-C と同じ SLV-T バンク 0x40 のロックの表示を使う
	case px4::SystemType::J83B:
		ret = cxd2856er_is_ts_locked_isdbc(&cxd2856er_, &locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	/* ISDBT2071 と同じく、ロックした時点で USB の受信を始め、選局の前に届いた TS を捨てる */
	if (!ret && locked) {
		lock.unlock();
		ret = SetCapture(true);
	}

	return ret;
}

int XitSqr100Device::XitSqr100Receiver::SetStreamId()
{
	if (params_.system != px4::SystemType::ISDB_S)
		return -EINVAL;

	if (!open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	/* 12 未満は相対 TS 番号、12 以上は TSID として扱う */
	if (params_.stream_id < 12) {
		ret = cxd2856er_set_slot_isdbs(&cxd2856er_, static_cast<u16>(params_.stream_id));
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetStreamId(%u): cxd2856er_set_slot_isdbs(%u) failed. (ret: %d)\n", index_, params_.stream_id, ret);
	} else if (params_.stream_id <= 0xffff) {
		ret = cxd2856er_set_tsid_isdbs(&cxd2856er_, static_cast<u16>(params_.stream_id));
		if (ret)
			dev_err(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetStreamId(%u): cxd2856er_set_tsid_isdbs(%u) failed. (ret: %d)\n", index_, params_.stream_id, ret);
	} else {
		ret = -EINVAL;
	}

	return ret;
}

int XitSqr100Device::XitSqr100Receiver::SetLnbVoltage(std::int32_t voltage)
{
	/* XIT-SQR100 は BS/CS アンテナへの給電に対応していない */
	return (voltage) ? -EINVAL : 0;
}

int XitSqr100Device::XitSqr100Receiver::SetCapture(bool capture)
{
	if (!open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetCapture(%u): capture: %s\n", index_, (capture) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (capture) {
		ret = parent_.PrepareCapture();
		if (ret)
			return ret;

		ret = parent_.StartCapture();
		if (ret)
			return ret;

		std::size_t size = 188 * parent_.config_.device.receiver_max_packets;

		if (!streaming_) {
			stream_buf_->Alloc(size);
			stream_buf_->SetThresholdSize(size / 10);
			stream_buf_->Start();
			streaming_ = true;
		} else {
			stream_buf_->Purge();
		}
	} else {
		ret = parent_.StopCapture();
		if (ret)
			return ret;

		stream_buf_->Stop();
		streaming_ = false;
	}

	dev_dbg(&parent_.dev_, "px4::XitSqr100Device::XitSqr100Receiver::SetCapture(%u): succeeded.\n", index_);

	return ret;
}

int XitSqr100Device::XitSqr100Receiver::ReadStat(px4::command::StatType type, std::int32_t &value)
{
	if (!open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	value = 0;

	switch (type) {
	case px4::command::StatType::SIGNAL_STRENGTH:
		// not implemented
		ret = -ENOSYS;
		break;

	case px4::command::StatType::CNR:
	{
		std::uint16_t val;

		switch (current_system_) {
		case px4::SystemType::ISDB_T:
			ret = cxd2856er_read_cnr_raw_isdbt(&cxd2856er_, &val);
			if (ret)
				break;

			/* PX-MLT 系と同じ換算式 (生値 0 は対数を取れないため 0 のまま返す) */
			if (val)
				value = static_cast<std::int32_t>((std::log10(val) * 10000) - 9031);

			break;

		case px4::SystemType::ISDB_S:
			ret = cxd2856er_read_cnr_raw_isdbs(&cxd2856er_, &val);
			if (ret)
				break;

			value = px4::Cxd2856erIsdbsCnr(val);
			break;

		case px4::SystemType::J83B:
			ret = cxd2856er_read_cnr_raw_isdbc(&cxd2856er_, &val);
			if (ret)
				break;

			// Sony の sony_demod_isdbc_monitor_SNR() の 64QAM・256QAM 用の換算式 (PX-MLT 系の J.83B と同じ)
			if (val)
				value = static_cast<std::int32_t>(-9500 * std::log((val < 126) ? 126 : val) + 95941);

			break;

		default:
			ret = -EINVAL;
			break;
		}

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

} // namespace px4
