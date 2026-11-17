// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for PIXELA XIT-SQR100 device (xit_sqr100_device.c)
 *
 * yyya-nico/px4_drv の feat/support-sqr100 (2026年10月) の XIT-SQR100 対応を、このリポジトリの内蔵カードリーダーの共通層へ合わせて移植した
 * 移植元では、Linux と Windows の実機で地デジ・BS/CS の受信と内蔵カードの APDU を短時間確認している
 * 基板電源の GPIO の信号名と極性は、Windows ドライバー (IT9300BDA.sys) の解析から得た候補で、回路図や実測では確かめられていない
 *
 * J.83 Annex B (5.6Msps) は、PX-MLT 系と同じ復調器 (CXD2856ER) の手順と、チューナー (CXD6866AER) の Sony の J.83B の設定で受信する
 * J.83B の信号での受信は、この機種ではまだ確認していない
 */

#include "print_format.h"
#include "xit_sqr100_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"
#include "ts_sync.h"
#include "cxd2856er_cnr.h"

#define XITSQR100_DEVICE_TS_SYNC_COUNT	4
#define XITSQR100_DEVICE_TS_SYNC_SIZE	(188 * XITSQR100_DEVICE_TS_SYNC_COUNT)

struct xit_sqr100_stream_context {
	struct ptx_chrdev *chrdev;
	u8 remain_buf[XITSQR100_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static void xit_sqr100_device_release(struct kref *kref);

/* lock を保持した状態で呼ぶ */
static int xit_sqr100_backend_set_power(struct xit_sqr100_device *xit,
					bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &xit->it930x;

	dev_dbg(xit->dev,
		"xit_sqr100_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&xit->available))
		return 0;

	/*
	 * Windows ドライバーの CXD2856+CXD6866 の初期化 (GPIO H2 を Low にして 100ms 後に High) を基に、移植元が実機で使っている手順
	 * GPIO H5 と H2 が基板のどの信号かは確かめられていないため、待機時間を含めて移植元の値を変えない
	 */
	if (state) {
		ret = it930x_write_gpio(it930x, 5, true);
		if (ret)
			return ret;

		msleep(50);

		ret = it930x_write_gpio(it930x, 2, false);
		if (ret)
			return ret;

		msleep(100);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(20);
	} else {
		it930x_write_gpio(it930x, 5, false);
		it930x_write_gpio(it930x, 2, false);
	}

	return 0;
}

static void xit_sqr100_device_stream_process(struct ptx_chrdev *chrdev,
					     u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i = 0;
		bool sync_remain = false;

		/* XIT-SQR100 は受信機番号のない通常の TS (同期バイト 0x47) を出力する */
		while (true) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely(!px4_ts_has_plain_sync(p[i * 188])))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (unlikely(i < XITSQR100_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		ptx_chrdev_put_stream(chrdev, p, 188 * i);

		p += 188 * i;
		remain -= 188 * i;

		if (unlikely(sync_remain))
			break;
	}

	*buf = p;
	*len = remain;
}

static int xit_sqr100_device_stream_handler(void *context, void *buf, u32 len)
{
	struct xit_sqr100_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= XITSQR100_DEVICE_TS_SYNC_SIZE)) {
			u32 t = XITSQR100_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = XITSQR100_DEVICE_TS_SYNC_SIZE;

			xit_sqr100_device_stream_process(stream_ctx->chrdev,
							 &ctx_remain_buf,
							 &ctx_remain_len);
			if (likely(!ctx_remain_len)) {
				p += t;
				remain -= t;
			}

			stream_ctx->remain_len = 0;
		} else {
			memcpy(ctx_remain_buf + ctx_remain_len, p, len);
			stream_ctx->remain_len += len;

			return 0;
		}
	}

	xit_sqr100_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int xit_sqr100_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "xit_sqr100_chrdev_init\n");

	/* 地デジと BS/CS の共用入力のため、既存の ioctl のチャンネル番号から方式を判別する */
	chrdev->params.system = PTX_UNSPECIFIED_SYSTEM;
	return 0;
}

static int xit_sqr100_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "xit_sqr100_chrdev_term\n");
	return 0;
}

static int xit_sqr100_set_demod_init_sleep(struct xit_sqr100_chrdev *chrdevx,
					   bool sleep)
{
	/* Sony の初期化のコールバックは、SLV-X の 0x08 へ待機状態を直接書き込む */
	return cxd2856er_write_slvx_reg(&chrdevx->cxd2856er, 0x08,
					(sleep) ? 0x01 : 0x00);
}

static int xit_sqr100_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	struct xit_sqr100_device *xit = container_of(chrdevx,
						     struct xit_sqr100_device,
						     chrdevx);

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_open %u\n", chrdev_group->id);

	mutex_lock(&xit->lock);

	if (!xit->card_powered) {
		ret = xit_sqr100_backend_set_power(xit, true);
		if (ret) {
			dev_err(xit->dev,
				"xit_sqr100_chrdev_open %u: xit_sqr100_backend_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			goto fail_backend_power;
		}
	}

	ret = cxd2856er_init(&chrdevx->cxd2856er);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: cxd2856er_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_demod_init;
	}

	/* 移植元と Windows ドライバーと同じく、チューナーの初期化の間は復調器を待機させる */
	ret = xit_sqr100_set_demod_init_sleep(chrdevx, true);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: CXD2856ER init sleep failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_tuner_init;
	}

	ret = cxd6866_init(&chrdevx->cxd6866);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: cxd6866_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_tuner_init;
	}

	ret = xit_sqr100_set_demod_init_sleep(chrdevx, false);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: CXD2856ER init wakeup failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	ret = cxd2856er_write_slvt_reg(&chrdevx->cxd2856er, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevx->cxd2856er,
					    0xc4, 0x80, 0x88);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&chrdevx->cxd2856er, 0x00, 0xa0);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevx->cxd2856er,
					    0xb9, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	xit->tuner_open = true;
	kref_get(&xit->kref);

	mutex_unlock(&xit->lock);
	return 0;

fail_backend:
	cxd6866_term(&chrdevx->cxd6866);

fail_tuner_init:
	cxd2856er_term(&chrdevx->cxd2856er);

fail_demod_init:
	if (!xit->card_powered)
		xit_sqr100_backend_set_power(xit, false);

fail_backend_power:
	mutex_unlock(&xit->lock);
	return ret;
}

static int xit_sqr100_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	struct xit_sqr100_device *xit = container_of(chrdevx,
						     struct xit_sqr100_device,
						     chrdevx);

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_release %u: kref count: %u\n",
		chrdev_group->id, kref_read(&xit->kref));

	mutex_lock(&xit->lock);

	if (!xit->tuner_open) {
		mutex_unlock(&xit->lock);
		return -EALREADY;
	}

	cxd6866_term(&chrdevx->cxd6866);
	cxd2856er_term(&chrdevx->cxd2856er);

	xit->tuner_open = false;
	if (!xit->card_powered)
		xit_sqr100_backend_set_power(xit, false);

	mutex_unlock(&xit->lock);

	kref_put(&xit->kref, xit_sqr100_device_release);
	return 0;
}

static int xit_sqr100_chrdev_tune(struct ptx_chrdev *chrdev,
				  struct ptx_tune_params *params)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	struct xit_sqr100_device *xit = container_of(chrdevx,
						     struct xit_sqr100_device,
						     chrdevx);
	union cxd2856er_system_params demod_params;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_tune %u\n", chrdev_group->id);

	memset(&demod_params, 0, sizeof(demod_params));

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		demod_params.bandwidth = params->bandwidth;

		ret = cxd2856er_wakeup(&chrdevx->cxd2856er,
				       CXD2856ER_ISDB_T_SYSTEM, &demod_params);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd2856er_wakeup(CXD2856ER_ISDB_T_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, ret);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2856er_wakeup(&chrdevx->cxd2856er,
				       CXD2856ER_ISDB_S_SYSTEM, &demod_params);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd2856er_wakeup(CXD2856ER_ISDB_S_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, ret);
		break;

	/*
	 * J.83 Annex B は TSMF を使わないため、地デジと同じくブリッジの null パケットの除去 (初期化時の設定) をそのまま使う
	 * 復調器の J.83B の手順と IF (3.75MHz) は PX-MLT 系と共通で、Sony のコードでもチューナーの種類によらない
	 */
	case PTX_J83B_SYSTEM:
		ret = cxd2856er_wakeup(&chrdevx->cxd2856er,
				       CXD2856ER_J83B_SYSTEM, &demod_params);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd2856er_wakeup(CXD2856ER_J83B_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, ret);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd6866_set_params_t(&chrdevx->cxd6866,
					   CXD6866_ISDB_T_SYSTEM,
					   params->freq, 6);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd6866_set_params_t(%u, 6) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd6866_set_params_s(&chrdevx->cxd6866,
					   CXD6866_ISDB_S_SYSTEM,
					   params->freq, 28860);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd6866_set_params_s(%u, 28860) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
		break;

	case PTX_J83B_SYSTEM:
		ret = cxd6866_set_params_j83b(&chrdevx->cxd6866, params->freq);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd6866_set_params_j83b(%u) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
		break;

	default:
		break;
	}

	if (ret)
		return ret;

	ret = cxd2856er_post_tune(&chrdevx->cxd2856er);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_tune %u: cxd2856er_post_tune() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	return 0;
}

static int xit_sqr100_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	bool unlocked = false;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbt(&chrdevx->cxd2856er,
						   locked, &unlocked);
		if (!ret && unlocked)
			ret = -ECANCELED;
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbs(&chrdevx->cxd2856er, locked);
		break;

	/* J.83B も ISDB-C と同じ SLV-T バンク 0x40 のロックの表示を使う */
	case PTX_J83B_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbc(&chrdevx->cxd2856er, locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int xit_sqr100_chrdev_set_stream_id(struct ptx_chrdev *chrdev, u16 stream_id)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;

	dev_dbg(chrdev_group->dev,
		"xit_sqr100_chrdev_set_stream_id %u\n", chrdev_group->id);

	/* 12 未満は相対 TS 番号、12 以上は TSID として扱う */
	if (stream_id < 12) {
		ret = cxd2856er_set_slot_isdbs(&chrdevx->cxd2856er, stream_id);
		if (ret)
			dev_err(chrdev_group->dev,
				"xit_sqr100_chrdev_set_stream_id %u: cxd2856er_set_slot_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, stream_id, ret);
	} else {
		ret = cxd2856er_set_tsid_isdbs(&chrdevx->cxd2856er, stream_id);
		if (ret)
			dev_err(chrdev_group->dev,
				"xit_sqr100_chrdev_set_stream_id %u: cxd2856er_set_tsid_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, stream_id, ret);
	}

	return ret;
}

static int xit_sqr100_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	struct xit_sqr100_device *xit = container_of(chrdevx,
						     struct xit_sqr100_device,
						     chrdevx);
	struct xit_sqr100_stream_context *stream_ctx = xit->stream_ctx;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_start_capture %u\n", chrdev_group->id);

	ret = it930x_purge_psb(&xit->it930x,
			       px4_device_params.psb_purge_timeout);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_start_capture %u: it930x_purge_psb() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* 前の選局の端数を次の選局の TS へつなげない */
	stream_ctx->remain_len = 0;

	ret = itedtv_bus_start_streaming(&xit->it930x.bus,
					 xit_sqr100_device_stream_handler,
					 stream_ctx);
	if (ret)
		dev_err(xit->dev,
			"xit_sqr100_chrdev_start_capture %u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
			chrdev_group->id, ret);

	return ret;
}

static int xit_sqr100_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	struct xit_sqr100_device *xit = container_of(chrdevx,
						     struct xit_sqr100_device,
						     chrdevx);

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_stop_capture %u\n", chrdev_group->id);

	itedtv_bus_stop_streaming(&xit->it930x.bus);
	return 0;
}

static int xit_sqr100_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? xit_sqr100_chrdev_start_capture(chrdev)
			: xit_sqr100_chrdev_stop_capture(chrdev);
}

static int xit_sqr100_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct xit_sqr100_chrdev *chrdevx = chrdev->priv;
	u16 val;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd2856er_read_cnr_raw_isdbt(&chrdevx->cxd2856er, &val);
		if (!ret)
			*value = cxd2856er_cnr_raw_to_cndat_isdbt(val);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2856er_read_cnr_raw_isdbs(&chrdevx->cxd2856er, &val);
		if (!ret)
			*value = cxd2856er_cnr_raw_to_cndat_isdbs(val);
		break;

	/* PX-MLT 系と同じく、地デジと同じ recpt1 の式で dB へ換算できる CNDAT を返す */
	case PTX_J83B_SYSTEM:
		ret = cxd2856er_read_cnr_raw_isdbc(&chrdevx->cxd2856er, &val);
		if (!ret)
			*value = cxd2856er_cnr_raw_to_cndat_isdbc(val);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations xit_sqr100_chrdev_ops = {
	.init = xit_sqr100_chrdev_init,
	.term = xit_sqr100_chrdev_term,
	.open = xit_sqr100_chrdev_open,
	.release = xit_sqr100_chrdev_release,
	.tune = xit_sqr100_chrdev_tune,
	.check_lock = xit_sqr100_chrdev_check_lock,
	.set_stream_id = xit_sqr100_chrdev_set_stream_id,
	/* XIT-SQR100 は BS/CS アンテナへの給電に対応していない */
	.set_lnb_voltage = NULL,
	.set_capture = xit_sqr100_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = xit_sqr100_chrdev_read_cnr_raw
};

static int xit_sqr100_device_load_config(struct xit_sqr100_device *xit,
					 struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = xit->dev;
	struct it930x_bridge *it930x = &xit->it930x;
	struct it930x_stream_input *input = &it930x->config.input[0];
	struct xit_sqr100_chrdev *chrdevx = &xit->chrdevx;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"xit_sqr100_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config->system_cap = PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM | PTX_J83B_SYSTEM;

	/*
	 * Windows の USB キャプチャでは、TS 入力はシリアルの port 0 (0xda58 = 0 、0xda73 = 1 、0xda4c = 1)
	 * 使わない入力にも別の port を割り当てる (ゼロのままだと、後の設定で port 0 が無効になる)
	 */
	for (i = 0; i < 5; i++)
		it930x->config.input[i].port_number = i;

	input->enable = true;
	input->is_parallel = false;
	input->port_number = 0;
	input->slave_number = 0;
	input->i2c_bus = 3;
	input->i2c_addr = 0x64;
	input->packet_len = 188;
	input->sync_byte = 0x47;

	/* 復調器の I2C アドレスは Windows ドライバーの 8 ビット表記 0xC8 に当たり、PX-MLT 系と同じく SLV-X は SLV-T + 2 */
	chrdevx->cxd2856er.dev = dev;
	chrdevx->cxd2856er.i2c = &it930x->i2c_master[input->i2c_bus - 1];
	chrdevx->cxd2856er.i2c_addr.slvx = input->i2c_addr + 2;
	chrdevx->cxd2856er.i2c_addr.slvt = input->i2c_addr;
	chrdevx->cxd2856er.config.xtal = 24000;
	chrdevx->cxd2856er.config.tuner_i2c = true;
	chrdevx->cxd2856er.config.serial_ts_clock = true;

	/* Windows ドライバーが FREIA に渡す構成フラグ 0x10004000 (内部ループフィルター・REFOUT 800mVpp) と 24MHz の水晶 */
	chrdevx->cxd6866.dev = dev;
	chrdevx->cxd6866.i2c = &chrdevx->cxd2856er.i2c_master;
	chrdevx->cxd6866.i2c_addr = 0x60;
	chrdevx->cxd6866.config.xtal = 24000;
	chrdevx->cxd6866.config.loop_filter_internal = true;
	chrdevx->cxd6866.config.outlmt_dtv_1_2vpp = false;
	chrdevx->cxd6866.config.refout_enable = true;

	return 0;
}

/* 機種のロックを保持した状態で呼ばれる */
static int xit_sqr100_card_set_power(void *context, bool on)
{
	struct xit_sqr100_device *xit = context;
	int ret = 0;

	/* チューナーを開いていれば、基板の電源はチューナー側の参照で入ったままになっている */
	if (!xit->tuner_open)
		ret = xit_sqr100_backend_set_power(xit, on);

	/* 移植元の実機の確認では、GPIO H7 を Low にしないとカードの ATR を受信できない */
	if (on && !ret) {
		ret = it930x_write_gpio(&xit->it930x, 7, false);
		if (ret && !xit->tuner_open)
			xit_sqr100_backend_set_power(xit, false);
	}

	/* 停止は成否にかかわらず停止済みとして記録し、投入は成功したときだけ記録する */
	if (!on || !ret)
		xit->card_powered = on;

	return ret;
}

int xit_sqr100_device_init(struct xit_sqr100_device *xit, struct device *dev,
			   struct ptx_chrdev_context *chrdev_ctx,
			   struct completion *quit_completion)
{
	int ret = 0;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct xit_sqr100_stream_context *stream_ctx;

	if (!xit || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "xit_sqr100_device_init\n");

	get_device(dev);

	kref_init(&xit->kref);
	mutex_init(&xit->lock);
	xit->card = NULL;
	xit->dev = dev;
	xit->quit_completion = quit_completion;
	xit->tuner_open = false;
	xit->card_powered = false;

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(xit->dev,
			"xit_sqr100_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	xit->stream_ctx = stream_ctx;

	it930x = &xit->it930x;
	bus = &it930x->bus;

	ret = itedtv_bus_init(bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_bridge;

	ret = it930x_raise(it930x);
	if (ret)
		goto fail_device;

	ret = xit_sqr100_device_load_config(xit, &chrdev_config);
	if (ret)
		goto fail_device;

	chrdev_config.ops = &xit_sqr100_chrdev_ops;
	chrdev_config.options = PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE |
				PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = &xit->chrdevx;

	ret = it930x_load_firmware(it930x, IT930X_FIRMWARE_FILENAME);
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(it930x);
	if (ret)
		goto fail_device;

	/*
	 * XIT-SQR100 の IT930x は USB 転送用の TS の暗号化が有効になっているため、IT9300_setTsEncrypt(false) と同じ順で無効にする
	 * 放送の B-CAS のスクランブルとは別の機能で、有効のままだと TS の一部が化けて adaptation field や continuity counter が壊れる
	 */
	ret = it930x_write_reg(it930x, 0xdab0, 1);
	if (ret)
		goto fail_device;

	ret = it930x_write_reg(it930x, 0xdaae, 0);
	if (ret)
		goto fail_device;

	/* GPIO (基板電源は切った状態から始め、H7 は Windows ドライバーの初期化と同じく High にする) */
	ret = it930x_set_gpio_mode(it930x, 5, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 5, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 7, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 7, true);
	if (ret)
		goto fail_device;

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(it930x, 0, &filter);
		if (ret)
			goto fail_device;
	}

	chrdev_group_config.owner_kref = &xit->kref;
	chrdev_group_config.owner_kref_release = xit_sqr100_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = XITSQR100_CHRDEV_NUM;
	chrdev_group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_device;

	xit->chrdev_group = chrdev_group;
	xit->chrdevx.chrdev = &chrdev_group->chrdev[0];
	stream_ctx->chrdev = &chrdev_group->chrdev[0];

	atomic_set(&xit->available, 1);

	{
		struct px4_card_backend card_backend = {
			.it930x = it930x,
			.lock = &xit->lock,
			.available = &xit->available,
			/* カードの検出と通信には、チューナーと共有する基板電源と GPIO H7 の Low が必要 */
			.set_power = xit_sqr100_card_set_power,
			.context = xit,
		};

		/* カードリーダーを登録できなくても、チューナーは使えるようにする */
		ret = px4_card_register(dev, &card_backend, &xit->card);
		if (ret)
			dev_warn(dev, "Card reader registration failed: %d\n", ret);
	}
	return 0;

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(xit->stream_ctx);

fail:
	mutex_destroy(&xit->lock);
	put_device(dev);
	return ret;
}

static void xit_sqr100_device_release(struct kref *kref)
{
	struct xit_sqr100_device *xit = container_of(kref,
						     struct xit_sqr100_device,
						     kref);

	dev_dbg(xit->dev, "xit_sqr100_device_release\n");

	it930x_term(&xit->it930x);
	itedtv_bus_term(&xit->it930x.bus);

	kfree(xit->stream_ctx);
	mutex_destroy(&xit->lock);
	put_device(xit->dev);

	complete(xit->quit_completion);
}

void xit_sqr100_device_term(struct xit_sqr100_device *xit)
{
	dev_dbg(xit->dev,
		"xit_sqr100_device_term: kref count: %u\n",
		kref_read(&xit->kref));

	/* カードの操作と並行しても、登録の解除後にハードウェアへ触れないよう、ロックの下で切り替える */
	mutex_lock(&xit->lock);
	atomic_xchg(&xit->available, 0);
	mutex_unlock(&xit->lock);

	px4_card_unregister(xit->card);
	xit->card = NULL;
	ptx_chrdev_group_destroy(xit->chrdev_group);

	kref_put(&xit->kref, xit_sqr100_device_release);
}
