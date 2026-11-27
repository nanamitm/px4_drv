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
	struct {
		bool lna;
		/* 待機中も地上波の RF 入力の整合を保つか (Sony の SONY_HELENE_CONFIG_POWERSAVE_TERR_RFIN_MATCHING) */
		bool rfin_matching;
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
	/* 待機中の RF 入力整合をハードウェアに反映済みか。 */
	bool standby_rfin_matching;
	/* 地上波側で選局した周波数 (kHz)。入力レベルの換算に使う最大 RF 利得が周波数で変わるため保持する */
	u32 freq;
};

#ifdef __cplusplus
extern "C" {
#endif
int cxd2858er_init(struct cxd2858er_tuner *tuner);
int cxd2858er_term(struct cxd2858er_tuner *tuner);
int cxd2858er_apply_standby_rfin_matching(struct cxd2858er_tuner *tuner);

int cxd2858er_set_params_t(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 bandwidth);
int cxd2858er_set_params_c(struct cxd2858er_tuner *tuner,
			   u32 freq, bool j83b);
int cxd2858er_set_params_s(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 symbol_rate);
int cxd2858er_stop(struct cxd2858er_tuner *tuner);
int cxd2858er_calc_rf_level(enum cxd2858er_system system, u32 freq,
			    u8 if_agc, u8 rf_agc, u8 if_bpf_gc, s32 *level);
int cxd2858er_read_rf_level(struct cxd2858er_tuner *tuner, s32 *level);
#ifdef __cplusplus
}
#endif

#endif
