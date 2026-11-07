// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "runtime.h"
#include <array>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" {
#include "mb86H/stream_input.h"
}

namespace px4::dt295 {
class SourceFirmware {
public:
    int Load(const std::wstring &path, const std::wstring &digest);
    int Start(dt295_io io);
    int Tune(bool satellite, uint32_t khz, uint32_t tsid);
    int Locked(int &locked);
    int Capture(bool enable);
    int Service();
    int Close();
    int Feed(const uint8_t *data, size_t size, mb86H_stream_packet_sink packet, void *ctx);
    void InputStats(uint64_t &discarded, uint64_t &desyncs) const;

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
    Bytes Mailbox();
    Bytes Exchange(unsigned op, uint32_t cursor = 0, uint32_t budget = 0);
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
    int Failure(const char *stage, int code) const;

    dt295_io io_{};
    Bytes auxiliary_, resident_, adapter_, hook_, init_, boot_, aux_ram_, ring_;
    std::vector<Preimage> direct_saved_, ts_saved_;
    mb86H_stream_input input_{};
    uint32_t clock_ = 0, sequence_ = 0, cursor_ = 0, tsid_ = 0;
    bool aux_changed_ = false, aux_started_ = false, clock_changed_ = false, ts_changed_ = false;
    bool ring_running_ = false, ready_ = false, satellite_ = false, tuned_ = false;
};
}
