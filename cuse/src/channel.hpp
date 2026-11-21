// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cerrno>
#include <cstdint>
#include <sys/ioctl.h>
#include "ptx_ioctl.h"

namespace px4::cuse {
// PTX_SET_CHANNEL の freq_no と slot を、PIX-DT295 の選局の引数へ換算する。
// 換算式は driver/ptx_chrdev.c の ptx_chrdev_set_channel() と同じ。
struct Tune {
    bool satellite = false;
    // 衛星で slot が相対 TS 番号 (0〜7) のとき true。SourceFirmware::Tune() が TMCC から TSID を求める。
    bool relative = false;
    std::uint32_t khz = 0, tsid = 0;
};

// 衛星の slot が 8 未満なら相対 TS 番号、それ以上なら TSID として扱う (px4_chrdev_set_stream_id_s() と同じ)。
constexpr std::uint32_t RelativeStreamCount = 8;

inline int ChannelToTune(std::uint32_t system, const ptx_freq &freq, Tune &tune)
{
    tune = Tune{};
    if (system == PTX_UNSPECIFIED_SYSTEM)
        system = (freq.freq_no >= 0 && freq.freq_no < 24) ? PTX_ISDB_S_SYSTEM : PTX_ISDB_T_SYSTEM;
    if (system == PTX_ISDB_S_SYSTEM) {
        if (freq.freq_no < 0 || freq.freq_no >= 24 || freq.slot < 0)
            return -EINVAL;
        tune.satellite = true;
        tune.khz = freq.freq_no < 12 ? 1049480 + 38360 * freq.freq_no : 1613000 + 40000 * (freq.freq_no - 12);
        tune.relative = static_cast<std::uint32_t>(freq.slot) < RelativeStreamCount;
        tune.tsid = static_cast<std::uint32_t>(freq.slot);
        return 0;
    }
    if (system != PTX_ISDB_T_SYSTEM)
        return -EINVAL;
    if ((freq.freq_no >= 3 && freq.freq_no <= 12) || (freq.freq_no >= 22 && freq.freq_no <= 62))
        tune.khz = 93143 + freq.freq_no * 6000 + freq.slot + (freq.freq_no == 12 ? 2000 : 0);
    else if (freq.freq_no >= 63 && freq.freq_no <= 112)
        tune.khz = 95143 + freq.freq_no * 6000 + freq.slot;
    else
        return -EINVAL;
    return 0;
}
}
