// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "device.hpp"
#include "xz3/session.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace px4::cuse {
// I-O DATA GV-MVP/XZ3 (04bb:0530)。WinUSB 版と共有する xz3::Session で制御し、
// 受信は操作12 で機器内のリングから送り出させた Bulk IN (0x81) の 192 バイト単位を 188 バイトの TS にする。
// 地デジ・BS・110度CS を 1 つの受信機で扱う (docs/20261121_gv_mvp_xz3_linux_cuse_design.md)。
class Xz3Device final : public Device, private xz3::StreamSink {
public:
    static constexpr std::uint16_t Vendor = 0x04bb, Product = 0x0530;
    Xz3Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~Xz3Device() override;

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

    int CardOpen() override { return card_reader_.Open(); }
    // 電源は CardNode が CardPowerDown で先に切る。
    void CardClose() override { card_reader_.Close(false); }
    void CardDiscard() override { card_reader_.Discard(); }
    int CardDetect(bool &present) override { return card_reader_.Detect(present); }
    int CardReset() override { return card_reader_.Reset(); }
    int CardSetBaudrate(std::uint32_t baudrate) override { return card_reader_.SetBaudrate(baudrate); }
    int CardDataReady(bool &ready) override { return card_reader_.IsDataReady(ready); }
    std::size_t CardRead(std::uint8_t *data, std::size_t capacity) override;
    int CardWrite(const std::uint8_t *data, std::size_t size) override;
    int CardPowerDown() override { return card_reader_.PowerDown(); }

private:
    class Port;
    void Restore();
    void Started() override;
    void PutPacket(const std::uint8_t *packet) override;
    void Flush() override;
    void Failed(int error) override;

    std::unique_ptr<Port> port_;
    std::unique_ptr<xz3::Session> session_;
    xz3::CardReader card_reader_{available_};
    // VideoNode の状態のロックの下で使う。
    bool tuned_ = false, satellite_ = false, restore_failed_ = false;
    // TS の worker だけが使う。1 回の読み出し分をまとめて VideoNode へ渡す。
    std::vector<std::uint8_t> batch_;
    std::atomic<std::uint64_t> packets_{0};
};
}
