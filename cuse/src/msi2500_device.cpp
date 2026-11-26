// SPDX-License-Identifier: GPL-2.0-only
#include "msi2500_device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef PX4_CUSED_LIBDIR
#define PX4_CUSED_LIBDIR "/usr/local/lib/px4-cused"
#endif

extern char **environ;

namespace px4::cuse {
namespace {
constexpr unsigned char IsoIn = 0x81;
constexpr int StreamAlternate = 1;
constexpr std::uint8_t UsbStartRequest = 0x43, UsbStopRequest = 0x45;
// 1 転送のパケット数 (High-Speed は 8 の倍数、64 パケット = 8 ms) と同時に投入する転送数 (256 ms 分)。
// WinUSB 版の helper は 64 転送 (512 ms) を投入するが、usbfs は既定で全機器の転送のバッファーを 16 MB に制限し
// (usbfs_memory_mb)、64 転送では約 12.6 MB をこの機器だけで使うため、半分にして N150 の実機で欠落を確かめた
constexpr int Packets = 64;
constexpr int TransferCount = 32;
constexpr std::size_t Block = 1024;
// WinUSB 版の helper の CaptureLimits と同じ。データの届かない時間と、転送の失敗が続く回数の上限
constexpr std::uint64_t StallMs = 3000;
constexpr std::uint32_t MaxConsecutiveErrors = 64;
// 選局から最初の TS までの待機。WinUSB 版の検証環境では約 2 秒で、helper は 10 秒 TS が出なければ復調を作り直す。
// 弱電界の作り直しを 1 回含められるよう、RTL-SDR と同じ 15 秒とする
constexpr auto FirstTsTimeout = std::chrono::seconds(15);

std::string LibDir()
{
    const char *dir = std::getenv("PX4_CUSED_LIBDIR");
    return dir && *dir ? dir : PX4_CUSED_LIBDIR;
}

// 環境変数の整数。範囲外は既定値を使い、ログへ残す
int EnvInt(const char *name, int fallback, int min, int max)
{
    const char *text = std::getenv(name);
    if (!text || !*text)
        return fallback;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (*end || value < min || value > max) {
        msg_warn("MSi2500: %s=%s is not %d..%d; using %d\n", name, text, min, max, fallback);
        return fallback;
    }
    return static_cast<int>(value);
}
}

bool Msi2500PortSelected(libusb_device *usb)
{
    return UsbPortListed(usb, "PX4_CUSED_MSI2500_PORTS", "MSi2500");
}

Msi2500Device::Msi2500Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                             std::function<void()> changed)
    : Device(handle, bus, address, "MSi2500", "msi2500video", std::move(changed)),
      helper_path_(LibDir() + "/msi2500_frontend"),
      // 利得は MSi001 の LNA・ミキサー・IF の合計 (dB)。WinUSB 版の GainDb と GainDbJ83B の既定値と同じ
      gain_(EnvInt("PX4_CUSED_MSI2500_GAIN", 40, 0, 102)), gain_j83b_(EnvInt("PX4_CUSED_MSI2500_GAIN_J83B", 46, 0, 102)),
      // 0 は helper の既定 (論理 CPU 数、最大 4)
      viterbi_threads_(EnvInt("PX4_CUSED_MSI2500_VITERBI_THREADS", 0, 0, 64)),
      session_(static_cast<rtlsdr::HelperPort &>(*this),
               [this](const std::uint8_t *data, std::size_t size, std::uint64_t) { video_.Push(data, size); }, "MSi2500")
{
}

Msi2500Device::~Msi2500Device()
{
    StopSessions();
    session_.Stop();
}

int Msi2500Device::InitModel()
{
    // interface 0 の alternate 1 にある等時転送 IN 0x81 から生ブロックを読む (WinUSB 版の helper と同じ)
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle_), &config))
        return -EIO;
    if (config->bNumInterfaces >= 1) {
        const auto &intf = config->interface[0];
        for (int a = 0; a < intf.num_altsetting; ++a) {
            const auto &alt = intf.altsetting[a];
            if (alt.bAlternateSetting != StreamAlternate)
                continue;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto &ep = alt.endpoint[e];
                if (ep.bEndpointAddress == IsoIn &&
                    (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                    // 下位 11 ビットがパケット長、11〜12 ビットがマイクロフレームあたりの追加の回数
                    packet_size_ = (ep.wMaxPacketSize & 0x7ff) * (1 + ((ep.wMaxPacketSize >> 11) & 3));
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    if (packet_size_ <= 0 || packet_size_ % static_cast<int>(Block)) {
        msg_err("%s: isochronous IN 0x81 on alternate 1 was not found (packet_size=%d)\n", label_.c_str(), packet_size_);
        return -ENOSYS;
    }
    // 別のプロセスが残した状態に関係なく、取り込みを止めた状態から始める
    libusb_set_interface_alt_setting(handle_, 0, 0);
    msg_info("%s: packet_size=%d gain=%d gain_j83b=%d viterbi_threads=%d helper=%s\n", label_.c_str(), packet_size_,
             gain_, gain_j83b_, viterbi_threads_, helper_path_.c_str());
    return 0;
}

void Msi2500Device::OnRemove()
{
    // 起動し直しを止めてから helper と受信を止める。抜去の後の USB の操作は失敗で終わる
    session_.Disconnect();
    session_.Stop();
}

void Msi2500Device::OnShutdown()
{
    session_.Stop();
}

int Msi2500Device::VideoOpen()
{
    if (leaked_)
        return -EIO;
    if (access(helper_path_.c_str(), X_OK)) {
        msg_err("%s: helper is not installed: %s\n", label_.c_str(), helper_path_.c_str());
        return -ENOENT;
    }
    tuned_ = false;
    return 0;
}

void Msi2500Device::VideoClose()
{
    session_.Stop();
    tuned_ = false;
}

bool Msi2500Device::AcceptsSystem(std::uint32_t system) const
{
    // 地デジだけを PTX_SET_SYSTEM_MODE で受け付ける。J.83B はカーネルと同じく拡張 ioctl でだけ指定する (PX-S1UD と同じ)
    return system == PTX_UNSPECIFIED_SYSTEM || system == PTX_ISDB_T_SYSTEM;
}

bool Msi2500Device::AcceptsParams(std::uint32_t system) const
{
    return system == PTX_J83B_SYSTEM;
}

int Msi2500Device::StartTune(bool j83b, std::uint32_t hz, std::uint32_t khz, bool streaming)
{
    const auto started = Clock::now();
    int rc = streaming ? -EBUSY : 0;
    if (!rc) {
        tuned_ = false;
        system_j83b_ = j83b;
        rc = session_.Start(hz);
    }
    // 最初の TS を待つ。成功はカーネルと同じくロックを意味し、届かなければ EAGAIN で選び直しを求める
    while (!rc && !session_.Locked()) {
        if (session_.Exhausted())
            rc = -EIO;
        else if (Clock::now() - started >= FirstTsTimeout)
            rc = -EAGAIN;
        else if (removal_.Wait(10))
            rc = -ECANCELED;
    }
    tuned_ = !rc || rc == -EAGAIN;
    msg_info("%s: tune %s kHz=%u result=%d after_ms=%lld\n", label_.c_str(), j83b ? "J.83B" : "ISDB-T", khz, rc,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()));
    return rc;
}

int Msi2500Device::SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal)
{
    // 制御転送の失敗も、WinUSB 版と同じく選局の失敗として返し、次の選局で設定し直す
    fatal = false;
    Tune tune;
    std::uint32_t hz = 0;
    // 復調は UHF 13〜52ch (freq_no 63〜102) の 1/7 MHz のずれを含む格子だけを前提とするため、slot による補正も断る
    // (WinUSB 版の 473,143〜707,143 kHz の検査と同じ)
    int rc = ChannelToTune(system == PTX_UNSPECIFIED_SYSTEM ? PTX_ISDB_T_SYSTEM : system, freq, tune);
    if (!rc && (tune.satellite || freq.freq_no < 63 || freq.freq_no > 102 || freq.slot != 0 ||
                !rtlsdr::HelperFrequency(tune.khz, hz)))
        rc = -EINVAL;
    if (rc) {
        msg_info("%s: set channel freq_no=%d slot=%d result=%d\n", label_.c_str(), freq.freq_no, freq.slot, rc);
        return rc;
    }
    return StartTune(false, hz, tune.khz, streaming);
}

int Msi2500Device::TuneParams(std::uint32_t system, std::uint32_t khz, bool streaming, bool &fatal)
{
    fatal = false;
    // チャンネル定義の中心周波数 (kHz) をそのまま使う。地域によって H001〜H105 からずれた周波数があるため、
    // 6.5 MHz 刻みに限らず helper の受け付ける範囲 (90〜775 MHz) で受ける (WinUSB 版と同じ)
    if (system != PTX_J83B_SYSTEM || khz < 90000 || khz > 775000)
        return -EINVAL;
    return StartTune(true, khz * 1000, khz, streaming);
}

int Msi2500Device::LockStatus(bool &locked)
{
    locked = tuned_ && session_.Locked();
    return 0;
}

int Msi2500Device::ReadCn(std::uint32_t &raw)
{
    // helper の MER (dB×100、ISDB-T は B 階層、J.83B は判定帰還中の復調器) を、PX-S1UD と同じく地デジの換算式で
    // 同じ dB になる値へ換算する。MER を受け取る前と TS が届いていない間は式の下限を返す
    const auto mer = session_.MerX100();
    raw = DbToCndat(tuned_ && session_.Locked() && mer >= 0 ? mer / 100.0 : 0);
    return 0;
}

int Msi2500Device::StartStream(bool &fatal)
{
    // helper は選局から閉じるまで動き続け、VideoNode は START の前と STOP の後の TS を捨てる
    fatal = false;
    return 0;
}

int Msi2500Device::StopStream(bool &fatal)
{
    fatal = false;
    // 抜去では、read と poll へ EOF ではなく抜去の契約の EIO を先に返す (RTL-SDR と同じ)
    return removal_.Removed() ? -EIO : 0;
}

int Msi2500Device::LoadOps(std::uint32_t hz, std::vector<ControlOp> &setup, std::vector<ControlOp> &stop)
{
    // helper の ops は計算だけで終わる。標準出力を読み切ってから終了を待つ
    int out[2];
    if (pipe2(out, O_CLOEXEC))
        return -errno;
    std::vector<std::string> args = {helper_path_, "ops", "--frequency", std::to_string(hz), "--gain",
                                     std::to_string(system_j83b_ ? gain_j83b_ : gain_)};
    if (system_j83b_) {
        args.push_back("--system");
        args.push_back("j83b");
    }
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, helper_path_.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (spawned) {
        close(out[0]);
        msg_err("%s: helper ops spawn failed (%s)\n", label_.c_str(), std::strerror(spawned));
        return -EIO;
    }
    std::string text;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    char buffer[4096];
    for (;;) {
        pollfd p{out[0], POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0)
            break;
        const ssize_t got = read(out[0], buffer, sizeof(buffer));
        if (got <= 0)
            break;
        text.append(buffer, static_cast<std::size_t>(got));
    }
    close(out[0]);
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        msg_err("%s: helper ops failed status=%d\n", label_.c_str(), status);
        return -EIO;
    }
    setup.clear();
    stop.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        char stage[8] = {};
        unsigned request = 0, value = 0, index = 0;
        if (std::sscanf(line.c_str(), "%7s %x %x %x", stage, &request, &value, &index) != 4 || request > 0xff ||
            value > 0xffff || index > 0xffff) {
            msg_err("%s: invalid helper ops line: %s\n", label_.c_str(), line.c_str());
            return -EIO;
        }
        const ControlOp op{static_cast<std::uint8_t>(request), static_cast<std::uint16_t>(value),
                           static_cast<std::uint16_t>(index)};
        if (!std::strcmp(stage, "setup"))
            setup.push_back(op);
        else if (!std::strcmp(stage, "stop"))
            stop.push_back(op);
        else {
            msg_err("%s: invalid helper ops stage: %s\n", label_.c_str(), line.c_str());
            return -EIO;
        }
    }
    if (setup.empty()) {
        msg_err("%s: helper ops returned no setup\n", label_.c_str());
        return -EIO;
    }
    return 0;
}

int Msi2500Device::SendOps(const std::vector<ControlOp> &ops)
{
    for (const auto &op : ops) {
        // WinUSB 版と同じく宛先をデバイス (0x40) にする。宛先をエンドポイントにすると、上位のビットを wIndex に詰めた
        // 書き込みを WinUSB が拒否した (winusb/docs/msi2500-fullseg-design.md の 2.2 節)
        const int rc = libusb_control_transfer(handle_, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_DEVICE, op.request, op.value, op.index, nullptr, 0, 1000);
        if (rc < 0) {
            msg_err("%s: control request=0x%02x value=0x%04x index=0x%04x failed libusb=%s\n", label_.c_str(),
                    op.request, op.value, op.index, libusb_error_name(rc));
            return rc == LIBUSB_ERROR_NO_DEVICE || removal_.Removed() ? -ECANCELED : -EIO;
        }
    }
    return 0;
}

void Msi2500Device::PowerDown()
{
    if (!powered_)
        return;
    powered_ = false;
    if (removal_.Removed())
        return;
    // WinUSB 版の helper と Linux の msi2500 ドライバーと同じく、転送を止めてから 20 ms あけて停止要求を送り、
    // USB インターフェースと ADC を休止させる。転送の最中に停止要求を送ると、長めの取り込みの後で次の開始で
    // データが届かない状態に入ることがあった
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SendOps(std::vector<ControlOp>{ControlOp{UsbStopRequest, 0, 0}});
    SendOps(stop_ops_);
    // 等時転送の帯域の予約 (alternate 1 は 1 マイクロフレームに 3 × 1024 バイト) を解放する
    libusb_set_interface_alt_setting(handle_, 0, 0);
}

int Msi2500Device::Launch(std::uint32_t hz, bool)
{
    if (removal_.Removed())
        return -ECANCELED;
    if (leaked_)
        return -EIO;
    usb_failed_ = false;
    std::vector<ControlOp> setup, stop;
    int rc = LoadOps(hz, setup, stop);
    if (!rc) {
        stop_ops_ = std::move(stop);
        powered_ = true;
        rc = SendOps(setup);
    }
    if (!rc) {
        if (const int r = libusb_set_interface_alt_setting(handle_, 0, StreamAlternate)) {
            msg_err("%s: alternate %d failed libusb=%s\n", label_.c_str(), StreamAlternate, libusb_error_name(r));
            rc = r == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO;
        }
    }
    if (!rc) {
        std::vector<std::string> args = {helper_path_, "serve", "--stdin", "--frequency", std::to_string(hz), "--gain",
                                         std::to_string(system_j83b_ ? gain_j83b_ : gain_)};
        if (system_j83b_) {
            args.push_back("--system");
            args.push_back("j83b");
        } else if (viterbi_threads_) {
            args.push_back("--viterbi-threads");
            args.push_back(std::to_string(viterbi_threads_));
        }
        rc = helper_.Spawn(args);
    }
    if (!rc) {
        rc = StartReceive();
        if (rc)
            helper_.Kill();
    }
    // WinUSB 版の helper と同じく、転送を投入してから開始要求を送る
    if (!rc && (rc = SendOps(std::vector<ControlOp>{ControlOp{UsbStartRequest, 0, 0}}))) {
        StopReceive();
        helper_.Kill();
    }
    if (rc)
        PowerDown();
    else
        msg_info("%s: started system=%s frequency=%u gain=%d\n", label_.c_str(), system_j83b_ ? "J.83B" : "ISDB-T", hz,
                 system_j83b_ ? gain_j83b_ : gain_);
    return rc;
}

int Msi2500Device::Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait)
{
    if (usb_failed_)
        return -EIO;
    if (resubmit_) {
        const auto last = last_data_ms_.load();
        if (last && rtlsdr::NowMs() - last > StallMs) {
            msg_err("%s: no data for %llu ms; restarting the helper\n", label_.c_str(),
                    static_cast<unsigned long long>(rtlsdr::NowMs() - last));
            usb_failed_ = true;
            return -EIO;
        }
    }
    return helper_.Read(buffer, size, wait);
}

void Msi2500Device::RequestStop()
{
    StopReceive();
    helper_.RequestStop();
}

void Msi2500Device::WaitStopped(std::chrono::milliseconds limit)
{
    helper_.WaitStopped(limit);
}

std::uint32_t Msi2500Device::Kill()
{
    StopReceive();
    return helper_.Kill();
}

void Msi2500Device::Release()
{
    helper_.Release();
    PowerDown();
}

int Msi2500Device::StartReceive()
{
    const std::size_t length = static_cast<std::size_t>(packet_size_) * Packets;
    try {
        for (int i = 0; i < TransferCount; ++i) {
            auto t = std::make_unique<Transfer>();
            t->state = receive_;
            t->buffer.resize(length);
            t->blocks.reserve(length);
            t->transfer = libusb_alloc_transfer(Packets);
            if (!t->transfer)
                throw std::bad_alloc();
            libusb_fill_iso_transfer(t->transfer, handle_, IsoIn, t->buffer.data(), static_cast<int>(length), Packets,
                                     IsoDone, t.get(), 0);
            libusb_set_iso_packet_lengths(t->transfer, static_cast<unsigned>(packet_size_));
            transfers_.push_back(std::move(t));
        }
    } catch (...) {
        transfers_.clear();
        return -ENOMEM;
    }
    stat_transfers_ = stat_packets_ = stat_bad_packets_ = stat_empty_packets_ = stat_odd_packets_ = stat_blocks_ =
        stat_errors_ = 0;
    consecutive_errors_ = 0;
    last_data_ms_ = rtlsdr::NowMs();
    int rc = 0;
    {
        std::lock_guard<std::mutex> lock(receive_->mutex);
        resubmit_ = true;
        for (auto &t : transfers_) {
            rc = libusb_submit_transfer(t->transfer);
            if (rc)
                break;
            t->in_flight = true;
            ++in_flight_;
        }
    }
    if (!rc)
        return 0;
    msg_err("%s: isochronous submit failed libusb=%s\n", label_.c_str(), libusb_error_name(rc));
    StopReceive();
    return rc == LIBUSB_ERROR_NO_DEVICE ? -ECANCELED : -EIO;
}

void Msi2500Device::StopReceive()
{
    if (transfers_.empty())
        return;
    {
        // 再投入の判定と投入を、停止の判定とキャンセルの間に挟ませない。
        std::unique_lock<std::mutex> lock(receive_->mutex);
        resubmit_ = false;
        for (auto &t : transfers_)
            if (t->in_flight)
                libusb_cancel_transfer(t->transfer);
        if (!receive_changed_.wait_for(lock, std::chrono::seconds(5), [this] { return in_flight_ == 0; })) {
            // libusb が使い続ける転送は、遅延した完了時にコールバックから解放する。
            // デバイスが先に破棄されても、コールバックが使うのは転送と共有状態だけ。
            msg_err("%s: %d isochronous transfers did not complete\n", label_.c_str(), in_flight_);
            leaked_ = true;
            receive_->owner = nullptr;
            for (auto &t : transfers_)
                if (t->in_flight)
                    t.release();
            transfers_.clear();
            return;
        }
    }
    msg_info("%s: usb transfers=%llu packets=%llu bad_packets=%llu empty_packets=%llu odd_packets=%llu blocks=%llu "
             "transfer_errors=%llu\n",
             label_.c_str(), static_cast<unsigned long long>(stat_transfers_.load()),
             static_cast<unsigned long long>(stat_packets_.load()),
             static_cast<unsigned long long>(stat_bad_packets_.load()),
             static_cast<unsigned long long>(stat_empty_packets_.load()),
             static_cast<unsigned long long>(stat_odd_packets_.load()), static_cast<unsigned long long>(stat_blocks_.load()),
             static_cast<unsigned long long>(stat_errors_.load()));
    transfers_.clear();
}

void LIBUSB_CALL Msi2500Device::IsoDone(libusb_transfer *transfer)
{
    auto &t = *static_cast<Transfer *>(transfer->user_data);
    const auto state = t.state;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->owner) {
        delete &t;
        return;
    }
    auto &self = *state->owner;
    bool resubmit = false;
    // 停止後の完了は、helper へ渡すデータの処理を省いて転送の回収だけを行う。
    const auto status = self.resubmit_ ? transfer->status : LIBUSB_TRANSFER_CANCELLED;
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED: {
        self.consecutive_errors_ = 0;
        ++self.stat_transfers_;
        // パケットごとに状態と長さを確かめ、失敗したパケットと空のパケットを除いて 1024 バイトのブロックだけを渡す。
        // 1 パケットの通信エラーで転送ごと捨てると欠落が 64 倍に広がるため (WinUSB 版の helper と同じ扱い)。
        // 除いたブロックは helper の OFDM がブロック番号の欠落として扱う
        t.blocks.clear();
        for (int i = 0; i < transfer->num_iso_packets; ++i) {
            const auto &d = transfer->iso_packet_desc[i];
            ++self.stat_packets_;
            if (d.status != LIBUSB_TRANSFER_COMPLETED) {
                ++self.stat_bad_packets_;
                continue;
            }
            if (!d.actual_length) {
                ++self.stat_empty_packets_;
                continue;
            }
            if (d.actual_length % Block)
                ++self.stat_odd_packets_;
            const std::size_t whole = d.actual_length / Block * Block;
            const auto *data = libusb_get_iso_packet_buffer_simple(transfer, static_cast<unsigned>(i));
            t.blocks.insert(t.blocks.end(), data, data + whole);
        }
        if (!t.blocks.empty()) {
            self.stat_blocks_ += t.blocks.size() / Block;
            self.last_data_ms_ = rtlsdr::NowMs();
            // キューが満杯なら helper が失敗を返し、Session が受信ごと起動し直す。生ブロックを捨てて続けることはしない
            self.helper_.Feed(t.blocks.data(), t.blocks.size());
        }
        resubmit = true;
        break;
    }
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        // 抜去。デバイスファイルの扱いは Device::Remove が行い、Session は抜去の後に起動し直さない
        self.usb_failed_ = true;
        break;
    default:
        ++self.stat_errors_;
        if (++self.consecutive_errors_ <= MaxConsecutiveErrors) {
            if (self.consecutive_errors_ <= 3)
                msg_warn("%s: isochronous transfer failed status=%d\n", self.label_.c_str(), transfer->status);
            resubmit = true;
        } else if (!self.usb_failed_.exchange(true))
            msg_err("%s: %u consecutive isochronous transfer errors; restarting the helper\n", self.label_.c_str(),
                    self.consecutive_errors_.load());
        break;
    }
    if (resubmit) {
        const int rc = libusb_submit_transfer(transfer);
        if (!rc)
            return;
        msg_err("%s: isochronous resubmit failed libusb=%s\n", self.label_.c_str(), libusb_error_name(rc));
        self.usb_failed_ = true;
    }
    t.in_flight = false;
    --self.in_flight_;
    self.receive_changed_.notify_all();
}
}
