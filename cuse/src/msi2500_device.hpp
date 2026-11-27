// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "msi2500_ops.hpp"
#include "rtlsdr_helper.hpp"
#include "rtlsdr/session.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace px4::cuse {
// 接続位置が環境変数 PX4_CUSED_MSI2500_PORTS に含まれるか。MSi2500 は汎用の SDR (Linux の本体の msi2500 ドライバー、
// SDRplay の API) でほかの用途と取り合い、シリアル番号を返さない個体もあるため、利用者が指定した機器だけを開く前に選ぶ
bool Msi2500PortSelected(libusb_device *usb);

// RSP1 互換機 (Mirics MSi2500 + MSi001、1df7:2500) のフルセグと J.83B。デーモンが USB を持ち、helper (msi2500_frontend)
// の ops が出す制御転送の列で MSi2500 と MSi001 を設定して、等時転送の生ブロックを helper の serve --stdin の標準入力へ渡す。
// helper の監視と起動し直しは RTL-SDR と同じ rtlsdr::Session が行う (helper の出力の形式が同じため)。
// カードリーダーとアンテナ給電はない (docs/20261123_msi2500_linux_cuse_design.md)。
class Msi2500Device final : public Device, private rtlsdr::HelperPort {
public:
    static constexpr std::uint16_t Vendor = 0x1df7, Product = 0x2500;
    Msi2500Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~Msi2500Device() override;

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
    // rtlsdr::HelperPort。Session のハンドルのロックの下で、制御転送、等時転送の受信、helper を一組で扱う
    int Launch(std::uint32_t hz, bool restarted) override;
    int Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait) override;
    void RequestStop() override;
    void WaitStopped(std::chrono::milliseconds limit) override;
    std::uint32_t Kill() override;
    void Release() override;
    bool DevicePresent() override { return !removal_.Removed(); }

    // 1 回の制御転送 (bmRequestType 0x40、wLength 0)
    using ControlOp = msi2500::ControlOp;
    // helper の ops を実行して、取り込みの前に送る列 (setup) と停止要求の後に送る列 (stop) を得る
    int LoadOps(std::uint32_t hz, std::vector<ControlOp> &setup, std::vector<ControlOp> &stop);
    int SendOps(const std::vector<ControlOp> &ops);
    int StartReceive();
    void StopReceive();
    // 転送を止めた後に、停止要求、ADC の休止、alternate 0 (等時転送の帯域の解放) を送る
    void PowerDown();
    int StartTune(bool j83b, std::uint32_t hz, std::uint32_t khz, bool streaming);
    static void LIBUSB_CALL IsoDone(libusb_transfer *transfer);

    struct ReceiveState {
        explicit ReceiveState(Msi2500Device *device) : owner(device) {}
        std::mutex mutex;
        // 停止の期限を過ぎたら、残ったコールバックからデバイスへの参照を切る。
        Msi2500Device *owner;
    };

    struct Transfer {
        ~Transfer() { libusb_free_transfer(transfer); }
        std::shared_ptr<ReceiveState> state;
        libusb_transfer *transfer = nullptr;
        bool in_flight = false; // ReceiveState::mutex で保護する
        std::vector<std::uint8_t> buffer;
        // 完了したパケットのうち、1024 バイトのブロックだけを集めて helper へ渡す
        std::vector<std::uint8_t> blocks;
    };

    const std::string helper_path_;
    int gain_ = 40, gain_j83b_ = 46, viterbi_threads_ = 0;
    // alternate 1 の等時転送 IN 0x81 の 1 パケット (1 マイクロフレーム) の最大長。InitModel で記述子から求める
    int packet_size_ = 0;
    // 起動中 (起動し直しを含む) の helper の系統。Session::Start の前に設定し、Launch が使う
    bool system_j83b_ = false;
    std::vector<ControlOp> stop_ops_;
    bool powered_ = false;
    RtlSdrHelperProcess helper_;
    std::vector<std::unique_ptr<Transfer>> transfers_;
    std::shared_ptr<ReceiveState> receive_ = std::make_shared<ReceiveState>(this);
    std::condition_variable receive_changed_;
    int in_flight_ = 0; // ReceiveState::mutex で保護する
    std::atomic_bool resubmit_{false};
    // 受信の失敗 (抜去、転送の失敗の連続、3 秒の無受信)。Read が失敗を返し、Session が helper と受信を起動し直す
    std::atomic_bool usb_failed_{false};
    std::atomic<std::uint64_t> last_data_ms_{0};
    std::atomic<std::uint32_t> consecutive_errors_{0};
    std::atomic<std::uint64_t> stat_transfers_{0}, stat_packets_{0}, stat_bad_packets_{0}, stat_empty_packets_{0},
        stat_odd_packets_{0}, stat_blocks_{0}, stat_errors_{0};
    bool leaked_ = false;
    // VideoNode の状態のロックの下で使う
    bool tuned_ = false;
    // 最後に置き、helper の監視のスレッドをほかのメンバーより先に止める
    rtlsdr::Session session_;
};
}
