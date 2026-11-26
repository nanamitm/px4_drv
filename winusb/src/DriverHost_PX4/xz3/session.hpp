// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// GV-MVP/XZ3 の 1 台分の制御を、WinUSB 版 (DriverHost_PX4) と Linux 版 (px4-cused) で共有する。
// 中継プログラムの導入と復元、選局、操作12 による TS の取り出し、カード操作の調停は、
// Windows の実測から決めた順序と時間を含み、OS ごとに複製すると片方だけが修正されるため、
// USB の転送と待機 (SessionPort) と TS の受け渡し (StreamSink) 以外をここへ集める。
// 設計は docs/20261121_gv_mvp_xz3_linux_cuse_design.md を参照。
#include "card_uart.hpp"
#include "../dt295/card_response.hpp"
#include "frontend.hpp"
#include "../torne/mb86h47_protocol.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace px4::xz3 {
using Clock = std::chrono::steady_clock;
using Packet = std::array<std::uint8_t, 64>;

void PutWord(std::uint8_t* p, std::uint32_t value);
std::uint32_t Word(const std::uint8_t* p);

struct Image {
    const std::uint8_t* bytes;
    std::size_t size;
};

// 中継プログラムの生成物 (winusb/pkg/DriverHost_PX4/firmware/xz3-*.bin)。
// Windows はリソース、Linux は実行ファイルへ埋め込んだものを渡す。
struct Images {
    Image bootstrap, runtime, hook;
};

// boot0 のメモリ要求 (vendor request 0xBC) と、機器の操作の間の待機。制御ロックの下でだけ使う。
class RelayPort {
public:
    virtual ~RelayPort() = default;
    // 操作全体の期限を設定する。各転送では更新しない。
    virtual void Begin(Clock::time_point deadline) noexcept = 0;
    virtual int Transfer(const px4::torne::MemoryRequest& request, std::uint8_t* data, std::size_t length,
                         std::size_t& transferred) = 0;
    // 期限と抜去で中断する。
    virtual int Pause(unsigned milliseconds) = 0;
};

class Relay {
public:
    Relay(RelayPort& port, const Images& images)
        : port_(port), bootstrap_(images.bootstrap), runtime_(images.runtime), hook_(images.hook) {}

    bool Valid() const;
    int Init();
    int Install(const Image& image);
    // 共通版 (操作7、11〜14) が導入され、フックが有効な間だけtrue。
    bool RuntimeInstalled() const { return runtime_installed_; }
    // フックを外した後、本体を1語ずつ戻す。yieldは各語の後に呼ばれ、呼び出し側はその中で
    // 制御ロックを一時的に手放せる。フックは外れているため、その間のカードUARTの要求は
    // boot0が通常どおり処理する。操作全体の期限は、yieldの後も最初の値を使う。
    bool Restore(const std::function<void()>& yield = {});
    int Delay(unsigned ms);
    // カードUARTの1語をboot0のメモリ要求で読み書きする。boot0のUSBハンドラーは、フックの導入中も
    // ステージング以外の番地を処理する。呼び出し側が制御ロックを保持し、Beginで期限を設定する。
    void Begin(Clock::time_point deadline) { port_.Begin(deadline); }
    bool ReadWord(unsigned address, std::uint32_t& value);
    bool WriteWord(unsigned address, std::uint32_t value);
    int Exchange(unsigned op, Packet& reply, const std::uint8_t* data = nullptr,
                 std::size_t size = 0, unsigned addr = 0, std::size_t rx = 0);

private:
    void Touch(unsigned address, std::size_t size);
    bool Read(unsigned address, std::uint8_t* data, std::size_t size);
    bool Write(unsigned address, const std::uint8_t* data);
    bool WriteRange(unsigned address, const std::uint8_t* data, std::size_t size,
                    const std::function<void()>& yield = {}, Clock::time_point deadline = {});
    bool Verify(unsigned address, const std::uint8_t* data, std::size_t size);
    bool Unhook();

    RelayPort& port_;
    const Image bootstrap_, runtime_, hook_;
    std::array<std::uint8_t, 2236> saved_body_{};
    std::array<std::uint8_t, 8> saved_hook_{};
    std::uint32_t sequence_ = 0;
    bool body_changed_ = false, hook_changed_ = false, runtime_installed_ = false;
    // ステージングに残っている直前の応答の写し。
    Packet staged_{};
    bool staged_valid_ = false;
};

/*
 * カードUARTの16語だけを、Relayのboot0のメモリ要求で読み書きする。
 * 共通版の導入中は、FIFOの読み書きを操作14で最大31バイトずつまとめて行う。
 * 失敗後は、同じ操作内の後続の転送を止める。
 * Pauseの間は制御ロックを手放すため、その間に受信を閉じた復元 (Restore) や最初の選局の導入 (Init) で
 * 共通版の有無が変わる。一括転送を使うかは、作成時の値を持たず、制御ロックの下の各転送の直前に判断する。
 */
class RelayCardBus final : public px4::dt295::CardBus {
public:
    RelayCardBus(Relay& relay, std::function<int(unsigned)> yield)
        : relay_(relay), yield_(std::move(yield)) {}

    bool Bulk() const override { return relay_.RuntimeInstalled(); }
    int Fifo(bool write, std::uint8_t* data, std::size_t& count) override;
    int Read32(std::uint32_t address, std::uint32_t& value) override;
    int Write32(std::uint32_t address, std::uint32_t value) override;
    int Pause(unsigned milliseconds) override;
    int Error() const { return error_; }

private:
    int Check(std::uint32_t address) const;

    Relay& relay_;
    std::function<int(unsigned)> yield_;
    int error_ = 0;
};

// 操作12 の送出量と同じ大きさの Bulk IN (0x81) の読み出し 1 本。
// 破棄するときは発行中の読み出しを取り消し、完了を待ってからバッファを解放する。
class BulkReader {
public:
    virtual ~BulkReader() = default;
    // 送出量の段階 stage の大きさで読み出しを発行する。
    virtual bool Submit(unsigned stage) = 0;
    // 発行した読み出しの完了を待つ。満杯で完了したときだけ true。抜去と 2 秒の期限で false を返す。
    virtual bool Collect() = 0;
    virtual unsigned Stage() const = 0;
    virtual const std::uint8_t* Data() const = 0;
};

class SessionPort : public RelayPort {
public:
    // 機器が抜かれていないか。
    virtual bool Available() const noexcept = 0;
    // 機器を操作しない待機。抜去で -ENODEV を返す。
    virtual int WaitRemoved(unsigned milliseconds) = 0;
    // カード操作を待つ間の短い待機 (約 1 ms)。
    virtual void Nap() noexcept = 0;
    // Bulk IN (0x81) に残ったデータを捨てる。0 か OS のエラーを返す。
    virtual int FlushStream() = 0;
    // Bulk IN (0x81) を size バイトまで 1 回読む。timeout_ms の間に届かなければ取り消し、
    // transferred を 0 にして 0 を返す。抜去で -ENODEV、そのほかの失敗で負の値を返す。
    virtual int ReadStream(std::uint8_t* data, std::size_t size, unsigned timeout_ms,
                           std::size_t& transferred) = 0;
    virtual std::unique_ptr<BulkReader> CreateReader() = 0;
};

// 取り出した TS の受け渡し先。受信の worker だけが呼ぶ。
class StreamSink {
public:
    virtual ~StreamSink() = default;
    // 操作11 が成功し、worker を始める直前に呼ぶ。
    virtual void Started() = 0;
    virtual void PutPacket(const std::uint8_t* packet) = 0;
    // 1 回の読み出し分の PutPacket の後に呼ぶ。
    virtual void Flush() = 0;
    // 停止の要求ではなく、転送の異常で worker が終わったときに呼ぶ。
    virtual void Failed(int error) { (void)error; }
};

/*
 * 1 台分の中継プログラム、選局、TS の取り出し、カード操作。
 * 受信の状態 (開いているか、選局済みか、失敗したか) は所有者が管理し、この類は機器の操作と
 * EP0 の時分割だけを担当する。control_mutex_ は Relay (EP0 の制御転送) を守り、
 * 所有者が受信の状態の mutex を持つ場合は、その mutex、control_mutex_ の順に取得する。
 */
class Session final : private FrontendIo {
public:
    Session(SessionPort& port, const Images& images);
    ~Session() override;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Valid() const { return relay_.Valid(); }
    // 最初の選局では、中継プログラムの導入と復調器の初期化を同じ制御ロックの下で先に行う。
    // その失敗は init_failed で返し、所有者は Restore まで受信を失敗の状態にする。
    // 地デジはロック直後の訂正前の TS を避けるため、制御ロックを手放して 350 ms 待つ。
    // relative が true なら、衛星の tsid を相対 TS 番号 (0〜7) として扱う (Linux 版の PTX の ioctl)。
    int Tune(bool satellite, std::uint32_t frequency_khz, std::uint32_t tsid, bool& init_failed,
             bool relative = false);
    bool Initialized() const { return initialized_; }
    // 操作11 で DMA を始め、worker で操作12 を繰り返す。呼び出し側は Running でないことを確かめる。
    // 開始に失敗した後の停止 (操作13) も失敗した場合は stop_failed を立てる。
    int StartStream(bool satellite, StreamSink& sink, bool& stop_failed);
    bool Running() const { return worker_.joinable(); }
    // worker を止め、操作13 で DMA を止める。失敗した場合、所有者は中継プログラムを復元しない。
    int StopStream();
    // フックと本体を元へ戻す。DMA が止まっているときだけ呼ぶ。
    // 本体の書き戻しは約1.8秒かかり、SmartCard が ATR から求める T=1 のブロックの待機時間 (B-CAS は1.6秒) を超えるため、
    // 待っているカード操作があれば1語ごとに制御ロックを譲る。
    bool Restore();
    bool StreamStarted() const { return stream_started_; }
    // worker が失敗したときのエラー。選局ごとに Clear で消す。
    int WorkerError() const { return worker_error_; }
    void ClearWorkerError() { worker_error_ = 0; }
    std::int32_t Cnr() const { return cnr_; }
    // TC90522 の CNDAT の生の値。受信中は TS の転送の応答から更新した値、受信していなければ復調器から読む。
    std::uint32_t ReadCndat();
    /*
     * 内蔵カードUARTの操作。受信の有無に関係なく、boot0のメモリ要求でカードUARTを読み書きする。
     * 操作12の間はEP0が応答しないため、TS workerと同じ制御ロックで操作12の合間に行い、
     * 手順内の待機では制御ロックを手放す。カード操作の後のCardBusyWindowの間は、TS workerが
     * 最小の送出量を使い、続くカード操作の待ちを短くする。
     */
    int Card(const std::function<int(CardUart&)>& operation);

private:
    class RingStats;
    int I2c(std::uint8_t address, const std::uint8_t* tx, std::size_t size,
            std::uint8_t* rx, std::size_t count) override;
    int Delay(unsigned milliseconds) override { return relay_.Delay(milliseconds); }
    int ReadLoop(bool satellite, StreamSink& sink);
    int DiscardResidue();
    unsigned NextStage(Clock::duration elapsed, unsigned stage) const;
    void HoldForCard();
    void Deliver(const std::uint8_t* bytes, std::size_t length, StreamSink& sink);

    SessionPort& port_;
    Relay relay_;
    Frontend frontend_;
    std::mutex control_mutex_;
    std::atomic<int> card_waiters_{0};
    // 最後のカード操作からCardBusyWindowが経つ時刻と、最後のカード操作を終えた時刻
    // (steady_clockの値)。
    std::atomic<Clock::rep> card_until_{0}, card_last_{0};
    std::thread worker_;
    std::atomic_bool stop_{false};
    std::atomic_int worker_error_{0};
    std::atomic<std::int32_t> cnr_{0};
    std::atomic<std::uint32_t> cndat_{0};
    bool stream_started_ = false, initialized_ = false;
};

/*
 * 内蔵カードリーダーの開閉と、ATRとT=1ブロックの受信途中の状態。
 * 状態は抜去・リセット・Closeで一括して破棄する。Session は受信と共有し、Attach から Detach までだけ使う。
 */
class CardReader final {
public:
    explicit CardReader(const std::atomic_bool& available) : available_(available) {}
    void Attach(Session& session);
    // 実行中のカード操作が Session を使い終わるまで待ち、受信途中のカード応答を破棄する。
    void Detach();
    int Open();
    // power_down が true ならカードへの給電も止める。電源の操作を別に行う所有者は false を渡す。
    void Close(bool power_down = true);
    // 受信途中のATRとT=1ブロックを破棄する。
    void Discard();
    // CMDVCCNをHighにしてカードへの給電を止める。
    int PowerDown();
    int Detect(bool& detected);
    int Reset();
    int SetBaudrate(std::uint32_t baudrate);
    int IsDataReady(bool& ready);
    int Read(std::uint8_t* buf, std::uint8_t& len);
    int Write(const std::uint8_t* buf, std::uint8_t len);

private:
    void ResetBuffer();
    const std::atomic_bool& available_;
    std::mutex mutex_;
    Session* session_ = nullptr;
    bool open_ = false;
    px4::dt295::CardResponse response_;
    // 直前の受信FIFOの読み出しの時刻と、それが空だったか。
    Clock::time_point polled_{};
    bool polled_empty_ = false;
};

}
