// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../device_base.hpp"
#include <memory>
#include <atomic>

namespace px4::pxbcud {
class Receiver;
class Device final : public px4::DeviceBase {
public:
    Device(const std::wstring &path, const DeviceDefinition &definition,
           std::uintptr_t index, ReceiverManager &manager);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool available) override;
    ReceiverBase *GetReceiver(int id) const override;
private:
    std::atomic_bool available_{true};
    HANDLE removed_=nullptr;
    std::unique_ptr<Receiver> receiver_;
};
}
