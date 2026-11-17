// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD6866AER (FREIA) tuner driver definitions (cxd6866.h)
 *
 * XIT-SQR100 の地上波・ケーブル・衛星共用のチューナー
 * 初期化と ISDB-T・ISDB-S の選局は yyya-nico/px4_drv の feat/support-sqr100 (2026年10月) から移植した
 * J.83B の選局は、TBS の cxd2878.c にある Sony の FREIA の手順 (freia_tune()) に従う
 */

#ifndef __CXD6866_H__
#define __CXD6866_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/device.h>
#elif defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

struct cxd6866_config {
	/* 水晶の周波数 (kHz) 。16000 か 24000 で、XIT-SQR100 は 24000 */
	u32 xtal;
	/* PLL の内部ループフィルターを使う (Sony の SONY_FREIA_CONFIG_LOOPFILTER_INTERNAL) */
	bool loop_filter_internal;
	/* 地上波の IF 出力を 1.2Vpp に制限する (Sony の SONY_FREIA_CONFIG_OUTLMT_DTV_1_2Vpp) */
	bool outlmt_dtv_1_2vpp;
	/* 基準クロックを 800mVpp で出力する (Sony の SONY_FREIA_CONFIG_REFOUT_800mVpp) */
	bool refout_enable;
};

enum cxd6866_system {
	CXD6866_UNSPECIFIED_SYSTEM = 0,
	CXD6866_ISDB_T_SYSTEM,
	CXD6866_ISDB_S_SYSTEM,
	/* 日本の J.83 Annex B (5.6Msps) 。チューナーは地上波の動作モードで受信する */
	CXD6866_J83B_SYSTEM
};

struct cxd6866_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct cxd6866_config config;
	enum cxd6866_system system;
};

#ifdef __cplusplus
extern "C" {
#endif
int cxd6866_init(struct cxd6866_tuner *tuner);
int cxd6866_term(struct cxd6866_tuner *tuner);

int cxd6866_set_params_t(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 bandwidth);
int cxd6866_set_params_s(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 symbol_rate);
/* freq は搬送波の中心周波数 (kHz) */
int cxd6866_set_params_j83b(struct cxd6866_tuner *tuner, u32 freq);
#ifdef __cplusplus
}
#endif

#endif
