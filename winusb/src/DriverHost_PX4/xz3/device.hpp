// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "../device_base.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace px4::xz3 {

class Device final : public DeviceBase {
public:
    Device(const std::wstring& path, const DeviceDefinition& definition,
           std::uintptr_t index, ReceiverManager& manager);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool available) override;
    ReceiverBase* GetReceiver(int id) const override;
    // 内蔵カードリーダー。受信機と同じ制御ロックを使い、受信の有無に関係なく利用できる。
    bool HasCardReader() const noexcept override { return card_reader_; }
    int OpenCard() override;
    void CloseCard() override;
    int DetectCard(bool& detected) override;
    int ResetCard() override;
    int SetCardBaudrate(std::uint32_t baudrate) override;
    int IsCardDataReady(bool& ready) override;
    int ReadCardData(std::uint8_t* buf, std::uint8_t& len) override;
    int WriteCardData(const std::uint8_t* buf, std::uint8_t len) override;

private:
    class Receiver;
    void ResetCardBuffer();
    std::unique_ptr<Receiver> receiver_;
    // ATRとT=1ブロックの受信途中の状態。抜去・リセット・Closeで一括して破棄する。
    std::mutex card_mutex_;
    bool card_reader_ = false, card_open_ = false, card_atr_ = true;
    std::vector<std::uint8_t> card_received_;
    std::size_t card_remaining_ = 0;
    // 直前の受信FIFOの読み出しの時刻と、それが空だったか。
    std::chrono::steady_clock::time_point card_polled_{};
    bool card_polled_empty_ = false;
    std::atomic_bool available_{true};
    HANDLE removed_ = nullptr;
    bool registered_ = false;
};

}
