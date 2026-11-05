// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "winusb_control.hpp"
#include "mb86h47_i2c.hpp"
#include <memory>
#include <string>
namespace px4::torne {
// 同じUSB個体・制御worker内だけで利用する。外部から同時にTransfer/Stopしない。
class SourceFirmware final {
public:
    SourceFirmware(MemoryAccess &,WinUsbControl &,HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE cancel);
    ~SourceFirmware();
    SourceFirmware(const SourceFirmware &)=delete;
    SourceFirmware &operator=(const SourceFirmware &)=delete;
    bool Start(const std::wstring &path,const std::wstring &sha256,WinUsbControl::Clock::time_point deadline);
    bool Stop() noexcept;
    I2cResult Transfer(std::uint8_t address,const std::uint8_t *write,std::size_t write_size,
        std::uint8_t *read,std::size_t read_size,WinUsbControl::Clock::time_point deadline);
    const std::string &Error() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
