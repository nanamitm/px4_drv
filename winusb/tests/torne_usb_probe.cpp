// SPDX-License-Identifier: GPL-2.0-only
// Default: read-only descriptor probe, without vendor requests or resets.
// --initialize: explicitly requested MB86H47 bootstrap diagnostic; no tuning/TS.
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>

#include "../src/DriverHost_PX4/torne/winusb_control.hpp"
#include "../src/DriverHost_PX4/torne/dib8096_registers.hpp"
#include "../src/DriverHost_PX4/torne/device_identity.hpp"
#include "../src/DriverHost_PX4/torne/mb86h47_card.hpp"
#include "../src/WinSCard_PX4/bcas_atr.hpp"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

static constexpr GUID torne_interface = {0xb8f4b677, 0xe19a, 0x4beb, {0xa6, 0x0c, 0xc0, 0xa7, 0xac, 0x2d, 0x0f, 0x6b}};

static bool Inspect(const wchar_t *path, unsigned initialization_timeout, bool identify, bool card_status, bool card_atr)
{
	std::wcout << L"Interface: " << path << L'\n';
	std::cout << "Interface ID: " << px4::torne::InterfaceId(path) << '\n';
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		std::cerr << "CreateFile failed: " << GetLastError() << '\n';
		return false;
	}
	WINUSB_INTERFACE_HANDLE usb = nullptr;
	if (!WinUsb_Initialize(file, &usb)) {
		std::cerr << "WinUsb_Initialize failed: " << GetLastError() << '\n';
		CloseHandle(file);
		return false;
	}
	USB_DEVICE_DESCRIPTOR descriptor{};
	ULONG length = 0;
	bool ok = WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
		reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &length) &&
		length == sizeof(descriptor) && descriptor.bLength == sizeof(descriptor) &&
		descriptor.bDescriptorType == USB_DEVICE_DESCRIPTOR_TYPE &&
		descriptor.idVendor == 0x054c && descriptor.idProduct == 0x04b2;
	if (!ok) {
		std::cerr << "Device descriptor mismatch or read failure: " << GetLastError() << '\n';
	} else {
		std::cout << "VID=054C PID=04B2 serial_descriptor_index=" << unsigned(descriptor.iSerialNumber) << '\n';
		UCHAR alternate = 0xff;
		USB_INTERFACE_DESCRIPTOR interface_descriptor{};
		ok = WinUsb_GetCurrentAlternateSetting(usb, &alternate) &&
			WinUsb_QueryInterfaceSettings(usb, alternate, &interface_descriptor);
		if (ok) {
			std::cout << "interface=" << unsigned(interface_descriptor.bInterfaceNumber)
				<< " alternate=" << unsigned(alternate) << " endpoints=" << unsigned(interface_descriptor.bNumEndpoints) << '\n';
			bool stream = false, command = false, answer = false;
			for (UCHAR i = 0; i < interface_descriptor.bNumEndpoints; ++i) {
				WINUSB_PIPE_INFORMATION pipe{};
				if (!WinUsb_QueryPipe(usb, alternate, i, &pipe)) {
					ok = false;
					std::cerr << "QueryPipe failed: " << GetLastError() << '\n';
					break;
				}
				std::cout << "endpoint=0x" << std::hex << unsigned(pipe.PipeId) << std::dec
					<< " type=" << unsigned(pipe.PipeType) << " max_packet=" << pipe.MaximumPacketSize << '\n';
				if (pipe.PipeType == UsbdPipeTypeBulk) {
					stream |= pipe.PipeId == 0x81;
					command |= pipe.PipeId == 0x02;
				}
				// 応答EPは実機descriptorではinterrupt。ReadPipeは両形式を扱える。
				answer |= pipe.PipeId == 0x83 && pipe.PipeType == UsbdPipeTypeInterrupt;
			}
			ok = ok && interface_descriptor.bInterfaceNumber == 0 && alternate == 0 && stream && command && answer;
		} else {
			std::cerr << "Interface descriptor read failed: " << GetLastError() << '\n';
		}
	}
	std::cout << (ok ? "DESCRIPTORS MATCH (tuning and TS untested)\n" : "DESCRIPTORS FAILED\n");
    const auto handles = std::make_shared<px4::torne::WinUsbHandles>(file, usb);
    if (ok && initialization_timeout) {
		HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!cancel) {
			ok = false;
		} else {
			px4::torne::NativeControlIo native;
			px4::torne::WinUsbControl control(handles, cancel, native);
			const auto deadline = px4::torne::WinUsbControl::Clock::now() + std::chrono::milliseconds(initialization_timeout);
			control.Begin(deadline);
			px4::torne::MemoryAccess memory(control);
			px4::torne::Bootstrap bootstrap(memory, control);
			const auto begin = px4::torne::WinUsbControl::Clock::now();
			const auto result = bootstrap.Initialize(GetTickCount() | 1u);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(px4::torne::WinUsbControl::Clock::now() - begin).count();
			ok = result == px4::torne::BootstrapResult::ok;
			std::cout << "BOOTSTRAP result=" << static_cast<unsigned>(result)
				<< " memory_result=" << static_cast<unsigned>(bootstrap.LastMemoryResult())
				<< " os_error=" << bootstrap.LastSystemError() << " address=0x" << std::hex
				<< bootstrap.LastAddress() << std::dec << " elapsed_ms=" << elapsed << '\n';
			std::cout << (ok ? "BOOTSTRAP PASSED (demod initialization, tuning and TS untested)\n" : "BOOTSTRAP FAILED\n");
			if (ok && card_status) {
				px4::torne::CardUart card(memory, control);
				bool present = false;
				ok = card.Present(present);
				std::cout << "CARD STATUS id=" << px4::torne::InterfaceId(path)
					<< " ok=" << ok << " present=" << present
					<< " memory_result=" << static_cast<unsigned>(card.Result()) << '\n';
				if (ok && card_atr && present) {
					std::vector<std::uint8_t> atr;
					ok = card.Reset();
					const auto atr_deadline = (std::min)(deadline, px4::torne::WinUsbControl::Clock::now() + std::chrono::seconds(1));
					bool known_bcas = false;
					while (ok && !known_bcas && px4::torne::WinUsbControl::Clock::now() < atr_deadline) {
						std::array<std::uint8_t, 33> bytes{};
						std::size_t count = 0;
						ok = card.ReadBytes(bytes.data(), bytes.size(), count);
						if (count) atr.insert(atr.end(), bytes.begin(), bytes.begin() + count);
						if (atr.size() > 33) { ok = false; break; }
						known_bcas = px4::winscard::IsBcasAtr(atr.data(), atr.size());
						if (ok && !known_bcas) ok = control.Pause(5) == 0;
					}
					control.Begin(px4::torne::WinUsbControl::Clock::now() + std::chrono::seconds(2));
					const bool powered_down = card.PowerDown();
					ok = ok && known_bcas && powered_down;
					std::cout << "CARD ATR bytes=" << atr.size() << " known_bcas=" << known_bcas
						<< " powered_down=" << powered_down << " ok=" << ok << '\n';
				}
			}
			if (ok && identify) {
				const auto reset = bootstrap.ResetDemodulator();
				std::cout << "DEMOD RESET result=" << static_cast<unsigned>(reset) << " os_error=" << bootstrap.LastSystemError() << '\n';
				ok = reset == px4::torne::BootstrapResult::ok;
				px4::torne::MailboxI2c bus(memory, control);
				ok = ok && bus.BeginAfterBootstrap(bootstrap);
				px4::torne::DiB8096Registers registers(bus);
				if (ok) {
					const auto enumerated = registers.Write(1287, 3, deadline);
					std::cout << "DEMOD ENUMERATE result=" << static_cast<unsigned>(enumerated) << " os_error=" << bus.LastSystemError() << '\n';
					ok = enumerated == px4::torne::I2cResult::ok;
				}
				if (ok) {
					std::uint16_t vendor = 0, device = 0;
					bool recognized = false;
					const auto identified = registers.Identify(vendor, device, recognized, deadline);
					std::cout << "DEMOD IDENTIFY result=" << static_cast<unsigned>(identified) << " vendor=0x" << std::hex
						<< vendor << " device=0x" << device << std::dec << " recognized=" << recognized
						<< " os_error=" << bus.LastSystemError() << '\n';
					ok = identified == px4::torne::I2cResult::ok && recognized;
				}
				std::cout << (ok ? "DEMOD IDENTIFICATION PASSED (full demod initialization, tuning and TS untested)\n" : "DEMOD IDENTIFICATION FAILED\n");
			}
			CloseHandle(cancel);
		}
	}
	return ok;
}

int main(int argc, char **argv)
{
	std::string requested_id;
	// 末尾の任意selectorを先に取り除き、既存診断モードの引数契約を維持する。
	if (argc >= 3 && std::string(argv[argc - 2]) == "--device-id") {
		if (!px4::torne::NormalizeInterfaceId(argv[argc - 1], requested_id)) {
			std::cerr << "--device-id requires exactly 16 hexadecimal digits\n";
			return 2;
		}
		argc -= 2;
	}
	unsigned initialization_timeout = 0;
	bool identify = false;
	bool card_status = false;
	bool card_atr = false;
	// 復調器の初期化、選局、TS の受信の診断は、Go の診断ヘルパーと一緒に削除した。
	// 選局と受信は DriverHost_PX4 の通常の経路 (channel_switch_stress など) で確かめる。
	if (argc != 1) {
		const std::string mode = argv[1];
		if (argc != 4 || (mode != "--card-atr" && mode != "--card-status" && mode != "--initialize" && mode != "--identify") || std::string(argv[2]) != "--timeout-ms") {
			std::cerr << "Usage: torne_usb_probe.exe [--initialize|--identify|--card-status|--card-atr --timeout-ms <positive-budget>] [--device-id <16 hex digits>]\n";
			return 2;
		}
		card_atr = mode == "--card-atr";
		card_status = card_atr || mode == "--card-status";
		identify = mode == "--identify";
		try {
			const std::string value = argv[3];
			std::size_t parsed = 0;
			const auto timeout = std::stoul(value, &parsed);
			if (value.empty() || value[0] == '-' || parsed != value.size() || !timeout || timeout >= MAXDWORD)
				throw std::invalid_argument("timeout");
			initialization_timeout = static_cast<unsigned>(timeout);
		} catch (...) {
			std::cerr << "Invalid initialization timeout\n";
			return 2;
		}
	}
	HDEVINFO info = SetupDiGetClassDevsW(&torne_interface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (info == INVALID_HANDLE_VALUE)
		return 1;
	bool ok = true;
	unsigned count = 0;
	std::vector<std::wstring> paths;
	for (DWORD i = 0; ; ++i) {
		SP_DEVICE_INTERFACE_DATA entry{};
		entry.cbSize = sizeof(entry);
		if (!SetupDiEnumDeviceInterfaces(info, nullptr, &torne_interface, i, &entry)) {
			ok = ok && GetLastError() == ERROR_NO_MORE_ITEMS;
			break;
		}
		DWORD required = 0;
		SetupDiGetDeviceInterfaceDetailW(info, &entry, nullptr, 0, &required, nullptr);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
			ok = false;
			continue;
		}
		// ワイド文字構造体を格納できるアラインメントを確保する。
		std::vector<std::uint64_t> storage((required + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
		auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
		detail->cbSize = sizeof(*detail);
		if (!SetupDiGetDeviceInterfaceDetailW(info, &entry, detail, required, nullptr, nullptr)) {
			ok = false;
			continue;
		}
		++count;
		paths.emplace_back(detail->DevicePath);
	}
	SetupDiDestroyDeviceInfoList(info);
	// 不完全な列挙・未指定の複数台・ID衝突は拒否。選択失敗で他の個体へfallbackしない。
	if (initialization_timeout || !requested_id.empty()) {
		std::size_t selected = paths.size();
		const auto selection = px4::torne::SelectInterface(paths, requested_id, selected);
		if (!ok || selection != px4::torne::SelectionResult::ok) {
			std::cerr << "Exactly one completely enumerated device must match; selection=" << static_cast<unsigned>(selection) << '\n';
			return 1;
		}
		const std::wstring chosen = paths[selected];
		paths.assign(1, chosen);
	}
	if (initialization_timeout)
		std::cout << "INITIALIZATION MODE: caller must have stopped BonDriverCoax/coax on this device.\n";
	for (const auto &path : paths) {
		const bool device_ok = Inspect(path.c_str(), initialization_timeout, identify, card_status, card_atr);
		ok = ok && device_ok;
	}
	std::cout << "Present interfaces: " << count << '\n';
	return count && ok ? 0 : 1;
}
