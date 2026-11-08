// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
extern "C" {
#include "protocol.h"
}

namespace px4::siano {
// J.83B の IQ の送出の開始と停止 (data は中心周波数 Hz で開始、0 で停止) と、その応答、IQ のブロックのメッセージ。
// Siano の公開ヘッダーにない番号で、利用者が用意した J.83B 用の firmware (J83BFirmwarePath) だけが扱う。
// BonDriver_S1UD.dll がこの firmware へ送る要求から確認した
constexpr std::uint16_t MSG_IQ_STREAM_REQ=0x037A, MSG_IQ_STREAM_RES=0x037B, MSG_IQ_DATA=0x037C;
// IQ のブロックのメッセージ本体 (int8 の I0, I1, Q0, Q1 の組で 7360 サンプルと、20 ビットのブロック番号を含む 4 バイト)
constexpr std::size_t IqBlockBytes=14724;
// Exchange は送信と指定応答の待機を一つの絶対期限内で処理する。
class Io {
public:
    virtual ~Io() = default;
    virtual int Exchange(const std::vector<std::uint8_t>& message, std::uint16_t response,
                         std::vector<std::uint8_t>& payload, unsigned timeout_ms) = 0;
    virtual int Delay(unsigned milliseconds) = 0;
    // firmware を読み込み直している間か。J.83B 用の firmware は SW_RELOAD_START で ROM のブートローダーへ戻り、
    // その際に bulk IN の受信が一度失敗するため、受信側はこの間だけ失敗を回復させる
    virtual void Reloading(bool) {}
};
// ドロップ調査用の受信品質。EX 応答 (firmware 8.0 以降) の sms_isdbt_stats_ex だけを解釈する。
struct SignalStats {
    bool rf_locked=false, demod_locked=false;
    std::int32_t snr=0, rssi=0;
    std::uint32_t frequency=0, layers=0;
    // segments が 255 の階層は存在しない。ber の 0xffffffff は値なし。
    struct Layer {std::uint32_t segments=255, ber=0xffffffff, error_ts=0, total_ts=0, tmcc_errors=0;} layer[3];
};
class Frontend final {
public:
    explicit Frontend(Io& io) : io_(io) {}
    // force は、ISDB-T で稼働中の機器にも firmware を読み込み直す (J.83B 用と通常の firmware の切り替え)。
    // isdbt が false (J.83B 用の firmware) のときは、起動後のモードが ISDB-T でなくても成功とする
    int Initialize(const std::vector<std::uint8_t>& firmware, bool force=false, bool isdbt=true);
    int Tune(std::uint32_t frequency_khz);
    // J.83B 用の firmware で、中心周波数 (Hz) に選局して IQ の送出を始める。0 で送出を止める
    // stats を渡すと応答の内容を返す。停止の応答は、送出中のチップのバッファーの統計を含む (IqStats)
    int SetIqStream(std::uint32_t frequency_hz, std::vector<std::uint8_t>* stats=nullptr);
    int RequestStats(std::vector<std::uint8_t>& payload);
    int DecodeStats(const std::vector<std::uint8_t>& payload, bool& locked, std::int32_t& cnr) const;
    int DecodeSignal(const std::vector<std::uint8_t>& payload, SignalStats& stats) const;
    std::uint16_t Version() const { return version_; }
private:
    int Request(std::uint16_t type, std::uint16_t response,
                const std::vector<std::uint32_t>& words, std::vector<std::uint8_t>& payload, unsigned timeout_ms=10000);
    int VersionInfo();
    int Reload(const std::vector<std::uint8_t>& firmware, bool isdbt);
    Io& io_;
    std::uint8_t mode_=255, protocols_=0;
    std::uint16_t version_=0;
};
}
