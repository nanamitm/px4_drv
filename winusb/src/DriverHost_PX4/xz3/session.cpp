// SPDX-License-Identifier: GPL-2.0-only
#include "session.hpp"
#include "pump_progress.hpp"
#include "stream_records.hpp"

#include "../tc90522_cnr.hpp"
#include "msg.h"

#include <algorithm>
#include <cerrno>
#include <new>
#include <system_error>

namespace px4::xz3 {
namespace {
constexpr unsigned Body = 0xea61c, Hook = 0xe9a10, Stage = 0xc4600;
constexpr unsigned BodySize = 2236;
/*
 * 操作12の間はEP0が応答せず、カードUARTの操作を挟めない。一方、ファームウェアのUSB FIFOへの
 * 送出は約6.6 MB/秒で、110度CS (約6.5 MB/秒) との差は約2%しかなく、要求ごとの固定の負担
 * (約0.85 ms) が積み重なると送出が追いつかない。実測では384 KiBの連続で110度CSの未処理量が
 * 増え続け、約60秒でリングが溢れた。このため、直前の要求の時間から次の要求の時間がこの値を
 * 超えない最大の段階を選び、地デジでは384 KiB (約0.18秒)、110度CSでは1.5 MiB (約0.24秒) にする。
 */
constexpr auto PumpTarget = std::chrono::milliseconds(250);
/*
 * カード操作の後のCardBusyWindowの間の目標。T=1の応答待ちの間に続くカード操作の待ちを短くする。
 * 要求ごとに約21 msの制御転送があり、110度CSでは384 KiBの連続で送出が追いつかないため、
 * 110度CSで768 KiB (約0.12秒)、地デジとBSで384 KiBになる値にする。
 */
constexpr auto PumpCardTarget = std::chrono::milliseconds(125);
constexpr auto CardBusyWindow = std::chrono::seconds(1);
/*
 * SmartCardは応答を待つ間、約5 msおきにカード操作を繰り返す。その間に操作12が始まると、
 * 次のカード操作は操作12の終わりまで待つ。流量に余裕があるときは、カード操作が続く間、
 * 操作12の開始をCardIdleGapの空きが生じるまで、最長CardHoldMaxだけ遅らせる。
 * 余裕の目安はUSB FIFOへの送出 (約6.6 MB/秒) との差で、110度CS (約6.5 MB/秒) のように
 * 余裕がない流量では遅らせない。遅らせた分の未処理量は、送出との差で解消する。
 */
// 空だった受信FIFOの読み出しから、次に機器へ問い合わせるまでの間隔。
constexpr auto CardPollInterval = std::chrono::milliseconds(20);
// カード操作の間隔は、CardPollIntervalとWindowsの待機の粒度 (約16 ms) を足した長さになる。
constexpr auto CardIdleGap = std::chrono::milliseconds(50);
constexpr auto CardHoldMax = std::chrono::milliseconds(150);
constexpr double CardHoldRate = 4.0e6;
// CNDATを読む操作12の間隔（回数）。操作12ごとのI²Cの読み出しはその間リングを送らないため、
// 110度CSで約2秒、地デジで約1.5秒に1回にとどめる。
constexpr unsigned CnInterval = 8;
/*
 * XZ3は電源投入後、TSブロックが入力のTSを正しく取り込めない状態で始まることがあり、
 * 中継プログラムの導入のし直しやTSブロックのリセットでは回復せず、USBの挿し直しで正常な状態になるまで続く
 * (docs/20261121_gv_mvp_xz3_linux_cuse_design.md の「電源投入後の壊れた状態」)。
 * この状態では、DMAの書き込み位置が0のままTSが届かないか、中継プログラムが書き込み位置を越えて
 * 未書き込みの領域を送る。受信の途中から越え始めることもある。
 * 中継プログラムは書き込み位置までしか送らないため、1回の操作12の送出量は、直前の応答の時点の未処理量と
 * その操作12の間にDMAが書いた量の和を越えない。これを越えた送出と、受信開始からNoInputLimitの間に
 * 書き込み位置が進まないことを、壊れたTSを録画し続けないよう受信の失敗として扱い、挿し直しを求める。
 */
constexpr auto NoInputLimit = std::chrono::seconds(3);
using namespace std::chrono_literals;

std::uint64_t Fingerprint(const std::uint8_t* p, std::size_t size)
{
    std::uint64_t value = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i)
        value = (value ^ p[i]) * 1099511628211ull;
    return value;
}

// カードUARTの16語だけを、Relayのboot0のメモリ要求で読み書きする。
// 共通版の導入中は、FIFOの読み書きを操作14で最大31バイトずつまとめて行う。
// 失敗後は、同じ操作内の後続の転送を止める。
class RelayCardBus final : public px4::dt295::CardBus {
public:
    RelayCardBus(Relay& relay, std::function<int(unsigned)> yield)
        : relay_(relay), yield_(std::move(yield)), bulk_(relay.RuntimeInstalled()) {}

    bool Bulk() const override { return bulk_; }

    int Fifo(bool write, std::uint8_t* data, std::size_t& count) override
    {
        const std::size_t requested = count;
        count = 0;
        if (error_)
            return error_;
        if (!bulk_)
            return -EOPNOTSUPP;
        Packet reply{};
        if (write) {
            for (std::size_t at = 0; at < requested; at += CardUart::FifoChunk) {
                const auto size = (std::min)(CardUart::FifoChunk, requested - at);
                if (relay_.Exchange(14, reply, data + at, size))
                    return error_ = -EIO;
            }
            count = requested;
            return 0;
        }
        // 操作14は最大31バイトを取り出す。それより小さい受け皿では取り出した分を失う。
        if (requested < CardUart::FifoChunk)
            return error_ = -EINVAL;
        if (relay_.Exchange(14, reply))
            return error_ = -EIO;
        const auto received = Word(reply.data() + 20);
        if (received > CardUart::FifoChunk)
            return error_ = -EPROTO;
        std::copy_n(reply.data() + 24, received, data);
        count = received;
        return 0;
    }

    int Read32(std::uint32_t address, std::uint32_t& value) override
    {
        if (const int rc = Check(address))
            return rc;
        return relay_.ReadWord(address, value) ? 0 : error_ = -EIO;
    }

    int Write32(std::uint32_t address, std::uint32_t value) override
    {
        if (const int rc = Check(address))
            return rc;
        return relay_.WriteWord(address, value) ? 0 : error_ = -EIO;
    }

    int Pause(unsigned milliseconds) override
    {
        if (error_)
            return error_;
        if (const int rc = yield_(milliseconds))
            error_ = rc;
        return error_;
    }

    int Error() const { return error_; }

private:
    int Check(std::uint32_t address) const
    {
        if (error_)
            return error_;
        return address < 0xfea000 || address > 0xfea03c || (address & 3) ? -EINVAL : 0;
    }

    Relay& relay_;
    std::function<int(unsigned)> yield_;
    bool bulk_;
    int error_ = 0;
};
}

void PutWord(std::uint8_t* p, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

std::uint32_t Word(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}

bool Relay::Valid() const
{
    const auto valid_body = [](const Image& image) {
        return image.bytes && image.size && image.size <= BodySize && image.size % 4 == 0;
    };
    return valid_body(bootstrap_) && valid_body(runtime_) &&
           hook_.bytes && hook_.size == saved_hook_.size();
}

int Relay::Init()
{
    if (!Valid())
        return -EINVAL;
    port_.Begin(Clock::now() + 15s);
    std::array<std::uint8_t, 16> data{};
    constexpr std::array<std::uint8_t, 8> expected_hook{0xc4,0x4d,0,0x84,0x9b,0x80,0,0};
    if (!Read(0x82008, data.data(), 2) || data[0] || data[1] ||
        !Read(0xe9ce8, data.data(), 8) || Fingerprint(data.data(), 8) != 0xce1c939aaeb27baeull ||
        !Read(0xe91e4, data.data(), 16) || Fingerprint(data.data(), 16) != 0x7df1d1a09a7c2775ull ||
        !Read(0xe9a2c, data.data(), 4) || Word(data.data()) != 0x00f00000 ||
        !Read(0xc4810, data.data(), 4) || Word(data.data()) != 0x82000000 ||
        !Read(Hook, saved_hook_.data(), saved_hook_.size()) || saved_hook_ != expected_hook ||
        !Read(Body, saved_body_.data(), saved_body_.size()) ||
        Fingerprint(saved_body_.data(), saved_body_.size()) != 0x16073e2aa1fd67b1ull) {
        msg_err("GV-MVP/XZ3: unsupported boot RAM image or read failure\n");
        return -EIO;
    }
    int result = Install(bootstrap_);
    if (result)
        return result;
    Packet reply{};
    if ((result = Exchange(6, reply)))
        return result;
    constexpr unsigned delay[]{10, 10, 1000, 10, 500, 200};
    for (std::uint8_t step = 0; step < 6; ++step) {
        if ((result = Exchange(5, reply, &step, 1)) || (result = Delay(delay[step])))
            return result;
    }
    if ((result = Exchange(3, reply)))
        return result;
    // ブートストラップは機器初期化だけを担当する。以降の選局・TS取得は共通版を使う。
    return Install(runtime_);
}

int Relay::Install(const Image& image)
{
    port_.Begin(Clock::now() + 15s);
    if (!Unhook())
        return -EIO;
    body_changed_ = true;
    if (!WriteRange(Body, image.bytes, image.size) || !Verify(Body, image.bytes, image.size))
        return -EIO;
    // 通常ハンドラーを残したまま末尾を置き、先頭ワードでフックを有効にする。
    hook_changed_ = true;
    if (!Write(Hook + 4, hook_.bytes + 4) || !Write(Hook, hook_.bytes))
        return -EIO;
    runtime_installed_ = image.bytes == runtime_.bytes;
    return 0;
}

bool Relay::Restore()
{
    port_.Begin(Clock::now() + 15s);
    if (!Unhook())
        return false;
    if (!body_changed_)
        return true;
    const bool body = WriteRange(Body, saved_body_.data(), saved_body_.size()) &&
                      Verify(Body, saved_body_.data(), saved_body_.size());
    // 0xc4600はboot0のUSB IN応答バッファで、通常の読み出しでも上書きされる。
    // フックと本体を戻した後の照合で、ステージングは通常の応答に置き換わる。
    body_changed_ = !body;
    if (body_changed_)
        msg_err("GV-MVP/XZ3: boot RAM restoration failed\n");
    return !body_changed_;
}

int Relay::Delay(unsigned ms)
{
    port_.Begin(Clock::now() + std::chrono::milliseconds(ms) + 1s);
    return port_.Pause(ms);
}

bool Relay::ReadWord(unsigned address, std::uint32_t& value)
{
    std::array<std::uint8_t, 4> bytes{};
    if (!Read(address, bytes.data(), bytes.size()))
        return false;
    value = Word(bytes.data());
    return true;
}

bool Relay::WriteWord(unsigned address, std::uint32_t value)
{
    std::array<std::uint8_t, 4> bytes{};
    PutWord(bytes.data(), value);
    return Write(address, bytes.data());
}

int Relay::Exchange(unsigned op, Packet& reply, const std::uint8_t* data,
                    std::size_t size, unsigned addr, std::size_t rx)
{
    Packet request{};
    PutWord(request.data(), 0x585a4931);
    PutWord(request.data() + 4, ++sequence_);
    PutWord(request.data() + 8, op);
    PutWord(request.data() + 12, static_cast<std::uint32_t>(size));
    PutWord(request.data() + 16, addr);
    PutWord(request.data() + 20, static_cast<std::uint32_t>(rx));
    if (size)
        std::copy_n(data, size, request.data() + 24);
    port_.Begin(Clock::now() + (op == 12 ? 6s : 2s));
    // 最後にマジックを書き、全フィールドが揃ってから読み出しで起動する。
    // relayはヘッダー24バイトとsizeで指定された送信データだけを参照する。
    // boot0のメモリ要求は1語ごとに約3 msかかる。ステージングに直前の応答が残っている間は、
    // 要求と同じ値の語を書き直さない。先頭のマジックは、boot0の書き込みの受け取りで
    // 上書きされるため、毎回最後に書く。操作12では7回の転送が6回、操作14の読み出しでは4回になる。
    const int last = static_cast<int>(20 + ((size + 3) & ~std::size_t{3}));
    for (int at = last; at >= 0; at -= 4) {
        if (at && staged_valid_ &&
            std::equal(request.data() + at, request.data() + at + 4, staged_.data() + at))
            continue;
        if (!Write(Stage + at, request.data() + at))
            return -EIO;
    }
    const bool read = Read(Stage, reply.data(), reply.size());
    // 応答の読み出しの後、ステージングには応答がそのまま残る。
    staged_ = reply;
    staged_valid_ = read;
    if (!read || Word(reply.data()) != 0x585a4931 ||
        Word(reply.data() + 4) != sequence_ || Word(reply.data() + 8) != (0x80000000u | op))
        return -EIO;
    const auto status = Word(reply.data() + 12);
    if (op == 12) {
        if (ValidPumpReply(status, static_cast<unsigned>(rx), Word(reply.data() + 52),
                           Word(reply.data() + 44), Word(reply.data() + 48)))
            return 0;
        msg_err("GV-MVP/XZ3: TS pump status=%u peak=%u cursor=%u producer=%u sent=%u\n",
                status, Word(reply.data() + 40), Word(reply.data() + 44),
                Word(reply.data() + 48), Word(reply.data() + 52));
    } else if (!status) {
        return 0;
    } else {
        msg_err("GV-MVP/XZ3: relay op=%u status=%u\n", op, status);
    }
    return -EIO;
}

// ステージング (0xc4600) はboot0のUSB IN応答バッファで、ほかの番地の読み書きでも
// 上書きされる可能性がある。ステージング以外への要求では、保持した応答の写しを無効にする。
void Relay::Touch(unsigned address, std::size_t size)
{
    if (address < Stage || address + size > Stage + staged_.size())
        staged_valid_ = false;
}

bool Relay::Read(unsigned address, std::uint8_t* data, std::size_t size)
{
    Touch(address, size);
    // 要求が失敗した場合も、ステージングの内容は分からなくなる。
    if (address == Stage)
        staged_valid_ = false;
    for (std::size_t at = 0; at < size; at += 64) {
        const unsigned offset = address + static_cast<unsigned>(at);
        const px4::torne::MemoryRequest request{0xc0, 0xbc,
            static_cast<std::uint16_t>((offset >> 8) & 0xff00), static_cast<std::uint16_t>(offset)};
        const auto count = (std::min)(size - at, std::size_t{64});
        std::size_t transferred = 0;
        if (port_.Transfer(request, data + at, count, transferred) || transferred != count)
            return false;
    }
    return true;
}

bool Relay::Write(unsigned address, const std::uint8_t* data)
{
    Touch(address, 4);
    std::array<std::uint8_t, 4> word{data[0], data[1], data[2], data[3]};
    const px4::torne::MemoryRequest request{0x40, 0xbc,
        static_cast<std::uint16_t>((address >> 8) & 0xff00), static_cast<std::uint16_t>(address)};
    std::size_t transferred = 0;
    const bool ok = !port_.Transfer(request, word.data(), word.size(), transferred) &&
                    transferred == word.size();
    // ステージングへの書き込みは写しにも反映する。失敗した場合は内容が分からなくなる。
    if (address >= Stage && address + 4 <= Stage + staged_.size()) {
        if (ok)
            std::copy(word.begin(), word.end(), staged_.begin() + (address - Stage));
        else
            staged_valid_ = false;
    }
    return ok;
}

bool Relay::WriteRange(unsigned address, const std::uint8_t* data, std::size_t size)
{
    for (std::size_t at = 0; at < size; at += 4)
        if (!Write(address + static_cast<unsigned>(at), data + at))
            return false;
    return true;
}

bool Relay::Verify(unsigned address, const std::uint8_t* data, std::size_t size)
{
    std::array<std::uint8_t, BodySize> actual{};
    if (!Read(address, actual.data(), size)) {
        msg_err("GV-MVP/XZ3: RAM read failed address=%06x\n", address);
        return false;
    }
    const auto mismatch = std::mismatch(data, data + size, actual.data());
    if (mismatch.first == data + size)
        return true;
    msg_err("GV-MVP/XZ3: RAM mismatch address=%06x offset=%zu expected=%02x actual=%02x\n",
            address, static_cast<std::size_t>(mismatch.first - data), *mismatch.first, *mismatch.second);
    return false;
}

bool Relay::Unhook()
{
    runtime_installed_ = false;
    if (!hook_changed_)
        return true;
    if (!Write(Hook, saved_hook_.data()) || !Write(Hook + 4, saved_hook_.data() + 4) ||
        !Verify(Hook, saved_hook_.data(), saved_hook_.size()))
        return false;
    hook_changed_ = false;
    return true;
}

/*
 * XZ3のTSリングは6,291,456バイトで、DMAは操作12の外側でも書き込みを続ける。
 * 衛星の約6.7 MB/秒では約0.94秒で満杯になるため、要求間の空き時間と
 * ファームウェアが返す最大未処理量を記録し、ドロップの時刻と照合できるようにする。
 * 溢れた場合は未処理量が周回後の値として小さく見える可能性があるため、時間も併記する。
 */
class Session::RingStats {
public:
    explicit RingStats(bool satellite) : satellite_(satellite), period_start_(Clock::now()) {}
    RingStats(const RingStats&) = delete;
    ~RingStats()
    {
        if (total_chunks_)
            msg_info("GV-MVP/XZ3: ring end satellite=%d chunks=%llu peak_bytes=%u high_chunks=%llu max_producer=%u max_host_gap_us=%lld max_op_us=%lld\n",
                     satellite_ ? 1 : 0, total_chunks_, total_peak_, total_high_, total_producer_,
                     total_gap_us_, total_op_us_);
    }

    void Record(Clock::time_point begin, Clock::time_point end, std::uint32_t peak,
                std::uint32_t cursor, std::uint32_t producer, unsigned stage)
    {
        const auto op_us = Micro(end - begin);
        min_stage_ = (std::min)(min_stage_, stage);
        max_stage_ = (std::max)(max_stage_, stage);
        // 前回の操作12の完了から今回の開始までは、ファームウェアがリングを送出しない。
        const auto gap_us = has_last_ ? Micro(begin - last_end_) : 0;
        has_last_ = true;
        last_end_ = end;
        ++chunks_;
        ++total_chunks_;
        peak_ = (std::max)(peak_, peak);
        total_peak_ = (std::max)(total_peak_, peak);
        // 書き込み位置がリング末尾の近くまで進めば、DMAは設定したサイズで周回している。
        producer_ = (std::max)(producer_, producer);
        total_producer_ = (std::max)(total_producer_, producer);
        gap_us_ = (std::max)(gap_us_, gap_us);
        total_gap_us_ = (std::max)(total_gap_us_, gap_us);
        op_us_ = (std::max)(op_us_, op_us);
        total_op_us_ = (std::max)(total_op_us_, op_us);
        if (peak >= HighWater) {
            ++high_;
            ++total_high_;
            msg_warn("GV-MVP/XZ3: ring high chunk=%llu peak_bytes=%u ring_bytes=%u cursor=%u producer=%u host_gap_us=%lld op_us=%lld\n",
                     total_chunks_, peak, RingBytes, cursor, producer, gap_us, op_us);
        }
        if (end - period_start_ >= std::chrono::seconds(10)) {
            // stageは操作12の送出量の段階 (0=3 MiB、3=384 KiB) の範囲。
            msg_info("GV-MVP/XZ3: ring stats chunks=%llu peak_bytes=%u ring_bytes=%u high_chunks=%llu max_producer=%u max_host_gap_us=%lld max_op_us=%lld stage=%u-%u\n",
                     chunks_, peak_, RingBytes, high_, producer_, gap_us_, op_us_, min_stage_, max_stage_);
            period_start_ = end;
            chunks_ = high_ = 0;
            peak_ = producer_ = 0;
            gap_us_ = op_us_ = 0;
            min_stage_ = PumpStages;
            max_stage_ = 0;
        }
    }

private:
    // ts_probe.cのRING_SIZEと同じ値。
    // 3/4を超えた要求を個別に記録する。
    static constexpr std::uint32_t HighWater = RingBytes / 4 * 3;

    static long long Micro(Clock::duration value)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
    }

    bool satellite_;
    bool has_last_ = false;
    unsigned min_stage_ = PumpStages, max_stage_ = 0;
    Clock::time_point period_start_, last_end_{};
    unsigned long long chunks_ = 0, high_ = 0, total_chunks_ = 0, total_high_ = 0;
    std::uint32_t peak_ = 0, total_peak_ = 0, producer_ = 0, total_producer_ = 0;
    long long gap_us_ = 0, op_us_ = 0, total_gap_us_ = 0, total_op_us_ = 0;
};

Session::Session(SessionPort& port, const Images& images)
    : port_(port), relay_(port, images), frontend_(*this) {}

Session::~Session()
{
    stop_ = true;
    if (worker_.joinable())
        worker_.join();
}

int Session::Tune(bool satellite, std::uint32_t frequency_khz, std::uint32_t tsid, bool& init_failed,
                  bool relative)
{
    init_failed = false;
    // 初期化と選局の間はカード操作を待たせる。どちらもI²Cと待機を交互に行う連続した手順で、
    // 途中でRelayを譲るとフックの導入途中や選局途中の状態でカードUARTへ要求が入る。
    std::unique_lock<std::mutex> control(control_mutex_);
    int result = 0;
    if (!initialized_) {
        if ((result = relay_.Init()) || (result = frontend_.Initialize())) {
            init_failed = true;
            return result;
        }
        initialized_ = true;
    }
    result = satellite && relative ? frontend_.TuneRelative(frequency_khz, tsid)
                                   : frontend_.Tune(satellite, frequency_khz, tsid);
    control.unlock();
    // 地デジはロック直後に訂正前のTSを出すことがある。
    // ファームウェア交換による待ちがなくなったため、DMAを始める前に安定を待つ。
    // この待機は機器を操作しないため、制御ロックを手放してカード操作を通す。
    if (!result && !satellite)
        result = port_.WaitRemoved(350);
    return result;
}

int Session::StartStream(bool satellite, StreamSink& sink, bool& stop_failed)
{
    stop_failed = false;
    if (port_.FlushStream())
        return -EIO;
    Packet reply{};
    int result = 0;
    {
        std::lock_guard<std::mutex> control(control_mutex_);
        // 最初の転送を待つと、スキャン側の最初の統計取得に間に合わない場合がある。
        // DMAを始める前に新しいチャンネルの値を1回読み、以後はTS転送応答で更新する。
        cndat_ = frontend_.ReadCndat();
        cnr_ = px4::tc90522::CnrMilliDb(satellite, cndat_);
        // 開始の応答を失った場合にも、停止を試してからRAMを復元する。
        stream_started_ = true;
        result = relay_.Exchange(11, reply);
    }
    if (result) {
        stop_failed = StopStream() != 0;
        return result;
    }
    stop_ = false;
    sink.Started();
    try {
        worker_ = std::thread([this, satellite, &sink] {
            int result = 0;
            try { result = ReadLoop(satellite, sink); }
            catch (const std::bad_alloc&) { result = -ENOMEM; }
            // 同じ利用者のセッションから再選局できるよう、接続は維持する。
            if (result && !stop_ && port_.Available()) {
                worker_error_ = result;
                msg_err("GV-MVP/XZ3: TS reader failed ret=%d\n", result);
                sink.Failed(result);
            }
        });
    } catch (const std::system_error&) {
        stop_failed = StopStream() != 0;
        return -EIO;
    }
    return 0;
}

int Session::StopStream()
{
    stop_ = true;
    if (worker_.joinable())
        worker_.join();
    cnr_ = 0;
    cndat_ = 0;
    if (!stream_started_ || !port_.Available())
        return 0;
    Packet reply{};
    int result = 0;
    {
        std::lock_guard<std::mutex> control(control_mutex_);
        result = relay_.Exchange(13, reply);
    }
    if (result || Word(reply.data() + 36))
        return result ? result : -EIO;
    stream_started_ = false;
    if (const int error = port_.FlushStream())
        msg_warn("GV-MVP/XZ3: EP81 flush after stop failed error=%d\n", error);
    return 0;
}

bool Session::Restore()
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (!relay_.Restore())
        return false;
    initialized_ = false;
    return true;
}

std::uint32_t Session::ReadCndat()
{
    if (Running())
        return cndat_;
    std::lock_guard<std::mutex> control(control_mutex_);
    return frontend_.ReadCndat();
}

int Session::Card(const std::function<int(CardUart&)>& operation)
{
    if (!port_.Available())
        return -ENODEV;
    const auto mark_busy = [this] {
        card_until_ = (Clock::now() + CardBusyWindow).time_since_epoch().count();
    };
    mark_busy();
    ++card_waiters_;
    std::unique_lock<std::mutex> control(control_mutex_);
    --card_waiters_;
    if (!port_.Available())
        return -ENODEV;
    relay_.Begin(Clock::now() + 2s);
    RelayCardBus bus(relay_, [this, &control](unsigned ms) {
        control.unlock();
        const int rc = port_.WaitRemoved(ms);
        ++card_waiters_;
        control.lock();
        --card_waiters_;
        relay_.Begin(Clock::now() + 2s);
        return rc;
    });
    CardUart uart(bus);
    const int rc = operation(uart);
    mark_busy();
    card_last_ = Clock::now().time_since_epoch().count();
    if (rc && bus.Error())
        msg_err("GV-MVP/XZ3: card transfer failed errno=%d\n", bus.Error());
    return rc;
}

int Session::I2c(std::uint8_t address, const std::uint8_t* tx, std::size_t size,
                 std::uint8_t* rx, std::size_t count)
{
    if (size > 40 || count > 40 - size)
        return -EINVAL;
    Packet reply{};
    const int result = relay_.Exchange(7, reply, tx, size, address, count);
    if (result)
        return result;
    if (Word(reply.data() + 16) != 1 || Word(reply.data() + 20) != count)
        return -EIO;
    if (count)
        std::copy_n(reply.data() + 24, count, rx);
    return 0;
}

int Session::ReadLoop(bool satellite, StreamSink& sink)
{
    const std::unique_ptr<BulkReader> reads[]{port_.CreateReader(), port_.CreateReader()};
    if (!reads[0] || !reads[1])
        return -ENOMEM;
    // 流量を測る前は最小の段階から始める。読み出しは2本を先に発行するため、
    // 選んだ段階は2回後の操作12から使われる。
    unsigned stage = PumpStages - 1;
    // 流量を測るまでは余裕がないものとして扱い、操作12を遅らせない。
    double rate = CardHoldRate;
    if (!reads[0]->Submit(stage) || !reads[1]->Submit(stage))
        return -EIO;
    Packet reply{};
    const unsigned cn_address = satellite ? 0x19 : 0x18;
    RingStats stats(satellite);
    PumpProgress progress;
    bool short_pump = false;
    // DMA の書き込み位置が一度でも進んだか。操作11はリングの読む位置と書き込み位置を 0 から始める。
    bool input_seen = false;
    std::uint32_t last_cursor = 0, last_producer = 0;
    const auto started = Clock::now();
    for (unsigned head = 0, count = 0; port_.Available() && !stop_; ++count) {
        // 待っているカード操作があれば、次の操作12の前に先に通す。
        while (card_waiters_.load() && port_.Available() && !stop_)
            std::this_thread::yield();
        if (rate < CardHoldRate)
            HoldForCard();
        const bool read_cn = count % CnInterval == 0;
        const auto request = progress.Stage(reads[head]->Stage(),
                                            PumpSize(reads[0]->Stage()) + PumpSize(reads[1]->Stage()));
        const auto begin = Clock::now();
        {
            std::lock_guard<std::mutex> control(control_mutex_);
            if (stop_ || !port_.Available())
                break;
            if (relay_.Exchange(12, reply, nullptr, 0, read_cn ? cn_address : 0, request))
                return -EIO;
        }
        const auto sent = Word(reply.data() + 52);
        // 電源投入後の壊れた状態は、この操作12で送られたデータを渡す前に検出する。
        const auto cursor = Word(reply.data() + 44), producer = Word(reply.data() + 48);
        if (cursor < RingBytes && producer < RingBytes) {
            const auto ring = [](std::uint32_t from, std::uint32_t to) {
                return to >= from ? to - from : RingBytes - from + to;
            };
            const auto available = static_cast<std::uint64_t>(ring(last_cursor, last_producer)) +
                                   ring(last_producer, producer);
            if (sent > available) {
                msg_err("GV-MVP/XZ3: TS capture is in a bad state after power-on (op12=%u sent=%u available=%llu "
                        "cursor=%u producer=%u); reconnect USB\n",
                        count, sent, static_cast<unsigned long long>(available), cursor, producer);
                return -EIO;
            }
            last_cursor = cursor;
            last_producer = producer;
            if (producer)
                input_seen = true;
        }
        if (!input_seen && Clock::now() - started >= NoInputLimit) {
            msg_err("GV-MVP/XZ3: no TS input %lld ms after the stream started (bad state after power-on); "
                    "reconnect USB\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(NoInputLimit).count()));
            return -EIO;
        }
        const bool partial = sent < PumpSize(request);
        if (partial && !short_pump)
            msg_warn("GV-MVP/XZ3: TS pump sent %u/%u bytes; waiting for more input\n", sent, PumpSize(request));
        else if (!partial && short_pump)
            msg_info("GV-MVP/XZ3: TS pump returned to full transfers\n");
        short_pump = partial;

        // 部分送出はUSB上では短いパケットにならない。読み出しをキャンセルせず、次の
        // 操作12で続きを送る。満杯になった読み出しだけを順に回収し、192バイト境界を維持する。
        progress.Sent(sent);
        while (!stop_ && progress.Complete(PumpSize(reads[head]->Stage()))) {
            auto& read = *reads[head];
            if (!read.Collect())
                return -EIO;
            progress.Collected(PumpSize(read.Stage()));
            if (stop_)
                break;
            const auto next = partial ? read.Stage() : NextStage(Clock::now() - begin, request);
            Deliver(read.Data(), PumpSize(read.Stage()), sink);
            if (!read.Submit(next))
                return -EIO;
            head ^= 1;
        }
        const auto end = Clock::now();
        stats.Record(begin, end, Word(reply.data() + 40), Word(reply.data() + 44), Word(reply.data() + 48),
                     request);
        if (partial) {
            rate = CardHoldRate;
            cnr_ = 0;
            cndat_ = 0;
        } else {
            // 未処理量がない状態の操作12の時間は、送出量を流量で割った値に近い。
            if (const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count())
                rate = static_cast<double>(sent) * 1e6 / static_cast<double>(us);
            if (read_cn) {
                cndat_ = Word(reply.data() + 16);
                cnr_ = px4::tc90522::CnrMilliDb(satellite, cndat_);
            }
        }
    }
    return 0;
}

// 直前の操作12の時間から、次の要求が目標の時間を超えない最大の段階を選ぶ。
// カード操作の後のCardBusyWindowの間は、カード操作の待ちを短くするため目標をPumpCardTargetにする。
// 段階を流量に関係なく最小にすると、要求ごとの固定の負担 (約21 msの制御転送) で
// 110度CSの送出が追いつかなくなるため、時間で選ぶ。
unsigned Session::NextStage(Clock::duration elapsed, unsigned stage) const
{
    const bool card = Clock::now().time_since_epoch().count() < card_until_.load();
    const auto target = std::chrono::duration_cast<std::chrono::microseconds>(
        card ? PumpCardTarget : PumpTarget).count();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    for (unsigned next = 0; next < PumpStages - 1; ++next) {
        // 送出量は段階ごとに半分になるため、時間も比例すると見積もる。
        const auto estimate = next <= stage ? us << (stage - next) : us >> (next - stage);
        if (estimate <= target)
            return next;
    }
    return PumpStages - 1;
}

// カード操作が続く間、次の操作12の開始を遅らせる。CardIdleGapの間カード操作がないか、
// CardHoldMaxを超えたら戻る。
void Session::HoldForCard()
{
    const auto begin = Clock::now();
    while (port_.Available() && !stop_ && Clock::now() - begin < CardHoldMax) {
        const auto now = Clock::now();
        if (now.time_since_epoch().count() >= card_until_.load())
            return;
        const auto last = Clock::time_point(Clock::duration(card_last_.load()));
        if (!card_waiters_.load() && now - last >= CardIdleGap)
            return;
        if (card_waiters_.load())
            std::this_thread::yield();
        else
            port_.Nap();
    }
}

void Session::Deliver(const std::uint8_t* bytes, std::size_t length, StreamSink& sink)
{
    const auto skipped = DeliverRecords(bytes, length, [&](const std::uint8_t* packet) { sink.PutPacket(packet); });
    if (skipped)
        msg_warn("GV-MVP/XZ3: discarded %zu TS records with invalid sync\n", skipped);
    sink.Flush();
}

void CardReader::Attach(Session& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = &session;
}

void CardReader::Detach()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    ResetBuffer();
    session_ = nullptr;
}

void CardReader::ResetBuffer()
{
    response_.Reset();
    polled_empty_ = false;
}

int CardReader::Open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || !available_)
        return -ENODEV;
    if (open_)
        return -EBUSY;
    open_ = true;
    ResetBuffer();
    return 0;
}

void CardReader::Close(bool power_down)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_)
        return;
    open_ = false;
    ResetBuffer();
    // CMDVCCNをHighにしてカードへの給電を止める。
    if (power_down && available_ && session_ && session_->Card([](CardUart& uart) { return uart.PowerDown(); }))
        msg_warn("GV-MVP/XZ3: card power down failed\n");
}

void CardReader::Discard()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ResetBuffer();
}

int CardReader::PowerDown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    return session_->Card([](CardUart& uart) { return uart.PowerDown(); });
}

int CardReader::Detect(bool& detected)
{
    std::lock_guard<std::mutex> lock(mutex_);
    detected = false;
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    const int rc = session_->Card([&detected](CardUart& uart) { return uart.Present(detected); });
    // 抜去を検出したら、受信途中のATRやブロックを次のカードへ持ち越さない。
    if (rc || !detected)
        ResetBuffer();
    return rc;
}

int CardReader::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    ResetBuffer();
    return session_->Card([](CardUart& uart) { return uart.Reset(); });
}

int CardReader::SetBaudrate(std::uint32_t baudrate)
{
    // ATRで指定されるB-CASの19200bpsとA-CASの38400bpsを受け付ける。
    if (baudrate != 19200 && baudrate != 38400)
        return -ENOTSUP;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    return session_->Card([baudrate](CardUart& uart) { return uart.SetBaudrate(baudrate); });
}

int CardReader::IsDataReady(bool& ready) try
{
    std::lock_guard<std::mutex> lock(mutex_);
    ready = false;
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    if (response_.Ready()) {
        ready = true;
        return 0;
    }
    /*
     * SmartCardは応答を待つ間、約5 msおきにここを呼ぶ。XZ3の受信FIFOの読み出しは1回で
     * 約9〜21 msの制御転送になり、110度CSでは操作12の送出の余裕を削る。カードが応答を
     * 計算している間の空の読み出しを減らすため、空だった読み出しからCardPollIntervalの間は
     * 機器へ問い合わせない。受信が続いている間は間隔を空けない。
     */
    const auto now = Clock::now();
    if (polled_empty_ && now - polled_ < CardPollInterval)
        return 0;
    // 共通版の導入中は操作14の1往復で最大31バイトを読む。導入前の直接アクセスは
    // 1バイトごとに約3 msかかるが、TSを受信していないため操作12を待たせない。
    std::uint8_t bytes[CardUart::FifoChunk]{};
    std::size_t count = 0;
    const int rc = session_->Card([&](CardUart& uart) { return uart.ReadBytes(bytes, sizeof(bytes), count); });
    if (rc)
        return rc;
    polled_ = now;
    polled_empty_ = count == 0;
    if (const int appended = response_.Append(bytes, count))
        return appended;
    ready = response_.Ready();
    return 0;
} catch (...) {
    return -ENOMEM;
}

int CardReader::Read(std::uint8_t* buf, std::uint8_t& len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !available_)
        return -ENODEV;
    if (!buf || !len)
        return -EINVAL;
    len = static_cast<std::uint8_t>(response_.Read(buf, len));
    return 0;
}

int CardReader::Write(const std::uint8_t* buf, std::uint8_t len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || !available_ || !session_)
        return -ENODEV;
    if (!buf || !len)
        return -EINVAL;
    // FIFOへの積み込みを区切り、各回の制御ロックの保持を短くする。送信開始は最後に1回だけ行う。
    // 共通版の導入中は操作14の1往復で31バイトを積む。導入前は1バイトごとに1回の制御転送になる。
    constexpr std::size_t Chunk = CardUart::FifoChunk;
    int rc = 0;
    for (std::size_t at = 0; !rc && at < len; at += Chunk) {
        const auto size = (std::min)(Chunk, std::size_t(len) - at);
        rc = session_->Card([&](CardUart& uart) {
            int result = at ? 0 : uart.BeginTransmit();
            if (!result)
                result = uart.Queue(buf + at, size);
            return result;
        });
    }
    if (!rc)
        rc = session_->Card([len](CardUart& uart) { return uart.Transmit(len); });
    return rc;
}

}
