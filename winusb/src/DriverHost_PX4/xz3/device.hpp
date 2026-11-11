// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "../device_base.hpp"
#include <atomic>
#include <memory>

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

private:
    class Receiver;
    std::unique_ptr<Receiver> receiver_;
    std::atomic_bool available_{true};
    HANDLE removed_ = nullptr;
    bool registered_ = false;
};

}
