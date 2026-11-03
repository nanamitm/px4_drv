#include "backend_power_state.hpp"
#include <cstdio>
#include <vector>

int main()
{
	using Power = px4::BackendPowerState;
	Power power;
	std::vector<bool> operations;
	bool fail_off = false, fail_on = false;
	auto gpio = [&](bool on) { operations.push_back(on); return (on ? fail_on : fail_off) ? -EIO : 0; };
	bool ok = true;
	power.SetOff();
	ok &= power.Request(true, true, 0, gpio) == 0;
	ok &= power.Request(true, true, 1, gpio) == 0 && operations.size() == 1;
	/* cardだけ、receiverだけ、両方の参照が残る各状態でoffを拒否する。 */
	for (unsigned users : {1U, 2U, 5U})
		ok &= power.Request(false, true, users, gpio) == -EBUSY && operations.size() == 1;
	fail_off = true;
	ok &= power.Request(false, true, 0, gpio) == -EIO && power.Get() == Power::State::Unknown;
	ok &= power.Request(true, true, 0, gpio) == -EIO && !operations.back();
	fail_off = false;
	ok &= power.Request(true, true, 0, gpio) == 0 &&
		operations[operations.size() - 2] == false && operations.back() == true;
	ok &= power.Request(false, true, 0, gpio) == 0;
	fail_on = true;
	ok &= power.Request(true, true, 0, gpio) == -EIO && power.Get() == Power::State::Off &&
		!operations.back();
	fail_off = true;
	ok &= power.Request(true, true, 0, gpio) == -EIO && power.Get() == Power::State::Unknown;
	const auto before_active = operations.size();
	ok &= power.Request(true, true, 1, gpio) == -EIO && operations.size() == before_active;
	power.Disconnect();
	ok &= power.Request(false, false, 0, gpio) == 0 && operations.size() == before_active;
	ok &= power.Request(true, false, 0, gpio) == -ENODEV && operations.size() == before_active;
	ok &= power.Request(true, true, 0, gpio) == -ENODEV;
	std::puts(ok ? "backend_power_test: PASS" : "backend_power_test: FAILED");
	return ok ? 0 : 1;
}
