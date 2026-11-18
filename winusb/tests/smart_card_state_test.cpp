#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <new>
#include <memory>
#include <vector>

#include "atr_length.hpp"
#include "smart_card.hpp"
#include "../src/DriverHost_PX4/dt295/card_response.hpp"
#include "mock_card_device.hpp"

namespace {

bool Check(bool condition, const char *message)
{
	if (condition)
		return true;
	std::fprintf(stderr, "%s\n", message);
	return false;
}

} // namespace

int main()
{
	auto device = std::make_shared<MockCardDevice>();
	px4::SmartCard card(device);
	bool succeeded = Check(card.Open() == 0 && device->is_open,
		"Opening an empty reader failed.");

	bool present = true;
	bool initialized = true;
	std::vector<std::uint8_t> atr;
	succeeded = Check(card.GetStatus(present, initialized, atr) == 0 &&
		!present && !initialized && atr.empty(),
		"An empty reader returned stale card state.") && succeeded;

	/* 挿入検出時に ATR と T=1 を初期化する */
	device->is_present = true;
	succeeded = Check(card.GetStatus(present, initialized, atr) == 0 &&
		present && initialized && atr.size() == 13 && device->reset_count == 1 &&
		device->baudrate == 19200 &&
		device->written_pcbs.size() == 2 && device->written_pcbs[0] == 0xc0 &&
		device->written_pcbs[1] == 0xc1 && device->ifs_values.size() == 1 &&
		device->ifs_values[0] == 251,
		"Card insertion did not initialize the session.") && succeeded;

	const std::uint8_t apdu[] = { 0x90, 0x30, 0x00, 0x00, 0x00 };
	std::uint8_t response[16] = {};
	std::size_t response_length = sizeof(response);
	/* UART_RX_READY が立つまで途中フレームを読み出さない */
	device->delay_next_response = true;
	/* 同じ UART 読み出しへ連結された重複応答は先頭フレームの処理後に残さない */
	device->duplicate_next_response = true;
	device->read_called_before_ready = false;
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2 &&
		!device->read_called_before_ready && device->read_queue.empty(),
		"Initial APDU transmission failed.") && succeeded;

	/* 1回の読み出し上限を超える重複応答でも、先頭フレームだけを処理して残りを FIFO に残さない */
	device->large_duplicate_response = true;
	std::uint8_t large_response[256] = {};
	std::size_t large_response_length = sizeof(large_response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), large_response,
		large_response_length) == 0 && large_response_length == 200 &&
		device->read_queue.empty(),
		"A duplicated response across the read limit was not discarded.") && succeeded;

	/* FIFO の残留を持ち越さず、続く APDU も同じセッションで成功する */
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"The following APDU failed after a duplicated long response.") && succeeded;

	/* 完成フレームが読み出し上限と同じ長さでも、後続の重複を残さない */
	device->max_duplicate_response = true;
	large_response_length = sizeof(large_response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), large_response,
		large_response_length) == 0 && large_response_length == 251 &&
		device->read_queue.empty(),
		"A duplicated response at the read limit was not discarded.") && succeeded;

	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"The following APDU failed after a maximum length response.") && succeeded;

	/* 抜去後は古い ATR と連番を破棄する */
	device->is_present = false;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == px4::SMART_CARD_NO_MEDIUM,
		"Card removal was not reported.") && succeeded;

	/* 再挿入後の最初の APDU でセッションを自動再構築する */
	device->is_present = true;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 2,
		"Card reinsertion did not recover automatically.") && succeeded;

	/* 一時的な通信失敗も次の APDU まで壊れた連番を持ち越さない */
	device->fail_next_apdu = true;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EIO,
		"Injected transport failure was not returned.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 3,
		"Transport failure did not recover on the next APDU.") && succeeded;

	/* 送受信中に例外が飛んでもセッションを破棄し、壊れた連番を残さない */
	device->throw_next_apdu = true;
	response_length = sizeof(response);
	bool threw = false;
	try {
		card.Transmit(apdu, sizeof(apdu), response, response_length);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	succeeded = Check(threw, "An injected exception was not propagated.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 4,
		"An exception did not invalidate the session.") && succeeded;

	/* 接触不良による検出エラーも例外や状態破損へ変えない */
	device->detect_error = -EIO;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EIO,
		"Card detection failure was not returned.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 5,
		"Card detection failure did not recover.") && succeeded;

	/* 正常な WTX が繰り返されても APDU 全体の期限を延長しない */
	device->endless_wtx = true;
	auto wtx_started = std::chrono::steady_clock::now();
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -ETIMEDOUT &&
		std::chrono::steady_clock::now() - wtx_started < std::chrono::seconds(5),
		"Repeated WTX requests extended the APDU indefinitely.") && succeeded;
	device->endless_wtx = false;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 6,
		"WTX timeout did not invalidate the session.") && succeeded;

	/*
	 * 最後の I ブロックへの応答を取りこぼすと、待機時間の経過後に I ブロックが再送され、
	 * 受理済みを示す R ブロックが返る
	 * ここからカード側 I ブロックの再送を要求して応答を回収できることを確かめる
	 * 通常の応答経路で成功していないことを、再送回数でも確かめる
	 */
	device->lose_final_response = true;
	device->cached_resend_count = 0;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2 &&
		device->cached_resend_count == 1 && device->reset_count == 6,
		"A lost final I-block response was not recovered.") && succeeded;

	/*
	 * 応答を読み終えた後から遅延した重複応答が届くと、読み捨ての対象にならず
	 * 次の APDU の先頭で読まれるが、直前の応答で受信連番が進んでいるため
	 * 必ず連番不一致で失敗し、古い応答が正しい応答として通る経路はない
	 * ここではその失敗が1回で済み、次の APDU で再初期化して回復することを確かめる
	 */
	const std::uint8_t stale_response[] = { 0x90, 0x00 };
	device->QueueBlock(device->response_sequence ? 0x00 : 0x40,
		stale_response, sizeof(stale_response));
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EPROTO,
		"A stale delayed response was not rejected.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 7,
		"A stale delayed response did not recover on the next APDU.") && succeeded;

	/* UART の残留バイトで ATR が壊れた場合だけ、カード全体を1回再初期化する */
	auto retry_device = std::make_shared<MockCardDevice>();
	retry_device->is_present = true;
	retry_device->malformed_atr_resets = 1;
	/* CRC 指定まで解析してから TCK で失敗し、次の LRC 用 ATR へ設定を残さないことも確認する */
	retry_device->malformed_atr_bytes = {
		0x3b, 0x80, 0x91, 0x01, 0x51, 0x40, 0x01, 0x01,
	};
	px4::SmartCard retry_card(retry_device);
	succeeded = Check(retry_card.Open() == 0 && retry_device->reset_count == 2,
		"Malformed ATR did not recover after one card reset.") && succeeded;
	retry_card.Close();

	/* TD2 が T=1 を示した後の TA3 / TC3 だけから IFSC と CRC を取得する */
	auto crc_device = std::make_shared<MockCardDevice>();
	crc_device->atr_bytes = { 0x3b, 0x80, 0x91, 0x01, 0x51, 0x40, 0x01, 0x00 };
	crc_device->expected_crc = true;
	crc_device->is_present = true;
	px4::SmartCard crc_card(crc_device);
	bool crc_present = false;
	bool crc_initialized = false;
	std::vector<std::uint8_t> crc_atr;
	succeeded = Check(crc_card.Open() == 0 &&
		crc_card.GetStatus(crc_present, crc_initialized, crc_atr) == 0 &&
		crc_present && crc_initialized && crc_atr.size() == 8,
		"TA3 / TC3 CRC parameters were not applied.") && succeeded;
	crc_card.Close();

	/* PIX-DT295の製品用バッファとSmartCardを接続し、CRC付きS/Iブロックを通す。 */
	auto dt295_device = std::make_shared<MockCardDevice>();
	dt295_device->atr_bytes = { 0x3b, 0x90, 0x12, 0x81, 0x41, 0x01, 0x43 };
	dt295_device->expected_crc = true;
	dt295_device->is_present = true;
	dt295_device->use_dt295_response = true;
	px4::SmartCard dt295_card(dt295_device);
	succeeded = Check(dt295_card.Open() == 0 &&
		dt295_card.GetStatus(crc_present, crc_initialized, crc_atr) == 0 && crc_initialized &&
		dt295_device->baudrate == 19200,
		"PIX-DT295 buffered CRC initialization failed.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(dt295_card.Transmit(apdu, sizeof(apdu), response, response_length) == 0 &&
		response_length == 2 && response[0] == 0x90 && response[1] == 0,
		"PIX-DT295 lost the last CRC byte of a fragmented response.") && succeeded;
	dt295_card.Close();

	/* ACAS の TA1=13 は PPS なしで IT930x の38400bps 設定へ切り替える */
	auto acas_device = std::make_shared<MockCardDevice>();
	acas_device->atr_bytes = {
		0x3b, 0xf0, 0x13, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0xfe, 0x46, 0x1f, 0x03, 0x19,
	};
	acas_device->is_present = true;
	acas_device->drop_first_ifs = true;
	px4::SmartCard acas_card(acas_device);
	succeeded = Check(acas_card.Open() == 0 &&
		acas_device->baudrate == 38400 &&
		acas_device->written_pcbs.size() == 3 &&
		acas_device->written_pcbs[0] == 0xc1 &&
		acas_device->written_pcbs[1] == 0xc0 &&
		acas_device->written_pcbs[2] == 0xc1 &&
		acas_device->ifs_values.size() == 2 &&
		acas_device->ifs_values[0] == 254 && acas_device->ifs_values[1] == 254,
		"ACAS IFS timeout did not recover through RESYNCH and IFS retry.") &&
		succeeded;

	/* TB3 の BWI=4 は500ms を超える正当なブロック待機時間を許容する */
	acas_device->ready_delay_checks = 120;
	response_length = sizeof(response);
	succeeded = Check(acas_card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"ACAS BWI was not applied to the T=1 block timeout.") && succeeded;
	acas_card.Close();

	/* ACAS の IFSC=254 でも UART 上限に合わせて I ブロックを連結送信する */
	auto long_apdu_device = std::make_shared<MockCardDevice>();
	long_apdu_device->atr_bytes = acas_device->atr_bytes;
	long_apdu_device->is_present = true;
	px4::SmartCard long_apdu_card(long_apdu_device);
	std::vector<std::uint8_t> long_apdu(254, 0);
	response_length = sizeof(response);
	succeeded = Check(long_apdu_card.Open() == 0 &&
		long_apdu_card.Transmit(long_apdu.data(), long_apdu.size(), response,
			response_length) == 0 &&
		response_length == 2 &&
		long_apdu_device->apdu_block_lengths.size() == 2 &&
		long_apdu_device->apdu_block_lengths[0] == 251 &&
		long_apdu_device->apdu_block_lengths[1] == 3,
		"ACAS APDU was not split at the UART frame limit.") && succeeded;
	long_apdu_card.Close();

	/* IT930x で生成できない TA1 は誤った通信速度へ丸めず明示的に拒否する */
	auto unsupported_device = std::make_shared<MockCardDevice>();
	unsupported_device->atr_bytes = {
		0x3b, 0xf0, 0x14, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0x7c, 0x45, 0x1f, 0x01, 0x9d,
	};
	unsupported_device->is_present = true;
	px4::SmartCard unsupported_card(unsupported_device);
	succeeded = Check(unsupported_card.Open() == -EOPNOTSUPP &&
		!unsupported_device->is_open,
		"An unsupported TA1 value was accepted.") && succeeded;

	/*
	 * スカパー！プレミアムサービスのカードは TA1=0x33 (744/4) で B-CAS と同じ19200bps になり、
	 * IT930x はこの ATR で RX_READY を立てないため受信バイト数の停止で読み出す
	 */
	const std::vector<std::uint8_t> premium_atr = {
		0x3b, 0xb0, 0x33, 0x00, 0x91, 0x81, 0x31, 0x6b, 0x35, 0xfc,
	};
	auto premium_device = std::make_shared<MockCardDevice>();
	premium_device->atr_bytes = premium_atr;
	premium_device->atr_without_ready = true;
	premium_device->report_length = true;
	premium_device->resynch_unsupported = true;
	premium_device->is_present = true;
	px4::SmartCard premium_card(premium_device);
	bool premium_present = false, premium_initialized = false;
	std::vector<std::uint8_t> premium_status_atr;
	succeeded = Check(premium_card.Open() == 0 &&
		premium_card.GetStatus(premium_present, premium_initialized,
			premium_status_atr) == 0 &&
		premium_present && premium_initialized &&
		premium_status_atr == premium_atr &&
		premium_device->baudrate == 19200 &&
		premium_device->atr_read_without_ready &&
		!premium_device->read_called_before_ready &&
		premium_device->written_pcbs.size() >= 2 &&
		premium_device->written_pcbs[0] == 0xc0 &&
		premium_device->written_pcbs[1] == 0xc1 &&
		premium_device->ifs_values.size() == 1 && premium_device->ifs_values[0] == 251,
		"A card without RESYNCH support was not initialized through IFS.") &&
		succeeded;

	/* RESYNCH への応答は、実機で確認した形以外を従来どおり拒否する */
	auto bad_resynch_device = std::make_shared<MockCardDevice>();
	bad_resynch_device->is_present = true;
	bad_resynch_device->resynch_empty_reply = true;
	px4::SmartCard bad_resynch_card(bad_resynch_device);
	succeeded = Check(bad_resynch_card.Open() == -EPROTO &&
		!bad_resynch_device->is_open,
		"An unexpected RESYNCH reply was accepted.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(premium_card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"A T=1 APDU failed after an ATR read without RX_READY.") && succeeded;
	premium_card.Close();

	/* 受信バイト数を返せない機種は、従来どおり RX_READY を待って期限で失敗する */
	auto no_length_device = std::make_shared<MockCardDevice>();
	no_length_device->atr_bytes = premium_atr;
	no_length_device->atr_without_ready = true;
	no_length_device->is_present = true;
	px4::SmartCard no_length_card(no_length_device);
	succeeded = Check(no_length_card.Open() == -ETIMEDOUT &&
		!no_length_device->atr_read_without_ready &&
		!no_length_device->read_called_before_ready,
		"An ATR was read before RX_READY on a device without a received length.") &&
		succeeded;

	/* RX_READY が立つ B-CAS は、受信バイト数を返せる機種でも従来の読み出しを使う */
	auto ready_device = std::make_shared<MockCardDevice>();
	ready_device->report_length = true;
	ready_device->is_present = true;
	px4::SmartCard ready_card(ready_device);
	succeeded = Check(ready_card.Open() == 0 &&
		ready_device->baudrate == 19200 &&
		!ready_device->length_queried &&
		!ready_device->read_called_before_ready,
		"A B-CAS ATR with RX_READY did not use the ready path.") && succeeded;
	ready_card.Close();

	/*
	 * 受信を1バイトずつ溜める機種 (PIX-DT295 、torne 、GV-MVP/XZ3) は ATR の構造から
	 * 全体の長さを求め、B-CAS 以外の ATR でも受信を終えた時点を判定する
	 */
	{
		std::size_t atr_length = 0;
		bool atr_length_succeeded = true;
		for (std::size_t received = 1; received < 7; received++) {
			atr_length_succeeded = atr_length_succeeded &&
				px4::GetAtrLength(premium_atr.data(), received, atr_length) == -EAGAIN;
		}
		atr_length_succeeded = atr_length_succeeded &&
			px4::GetAtrLength(premium_atr.data(), 7, atr_length) == 0 &&
			atr_length == premium_atr.size();
		const std::uint8_t bcas_atr[] = {
			0x3b, 0xf0, 0x12, 0x00, 0xff, 0x91, 0x81,
			0xb1, 0x7c, 0x45, 0x1f, 0x03, 0x99,
		};
		atr_length_succeeded = atr_length_succeeded &&
			px4::GetAtrLength(bcas_atr, sizeof(bcas_atr), atr_length) == 0 &&
			atr_length == sizeof(bcas_atr);
		/* T=0 だけを示す ATR は TCK を持たない */
		const std::uint8_t t0_atr[] = { 0x3b, 0x12, 0x14, 0x50, 0x00 };
		atr_length_succeeded = atr_length_succeeded &&
			px4::GetAtrLength(t0_atr, sizeof(t0_atr), atr_length) == 0 &&
			atr_length == 5;
		/* 連続リセット時の残留バイトなど、TS が ATR の値でないものは形式不正にする */
		const std::uint8_t residual[] = { 0xfc, 0xff };
		atr_length_succeeded = atr_length_succeeded &&
			px4::GetAtrLength(residual, sizeof(residual), atr_length) == -EBADMSG;
		/* 33バイトを超える ATR は規格外のため形式不正にする */
		const std::uint8_t too_long[] = {
			0x3b, 0xff, 0x11, 0x00, 0x00, 0xf1, 0x00, 0x00, 0x00,
			0xf1, 0x00, 0x00, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x01,
		};
		atr_length_succeeded = atr_length_succeeded &&
			px4::GetAtrLength(too_long, sizeof(too_long), atr_length) == -EBADMSG;
		succeeded = Check(atr_length_succeeded,
			"The ATR length was not derived from the ATR structure.") && succeeded;
	}

	/* 一度抜去を観測した接続は、別の監視者が再初期化しても復活させない。 */
	auto generation_device = std::make_shared<MockCardDevice>();
	generation_device->is_present = true;
	px4::SmartCard generation_card(generation_device);
	std::uint64_t first_generation = 0, replacement_generation = 0;
	succeeded = Check(generation_card.Open() == 0 &&
		generation_card.BindConnection(first_generation) == 0,
		"Cannot bind a card connection.") && succeeded;
	std::vector<std::uint8_t> generation_atr;
	succeeded = Check(generation_card.Reset(generation_atr) == 0 &&
		generation_card.CheckConnection(first_generation) == 0,
		"Reset of the same card invalidated its connection.") && succeeded;
	generation_device->is_present = false;
	succeeded = Check(generation_card.CheckConnection(first_generation) == -ENODEV,
		"Card removal did not invalidate the connection.") && succeeded;
	generation_device->is_present = true;
	succeeded = Check(generation_card.BindConnection(replacement_generation) == 0 &&
		replacement_generation != first_generation,
		"Replacement card reused its predecessor's generation.") && succeeded;
	const auto resets_before_stale_check = generation_device->reset_count;
	succeeded = Check(generation_card.CheckConnection(first_generation) == -ENODEV &&
		generation_device->reset_count == resets_before_stale_check &&
		generation_card.CheckConnection(replacement_generation) == 0,
		"Stale connection accessed or reset the replacement card.") && succeeded;
	generation_device->detect_error = -ENODEV;
	succeeded = Check(generation_card.CheckConnection(replacement_generation) == -ENODEV &&
		generation_card.CheckConnection(replacement_generation) == -ENODEV,
		"USB loss allowed an old connection to recover implicitly.") && succeeded;
	generation_card.Close();
	/* presence照会に成功した直後のUSB切断も、古い接続を無効化する。 */
	succeeded = Check(generation_card.Open() == 0 &&
		generation_card.BindConnection(replacement_generation) == 0,
		"Cannot reopen generation test card.") && succeeded;
	generation_device->write_error = -ENODEV;
	response_length = sizeof(response);
	succeeded = Check(generation_card.Transmit(apdu, sizeof(apdu), response, response_length) == -ENODEV &&
		generation_card.CheckConnection(replacement_generation) == -ENODEV,
		"USB loss during APDU did not invalidate the connection.") && succeeded;
	generation_card.Close();
	/* 接続経由の APDU は、TS と同じ USB ブリッジへのカード検出を1回だけ発行する。 */
	succeeded = Check(generation_card.Open() == 0 &&
		generation_card.BindConnection(replacement_generation) == 0 &&
		generation_card.ResetConnection(replacement_generation, generation_atr) == 0,
		"Cannot reopen generation test card for probe counting.") && succeeded;
	const int detects_before_apdu = generation_device->detect_count;
	response_length = sizeof(response);
	succeeded = Check(generation_card.TransmitConnection(replacement_generation, apdu,
		sizeof(apdu), response, response_length) == 0 &&
		generation_device->detect_count == detects_before_apdu + 1,
		"A connection APDU probed card presence more than once.") && succeeded;
	/* Transmit 内の照会で観測した抜去は、未挿入ではなく接続の失効として返す。 */
	generation_device->is_present = false;
	response_length = sizeof(response);
	succeeded = Check(generation_card.TransmitConnection(replacement_generation, apdu,
		sizeof(apdu), response, response_length) == -ENODEV &&
		generation_card.ResetConnection(replacement_generation, generation_atr) == -ENODEV,
		"Removal observed during an APDU did not invalidate the connection.") && succeeded;
	generation_device->is_present = true;
	succeeded = Check(generation_card.BindConnection(replacement_generation) == 0 &&
		generation_card.CheckGeneration(replacement_generation) == 0,
		"Cannot rebind after removal observed during an APDU.") && succeeded;
	generation_card.Close();
	succeeded = Check(generation_card.CheckGeneration(replacement_generation) == -ENODEV,
		"A closed reader accepted a connection generation.") && succeeded;
	card.Close();
	succeeded = Check(!device->is_open, "Closing the reader failed.") && succeeded;
	std::printf("smart_card_state_test: %s\n", succeeded ? "passed" : "failed");
	return succeeded ? 0 : 1;
}
