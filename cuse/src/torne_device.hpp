// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "torne_port.hpp"
#include "torne/session.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace px4::cuse {
// Sony torne CECH-ZD1J (054c:04b2)。WinUSB 版と共有する torne::Session で制御し、
// 受信は EP 0x81 の 192 バイト単位を TsDelivery で 188 バイトの TS へ変換する。
// 地上デジタル (UHF 13〜62ch) だけを扱う (docs/20261121_torne_linux_cuse_design.md)。
class TorneDevice final : public Device {
public:
    static constexpr std::uint16_t Vendor = 0x054c, Product = 0x04b2;
    TorneDevice(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~TorneDevice() override;

protected:
    int InitModel() override;
    void OnRemove() override {}
    void OnShutdown() override;

    int VideoOpen() override;
    void VideoClose() override;
    bool AcceptsSystem(std::uint32_t system) const override;
    int SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal) override;
    int LockStatus(bool &locked) override;
    int ReadCn(std::uint32_t &raw) override;
    int StartStream(bool &fatal) override;
    int StopStream(bool &fatal) override;

    int CardOpen() override;
    void CardClose() override { card_owner_.reset(); }
    void CardDiscard() override { card_state_.Discard(); }
    int CardDetect(bool &present) override;
    int CardReset() override;
    int CardSetBaudrate(std::uint32_t baudrate) override;
    int CardDataReady(bool &ready) override;
    std::size_t CardRead(std::uint8_t *data, std::size_t capacity) override { return card_state_.Read(data, capacity); }
    int CardWrite(const std::uint8_t *data, std::size_t size) override;
    int CardPowerDown() override;

private:
    // bootstrap 済みのブリッジ。受信とカードが同じ個体で別に bootstrap すると受信を壊すため、
    // 最初の利用者が作り、受信とカードで参照を共有する (WinUSB 版の ExperimentalSession と同じ)。
    struct Shared {
        Shared(libusb_device_handle *handle, RemovalSignal &removal, const std::string &label)
            : port(handle, removal, label), session(port) {}
        TornePort port;
        torne::Session session;
    };
    std::shared_ptr<Shared> AcquireSession();
    void SleepFrontend();
    static void LIBUSB_CALL BulkDone(libusb_transfer *transfer);
    int StopTransfers();

    std::mutex shared_mutex_;
    std::weak_ptr<Shared> shared_;
    std::shared_ptr<Shared> video_owner_;  // VideoNode の状態のロックの下で使う
    std::shared_ptr<Shared> card_owner_;   // CardNode のロックの下で使う
    torne::CardSession card_state_;

    // Bulk IN (StartStream から StopStream まで)
    struct Stream {
        std::vector<libusb_transfer *> transfers;
        std::vector<std::vector<std::uint8_t>> buffers;
        std::atomic_bool running{false};
        std::atomic_int error{0};
        std::mutex mutex;
        std::condition_variable changed;
        int in_flight = 0;
        bool first = false, stop = false;
        std::thread watchdog;
        std::unique_ptr<torne::TsDelivery> delivery;
        Clock::time_point started{}, first_at{}, previous_read{};
        std::uint64_t max_read_gap_us = 0;
    } stream_;
};
}
