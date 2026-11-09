// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../device_base.hpp"
#include <atomic>
#include <memory>
namespace px4::msi2500 {
class Receiver;
// RSP1 互換機 (Mirics MSi2500 + MSi001)。復調は helper (msi2500_frontend.exe serve) が行い、
// この機器は helper の起動と停止、TS の受け渡し、USB の利用可否の反映だけを持つ。
class Device final: public DeviceBase {
public:
    Device(const std::wstring &,const DeviceDefinition &,std::uintptr_t,ReceiverManager &);
    ~Device() override;
    int Init() override;
    void Term() override;
    void SetAvailability(bool) override;
    ReceiverBase *GetReceiver(int) const override;
private:
    std::uintptr_t index_;
    std::atomic_bool available_{true};
    std::unique_ptr<Receiver> receiver_;
};
}
