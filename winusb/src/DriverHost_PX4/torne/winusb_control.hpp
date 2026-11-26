// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <windows.h>
#include <winusb.h>

#include "mb86h47_protocol.hpp"
#include "mb86h47_bootstrap.hpp"

namespace px4::torne {

// I/O完了後までOVERLAPPEDとbufferを保持する契約。模擬APIも同じ寿命条件を使う。
class ControlIo {
public:
	virtual ~ControlIo() = default;
	virtual DWORD Submit(WINUSB_INTERFACE_HANDLE usb, const MemoryRequest &request,
		std::uint8_t *data, USHORT length, OVERLAPPED &operation) noexcept = 0;
	virtual DWORD Collect(WINUSB_INTERFACE_HANDLE usb, OVERLAPPED &operation,
		ULONG &transferred, bool wait) noexcept = 0;
	virtual DWORD Cancel(HANDLE file, OVERLAPPED &operation) noexcept = 0;
};

class NativeControlIo final : public ControlIo {
public:
	DWORD Submit(WINUSB_INTERFACE_HANDLE usb, const MemoryRequest &request,
		std::uint8_t *data, USHORT length, OVERLAPPED &operation) noexcept override;
	DWORD Collect(WINUSB_INTERFACE_HANDLE usb, OVERLAPPED &operation,
		ULONG &transferred, bool wait) noexcept override;
	DWORD Cancel(HANDLE file, OVERLAPPED &operation) noexcept override;
};

// 回収期限を過ぎた要求が残る間も、WinUSB とファイルのハンドルを生存させる。
class WinUsbHandles final {
public:
    WinUsbHandles(HANDLE file, WINUSB_INTERFACE_HANDLE usb, bool owned = true) noexcept
        : file_(file), usb_(usb), owned_(owned) {}
    ~WinUsbHandles();
    WinUsbHandles(const WinUsbHandles &) = delete;
    WinUsbHandles &operator=(const WinUsbHandles &) = delete;
    HANDLE File() const noexcept { return file_; }
    WINUSB_INTERFACE_HANDLE Usb() const noexcept { return usb_; }
    bool Ready() const noexcept { return deferred_ == 0; }
private:
    friend class WinUsbRequest;
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    bool owned_;
    std::atomic<unsigned> deferred_{0};
};

// 遅延完了に備え、イベント、OVERLAPPED と転送バッファの寿命をそろえる。
class WinUsbRequest final {
public:
    explicit WinUsbRequest(std::size_t size);
    ~WinUsbRequest();
    WinUsbRequest(const WinUsbRequest &) = delete;
    WinUsbRequest &operator=(const WinUsbRequest &) = delete;
    bool Reset() noexcept;
    static DWORD Finish(std::unique_ptr<WinUsbRequest> &request, const std::shared_ptr<WinUsbHandles> &handles,
                        ControlIo &io, DWORD submitted, DWORD reason, ULONG &transferred) noexcept;

    OVERLAPPED operation{};
    std::vector<std::uint8_t> buffer;
private:
    static unsigned __stdcall Reap(void *context) noexcept;
    std::shared_ptr<WinUsbHandles> retained_handles_;
};

// この個体の共有 Session の制御ロックの下で呼ぶ。
// Beginは各レジスター操作ではなく、初期化・選局などの操作全体に一度だけ適用する。
class WinUsbControl final : public ControlTransport, public BootstrapWait {
public:
	using Clock = std::chrono::steady_clock;
	WinUsbControl(std::shared_ptr<WinUsbHandles> handles, HANDLE cancel, ControlIo &io) noexcept;
    // XZ3 と診断ツールでは従来どおり、呼び出し側が USB ハンドルを所有する。
    WinUsbControl(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE cancel, ControlIo &io) noexcept;
	void Begin(Clock::time_point deadline,HANDLE stop=nullptr) noexcept { deadline_ = deadline; stop_=stop; }
	Clock::time_point Deadline() const noexcept { return deadline_; }
	HANDLE StopEvent() const noexcept { return stop_; }
	int Transfer(const MemoryRequest &request, std::uint8_t *data,
		std::size_t length, std::size_t &transferred) override;
	int Pause(unsigned milliseconds) override;
	int TransferPipe(UCHAR endpoint, std::uint8_t *data, std::size_t length,
		std::size_t &transferred, Clock::time_point deadline);

private:
	template<class Submit>
	int Execute(std::uint8_t *data, std::size_t length, bool read, std::size_t &transferred,
		Clock::time_point deadline, Submit submit);
	std::shared_ptr<WinUsbHandles> handles_;
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
	HANDLE cancel_;
	HANDLE stop_ = nullptr;
	DWORD Cancellation() const noexcept;
	ControlIo &io_;
	Clock::time_point deadline_ = (Clock::time_point::min)();
};

} // namespace px4::torne
