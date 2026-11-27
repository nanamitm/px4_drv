// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD2858ER driver (cxd2858er.c)
 *
 * Copyright (c) 2018-2021 nns779
 *
 * ISDB-C と J.83B 用に参照した PLEX の Linux ドライバーの Sony のコード (sony_helene.c・sony_tuner_helene.c):
 * https://github.com/soralis0912/it930x_driver_native/tree/64fd4f3fd04cd8c19855250ebd88278734d4eba6/api/DW_Frontend/CXD2856/tuner/terr_cable_sat_helene
 * Copyright 2016 Sony Semiconductor Solutions Corporation
 */

#include "print_format.h"
#include "cxd2858er.h"

#ifdef __linux__
#include <linux/delay.h>
#endif

static int cxd2858er_stop_t(struct cxd2858er_tuner *tuner);
static int cxd2858er_stop_s(struct cxd2858er_tuner *tuner);

static int cxd2858er_read_regs(struct cxd2858er_tuner *tuner,
			       u8 reg, u8 *buf, int len)
{
	u8 b;
	struct i2c_comm_request req[2];

	if (!buf || !len)
		return -EINVAL;

	b = reg;

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = &b;
	req[0].len = 1;

	req[1].req = I2C_READ_REQUEST;
	req[1].addr = tuner->i2c_addr;
	req[1].data = buf;
	req[1].len = len;

	return i2c_comm_master_request(tuner->i2c, req, 2);
}

static int cxd2858er_read_reg(struct cxd2858er_tuner *tuner,
			      u8 reg, u8 *val)
{
	return cxd2858er_read_regs(tuner, reg, val, 1);
}

static int cxd2858er_write_regs(struct cxd2858er_tuner *tuner,
				u8 reg, u8 *buf, int len)
{
	u8 b[255];
	struct i2c_comm_request req[1];

	if (!buf || !len || len > 254)
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = b;
	req[0].len = 1 + len;

	return i2c_comm_master_request(tuner->i2c, req, 1);
}

static int cxd2858er_write_reg(struct cxd2858er_tuner *tuner,
			       u8 reg, u8 val)
{
	return cxd2858er_write_regs(tuner, reg, &val, 1);
}

static int cxd2858er_write_reg_mask(struct cxd2858er_tuner *tuner,
				    u8 reg, u8 val, u8 mask)
{
	int ret = 0;
	u8 tmp;

	if (!mask)
		return -EINVAL;

	if (mask != 0xff) {
		ret = cxd2858er_read_regs(tuner, reg, &tmp, 1);
		if (ret)
			return ret;

		tmp &= ~mask;
		tmp |= (val & mask);
	} else {
		tmp = val;
	}

	return cxd2858er_write_regs(tuner, reg, &tmp, 1);
}

static int cxd2858er_power_on(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[20], tmp;

	/* T mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	/* rfin_matching のときは、Sony の SONY_HELENE_CONFIG_POWERSAVE_TERR_RFIN_MATCHING に従い、待機中も地上波の RF 入力の整合を保つ
	 * PX-MLT5PE は1つの地上波・ケーブルの RF 入力を全チューナーへ分配していて、既定の待機状態では、チューナーが待機と動作を切り替えるときに
	 * 隣のチューナーで受信中の 256QAM (ISDB-C の UHF 帯や J.83B) に誤りが出る
	 * 地デジと BS/CS の受信ではこの誤りは出ないため、PLEX の公開ドライバーと同じ既定の待機状態を基本とし、ISDB-C か J.83B を使うときだけ整合を保つ */
	ret = cxd2858er_write_reg(tuner, 0x67, (tuner->config.ter.rfin_matching) ? 0x02 : 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x43,
				  0x05 | ((tuner->config.ter.lna) ? 0x02
								  : 0x00));
	if (ret)
		return ret;

	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x0c, 0x14);
	if (ret)
		return ret;

	data[0] = 0x7a;
	data[1] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x99, data, 2);
	if (ret)
		return ret;

	switch (tuner->config.xtal) {
	case 16000:
		data[0] = 0x10;
		break;

	case 24000:
		data[0] = 0x18;
		break;

	default:
		return -EINVAL;
	}

	data[1] = 0x80 | (0x04 & 0x1f);
	data[2] = 0x80 | 0x26;
	data[3] = 0x00;
	data[4] = 0x00;
	data[5] = 0x00;
	data[6] = 0xc4;
	data[7] = 0x40;
	data[8] = 0x10;
	data[9] = 0x00;
	data[10] = 0x45;
	data[11] = 0x75;
	data[12] = 0x07;
	data[13] = 0x1c;
	data[14] = 0x3f;
	data[15] = 0x02;
	data[16] = 0x10;
	data[17] = 0x20;
	data[18] = 0x0a;
	data[19] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x81, data, 20);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x9b, 0x00);
	if (ret)
		return ret;

	msleep(10);

	ret = cxd2858er_read_reg(tuner, 0x1a, &tmp);
	if (ret)
		return ret;

	if (tmp != 0x00)
		return -EIO;

	data[0] = 0x90;
	data[1] = 0x06;

	ret = cxd2858er_write_regs(tuner, 0x17, data, 2);
	if (ret)
		return ret;

	msleep(1);

	ret = cxd2858er_read_reg(tuner, 0x19, &tmp);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x95, (tmp & 0xf0) >> 4);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x80, 0x01);
	if (ret)
		return ret;

	data[0] = 0x07;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x41, data, 2);
	if (ret)
		return ret;

	return 0;
}

int cxd2858er_init(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	if (!tuner->dev || !tuner->i2c || !tuner->i2c_addr)
		return -EINVAL;

	if (tuner->config.xtal != 16000 && tuner->config.xtal != 24000)
		return -EINVAL;

	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	tuner->standby_rfin_matching = false;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	ret = cxd2858er_power_on(tuner);
	if (!ret)
		tuner->standby_rfin_matching = tuner->config.ter.rfin_matching;

	i2c_comm_master_gate_ctrl(tuner->i2c, false);

	return ret;
}

int cxd2858er_apply_standby_rfin_matching(struct cxd2858er_tuner *tuner)
{
	int ret, gate_ret;

	if (tuner->system != CXD2858ER_UNSPECIFIED_SYSTEM ||
	    !tuner->config.ter.rfin_matching || tuner->standby_rfin_matching)
		return 0;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	/* 待機に入ったままのチューナーでは、RF_EXT を保持して整合ビットだけを更新する。 */
	ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x02, 0x02);
	gate_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);
	if (!ret && !gate_ret)
		tuner->standby_rfin_matching = true;

	return ret ? ret : gate_ret;
}

int cxd2858er_term(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	/* 整合の設定を受け取ったあと、選局せずに閉じる場合も待機レジスターへ反映する。 */
	if (tuner->system == CXD2858ER_UNSPECIFIED_SYSTEM)
		return cxd2858er_apply_standby_rfin_matching(tuner);

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (!ret) {
		switch (tuner->system) {
		case CXD2858ER_ISDB_T_SYSTEM:
		case CXD2858ER_ISDB_C_SYSTEM:
			cxd2858er_stop_t(tuner);
			break;

		case CXD2858ER_ISDB_S_SYSTEM:
			cxd2858er_stop_s(tuner);
			break;

		default:
			break;
		}

		i2c_comm_master_gate_ctrl(tuner->i2c, false);
	} else {
		tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
		tuner->standby_rfin_matching = false;
	}

	return 0;
}

int cxd2858er_set_params_t(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 bandwidth)
{
	int ret = 0;
	u8 data[17];

	if (system != CXD2858ER_ISDB_T_SYSTEM)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_S_SYSTEM:
		ret = cxd2858er_stop_s(tuner);
		break;

	default:
		break;
	}

	if (ret)
		goto exit;

	/* T mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		goto exit;

	data[0] = 0xc4;
	data[1] = 0x40;

	ret = cxd2858er_write_regs(tuner, 0x87, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x10;
	data[1] = 0x20;

	ret = cxd2858er_write_regs(tuner, 0x91, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x9c, data, 2);
	if (ret)
		goto exit;

	data[0] = 0xee;
	data[1] = 0x02;
	data[2] = 0x1e;
	data[3] = 0x67;

	switch (tuner->config.xtal) {
	case 16000:
		data[4] = 0x02;
		break;

	case 24000:
		data[4] = 0x03;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	data[5] = 0xb4;
	data[6] = 0x78;
	data[7] = 0x08;
	data[8] = 0x30;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 9);
	if (ret)
		goto exit;

	tuner->standby_rfin_matching = false;
	ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x00, 0x02);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x88;
	data[2] = 0x00;
	data[3] = 0x0b;
	data[4] = 0x22;
	data[5] = 0x00;
	data[6] = 0x17;
	data[7] = 0x1b;

	data[8] = freq & 0xff;
	data[9] = (freq >> 8) & 0xff;
	data[10] = (freq >> 16) & 0x0f;

	data[11] = 0xff;
	data[12] = 0x01;
	data[13] = 0x99;
	data[14] = 0x00;
	data[15] = 0x24;
	data[16] = 0x87;

	ret = cxd2858er_write_regs(tuner, 0x68, data, 17);
	if (ret)
		goto exit;

	msleep(50);

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		goto exit;

	tuner->system = system;
	tuner->freq = freq;

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}

int cxd2858er_set_params_c(struct cxd2858er_tuner *tuner,
			   u32 freq, bool j83b)
{
	int ret = 0;
	u8 data[17];

	/* 周波数のレジスターは kHz の20ビットのため、その範囲の周波数だけを書き込む */
	if (freq > 0xfffff)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_S_SYSTEM:
		ret = cxd2858er_stop_s(tuner);
		break;

	default:
		break;
	}
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		goto exit;
	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		goto exit;

	data[0] = 0xc4;
	data[1] = 0x40;
	ret = cxd2858er_write_regs(tuner, 0x87, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x16;
	data[1] = 0x26;
	ret = cxd2858er_write_regs(tuner, 0x91, data, 2);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x00;
	ret = cxd2858er_write_regs(tuner, 0x9c, data, 2);
	if (ret)
		goto exit;

	data[0] = 0xee;
	data[1] = 0x02;
	data[2] = 0x1e;
	data[3] = 0x67;
	/* sony_helene.c の TER_tune() に従い、ケーブル受信用の REF_R (Fref = 1 MHz) を水晶周波数に合わせる */
	data[4] = (tuner->config.xtal == 16000) ? 0x10 : 0x18;
	data[5] = 0x1c;
	data[6] = 0x78;
	data[7] = 0x08;
	data[8] = 0x1c;
	ret = cxd2858er_write_regs(tuner, 0x5e, data, 9);
	if (ret)
		goto exit;

	tuner->standby_rfin_matching = false;
	ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x00, 0x02);
	if (ret)
		goto exit;

	data[0] = 0x00;
	/* Sony の sony_helene.c の CXD2856 世代の表では、DVB-C 6 MHz (ISDB-C が使う設定) と J.83B 5.6 Msps の IF_BPF_GC はどちらも 0x04 */
	data[1] = 0x84;
	data[2] = 0x00;
	data[3] = 0x02;
	data[4] = 0x21;
	data[5] = 0x00;
	/* sony_helene.c の SONY_HELENE_DTV_J83B_5_6 に従い、J.83B では FIF_OFFSET=-5、BW_OFFSET=+2 を5ビットで格納する
	 * 他の設定値は DVB-C 6 MHz と共通で、書き込み後は sony_tuner_helene.c と同じ50ミリ秒の校正待ちが必要 */
	data[6] = j83b ? 0x1b : 0x1a;
	data[7] = j83b ? 0x02 : 0x1c;
	data[8] = freq & 0xff;
	data[9] = (freq >> 8) & 0xff;
	data[10] = (freq >> 16) & 0x0f;
	data[11] = 0xff;
	data[12] = 0x01;
	data[13] = 0xd9;
	data[14] = 0x0f;
	data[15] = 0x24;
	data[16] = 0x87;
	ret = cxd2858er_write_regs(tuner, 0x68, data, 17);
	if (ret)
		goto exit;

	msleep(50);
	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		goto exit;
	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		goto exit;

	tuner->system = CXD2858ER_ISDB_C_SYSTEM;
	tuner->freq = freq;

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}

int cxd2858er_set_params_s(struct cxd2858er_tuner *tuner,
			   enum cxd2858er_system system,
			   u32 freq, u32 symbol_rate)
{
	int ret = 0;
	u8 data[18];

	if (system != CXD2858ER_ISDB_S_SYSTEM)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_T_SYSTEM:
	case CXD2858ER_ISDB_C_SYSTEM:
		ret = cxd2858er_stop_t(tuner);
		break;

	default:
		break;
	}

	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x15, 0x02);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x43, 0x06);
	if (ret)
		goto exit;

	data[0] = 0x00;
	data[1] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x6a, data, 2);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x75, 0x99);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x9d, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x61, 0x07);
	if (ret)
		goto exit;

	/* S mode */
	ret = cxd2858er_write_reg(tuner, 0x01, 0x01);
	if (ret)
		goto exit;

	data[0] = 0xc4;
	data[1] = 0x40;

	switch (tuner->config.xtal) {
	case 16000:
		data[2] = 0x02;
		break;

	case 24000:
		data[2] = 0x03;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	data[3] = 0x00;
	data[4] = 0xb4;
	data[5] = 0x78;
	data[6] = 0x08;
	data[7] = 0x30;
	data[8] = 0xfe | ((tuner->config.sat.lna) ? 0x01 : 0x00);
	data[9] = 0x02;
	data[10] = 0x1e;

	switch (system) {
	case CXD2858ER_ISDB_S_SYSTEM:
		data[11] = 0x16;
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	freq = (freq + 2) / 4;

	data[12] = freq & 0xff;
	data[13] = (freq >> 8) & 0xff;
	data[14] = (freq >> 16) & 0x0f;

	data[15] = 0xff;
	data[16] = 0x00;
	data[17] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x04, data, 18);
	if (ret)
		goto exit;

	msleep(10);

	ret = cxd2858er_write_reg(tuner, 0x05, 0x00);
	if (ret)
		goto exit;

	ret = cxd2858er_write_reg(tuner, 0x04, 0xc0);
	if (ret)
		goto exit;

	tuner->system = system;

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}

static int cxd2858er_stop_t(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[3];

	/* ISDB-C も地上波側の RF 経路を使うため同じ手順で停止する */
	if (tuner->system != CXD2858ER_ISDB_T_SYSTEM &&
	    tuner->system != CXD2858ER_ISDB_C_SYSTEM)
		return -EINVAL;

	ret = cxd2858er_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	/* 電源投入時と同じく、rfin_matching のときは待機中も RF 入力の整合を保つ (bit0 の RF_EXT は保持する) */
	ret = cxd2858er_write_reg_mask(tuner, 0x67, (tuner->config.ter.rfin_matching) ? 0x02 : 0x00, 0xfe);
	if (ret)
		return ret;

	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (ret)
		return ret;

	tuner->standby_rfin_matching = tuner->config.ter.rfin_matching;
	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	return 0;
}

static int cxd2858er_stop_s(struct cxd2858er_tuner *tuner)
{
	int ret = 0;
	u8 data[3];

	if (tuner->system != CXD2858ER_ISDB_S_SYSTEM)
		return -EINVAL;

	ret = cxd2858er_write_reg(tuner, 0x15, 0x02);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x43,
				  0x05 | ((tuner->config.ter.lna) ? 0x02
								  : 0x00));
	if (ret)
		return ret;

	data[0] = 0x14;
	data[1] = 0x00;
	data[2] = 0x00;

	ret = cxd2858er_write_regs(tuner, 0x0c, data, 3);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x05, 0x00);
	if (ret)
		return ret;

	ret = cxd2858er_write_reg(tuner, 0x04, 0xc0);
	if (ret)
		return ret;

	/* 衛星の受信中に rfin_matching になったチューナーも、待機へ戻るときに地上波の RF 入力の整合を保つ状態にそろえる */
	if (tuner->config.ter.rfin_matching) {
		ret = cxd2858er_write_reg_mask(tuner, 0x67, 0x02, 0x02);
		if (ret)
			return ret;
	}

	tuner->standby_rfin_matching = tuner->config.ter.rfin_matching;
	tuner->system = CXD2858ER_UNSPECIFIED_SYSTEM;
	return 0;
}

int cxd2858er_stop(struct cxd2858er_tuner *tuner)
{
	int ret = 0;

	if (tuner->system == CXD2858ER_UNSPECIFIED_SYSTEM)
		return -EALREADY;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD2858ER_ISDB_T_SYSTEM:
	case CXD2858ER_ISDB_C_SYSTEM:
		cxd2858er_stop_t(tuner);
		break;

	case CXD2858ER_ISDB_S_SYSTEM:
		cxd2858er_stop_s(tuner);
		break;

	default:
		break;
	}

	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return 0;
}

/* Sony の sony_helene.c の sony_helene_ReadRssi() と sony_tuner_helene.c の sony_tuner_helene_terr_ReadRFLevel() に従い、
 * 地上波側の AGC の読み取り値から入力レベル (dBm × 1000) を求める
 * AGC の電圧は読み取り値 × 1.4 / 255 V で、式は Sony のコードと同じく 140 倍した整数で比べ、同じ位置で丸める
 * 衛星側は Sony のコードでも復調器の AGC から求める dB の値で、dBm として扱えるか確認できないため対象外にする */
int cxd2858er_calc_rf_level(enum cxd2858er_system system, u32 freq,
			    u8 if_agc, u8 rf_agc, u8 if_bpf_gc, s32 *level)
{
	/* IF の帯域通過フィルターの利得 (dB)。0x69 の下位4ビットで選ばれる */
	static const s32 if_bpf_gc_table[16] = {
		0, 0, 0, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 20
	};
	s32 if_agc_x140 = if_agc * 140;
	s32 max_agc_x140 = ((if_agc > rf_agc) ? if_agc : rf_agc) * 140;
	s32 if_gain, rf_gain, rf_gain_max, correction;

	switch (system) {
	case CXD2858ER_ISDB_T_SYSTEM:
		/* Sony の SONY_DTV_SYSTEM_ISDBT の IFOUT の補正 (-4.5 dB) */
		correction = -4500;
		break;

	case CXD2858ER_ISDB_C_SYSTEM:
		/* J.83B も同じ ISDB-C の状態で受信する。Sony のコードは ISDB-C と J.83B に同じ補正 (-1.5 dB) を使う */
		correction = -1500;
		break;

	default:
		return -EINVAL;
	}

	if_gain = 870 + if_bpf_gc_table[if_bpf_gc & 0x0f] * 100;
	if (if_agc_x140 <= 9945)
		if_gain += (769 * (9945 - if_agc_x140) + 1275) / 2550;

	/* RF 利得の最大値 (dB × 100) は周波数帯ごとの Sony の表の値 */
	if (freq > 700000)
		rf_gain_max = 4150;
	else if (freq > 600000)
		rf_gain_max = 4130;
	else if (freq > 532000)
		rf_gain_max = 4170;
	else if (freq > 464000)
		rf_gain_max = 4050;
	else if (freq > 400000)
		rf_gain_max = 4280;
	else if (freq > 350000)
		rf_gain_max = 4260;
	else if (freq > 320000)
		rf_gain_max = 4110;
	else if (freq > 285000)
		rf_gain_max = 4310;
	else if (freq > 215000)
		rf_gain_max = 4250;
	else if (freq > 184000)
		rf_gain_max = 4020;
	else if (freq > 172000)
		rf_gain_max = 3920;
	else if (freq > 150000)
		rf_gain_max = 4080;
	else if (freq > 86000)
		rf_gain_max = 4180;
	else if (freq > 65000)
		rf_gain_max = 4200;
	else
		rf_gain_max = 4020;

	/* 154 = 70 × 2.2、4536 = 70 × 64.8、1186 = 57 × 20.8 (Sony のコードの丸め値) */
	if (max_agc_x140 < 4896)
		rf_gain = rf_gain_max;
	else if (max_agc_x140 < 5457)
		rf_gain = rf_gain_max - (70 * (max_agc_x140 - 4896) + 127) / 255;
	else if (max_agc_x140 < 8823)
		rf_gain = rf_gain_max - 154;
	else if (max_agc_x140 < 24786)
		rf_gain = rf_gain_max - 154 - (70 * (max_agc_x140 - 8823) + 127) / 255;
	else if (max_agc_x140 < 30090)
		rf_gain = rf_gain_max - 4536 - (57 * (max_agc_x140 - 24786) + 127) / 255;
	else
		rf_gain = rf_gain_max - 4536 - 1186 - (160 * (max_agc_x140 - 30090) + 127) / 255;

	*level = (-if_gain - rf_gain) * 10 + correction;
	return 0;
}

/* Sony の sony_helene.c の X_read_agc() と同じ手順で、内部の論理回路と CPU を起こしてから
 * IF と RF の AGC を ADC で読み、読み終えたら選局後と同じ待機の設定へ戻す
 * 同じ I2C バスのチューナーは同じアドレスを使うため、呼び出し側は選局と同じチューナーのロックを持つ */
int cxd2858er_read_rf_level(struct cxd2858er_tuner *tuner, s32 *level)
{
	int ret = 0, ret2;
	u8 data[2], if_agc = 0, rf_agc = 0, if_bpf_gc = 0;

	/* Sony のコードは地上波の動作中だけ読み取りを受け付ける。ISDB-C と J.83B も地上波側の経路を使う */
	if (tuner->system != CXD2858ER_ISDB_T_SYSTEM &&
	    tuner->system != CXD2858ER_ISDB_C_SYSTEM)
		return -EINVAL;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	data[0] = 0xc4;
	data[1] = 0x41;

	ret = cxd2858er_write_regs(tuner, 0x87, data, 2);
	if (ret)
		goto restore;

	/* IF の AGC を ADC へつなぎ、変換を始める */
	data[0] = 0x05;
	data[1] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x59, data, 2);
	if (ret)
		goto restore;

	ret = cxd2858er_read_reg(tuner, 0x5b, &if_agc);
	if (ret)
		goto restore;

	/* RF の AGC を ADC へつなぎ、変換を始める */
	data[0] = 0x03;
	data[1] = 0x01;

	ret = cxd2858er_write_regs(tuner, 0x59, data, 2);
	if (ret)
		goto restore;

	ret = cxd2858er_read_reg(tuner, 0x5b, &rf_agc);

restore:
	/* 途中で失敗しても、ADC と CPU を起こしたまま受信を続けないよう、選局後と同じ待機の設定へ戻す */
	ret2 = cxd2858er_write_reg(tuner, 0x59, 0x04);
	if (!ret)
		ret = ret2;

	ret2 = cxd2858er_write_reg(tuner, 0x88, 0x00);
	if (!ret)
		ret = ret2;

	ret2 = cxd2858er_write_reg(tuner, 0x87, 0xc0);
	if (!ret)
		ret = ret2;

	if (ret)
		goto exit;

	ret = cxd2858er_read_reg(tuner, 0x69, &if_bpf_gc);
	if (ret)
		goto exit;

	ret = cxd2858er_calc_rf_level(tuner->system, tuner->freq,
				      if_agc, rf_agc, if_bpf_gc, level);

exit:
	i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret;
}
