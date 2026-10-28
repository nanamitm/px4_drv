// Copyright 2026 Till0196
// SPDX-License-Identifier: GPL-2.0-only
// C++ adaptation of coax mb86h47/i2c.go at
// 2d9f2c263a3b080ecef3a8ede1eb86c6afa48405; GPLv2 branch selected.
#pragma once

#include "mb86h47_bootstrap.hpp"
#include <vector>

namespace px4::torne {

enum class I2cResult {
	ok, invalid_argument, not_ready, memory_error, timeout, malformed_answer,
	bus_error, address_nack, data_nack, stuck
};

// 初期統合用の同期mailbox経路。ring/commandと併用すると順序保証を失うため、
// 同一個体で別のI2C経路を使用する場合は先に全要求をdrainする必要がある。
class MailboxI2c final {
public:
	using Clock = std::chrono::steady_clock;
	MailboxI2c(MemoryAccess &memory, BootstrapWait &clock) noexcept : memory_(memory), clock_(clock) {}
	bool BeginAfterBootstrap(const Bootstrap &bootstrap) noexcept;
	void Invalidate() noexcept { ready_ = false; }
	I2cResult Transfer(std::uint8_t address, const std::uint8_t *write, std::size_t write_size,
		std::uint8_t *read, std::size_t read_size, Clock::time_point deadline);
	MemoryResult LastMemoryResult() const noexcept { return memory_result_; }
	int LastSystemError() const noexcept { return memory_.LastTransportError(); }
	// 制御 mutex の所有者だけが呼ぶ。同期 mailbox と job の同時実行は不要で、
	// timeout 後は不明な完了を次の要求へ混ぜないようバス全体を無効にする。
	I2cResult RunJob(std::uint32_t kind,const std::vector<std::uint32_t> &parameters,
		unsigned expected_ms,std::array<std::uint32_t,3> &results,Clock::time_point deadline);
	I2cResult Captrim(std::uint8_t address,std::uint16_t lo4,std::uint32_t key,
		int start,int step,int target,std::int64_t settle_write,std::int64_t settle_read,
		int &best,int &diff,Clock::time_point deadline);

private:
	MemoryAccess &memory_;
	BootstrapWait &clock_;
	std::uint8_t sequence_ = 0;
	bool ready_ = false;
	MemoryResult memory_result_ = MemoryResult::ok;
	const Bootstrap *bootstrap_ = nullptr;
	std::uint64_t generation_ = 0;
	std::uint32_t job_sequence_ = 0;
	double spin_rate_ = 0;
	I2cResult SpinRate(Clock::time_point deadline);
	I2cResult StartJob(std::uint32_t kind,const std::vector<std::uint32_t> &parameters,Clock::time_point deadline);
};

} // namespace px4::torne
