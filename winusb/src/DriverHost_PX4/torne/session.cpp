// SPDX-License-Identifier: GPL-2.0-only
#include "session.hpp"
#include "reception_policy.hpp"
#include "tune_result.hpp"
#include "msg.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace px4::torne {

Session::Session(SessionPort &port)
    : port_(port), memory_(port.Transport()), bootstrap_(memory_, port.Wait()), bus_(memory_, port.Wait()),
      registers_(bus_), frontend_(bus_, port.Signal()), engine_(memory_, bootstrap_)
{
}

bool Session::Command(std::uint8_t command, const std::vector<std::uint8_t> &body,
    std::vector<std::uint8_t> &response, Clock::time_point deadline, bool stoppable)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    return CommandLocked(command, body, response, deadline, stoppable);
}

bool Session::CommandWithin(std::uint8_t command, const std::vector<std::uint8_t> &body,
    std::vector<std::uint8_t> &response, Clock::duration budget, bool stoppable)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    return CommandLocked(command, body, response, Clock::now() + budget, stoppable);
}

bool Session::ReadStatus(std::vector<std::uint8_t> &response, Clock::time_point deadline, bool stoppable, bool retry)
{
    return Command(StatusCommand(retry), {}, response, deadline, stoppable) && response.size() == 24;
}

bool Session::ReadStatusWithin(std::vector<std::uint8_t> &response, Clock::duration budget, bool stoppable)
{
    return CommandWithin(StatusCommand(false), {}, response, budget, stoppable) && response.size() == 24;
}

bool Session::Recover(bool bulk_i2c)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (healthy_) return true;
    msg_warn("torne: reinitializing the bridge after a failed control transfer\n");
    if (!InitializeBridgeLocked(bulk_i2c)) return false;
    healthy_ = true;
    return true;
}

void Session::Invalidate()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    healthy_ = false;
    bus_.Invalidate();
}

std::string Session::FrontendError()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    return frontend_.Error();
}

std::uint8_t Session::StatusCommand(bool retry)
{
    return static_cast<std::uint8_t>(retry ? 0x13 : NativeFrontend::ReadOnlyStatus);
}

bool Session::CommandLocked(std::uint8_t command, const std::vector<std::uint8_t> &body,
    std::vector<std::uint8_t> &response, Clock::time_point deadline, bool stoppable)
{
    if (!healthy_ || !frontend_started_) return false;
    if (stoppable && port_.Signal().StopRequested())
        return false;
    // 選局の停止はI2C要求の間かフロントエンドの待機で扱う。発行済みのmailbox要求は
    // 応答を回収し、完了不明の要求を次の選局へ残さない。USB抜去と期限は転送にも適用する。
    port_.Begin(deadline);
    frontend_.SetStoppable(stoppable);
    const bool ok = frontend_.Command(command, body, response, deadline);
    const bool stopped_cleanly = frontend_.StoppedCleanly();
    if (!ok && !stopped_cleanly) { healthy_ = false; bus_.Invalidate(); }
    return ok;
}

bool Session::InitializeBridge(bool bulk_i2c)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    return InitializeBridgeLocked(bulk_i2c);
}

bool Session::InitializeBridgeLocked(bool bulk_i2c)
{
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    port_.Begin(deadline);
    // 起動ごとに変わる値であればよく、時刻の精度は不要。
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const auto seed = (static_cast<std::uint32_t>(milliseconds) & 0x7fffffffu) | 1u;
    if (bootstrap_.Initialize(seed) != BootstrapResult::ok) {
        msg_err("torne: bootstrap failed result=%u address=%08x os_error=%d\n",
            static_cast<unsigned>(bootstrap_.Result()), bootstrap_.LastAddress(), bootstrap_.LastSystemError());
        return false;
    }
    if (!bus_.BeginAfterBootstrap(bootstrap_))
        return false;
    if (bulk_i2c) {
        const auto result = bus_.EnableBulk(seed, [this](std::uint8_t ep, std::uint8_t *data,
            std::size_t length, std::size_t &transferred, Clock::time_point limit) {
            const auto error = port_.TransferPipe(ep, data, length, transferred, limit);
            if (port_.TimedOut(error))
                return I2cResult::timeout;
            if (error)
                msg_err("torne: Bulk I2C endpoint 0x%02x failed, os_error=%d\n", unsigned(ep), error);
            return error ? I2cResult::memory_error : I2cResult::ok;
        }, deadline);
        if (result != I2cResult::ok) {
            msg_err("torne: Bulk I2C startup failed, result=%u\n", static_cast<unsigned>(result));
            return false;
        }
        frontend_.SetBulkTiming();
    }
    return true;
}

bool Session::InitializeFrontend(bool output_error_packets, bool batch_i2c_writes)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!healthy_) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    port_.Begin(deadline);
    if (bootstrap_.ResetDemodulator() != BootstrapResult::ok) return false;
    if (registers_.Write(1287, 3, deadline) != I2cResult::ok) return false;
    std::uint16_t vendor = 0, chip = 0;
    bool recognized = false;
    if (registers_.Identify(vendor, chip, recognized, deadline) != I2cResult::ok || !recognized) return false;
    frontend_started_ = true;
    frontend_.SetStoppable(false);
    frontend_.SetBatchWrites(batch_i2c_writes);
    std::vector<std::uint8_t> response;
    bool ok = frontend_.Command(0x10, {}, response, deadline) && response.empty();
    if (ok && output_error_packets) {
        // SMO error-discardを解除する診断用設定。受信できない層の復元や
        // ホスト側のnull補完は行わず、復調器が出したTEIもそのまま上位へ渡す。
        std::uint16_t mode = 0, verified = 0;
        ok = registers_.Read(299, mode, deadline) == I2cResult::ok &&
            registers_.Write(299, static_cast<std::uint16_t>(mode & ~0x0040u), deadline) == I2cResult::ok &&
            registers_.Read(299, verified, deadline) == I2cResult::ok && !(verified & 0x0040);
        msg_info("torne: OutputErrorPackets SMO=%04x verified=%04x result=%d\n", mode, verified, ok);
    }
    if (!ok) { healthy_ = false; bus_.Invalidate(); }
    return ok;
}

StreamResult Session::PrepareStream(Clock::time_point deadline, bool stoppable)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    port_.Begin(deadline, stoppable);
    return engine_.Prepare();
}

StreamResult Session::StartStream(Clock::time_point deadline, bool stoppable)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    port_.Begin(deadline, stoppable);
    return engine_.Start();
}

StreamResult Session::StopStream(Clock::time_point deadline)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    port_.Begin(deadline);
    return engine_.Stop();
}

bool Session::Card(Clock::duration budget, const std::function<bool(CardUart &)> &operation)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    port_.Begin(Clock::now() + budget);
    CardUart uart(memory_, port_.Wait());
    return operation(uart);
}

int TuneFrequency(Session &session, const std::string &id, std::uint32_t frequency, Clock::time_point deadline,
    bool stoppable, bool retry_initial, const TuneHooks &hooks)
{
    const auto hz = frequency * 1000u;
    std::vector<std::uint8_t> request(4), response;
    for (unsigned i = 0; i < 4; ++i) request[i] = static_cast<std::uint8_t>(hz >> (8 * i));
    if ((stoppable && !hooks.running()) || !session.Command(0x11, request, response, deadline, stoppable)) {
        msg_err("torne: tune command failed id=%s freq_khz=%u detail=%s\n", id.c_str(), frequency, session.FrontendError().c_str());
        return -EIO;
    }
    if (IsConfirmedSearchFailure(response.data(), response.size()) && !retry_initial) {
        msg_info("torne: confirmed search failure id=%s freq_khz=%u\n", id.c_str(), frequency);
        return -ENODATA;
    }
    const auto result = WaitInitialLock(deadline, [] { return Clock::now(); }, [&] {
        if ((stoppable && !hooks.running()) || !session.ReadStatus(response, deadline, stoppable, retry_initial))
            return RecoveryResult::fatal;
        return StatusLocked(response) ? RecoveryResult::ready : RecoveryResult::no_lock;
    }, hooks.sleep);
    return result == RecoveryResult::ready ? 0 : result == RecoveryResult::stopped ? -ECANCELED :
        result == RecoveryResult::fatal ? -EIO : -ETIMEDOUT;
}

void SleepFrontend(Session &session)
{
    std::vector<std::uint8_t> response;
    if (!session.Command(0x14, {}, response, Clock::now() + std::chrono::seconds(2)))
        msg_warn("torne: sleep/LED release failed\n");
}

bool StatusLocked(const std::vector<std::uint8_t> &status) noexcept
{
    return status.size() == 24 && status[0] == 1;
}

int ReadStatusValue(const std::vector<std::uint8_t> &status, StatusReading kind, std::int32_t &value) noexcept
{
    value = 0;
    if (status.size() != 24) return -EIO;
    double reading = 0;
    std::size_t offset = 0;
    if (kind == StatusReading::cnr && status[1]) offset = 8;
    else if (kind == StatusReading::signal_strength && status[2]) offset = 16;
    else return -ENOSYS;
    std::memcpy(&reading, status.data() + offset, sizeof(reading));
    if (!std::isfinite(reading) || reading < -200.0 || reading > 200.0) return -EIO;
    value = static_cast<std::int32_t>(std::lround(reading * 1000.0));
    return 0;
}

int CardSession::Detect(Session &session, bool &present)
{
    const bool ok = session.Card(std::chrono::seconds(1), [&](CardUart &uart) { return uart.Present(present); });
    if (!ok || !present) response_.Reset();
    return ok ? 0 : -EIO;
}

int CardSession::Reset(Session &session)
{
    response_.Reset();
    return session.Card(std::chrono::seconds(1), [](CardUart &uart) { return uart.Reset(); }) ? 0 : -EIO;
}

int CardSession::SetBaudrate(Session &session, std::uint32_t baudrate)
{
    if (baudrate != 19200 && baudrate != 38400) return -ENOTSUP;
    return session.Card(std::chrono::seconds(1), [&](CardUart &uart) { return uart.SetBaudrate(baudrate); }) ? 0 : -EIO;
}

int CardSession::DataReady(Session &session, bool &ready)
{
    ready = false;
    if (response_.Ready()) { ready = true; return 0; }
    std::array<std::uint8_t, 259> bytes{};
    std::size_t count = 0;
    if (!session.Card(std::chrono::seconds(1), [&](CardUart &uart) { return uart.ReadBytes(bytes.data(), bytes.size(), count); }))
        return -EIO;
    if (const int result = response_.Append(bytes.data(), count))
        return result;
    ready = response_.Ready();
    return 0;
}

int CardSession::Write(Session &session, const std::uint8_t *data, std::size_t size)
{
    return session.Card(std::chrono::seconds(1), [&](CardUart &uart) { return uart.WriteBytes(data, size); }) ? 0 : -EIO;
}

bool CardSession::PowerDown(Session &session)
{
    return session.Card(std::chrono::seconds(1), [](CardUart &uart) { return uart.PowerDown(); });
}

std::size_t TsDelivery::Deliver(const std::uint8_t *data, std::size_t size, const Sink &sink)
{
    output_.clear();
    framer_.Feed(data, size, output_);
    for (std::size_t offset = 0; offset < output_.size(); offset += StreamFramer::packet_size)
        quality_.Feed(output_.data() + offset);
    if (!output_.empty())
        sink(output_.data(), output_.size());
    return output_.size();
}
} // namespace px4::torne
