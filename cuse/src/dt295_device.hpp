// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "dt295/control_channel.hpp"
#include "dt295/source_firmware.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace px4::cuse {
// PIXELA PIX-DT295 (06b8:103b)。WinUSB 版と共有する SourceFirmware と ControlChannel で制御転送を直列化し、
// 受信中は SERVICE のループと Bulk IN の読み出しを動かす。
class Dt295Device final : public Device {
public:
    static constexpr std::uint16_t Vendor = 0x06b8, Product = 0x103b;
    Dt295Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~Dt295Device() override;

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

    void CardDiscard() override { card_session_.Discard(); }
    int CardDetect(bool &present) override { return card_session_.Detect(channel_, present); }
    int CardReset() override { return card_session_.Reset(channel_); }
    int CardSetBaudrate(std::uint32_t baudrate) override { return card_session_.SetBaudrate(channel_, baudrate); }
    int CardDataReady(bool &ready) override { return card_session_.DataReady(channel_, ready); }
    std::size_t CardRead(std::uint8_t *data, std::size_t capacity) override { return card_session_.Read(data, capacity); }
    int CardWrite(const std::uint8_t *data, std::size_t size) override { return card_session_.Write(channel_, data, size); }
    int CardPowerDown() override { return card_session_.PowerDown(channel_); }

private:
    int EnsureInitialized();
    void RestoreLocked();
    // Bulk IN と SERVICE を止める。戻り値は受信中の異常、fatal は受信の停止の制御転送が失敗したとき true。
    int StopTransfers(bool &fatal);
    static void LIBUSB_CALL BulkDone(libusb_transfer *transfer);

    dt295::SourceFirmware source_{true};
    UsbTransport transport_;
    dt295::ControlChannel channel_;
    // 受信のデバイスファイルの状態 (VideoNode の状態のロックの下で使う)
    bool initialized_ = false, restore_failed_ = false;
    // 内蔵カードリーダーの応答の状態 (CardNode のロックの下で使う)
    dt295::CardSession card_session_;

    // Bulk IN と SERVICE (StartStream から StopStream まで)
    struct Stream {
        std::vector<libusb_transfer *> transfers;
        std::vector<std::vector<std::uint8_t>> buffers;
        std::atomic_bool running{false}, deliver{false}, stop{false};
        std::atomic_int error{0};
        std::mutex mutex;
        std::condition_variable idle;
        int in_flight = 0;
        std::thread service;
        std::unique_ptr<dt295::TsDelivery> delivery;
        Clock::time_point previous_read{};
        std::uint64_t max_read_gap_us = 0;
    } stream_;
};
}
