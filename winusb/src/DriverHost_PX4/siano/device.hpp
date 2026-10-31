// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../device_base.hpp"
#include <memory>
namespace px4::siano {
class Receiver;
class Device final : public DeviceBase {
public:
    Device(const std::wstring&, const DeviceDefinition&, std::uintptr_t, ReceiverManager&);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool) override;
    ReceiverBase* GetReceiver(int) const override;
private:
    HANDLE removed_=nullptr;
    std::unique_ptr<Receiver> receiver_;
};
}
