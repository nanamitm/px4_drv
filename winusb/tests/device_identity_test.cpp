#include "receiver_manager.hpp"
#include <cstdio>
#include <stdexcept>
#include <algorithm>

extern "C" int msg_printf(const char *, ...) { return 0; }
namespace {
void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
class Receiver final : public px4::ReceiverBase {
public:
	Receiver() : ReceiverBase(0) {}
	unsigned int opens = 0;
	bool open = false;
	int Open() override { ++opens; if (open) return -EBUSY; open = true; return 0; }
	void Close() override { open = false; }
	int CheckLock(bool &locked) override { locked = true; return 0; }
	int SetLnbVoltage(std::int32_t) override { return 0; }
	int SetCapture(bool) override { return 0; }
	int ReadStat(px4::command::StatType, std::int32_t &) override { return 0; }
protected:
	int SetFrequency() override { return 0; }
	int SetStreamId() override { return 0; }
};
}

int main() try
{
	px4::ReceiverManager manager;
	Receiver first, sibling, second;
	px4::DeviceIdentity a{L"usb-path-a", L"123456789012345"}, b{L"usb-path-b", a.serial};
	px4::command::ReceiverInfo info{};
	wcscpy_s(info.device_name, L"Model"); info.index = 0;
	info.systems = px4::SystemType::ISDB_T;
	Require(manager.Register(info, &first, a), "register first");
	info.index = 1;
	Require(manager.Register(info, &sibling, a), "register sibling");
	info.index = 0;
	Require(manager.Register(info, &second, b), "register second");
	px4::command::ReceiverInfo key{}, result{};
	key.index = 0; key.systems = px4::SystemType::ISDB_T;
	std::uint32_t id = 0;
	wcscpy_s(key.device_name, (L"@serial:" + a.serial).c_str());
	Require(!manager.SearchAndOpen(key, result, id) && !first.opens && !second.opens,
		"duplicate serial selected a device");
	wcscpy_s(key.device_name, (L"@id:" + a.PathId()).c_str());
	Require(manager.SearchAndOpen(key, result, id) == &first, "path ID selected wrong receiver");
	Require(!wcscmp(result.device_name, L"Model"), "response replaced model with selector");
	wcscpy_s(key.device_name, (L"@serial:" + a.serial).c_str());
	Require(!manager.SearchAndOpen(key, result, id) && !second.opens,
		"duplicate serial became unique when one device was busy");
	manager.ClearDataId(&first); first.Close();
	Require(manager.Unregister(&second), "unregister second");
	Require(manager.SearchAndOpen(key, result, id) == &first, "unique serial was rejected");
	manager.ClearDataId(&first); first.Close();
	key.index = 1;
	Require(manager.SearchAndOpen(key, result, id) == &sibling, "same-device receiver index failed");
	manager.ClearDataId(&sibling); sibling.Close();
	wcscpy_s(key.device_name, L"@id:invalid");
	Require(!manager.SearchAndOpen(key, result, id), "invalid selector fell back to arbitrary device");
	wcscpy_s(key.device_name, L"Model"); key.index = 0;
	Require(manager.SearchAndOpen(key, result, id) == &first, "legacy model selection broke");
	manager.ClearDataId(&first); first.Close();
	std::fill(std::begin(key.device_name), std::end(key.device_name), L'x');
	Require(!manager.SearchAndOpen(key, result, id), "unterminated IPC name accepted");
	std::puts("device_identity_test: PASS"); return 0;
}
catch (const std::exception &error) { std::fprintf(stderr, "device_identity_test: %s\n", error.what()); return 1; }
