// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../device_base.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace px4::dt295 {
class Device final : public px4::DeviceBase {
public:
    Device(const std::wstring &,const px4::DeviceDefinition &,std::uintptr_t,px4::ReceiverManager &);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool) override;
    px4::ReceiverBase *GetReceiver(int) const override;
    // 内蔵カードリーダー。受信機と同じ制御ロックを使い、受信の有無に関係なく利用できる。
    bool HasCardReader() const noexcept override { return card_reader_; }
    int OpenCard() override;
    void CloseCard() override;
    int DetectCard(bool &detected) override;
    int ResetCard() override;
    int SetCardBaudrate(std::uint32_t baudrate) override;
    int IsCardDataReady(bool &ready) override;
    int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
    int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;
private:
    class Receiver;
    std::unique_ptr<Receiver> receiver_;
    // ATRとT=1ブロックの受信途中の状態。抜去・リセット・Closeで一括して破棄する。
    std::mutex card_mutex_;
    bool card_reader_=false,card_open_=false,card_atr_=true;
    std::vector<std::uint8_t> card_received_;
    std::size_t card_remaining_=0;
    std::atomic_bool available_{true};
    HANDLE removed_=nullptr;
    bool registered_=false;
};
}
