#pragma once
#include <cerrno>

namespace px4 {

/* 参照数は機種側が既存ロック下で所有する。GPIO の成否だけを記録する。 */
class BackendPowerState final {
public:
	enum class State { Unknown, Off, On, Disconnected };
	void SetOff() noexcept { state_ = State::Off; }
	void Disconnect() noexcept { state_ = State::Disconnected; }
	State Get() const noexcept { return state_; }

	template<class Operation>
	int Request(bool on, bool available, unsigned int users, Operation operation)
	{
		if (!available) { Disconnect(); return on ? -ENODEV : 0; }
		if (state_ == State::Disconnected) return -ENODEV;
		if (!on && users) return -EBUSY;
		if (on && state_ == State::On) return 0;
		if (!on && state_ == State::Off) return 0;
		/* 利用者が残る電源は、状態不明でも再投入しない。 */
		if (on && users) return -EIO;
		if (on && state_ == State::Unknown) {
			int ret = operation(false);
			if (ret) return ret;
			state_ = State::Off;
		}
		int ret = operation(on);
		state_ = ret ? State::Unknown : (on ? State::On : State::Off);
		if (ret && on) {
			/* 部分的な投入に失敗した場合も、未使用の電源を可能な限り停止する。 */
			if (!operation(false)) state_ = State::Off;
		}
		return ret;
	}
private:
	State state_ = State::Unknown;
};
}
