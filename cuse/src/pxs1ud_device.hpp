// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "iq_process.hpp"
#include "siano/session.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace px4::cuse {
// PLEX PX-S1UD (3275:0080)。WinUSB 版と共有する siano::Session で Siano SMS2270 を制御し、地デジ (ISDB-T) と
// J.83B (実験的) を受信する。制御の応答と TS は同じ Bulk IN に届き、J.83B は IQ を helper の子プロセスで復調する。
// カードリーダーはない (docs/20261122_px_s1ud_linux_cuse_design.md)。
class PxS1udDevice final : public Device, private siano::SessionPort {
public:
    static constexpr std::uint16_t Vendor = 0x3275, Product = 0x0080;
    PxS1udDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~PxS1udDevice() override;

protected:
    int InitModel() override;
    void OnRemove() override;
    void OnShutdown() override;

    int VideoOpen() override;
    void VideoClose() override;
    bool AcceptsSystem(std::uint32_t system) const override;
    int SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal) override;
    bool AcceptsParams(std::uint32_t system) const override;
    int TuneParams(std::uint32_t system, std::uint32_t khz, bool streaming, bool &fatal) override;
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
    // siano::SessionPort
    int Send(const std::uint8_t *data, std::size_t size, siano::Clock::time_point deadline) override;
    bool Removed() const override { return removal_.Removed(); }
    int Delay(unsigned milliseconds) override;
    int StartReceive() override;
    void StopReceive() override;
    void Failed(int error) override { video_.Fail(error); }
    bool AdvanceEpoch() override { return true; }
    int ReadFirmware(FirmwareKind kind, std::vector<std::uint8_t> &firmware) override;
    std::function<void(const std::uint8_t *, std::size_t)> HelperWriter() override;
    unsigned PendingReads() override { return static_cast<unsigned>(in_flight_.load()); }
    bool SignalLogEnabled() override { return true; }

    // 選局の後のロックの待機。地デジはカーネルと同じ 3 秒、J.83B は WinUSB 版と同じ 2 秒
    int WaitLock(std::chrono::milliseconds limit, const char *system, std::uint32_t khz);
    void CloseSession();
    // 読み込み直しの間に止めた受信の転送を出し直す。制御のスレッド (Send と Delay) から呼ぶ
    void ResubmitParked();
    static void LIBUSB_CALL BulkDone(libusb_transfer *transfer);

    struct Transfer {
        PxS1udDevice *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        std::vector<std::uint8_t> buffer;
        std::uint64_t generation = 0;
    };

    std::uint8_t in_ = 0, out_ = 0;
    std::size_t alignment_ = 0;
    std::unique_ptr<PosixIqProcess> helper_;
    std::unique_ptr<siano::Session> session_;
    // VideoNode の状態のロックの下で使う
    bool opened_ = false;
    // Bulk IN の受信 (VideoOpen から VideoClose まで)
    std::vector<std::unique_ptr<Transfer>> transfers_;
    std::mutex receive_mutex_;
    std::condition_variable receive_changed_;
    std::atomic_int in_flight_{0};
    std::atomic_bool resubmit_{false};
    // firmware の読み込み直しで受信が失敗し、出し直しを待つ転送
    std::vector<Transfer *> parked_;
    std::atomic_bool parked_any_{false};
    bool leaked_ = false;
    // 読み込み直しの間に回復させた受信の失敗の数 (完了の通知が数え、受信の停止で記録する)
    std::atomic<std::uint64_t> receive_failures_{0};
};
}
