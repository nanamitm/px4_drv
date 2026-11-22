// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "pxbcud/frontend.hpp"
#include "pxbcud/recovery_budget.hpp"
#include "pxbcud_stream.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace px4::cuse {
// PLEX PX-BCUD (3275:0085)。WinUSB 版と共有する pxbcud::Frontend で EM28178、TC90532、QM1D1C0045 系を制御し、
// TS は alternate setting 1 の isochronous IN (0x84) で受け取る。BS・110度CS だけを扱い、カードリーダーはない
// (docs/20261122_px_bcud_linux_cuse_design.md)。
class PxBcudDevice final : public Device {
public:
    static constexpr std::uint16_t Vendor = 0x3275, Product = 0x0085;
    PxBcudDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~PxBcudDevice() override;

protected:
    int InitModel() override { return 0; }
    void OnRemove() override { opened_ = false; }
    void OnShutdown() override;

    int VideoOpen() override;
    void VideoClose() override;
    bool AcceptsSystem(std::uint32_t system) const override;
    int SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal) override;
    int LockStatus(bool &locked) override;
    int ReadCn(std::uint32_t &raw) override;
    int StartStream(bool &fatal) override;
    int StopStream(bool &fatal) override;

    // カードリーダーを持たないため、card_id_ を設定せずカードのデバイスファイルを作らない。
    void CardDiscard() override {}
    int CardDetect(bool &) override { return -ENODEV; }
    int CardReset() override { return -ENODEV; }
    int CardSetBaudrate(std::uint32_t) override { return -ENODEV; }
    int CardDataReady(bool &) override { return -ENODEV; }
    std::size_t CardRead(std::uint8_t *, std::size_t) override { return 0; }
    int CardWrite(const std::uint8_t *, std::size_t) override { return -ENODEV; }
    int CardPowerDown() override { return 0; }

private:
    class UsbIo;
    // 受信が失敗した段階。WinUSB 版の fail() の stage と同じ名前でログへ残す。
    enum class Failure { None, Complete, Submit, PacketDescriptor, NoData, PatTimeout };

    int CheckEndpoint();
    // capture の停止と alternate setting 0。どちらかが失敗しても両方を試みる。control_ を保持して呼ぶ。
    int StopCaptureLocked();
    // 転送をすべて発行する。restart は同じ選局の自動復旧。
    int Submit(bool restart);
    // 転送をすべて取り消して回収を待つ。回収できなかったら false。
    bool CancelAndWait();
    void SetFailure(int error, Failure failure);
    void Run();
    // 自動復旧。再開できたら true。
    bool Recover(int error, Failure failure, bool waited_for_input, pxbcud::RecoveryBudget &budget,
                 Clock::time_point failed_at);
    void LogStreamEnd(Failure failure);
    static void LIBUSB_CALL IsoDone(libusb_transfer *transfer);

    // 自動復旧の試験用の模擬の故障。PX4_CUSED_FAULT_INJECTION を定義したビルド (make fault) だけが使い、
    // 配布するデーモンでは常に空のまま。定義の有無で型の大きさが変わらないよう、メンバーは常に置く。
    struct Fault;
    std::unique_ptr<Fault> fault_;
    std::unique_ptr<UsbIo> io_;
    // frontend_ と io_ の期限を、VideoNode の操作と受信の worker の自動復旧の間で直列化する。
    std::mutex control_;
    std::unique_ptr<pxbcud::Frontend> frontend_;
    // VideoNode の状態のロックの下で使う。
    bool opened_ = false;

    // isochronous の転送 (StartStream から StopStream まで)
    struct Stream {
        std::vector<libusb_transfer *> transfers;
        std::vector<std::uint8_t> buffer;
        std::unique_ptr<PxBcudStream> ts;
        std::mutex mutex;
        std::condition_variable changed;
        int in_flight = 0;
        bool stop = false;
        std::atomic_bool resubmit{false}, pat_ready{false}, received_any{false}, recovering{false};
        std::atomic_int error{0};
        std::atomic<Failure> failure{Failure::None};
        // steady_clock の時刻 (ナノ秒)。完了の通知が書き、worker が読む。
        std::atomic<std::int64_t> started_ns{0}, first_data_ns{0}, last_data_ns{0};
        // 完了の通知だけが使う。
        std::int64_t previous_completion_ns = 0;
        std::uint64_t max_gap_us = 0;
        std::thread worker;
        std::uint64_t recovery_count = 0;
        // worker の終わりの異常 (StopStream の戻り値)
        int terminal_error = 0;
        // 回収できなかった転送がある。libusb が使い続ける可能性があるため、解放せずに残す。
        bool leaked = false;
    } stream_;
};
}
