// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// CECH-ZD1J の 1 台分の制御を、WinUSB 版 (DriverHost_PX4) と Linux 版 (CUSE のデーモン) で共有する。
// ブリッジの起動、復調器の初期化と選局、TS エンジンの開始と停止、カード UART の操作は
// 実機の測定から決めた順序と期限を含み、OS ごとに複製すると片方だけが修正されるため、
// USB の転送と待機の実装 (SessionPort) と Bulk IN の読み出し以外をここへ集める。
// 設計は docs/20261121_torne_linux_cuse_design.md を参照。
#include "dib8096_registers.hpp"
#include "frontend_native_client.hpp"
#include "frontend_signal.hpp"
#include "mb86h47_bootstrap.hpp"
#include "mb86h47_card.hpp"
#include "mb86h47_i2c.hpp"
#include "mb86h47_stream.hpp"
#include "stream_framer.hpp"
#include "ts_quality.hpp"
#include "../dt295/card_response.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace px4::torne {
using Clock = std::chrono::steady_clock;

// 選局の期限。選局コマンド、ロック待ち、最初の TS の受信を含む操作全体に 1 回だけ設定する。
// BulkI2c=false の選局コマンドは DiB8000・DiB0090 のレジスターを1件ずつ USB 制御転送で操作するため、
// 所要時間は USB ホストの制御転送の往復時間に比例する。制御転送が平均約3.0msの環境では、
// ホスト起動後に初めて選ぶ周波数で選局コマンドだけに4.7～4.8秒、TS開始までに約5.1秒かかった。
// 確定した検索失敗は期限を待たずに返るため、無信号チャンネルの判定時間はこの値に依存しない。
constexpr auto TuneBudget = std::chrono::seconds(10);

// 1 台の機器の USB 制御転送、パイプ転送、待機、抜去と停止の通知を OS ごとに実装する。
// Session の制御 mutex の下でだけ使われる。
class SessionPort {
public:
    virtual ~SessionPort() = default;
    virtual ControlTransport &Transport() noexcept = 0;
    virtual BootstrapWait &Wait() noexcept = 0;
    virtual FrontendSignal &Signal() noexcept = 0;
    // 初期化、選局などの操作全体の期限を設定する。各レジスター操作では更新しない。
    // stoppable が true なら、受信機の停止要求でも転送と待機を中断する。
    virtual void Begin(Clock::time_point deadline, bool stoppable = false) noexcept = 0;
    // Bulk のコマンド経路 (EP 0x02 の書き込みと EP 0x83 の読み出し)。0 か OS のエラーを返す。
    virtual int TransferPipe(std::uint8_t endpoint, std::uint8_t *data, std::size_t length,
        std::size_t &transferred, Clock::time_point deadline) = 0;
    // TransferPipe のエラーが期限切れを表すか。
    virtual bool TimedOut(int error) const noexcept = 0;
};

// bootstrap 済みのブリッジ 1 つ分。受信とカードが同じ個体で別に bootstrap すると受信を壊すため、
// 所有者は個体ごとに 1 つだけ作り、受信とカードで参照を共有する。
// 制御 mutex はフロントエンドの I2C とブリッジの操作の両方を覆う。TS の読み出しの待機ではこの mutex を保持しない。
class Session final {
public:
    explicit Session(SessionPort &port);
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool Command(std::uint8_t command, const std::vector<std::uint8_t> &body,
        std::vector<std::uint8_t> &response, Clock::time_point deadline, bool stoppable = false);
    // 統計やロックの確認のように、操作全体の期限を持たない単発の要求に使う。
    // カード操作が制御mutexを数百ms保持するため、期限をロックの取得前に決めると待ち時間で期限が尽き、
    // 失敗した要求がセッションを無効にする。期限はロックを取得してから数える。
    bool CommandWithin(std::uint8_t command, const std::vector<std::uint8_t> &body,
        std::vector<std::uint8_t> &response, Clock::duration budget, bool stoppable = false);
    bool ReadStatus(std::vector<std::uint8_t> &response, Clock::time_point deadline,
        bool stoppable = false, bool retry = false);
    bool ReadStatusWithin(std::vector<std::uint8_t> &response, Clock::duration budget, bool stoppable = false);
    // 制御転送の失敗後は、応答の分からないI2C要求がmailboxに残っている可能性があり、同じbootstrapの
    // 世代のbusは再利用できない。受信とカードは同じセッションを共有するため、カードが開いたままだと
    // セッションが作り直されず、受信機を開き直しても初期化できなかった。受信機を開くときに限り、
    // bootstrapから新しい世代を作り直す。カードのT=1の状態は上位のSmartCardが次のAPDUで作り直す。
    bool Recover(bool bulk_i2c);
    void Invalidate();
    std::string FrontendError();
    bool InitializeBridge(bool bulk_i2c);
    bool InitializeFrontend(bool output_error_packets, bool batch_i2c_writes);

    // TS エンジン。USB の読み出しの投入と回収は所有者が行い、Prepare の後に読み出しを投入してから Start する。
    StreamResult PrepareStream(Clock::time_point deadline, bool stoppable);
    StreamResult StartStream(Clock::time_point deadline, bool stoppable);
    StreamResult StopStream(Clock::time_point deadline);

    // カード UART を制御 mutex の下で操作する。期限はロックを取得してから budget だけ数える。
    bool Card(Clock::duration budget, const std::function<bool(CardUart &)> &operation);

private:
    static std::uint8_t StatusCommand(bool retry);
    // control_mutexを保持して呼ぶ。
    bool CommandLocked(std::uint8_t command, const std::vector<std::uint8_t> &body,
        std::vector<std::uint8_t> &response, Clock::time_point deadline, bool stoppable);
    bool InitializeBridgeLocked(bool bulk_i2c);

    SessionPort &port_;
    MemoryAccess memory_;
    Bootstrap bootstrap_;
    MailboxI2c bus_;
    DiB8096Registers registers_;
    NativeFrontendClient frontend_;
    StreamEngine engine_;
    std::mutex control_mutex_;
    bool healthy_ = true, frontend_started_ = false;
};

// 選局の待ち合わせを所有者の停止と抜去へ結び付ける。
struct TuneHooks {
    // false なら停止要求か抜去で選局を中断する。stoppable な選局だけが要求の前に確かめる。
    std::function<bool()> running;
    // ms だけ待つ。停止要求か抜去で中断したら false を返す。
    std::function<bool(unsigned ms)> sleep;
};

// 周波数 (kHz) を選局し、ロックを待つ。選局コマンドは一度だけ発行し、Status 内の上流の再試行時刻を保持する。
// USB とカードとの待機を含む操作全体の期限は各ポーリングで更新しない。
// 戻り値は 0、確定した検索失敗の -ENODATA、期限切れの -ETIMEDOUT、中断の -ECANCELED、それ以外の失敗の -EIO。
int TuneFrequency(Session &session, const std::string &id, std::uint32_t frequency, Clock::time_point deadline,
    bool stoppable, bool retry_initial, const TuneHooks &hooks);
// 復調器を sleep し、LED を消す。失敗しても受信機は閉じる。
void SleepFrontend(Session &session);

// 24 バイトの Status の解釈。
bool StatusLocked(const std::vector<std::uint8_t> &status) noexcept;
enum class StatusReading { cnr, signal_strength };
// 値を千分の一の単位で返す。値がない場合は -ENOSYS、範囲外は -EIO。
int ReadStatusValue(const std::vector<std::uint8_t> &status, StatusReading kind, std::int32_t &value) noexcept;

// 内蔵カードリーダーの 1 つの接続の状態。ATR と T=1 の受信途中の状態を持ち、
// 抜去、リセット、電源断でまとめて破棄する。呼び出し側がカードのロックの下で使う。
class CardSession final {
public:
    void Discard() { response_.Reset(); }
    int Detect(Session &session, bool &present);
    int Reset(Session &session);
    // ATRで指定されるB-CASの19200bpsとA-CASの38400bpsを受け付ける。
    int SetBaudrate(Session &session, std::uint32_t baudrate);
    int DataReady(Session &session, bool &ready);
    std::size_t Read(std::uint8_t *data, std::size_t capacity) { return response_.Read(data, capacity); }
    int Write(Session &session, const std::uint8_t *data, std::size_t size);
    bool PowerDown(Session &session);

private:
    px4::dt295::CardResponse response_;
};

// EP 0x81 の 192 バイト単位の受信を 188 バイトの TS へ変換し、品質を計数して出力先へ渡す。
// Bulk IN の読み出しを所有する 1 つのスレッドだけが使い、選局ごとに作り直す。
class TsDelivery final {
public:
    // 出力先へは 188 バイトの倍数を渡す。
    using Sink = std::function<void(const std::uint8_t *packets, std::size_t size)>;
    explicit TsDelivery(bool accept_error_candidates) : framer_(accept_error_candidates) {}
    // 変換した TS のバイト数を返す。
    std::size_t Deliver(const std::uint8_t *data, std::size_t size, const Sink &sink);
    const StreamFramer &Framer() const noexcept { return framer_; }
    const TsQuality &Quality() const noexcept { return quality_; }

private:
    StreamFramer framer_;
    TsQuality quality_;
    std::vector<std::uint8_t> output_;
};
} // namespace px4::torne
