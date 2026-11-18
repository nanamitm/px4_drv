#include "receiver_manager.hpp"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <future>
#include <functional>
#include <thread>

extern "C" int msg_printf(const char *, ...) { return 0; }
namespace {
void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
class Receiver final : public px4::ReceiverBase {
public:
	Receiver() : ReceiverBase(0) {}
	unsigned int opens = 0;
	bool open = false;
	std::function<void()> before_open;
	int Open() override { ++opens; if (before_open) before_open(); if (open) return -EBUSY; open = true; return 0; }
	void Close() override { open = false; }
	int CheckLock(bool &locked) override { locked = true; return 0; }
	int SetLnbVoltage(std::int32_t) override { return 0; }
	int SetCapture(bool) override { return 0; }
	int ReadStat(px4::command::StatType, std::int32_t &) override { return 0; }
protected:
	int SetFrequency() override { return 0; }
	int SetStreamId() override { return 0; }
};

void TestConcurrentOpen()
{
	using namespace std::chrono_literals;
	px4::ReceiverManager manager;
	Receiver slow, other;
	const px4::DeviceIdentity a{L"slow-path", L"123456"}, b{L"other-path", a.serial};
	px4::command::ReceiverInfo info{}, slow_key{}, other_key{}, serial_key{};
	info.index = 0;
	Require(manager.Register(info, &slow, a, true), "register slow receiver");
	Require(manager.Register(info, &other, b), "register other receiver");
	wcscpy_s(slow_key.device_name, (L"@id:" + a.PathId()).c_str());
	wcscpy_s(other_key.device_name, (L"@id:" + b.PathId()).c_str());
	wcscpy_s(serial_key.device_name, (L"@serial:" + a.serial).c_str());
	std::promise<void> entered, release;
	const auto release_wait = release.get_future();
	slow.before_open = [&] {
		entered.set_value();
		release_wait.wait_for(5s);
	};
	auto opening = std::async(std::launch::async, [&] {
		px4::command::ReceiverInfo result{};
		std::uint32_t id = 0;
		return manager.SearchAndOpen(slow_key, result, id);
	});
	Require(entered.get_future().wait_for(2s) == std::future_status::ready, "slow Open did not start");
	const auto begin = std::chrono::steady_clock::now();
	px4::command::ReceiverInfo result{};
	std::uint32_t id = 0;
	Require(manager.SearchAndOpen(other_key, result, id) == &other && manager.SearchByDataId(id) == &other,
		"other receiver could not open and bind a stream");
	Require(std::chrono::steady_clock::now() - begin < 2s, "slow Open blocked the manager");
	Require(!manager.SearchAndOpen(slow_key, result, id), "slow receiver was opened twice");
	other.Close();
	manager.ClearDataId(&other);

	auto removal = std::async(std::launch::async, [&] { return manager.Unregister(&slow); });
	// 登録解除の開始を確認する。同じシリアルが曖昧でなくなるまで一覧を検索する。
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	px4::ReceiverBase *selected = nullptr;
	while (!(selected = manager.SearchAndOpen(serial_key, result, id)) &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool waits_for_open = removal.wait_for(0s) == std::future_status::timeout;
	release.set_value();
	Require(selected == &other && waits_for_open, "removal must hide the receiver and wait for its Open");
	Require(opening.get() == nullptr && removal.get() && !slow.open, "detached receiver was published or left open");
	other.Close();
	manager.ClearDataId(&other);
}
}

int main() try
{
	TestConcurrentOpen();
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
