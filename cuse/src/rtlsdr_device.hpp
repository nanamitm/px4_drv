// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "rtlsdr_helper.hpp"
#include "rtlsdr/session.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct rtlsdr_dev;

namespace px4::cuse {
// 接続位置 (sysfs と同じ "バス番号-ポートの経路"、例 3-4) が環境変数 PX4_CUSED_RTLSDR_PORTS に含まれるか。
// RTL2838 は汎用の SDR でほかの用途と取り合い、シリアル番号も重なるため、利用者が指定した機器だけを開く前に選ぶ
bool RtlSdrPortSelected(libusb_device *usb);

// RTL-SDR (RTL2838UHIDIR、0bda:2838、チューナー R820T) のワンセグ。デーモンが USB を持ち、RTL2832U と R820T を
// librtlsdr (RTL-SDR Blog V1.4.0、cuse/third_party/rtl-sdr-blog) で設定して、Bulk IN の I/Q を helper (rtlsdr_frontend --stdin)
// の標準入力へ渡す。helper の監視と起動し直しは WinUSB 版と共有する rtlsdr::Session が行う。
// カードリーダーとアンテナ給電はない (docs/20261122_rtlsdr_linux_cuse_design.md)。
class RtlSdrDevice final : public Device, private rtlsdr::HelperPort {
public:
    static constexpr std::uint16_t Vendor = 0x0bda, Product = 0x2838;
    RtlSdrDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~RtlSdrDevice() override;

protected:
    int InitModel() override;
    void OnRemove() override;
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
    // rtlsdr::HelperPort。Session のハンドルのロックの下で、USB の設定、Bulk IN の受信、helper を一組で扱う
    int Launch(std::uint32_t hz, bool restarted) override;
    int Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait) override;
    void RequestStop() override;
    void WaitStopped(std::chrono::milliseconds limit) override;
    std::uint32_t Kill() override;
    void Release() override;
    bool DevicePresent() override { return !removal_.Removed(); }

    // RTL2832U と R820T を開いて、WinUSB 版の helper と同じ順に周波数、標本化周波数、利得を設定する
    int Configure(std::uint32_t hz);
    void CloseTuner();
    int StartReceive();
    void StopReceive();
    static void LIBUSB_CALL BulkDone(libusb_transfer *transfer);

    struct Transfer {
        RtlSdrDevice *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        std::vector<std::uint8_t> buffer;
    };

    const std::string helper_path_;
    int gain_ = 297;
    rtlsdr_dev *rtl_ = nullptr;
    RtlSdrHelperProcess helper_;
    std::vector<std::unique_ptr<Transfer>> transfers_;
    std::mutex receive_mutex_;
    std::condition_variable receive_changed_;
    std::atomic_int in_flight_{0};
    std::atomic_bool resubmit_{false};
    // Bulk IN の受信の失敗。Read が失敗を返し、Session が helper と受信を起動し直す
    std::atomic_bool usb_failed_{false};
    bool leaked_ = false;
    // VideoNode の状態のロックの下で使う
    bool tuned_ = false;
    // 最後に置き、helper の監視のスレッドをほかのメンバーより先に止める
    rtlsdr::Session session_;
};
}
