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

// 抜去した機器の受信機が開いたまま残っても、同じパスで挿し直した機器の受信機だけが検索される
void TestRemovedDevice()
{
	px4::ReceiverManager manager;
	Receiver old_busy, old_free, new_first, new_second;
	const px4::DeviceIdentity path{L"usb-path-replug", L"000002119006501"};
	px4::command::ReceiverInfo info{}, key{}, result{};
	std::uint32_t id = 0;
	wcscpy_s(info.device_name, L"Model");
	info.systems = px4::SystemType::ISDB_T;
	key.systems = px4::SystemType::ISDB_T;
	key.index = -1;

	info.index = 0;
	Require(manager.Register(info, &old_busy, path), "register old busy receiver");
	info.index = 1;
	Require(manager.Register(info, &old_free, path), "register old free receiver");
	wcscpy_s(key.device_name, L"Model");
	Require(manager.SearchAndOpen(key, result, id) != nullptr, "open receiver before removal");
	px4::ReceiverBase *busy = old_busy.open ? static_cast<px4::ReceiverBase *>(&old_busy) : &old_free;
	Receiver &free_old = old_busy.open ? old_free : old_busy;
	const unsigned int free_opens = free_old.opens;

	manager.MarkRemoved(L"");
	Require(!manager.IsRemoved(busy), "empty path marked a receiver");
	manager.MarkRemoved(path.path);
	Require(manager.IsRemoved(busy) && manager.IsRemoved(&free_old), "receivers of the removed device were not marked");
	Require(!manager.SearchAndOpen(key, result, id) && free_old.opens == free_opens,
		"free receiver of the removed device was opened");

	info.index = 0;
	Require(manager.Register(info, &new_first, path), "register replugged first receiver");
	info.index = 1;
	Require(manager.Register(info, &new_second, path), "register replugged second receiver");
	Require(!manager.IsRemoved(&new_first) && !manager.IsRemoved(&new_second), "replugged receivers inherited removal");

	px4::ReceiverBase *selected = manager.SearchAndOpen(key, result, id);
	Require((selected == &new_first || selected == &new_second) && free_old.opens == free_opens,
		"model selection did not move to the replugged device");
	static_cast<Receiver *>(selected)->Close();
	manager.ClearDataId(selected);

	wcscpy_s(key.device_name, (L"@id:" + path.PathId()).c_str());
	selected = manager.SearchAndOpen(key, result, id);
	Require((selected == &new_first || selected == &new_second) && free_old.opens == free_opens,
		"path ID selection did not move to the replugged device");
	static_cast<Receiver *>(selected)->Close();
	manager.ClearDataId(selected);

	// 開いていた接続が閉じた後は、従来どおり古い受信機を登録から外せる
	static_cast<Receiver *>(busy)->Close();
	manager.ClearDataId(busy);
	Require(manager.Unregister(&old_busy) && manager.Unregister(&old_free), "unregister removed receivers");

	// 機器の Term() は受信機が閉じる前に登録から外すため、登録にない受信機も抜去として扱う
	Require(manager.IsRemoved(&old_busy), "receiver unregistered by device teardown was not reported as removed");
	Require(!manager.IsRemoved(&new_first) && !manager.IsRemoved(&new_second), "registered receivers reported as removed");
}
}

int main() try
{
	TestConcurrentOpen();
	TestRemovedDevice();
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
