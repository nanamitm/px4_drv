// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../device_base.hpp"
#include <atomic>
#include <memory>

namespace px4::dt295 {
class Device final : public px4::DeviceBase {
public:
    Device(const std::wstring &,const px4::DeviceDefinition &,std::uintptr_t,px4::ReceiverManager &);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool) override;
    px4::ReceiverBase *GetReceiver(int) const override;
private:
    class Receiver;
    std::unique_ptr<Receiver> receiver_;
    std::atomic_bool available_{true};
    HANDLE removed_=nullptr;
    bool registered_=false;
    bool torne_=false;
};
}
