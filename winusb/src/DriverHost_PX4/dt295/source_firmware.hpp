// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "runtime.h"
#include <array>
#include <initializer_list>
#include <vector>

extern "C" {
#include "mb86H/stream_input.h"
}

namespace px4::dt295 {
class SourceFirmware {
public:
    explicit SourceFirmware(bool direct = false) : direct_(direct) {}
    bool Direct() const { return direct_; }
    int LoadEmbedded();
    int Start(dt295_io io);
    int Tune(bool satellite, uint32_t khz, uint32_t tsid);
    int Locked(int &locked);
    int ReadCn(bool &satellite, uint32_t &raw);
    int Capture(bool enable);
    int Service();
    // 直前のSERVICEが送ったブロック数と、送出後の未送信量 (バイト)。成功したSERVICEの後だけ更新する。
    uint32_t LastSentBlocks() const { return last_sent_; }
    uint32_t LastPending() const { return last_pending_; }
    int Close();
    int Feed(const uint8_t *data, size_t size, mb86H_stream_packet_sink packet, void *ctx);
    void InputStats(uint64_t &discarded, uint64_t &desyncs) const;
    // PIF1を導入済みで、操作5 (カードFIFOの一括転送) を使えるときにtrue。
    bool Ready() const { return ready_; }
    // 内蔵カードUARTのFIFOを最大CardChunkバイト移す。readでは溜まった数までを読み、countへ返す。
    static constexpr size_t CardChunk = 32;
    int CardFifo(bool write, uint8_t *data, size_t &count);

private:
    using Bytes = std::vector<uint8_t>;
    struct Preimage { uint32_t address; Bytes bytes; };
    void Control(uint32_t address, uint8_t *bytes, size_t count, bool read, uint8_t selector = 0);
    Bytes Read(uint32_t address, size_t count);
    void Write(uint32_t address, const Bytes &bytes, size_t chunk, bool verify);
    Bytes MappedRead(uint32_t address, size_t count);
    void MappedWrite(uint32_t address, const Bytes &bytes);
    uint32_t Get(uint32_t address, unsigned width = 4);
    void Put(uint32_t address, uint32_t value, unsigned width = 4);
    void Checked(uint32_t address, uint32_t value, unsigned width = 4);
    void Save(uint32_t address, size_t count);
    void Delay(unsigned ms);
    Bytes Exchange(unsigned op, const Bytes &payload = {}, uint32_t argument = 0);
    Bytes DirectI2c(const Bytes &ops, unsigned count, unsigned read);
    void InitializeFrontend();
    void InstallPif();
    void EnableTsSystem();
    void PrepareDirectI2c();
    uint32_t DirectReg(uint32_t address, unsigned width = 4);
    void SetDirectReg(uint32_t address, uint32_t value, unsigned width = 4);
    void SetEndpoint(uint32_t group, uint32_t address, uint32_t value);
    void StartEndpoint();
    void StopEndpoint();
    Bytes I2c(uint8_t address, std::initializer_list<uint8_t> tx, unsigned rx = 0);
    Bytes I2c(uint8_t address, const uint8_t *tx, size_t size, unsigned rx);
    void Reg(uint8_t address, uint8_t reg, uint8_t value);
    void Rf(uint8_t address, uint8_t reg, uint8_t value);
    void Field(uint8_t reg, uint8_t mask, uint8_t value);
    void TerrestrialSleep();
    void SatelliteSleep();
    void TerrestrialTune(uint32_t khz);
    void SatelliteTune(uint32_t khz, uint16_t tsid);
    bool IsLocked();
    void StopRing();
    void Restore();
    void RecordRing(uint32_t producer);
    int Failure(const char *stage, int code) const;

    const bool direct_;
    dt295_io io_{};
    Bytes resident_, adapter_, hook_, boot_;
    std::vector<Preimage> direct_saved_, ts_saved_, i2c_saved_, board_saved_;
    mb86H_stream_input input_{};
    uint32_t clock_ = 0, sequence_ = 0, cursor_ = 0, tsid_ = 0;
    uint32_t stream_phase_ = 0xffffffff, endpoint_enable_ = 0, endpoint_buffer_ = 0, endpoint_mode_ = 0;
    bool endpoint_saved_ = false;
    uint64_t ring_services_ = 0, ring_high_ = 0;
    uint32_t ring_peak_ = 0, last_sent_ = 0, last_pending_ = 0;
    bool clock_changed_ = false, ts_changed_ = false;
    bool ring_running_ = false, ready_ = false, satellite_ = false, tuned_ = false;
};
}
