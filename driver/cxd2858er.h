// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD2858ER driver definitions (cxd2858er.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __CXD2858ER_H__
#define __CXD2858ER_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/device.h>
#elif defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

struct cxd2858er_config {
	u32 xtal;
	bool isdbc_reference_profile;
	/* Sony/TBS HELENE J83B_5_6 行。既定無効、信号で未検証。 */
	bool experimental_j83b;
	/* J.83B でも RF/IF 過大入力の検出レベルに地デジの値を使う試験用設定。既定無効。 */
	bool j83b_ter_overload_level;
	struct {
		bool lna;
	} ter;
	struct {
		bool lna;
	} sat;
};

enum cxd2858er_system {
	CXD2858ER_UNSPECIFIED_SYSTEM = 0,
	CXD2858ER_ISDB_T_SYSTEM,
	CXD2858ER_ISDB_S_SYSTEM,
	CXD2858ER_ISDB_C_SYSTEM
};

struct cxd2858er_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct cxd2858er_config config;
	enum cxd2858er_system system;
};

#ifdef __cplusplus
extern "C" {
#endif
int cxd2858er_init(struct cxd2858er_tuner *tuner);
int cxd2858er_term(struct cxd2858er_tuner *tuner);

int cxd2858er_set_params_t(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 bandwidth);
int cxd2858er_set_params_c(struct cxd2858er_tuner *tuner,
			   u32 freq, u32 bandwidth);
int cxd2858er_set_params_s(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 symbol_rate);
int cxd2858er_stop(struct cxd2858er_tuner *tuner);

/* 地上・ケーブル側の AGC 生値と、参照実装の換算式による入力レベルの推定値。
 * 推定値は CXD2858ER 用に校正されていない。 */
struct cxd2858er_rssi {
	u8 if_agc;
	u8 rf_agc;
	u8 if_bpf_gc;
	s32 rssi_x100;
};
int cxd2858er_read_rssi_t(struct cxd2858er_tuner *tuner, u32 freq,
			  struct cxd2858er_rssi *rssi);
#ifdef __cplusplus
}
#endif

#endif
