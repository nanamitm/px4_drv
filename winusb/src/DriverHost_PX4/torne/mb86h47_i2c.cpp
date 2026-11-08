// Copyright 2026 Till0196
// SPDX-License-Identifier: GPL-2.0-only
#include "mb86h47_i2c.hpp"

#include <algorithm>
#include <array>

namespace px4::torne {

bool MailboxI2c::BeginAfterBootstrap(const Bootstrap &bootstrap) noexcept
{
	if (bootstrap_ == &bootstrap && generation_ == bootstrap.Generation())
		return false;
	ready_ = bootstrap.IsInitialized() && bootstrap.Result() == BootstrapResult::ok;
	if (!ready_)
		return false;
	bootstrap_ = &bootstrap;
	generation_ = bootstrap.Generation();
	sequence_ = 0;
	job_sequence_ = 0;
	spin_rate_ = 0;
	memory_result_ = MemoryResult::ok;
	return ready_;
}

I2cResult MailboxI2c::Transfer(std::uint8_t address, const std::uint8_t *write, std::size_t write_size,
	std::uint8_t *read, std::size_t read_size, Clock::time_point deadline)
{
	memory_result_ = MemoryResult::ok;
	if ((address & 1) || write_size > 32 || read_size > 32 || (!write_size && !read_size) ||
		(write_size && !write) || (read_size && !read))
		return I2cResult::invalid_argument;
	if (!ready_ || !bootstrap_ || !bootstrap_->IsInitialized() || bootstrap_->Generation() != generation_) {
		ready_ = false;
		return I2cResult::not_ready;
	}
	if (clock_.Now() >= deadline)
		return I2cResult::timeout;
	++sequence_;
	if (!sequence_)
		sequence_ = 1;
	auto failed = [&](I2cResult result) {
		// core1側で完了したか不明な要求の連番を再利用すると、遅延応答へ誤一致し得る。
		ready_ = false;
		return result;
	};
	for (std::size_t offset = 0; offset < write_size; offset += 4) {
		if (clock_.Now() >= deadline)
			return failed(I2cResult::timeout);
		std::uint32_t word = 0;
		for (unsigned byte = 0; byte < 4; ++byte) {
			word <<= 8;
			if (offset + byte < write_size)
				word |= write[offset + byte];
		}
		memory_result_ = memory_.Write32(0x17a3f04 + static_cast<std::uint32_t>(offset), word);
		if (memory_result_ != MemoryResult::ok)
			return failed(I2cResult::memory_error);
	}
	if (clock_.Now() >= deadline)
		return failed(I2cResult::timeout);
	const auto command = (std::uint32_t(sequence_) << 24) | (std::uint32_t(address) << 16) |
		(static_cast<std::uint32_t>(write_size) << 8) | static_cast<std::uint32_t>(read_size);
	// データを配置し終えてからsequenceを含むコマンドを最後に書く。
	memory_result_ = memory_.Write32(0x17a3f00, command);
	if (memory_result_ != MemoryResult::ok)
		return failed(I2cResult::memory_error);
	std::array<std::uint8_t, 36> answer{};
	const auto answer_size = 4 + ((read_size + 3) & ~std::size_t(3));
	for (;;) {
		if (clock_.Now() >= deadline)
			return failed(I2cResult::timeout);
		memory_result_ = memory_.Transfer(0x17a3f80, true, answer.data(), answer_size);
		if (memory_result_ != MemoryResult::ok)
			return failed(I2cResult::memory_error);
		if (answer[0] == sequence_)
			break;
	}
	if (clock_.Now() >= deadline)
		return failed(I2cResult::timeout);
	if (answer[1] || answer[2])
		return failed(I2cResult::malformed_answer);
	switch (answer[3]) {
	case 0:
		if (read_size)
			std::copy_n(answer.data() + 4, read_size, read);
		return I2cResult::ok;
	case 1: return failed(I2cResult::bus_error);
	case 2: return I2cResult::address_nack;
	case 3: return I2cResult::data_nack;
	case 4: return failed(I2cResult::stuck);
	default: return failed(I2cResult::malformed_answer);
	}
}

// coax job.go の同期ジョブ経路。mailbox 操作は完了応答まで待つため、
// このアダプターには未回収の ring 要求がなく、job 開始前の drain は不要。
I2cResult MailboxI2c::StartJob(std::uint32_t kind,const std::vector<std::uint32_t> &parameters,Clock::time_point deadline)
{
	if(parameters.size()>14)return I2cResult::invalid_argument;
	if(!ready_ || !bootstrap_ || !bootstrap_->IsInitialized() || bootstrap_->Generation()!=generation_)return I2cResult::not_ready;
	if(clock_.Now()>=deadline)return I2cResult::timeout;
	std::array<std::uint8_t,60> data{};
	auto put=[&](std::size_t index,std::uint32_t word){for(unsigned i=0;i<4;++i)data[index*4+i]=static_cast<std::uint8_t>(word>>(24-8*i));};
	put(0,kind);
	for(std::size_t i=0;i<parameters.size();++i)put(i+1,parameters[i]);
	memory_result_=memory_.Transfer(0x17a3a04,false,data.data(),4*(parameters.size()+1));
	if(memory_result_==MemoryResult::ok && clock_.Now()<deadline){
		if(!++job_sequence_)++job_sequence_;
		memory_result_=memory_.Write32(0x17a3a00,job_sequence_);
		if(memory_result_==MemoryResult::ok && clock_.Now()<deadline)return I2cResult::ok;
	}
	ready_=false;
	return memory_result_==MemoryResult::ok?I2cResult::timeout:I2cResult::memory_error;
}

I2cResult MailboxI2c::RunJob(std::uint32_t kind,const std::vector<std::uint32_t> &parameters,
	unsigned expected_ms,std::array<std::uint32_t,3> &results,Clock::time_point deadline)
{
	results={};
	const auto limit=(std::min)(deadline,clock_.Now()+std::chrono::milliseconds(expected_ms+500));
	const auto started=StartJob(kind,parameters,limit);
	if(started!=I2cResult::ok)return started;
	auto fail=[&](I2cResult result){ready_=false;return result;};
	if(expected_ms>2){
		if(clock_.Now()+std::chrono::milliseconds(expected_ms-2)>=limit)return fail(I2cResult::timeout);
		if(clock_.Pause(expected_ms-2))return fail(I2cResult::memory_error);
	}
	std::array<std::uint8_t,20> answer{};
	auto word=[&](std::size_t offset){return (std::uint32_t(answer[offset])<<24)|(std::uint32_t(answer[offset+1])<<16)|(std::uint32_t(answer[offset+2])<<8)|answer[offset+3];};
	while(clock_.Now()<limit){
		memory_result_=memory_.Transfer(0x17a3a40,true,answer.data(),answer.size());
		if(memory_result_!=MemoryResult::ok)return fail(I2cResult::memory_error);
		if(clock_.Now()>=limit)return fail(I2cResult::timeout);
		if(word(0)==job_sequence_){
			for(unsigned i=0;i<3;++i)results[i]=word(8+4*i);
			switch(word(4)){
			case 0:return I2cResult::ok;
			case 2:return I2cResult::address_nack;
			case 3:return I2cResult::data_nack;
			case 1:return fail(I2cResult::bus_error);
			case 4:return fail(I2cResult::stuck);
			default:return fail(I2cResult::malformed_answer);
			}
		}
		if(clock_.Now()+std::chrono::milliseconds(1)>=limit)return fail(I2cResult::timeout);
		if(clock_.Pause(1))return fail(I2cResult::memory_error);
	}
	return fail(I2cResult::timeout);
}

I2cResult MailboxI2c::SpinRate(Clock::time_point deadline)
{
	if(spin_rate_>0)return I2cResult::ok;
	if(clock_.Now()+std::chrono::milliseconds(110)>=deadline)return I2cResult::timeout;
	auto result=StartJob(1,{100000000},deadline);
	if(result!=I2cResult::ok)return result;
	auto fail=[&](I2cResult value){ready_=false;return value;};
	if(clock_.Pause(10))return fail(I2cResult::memory_error);
	std::uint32_t before=0,after=0;
	memory_result_=memory_.Read32(0x17a3a60,before);
	if(memory_result_!=MemoryResult::ok)return fail(I2cResult::memory_error);
	const auto from=clock_.Now();
	if(clock_.Pause(100))return fail(I2cResult::memory_error);
	memory_result_=memory_.Read32(0x17a3a60,after);
	if(memory_result_!=MemoryResult::ok)return fail(I2cResult::memory_error);
	const auto elapsed=std::chrono::duration<double>(clock_.Now()-from).count();
	if(after==before || elapsed<=0 || clock_.Now()>=deadline)return fail(I2cResult::timeout);
	const double rate=static_cast<std::uint32_t>(after-before)/elapsed;
	// 新しい連番で校正ループを終了する。終了応答の回収前には I2C を再開できない。
	if(!++job_sequence_)++job_sequence_;
	memory_result_=memory_.Write32(0x17a3a00,job_sequence_);
	if(memory_result_!=MemoryResult::ok)return fail(I2cResult::memory_error);
	const auto limit=(std::min)(deadline,clock_.Now()+std::chrono::milliseconds(500));
	while(clock_.Now()<limit){
		std::uint32_t done=0;
		memory_result_=memory_.Read32(0x17a3a40,done);
		if(memory_result_!=MemoryResult::ok)return fail(I2cResult::memory_error);
		if(clock_.Now()>=limit)return fail(I2cResult::timeout);
		if(done==job_sequence_){spin_rate_=rate;return I2cResult::ok;}
		if(clock_.Pause(1))return fail(I2cResult::memory_error);
	}
	return fail(I2cResult::timeout);
}

I2cResult MailboxI2c::Captrim(std::uint8_t address,std::uint16_t lo4,std::uint32_t key,
	int start,int step,int target,std::int64_t settle_write,std::int64_t settle_read,
	int &best,int &diff,Clock::time_point deadline)
{
	if((address&1)||!key||settle_write<0||settle_read<0||settle_write>1000000000||settle_read>1000000000)return I2cResult::invalid_argument;
	auto result=SpinRate(deadline);
	if(result!=I2cResult::ok)return result;
	auto passes=[&](std::int64_t ns){return static_cast<std::uint32_t>(ns*1e-9*spin_rate_+0.5);};
	std::array<std::uint32_t,3> answer{};
	const auto expected=static_cast<unsigned>((settle_write+3*(settle_write+settle_read)+999999)/1000000);
	result=RunJob(2,{address,lo4,static_cast<std::uint32_t>(start),static_cast<std::uint32_t>(step),
		static_cast<std::uint32_t>(target),passes(settle_write),passes(settle_write),passes(settle_read),key},expected,answer,deadline);
	if(result==I2cResult::ok){best=static_cast<std::int32_t>(answer[0]);diff=static_cast<std::int32_t>(answer[1]);}
	return result;
}

} // namespace px4::torne
