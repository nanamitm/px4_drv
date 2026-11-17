// SPDX-License-Identifier: GPL-2.0-only
/*
 * CXD2856ER の C/N 推定用の生値を PT1/PT3 互換の値へ換算する (cxd2856er_cnr.h)
 *
 * 受信ソフトウェアが recpt1 の式で dB へ換算できるよう、地デジは CNDAT 、BS/CS は PT1/PT3 の衛星の値を返す
 * ISDB-C と J.83B は、地デジと同じ式で dB になる CNDAT を返す
 */

#ifndef __CXD2856ER_CNR_H__
#define __CXD2856ER_CNR_H__

#ifdef __linux__
#include <linux/types.h>
#elif defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
u32 cxd2856er_cnr_raw_to_cndat_isdbt(u16 val);
u32 cxd2856er_cnr_raw_to_cndat_isdbs(u16 val);
/* J.83B も ISDB-C と同じ式を使う */
u32 cxd2856er_cnr_raw_to_cndat_isdbc(u16 val);
#ifdef __cplusplus
}
#endif

#endif
