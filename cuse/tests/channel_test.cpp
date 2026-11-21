// SPDX-License-Identifier: GPL-2.0-only
// PTX_SET_CHANNEL の換算 (cuse/src/channel.hpp) が driver/ptx_chrdev.c と同じ結果になることを確認する。
#include "channel.hpp"
#include <cmath>
#include <cstdio>

using px4::cuse::ChannelToTune;
using px4::cuse::Tune;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

Tune Convert(std::uint32_t system, int freq_no, int slot, int &rc)
{
    Tune tune;
    rc = ChannelToTune(system, ptx_freq{freq_no, slot}, tune);
    return tune;
}
}

int main()
{
    int rc = 0;
    // UHF 13ch (freq_no 63) は 473.143 MHz。
    auto t = Convert(PTX_UNSPECIFIED_SYSTEM, 63, 0, rc);
    CHECK(!rc && !t.satellite && t.khz == 473143 && t.tsid == 0);
    // UHF 62ch (freq_no 112)
    t = Convert(PTX_ISDB_T_SYSTEM, 112, 0, rc);
    CHECK(!rc && t.khz == 767143);
    // CATV C22ch (freq_no 12) は 2 MHz 高い。地デジを明示したときだけ 3〜23 を CATV として扱う。
    t = Convert(PTX_ISDB_T_SYSTEM, 12, 0, rc);
    CHECK(!rc && t.khz == 93143 + 12 * 6000 + 2000);
    t = Convert(PTX_ISDB_T_SYSTEM, 13, 0, rc);
    CHECK(rc == -EINVAL);
    // slot は地デジでは周波数の補正値 (kHz)
    t = Convert(PTX_ISDB_T_SYSTEM, 63, -100, rc);
    CHECK(!rc && t.khz == 473043);
    // 未指定で 0〜23 は衛星。BS15 (freq_no 7) の相対 TS 番号 1。
    t = Convert(PTX_UNSPECIFIED_SYSTEM, 7, 1, rc);
    CHECK(!rc && t.satellite && t.relative && t.khz == 1049480 + 38360 * 7 && t.tsid == 1);
    // 8 以上は TSID そのもの
    t = Convert(PTX_ISDB_S_SYSTEM, 7, 0x40f1, rc);
    CHECK(!rc && t.satellite && !t.relative && t.tsid == 0x40f1);
    // ND02 (freq_no 12) は 1613 MHz
    t = Convert(PTX_ISDB_S_SYSTEM, 12, 0, rc);
    CHECK(!rc && t.khz == 1613000 && t.relative);
    // 範囲外
    Convert(PTX_ISDB_S_SYSTEM, 24, 0, rc);
    CHECK(rc == -EINVAL);
    Convert(PTX_ISDB_S_SYSTEM, 0, -1, rc);
    CHECK(rc == -EINVAL);
    Convert(PTX_UNSPECIFIED_SYSTEM, 113, 0, rc);
    CHECK(rc == -EINVAL);
    Convert(PTX_ISDB_C_SYSTEM, 63, 0, rc);
    CHECK(rc == -EINVAL);

    // dB から求めた CNDAT を recpt1 の式へ戻すと、元の dB に 0.01 dB 以内で戻る。
    for (double db = 3.2; db < 47.5; db += 0.37)
        CHECK(std::fabs(px4::cuse::CndatToDb(px4::cuse::DbToCndat(db)) - db) < 0.01);
    // 範囲外は端の値に揃える。
    CHECK(px4::cuse::DbToCndat(-5) == 5505024);
    CHECK(px4::cuse::DbToCndat(0) == 5505024);
    CHECK(px4::cuse::DbToCndat(100) == px4::cuse::DbToCndat(48));
    // 25.5 dB は CNDAT の数千台 (TC90522 の地デジで 25 dB 前後の典型値と同じ桁)。
    CHECK(px4::cuse::DbToCndat(25.5) > 1000 && px4::cuse::DbToCndat(25.5) < 20000);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS channel_test\n");
    return 0;
}
