// pxmlt_device.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#include "device_definition_set.hpp"
#include "device_base.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"
#include "ringbuffer.hpp"

#include "winusb_compat.h"

#include "i2c_comm.h"
#include "it930x.h"
#include "itedtv_bus.h"
#include "cxd2856er.h"
#include "cxd2858er.h"

namespace px4 {

#define PXMLT_DEVICE_TS_SYNC_COUNT	4
#define PXMLT_DEVICE_TS_SYNC_SIZE	(188 * PXMLT_DEVICE_TS_SYNC_COUNT)

enum class PxMltDeviceModel {
	PXMLT5U = 0,
	PXMLT5PE,
	PXMLT8PE3,
	PXMLT8PE5,
	ISDB6014_4TS
};

struct PxMltDeviceConfig final {
	PxMltDeviceConfig()
		: usb{ 816, 816, 5, false },
		device{ 2048, 2000, true, false, false, false, 0, false, false, false, false, 3000, 50, false, false, false, false, false }
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
		/* Requested through DiscardNullPackets=true when the experimental
		 * ISDB-C path is enabled.  It is applied at the final TS output. */
		bool discard_null_packets_on_output;
		/* Requested through DiscardTsmfHeaderPackets.  Enabled only while
		 * this receiver is tuned to ISDB-C. */
		bool discard_tsmf_header_packets;
		/* Explicit opt-in for the unverified CXD2856ER J.83C profile. */
		bool experimental_isdbc;
		/* 0: use BonDriver request; 64 or 256: override it for diagnostics. */
		unsigned int force_isdbc_modulation;
		/* Sony/TBS DVB-C 6 MHz 参照値との比較用。既定 false。 */
		bool isdbc_reference_profile;
		/* 信号で検証するまで ISDB-C 要求を J.83B に置換する暫定設定。 */
		bool experimental_j83b;
		/* J.83B の取得段階・FEC 計数の生値を C/N 取得時にログへ出す。
		 * 共有制御バスへ1回あたり16〜19回の I/O を足すため既定 false。 */
		bool experimental_j83b_monitor;
		/* 同じデバイスの別受信機の Open・選局から一定時間だけ、録画中の J.83B
		 * 受信機の RS 計数と IF AGC を短い間隔で記録する。既定 false。 */
		bool experimental_j83b_burst_monitor;
		unsigned int j83b_burst_window_ms;
		unsigned int j83b_burst_interval_ms;
		/* デバイスの初期化から終了まで、受信機やカードの利用と関係なくバックエンド
		 * 電源を入れたままにする。同じ基板の別 USB デバイスで録画中に、このデバイスの
		 * 電源投入が起きないようにするための設定で、既定 false。 */
		bool hold_backend_power;
		/* J.83B のチューナーの過大入力検出レベルに地デジの値を使う試験用設定。既定 false。 */
		bool j83b_ter_overload_level;
		/* 同じデバイスの別受信機が使用中なら、地デジの受信機を閉じるときにチューナーを
		 * 停止しない。J.83B の受信機を閉じた場合と同じく、次の Open の初期化か
		 * バックエンド電源の切断まで動作を続ける。比較試験用で既定 false。 */
		bool keep_tuner_on_close;
		/* HoldBackendPower で電源を保持したデバイスの初期化時に、全チューナーを一度
		 * 選局して動作させたままにし、以後の Open では初期化を省き、Close でも停止しない。
		 * Open 後の最初の選局でチューナーが停止状態から動き出す変化を、録画中に
		 * 起こさないための比較試験用設定で、既定 false。 */
		bool warm_tuners;
		/* J.83B モニターの読み取りに合わせて、チューナーの AGC 生値と入力レベルの
		 * 推定値をログへ出す。チューナーのレジスタを一時的に切り替えるため既定 false。 */
		bool experimental_tuner_rssi;
	} device;
};

class PxMltDevice final : public px4::DeviceBase {
public:
	PxMltDevice() = delete;
	PxMltDevice(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~PxMltDevice();

	// cannot copy
	PxMltDevice(const PxMltDevice &) = delete;
	PxMltDevice& operator=(const PxMltDevice &) = delete;

	// cannot move
	PxMltDevice(PxMltDevice &&) = delete;
	PxMltDevice& operator=(PxMltDevice &&) = delete;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	px4::ReceiverBase* GetReceiver(int id) const override;
	bool HasCardReader() const noexcept override { return model_ != PxMltDeviceModel::PXMLT8PE3; }
	int OpenCard() override;
	void CloseCard() override;
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(::it930x_uart_baudrate baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;

private:
	/* 同期探索で捨てた TS を計測する。受信結果には影響させず、ログだけに使う。
	 * 欠落が復調器の誤り判定 (同期バイト bit7) によるものか、境界の喪失によるものかを
	 * EDCB などのドロップ記録と時刻で照合できるようにする。 */
	struct StreamStats final {
		/* 同期を失ってから次のパケットを配信するまでを1回と数える */
		std::uint64_t resync_events;
		/* 同期探索で読み飛ばしたバイト数。誤りフラグ付きパケットのバイトも含む */
		std::uint64_t skipped_bytes;
		/* 境界が揃った位置で見つかった bit7 付きパケット数 (受信機番号 1-5 別)。
		 * 境界が不明な位置の誤りパケットは数えられないため下限値として扱う */
		std::uint64_t error_flagged[5];
		/* 直前に配信したパケットの直後を処理中なら true */
		bool aligned;
		/* 同期を失い、まだ次のパケットを配信していなければ true */
		bool in_resync;
		/* ログ出力の間引き用。前回出力時の値と時刻 */
		std::uint64_t logged_resync_events;
		std::uint64_t logged_skipped_bytes;
		std::uint64_t logged_error_flagged[5];
		std::uint64_t last_log_tick;
	};

	struct StreamContext final {
		std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[5];
		/* 最初のコールバック前も未処理データなしの状態として初期化 */
		std::uint8_t remain_buf[PXMLT_DEVICE_TS_SYNC_SIZE] = {};
		std::size_t remain_len = 0;
		StreamStats stats = {};
	};

	class PxMltReceiver final : public px4::ReceiverBase {
	public:
		PxMltReceiver() = delete;
		PxMltReceiver(PxMltDevice &parent, std::uintptr_t index);
		~PxMltReceiver();

		// cannot copy
		PxMltReceiver(const PxMltReceiver &) = delete;
		PxMltReceiver& operator=(const PxMltReceiver &) = delete;

		// cannot move
		PxMltReceiver(PxMltReceiver &&) = delete;
		PxMltReceiver& operator=(PxMltReceiver &&) = delete;

		int Open() override;
		void Close() override;
		int CheckLock(bool &locked) override;
		int SetLnbVoltage(std::int32_t voltage) override;
		int SetCapture(bool capture) override;
		int ReadStat(px4::command::StatType type, std::int32_t &value) override;

		/* バースト記録の 1 標本。選局中・停止中などの受信機は読まずに
		 * sampled=false を返す。busy は受信機の処理中で読まなかったことを示す。 */
		struct BurstSample {
			bool sampled;
			bool busy;
			int ret;
			std::uint32_t freq;
			std::uint64_t read_us;
			cxd2856er_j83b_rs_sample value;
		};
		void SampleJ83BBurst(BurstSample &out);

		/* ExperimentalWarmTuners: 開いていない受信機のチューナーを初期化・選局して
		 * 動作させたままにする。電源の切断時は InvalidateWarmTuner で無効にする。
		 * どちらもデバイスのロック下で呼ぶ。 */
		int WarmUpTuner();
		void InvalidateWarmTuner() noexcept { tuner_warm_ = false; }

	protected:
		int PrepareTune() override;
		int SetFrequency() override;
		int SetStreamId() override;

	private:
		static const struct PxMltReceiverCnTableIsdbS final {
			uint16_t val;
			uint32_t cnr;
		} isdbs_cn_table_[];

		PxMltDevice &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		bool lnb_power_;
		std::mutex *tuner_lock_;
		cxd2856er_demod cxd2856er_;
		cxd2858er_tuner cxd2858er_;
		px4::SystemType current_system_;
		bool streaming_;
		/* CheckLock のログ出力を状態変化時に限るための、直前に出力した状態。
		 * 値は state_names の添字と一致させる。 */
		enum : int {
			LOCK_LOG_STATE_UNKNOWN = -1,
			LOCK_LOG_STATE_WAITING = 0,
			LOCK_LOG_STATE_LOCKED = 1,
			LOCK_LOG_STATE_UNLOCKED = 2
		};
		int lock_log_state_;
		/* チューナーが電源投入後に初期化・選局済みで、動作を続けていれば true */
		bool tuner_warm_;
	};

	void LoadConfig();

	const i2c_comm_master& GetI2cMaster(int bus) const;

	int SetBackendPower(bool state);
	int SetLnbVoltage(std::int32_t voltage);

	int StartCapture();
	int StopCapture();

	static void StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[], int num, std::uint8_t **buf, std::size_t &len, StreamStats &stats);
	void LogStreamStats(bool total);
	static int StreamHandler(void *context, void *buf, std::uint32_t len);

	/* J.83B バースト記録。要求側は期限を設定するだけで I/O を行わない。 */
	void RequestJ83BBurst(std::uintptr_t trigger_index, const char *reason);
	void StartJ83BBurstWorker();
	void StopJ83BBurstWorker();
	void J83BBurstWorker();

	static const struct PxMltDeviceParam final {
		std::uint8_t i2c_addr;
		std::uint8_t i2c_bus;
		std::uint8_t port_number;
	} params_[][5];

	PxMltDeviceModel model_;
	PxMltDeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	std::atomic_bool init_;
	unsigned int open_count_;
	bool card_open_;
	/* HoldBackendPower による電源の保持。受信機・カードと並ぶ 3 つ目の利用者として数える。 */
	bool power_hold_;
	unsigned int lnb_power_count_;
	unsigned int streaming_count_;
	std::mutex tuner_lock_[2];
	int receiver_num_;
	std::unique_ptr<PxMltReceiver> receivers_[5];
	it930x_bridge it930x_;
	StreamContext stream_ctx_;

	std::thread burst_thread_;
	std::mutex burst_mtx_;
	std::condition_variable burst_cond_;
	bool burst_quit_;
	bool burst_pending_;
	bool burst_active_;
	std::uint32_t burst_seq_;
	std::uintptr_t burst_trigger_index_;
	const char *burst_reason_;
	std::chrono::steady_clock::time_point burst_begin_;
	std::chrono::steady_clock::time_point burst_deadline_;
};

} // namespace px4
