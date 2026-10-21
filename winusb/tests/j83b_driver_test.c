// SPDX-License-Identifier: GPL-2.0-only
/* 模擬 I2C による J.83B 設定・復帰・失敗境界の試験。
 * 信号ロック、USB 負荷、実際の TS 品質はこの試験の確認範囲外。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cxd2856er.h"
#include "cxd2858er.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct mock_bus {
	u8 bank[128];
	u8 regs[3][256][256];
	unsigned int calls;
	unsigned int fail_at;
	unsigned long long trace;
	bool gate_open;
};

static int gate(void *priv, bool open)
{
	((struct mock_bus *)priv)->gate_open = open;
	return 0;
}

static int request(void *priv, const struct i2c_comm_request *req, int num)
{
	struct mock_bus *bus = priv;
	int n, i;
	if (++bus->calls == bus->fail_at)
		return -EIO;
	for (n = 0; n < num; n++) {
		int addr = req[n].addr;
		int chip = addr == 0x64 ? 0 : addr == 0x66 ? 1 : 2;
		u8 reg = req[n].data[0];
		bus->trace = bus->trace * 1099511628211ULL ^ req[n].req;
		bus->trace = bus->trace * 1099511628211ULL ^ addr;
		if (req[n].req == I2C_READ_REQUEST) {
			CHECK(n > 0);
			reg = req[n - 1].data[0];
			for (i = 0; i < req[n].len; i++)
				req[n].data[i] = bus->regs[chip][bus->bank[addr]][(u8)(reg + i)];
		} else {
			for (i = 1; i < req[n].len; i++) {
				u8 at = reg + i - 1;
				if (chip != 2 && at == 0)
					bus->bank[addr] = req[n].data[i];
				else
					bus->regs[chip][bus->bank[addr]][at] = req[n].data[i];
			}
		}
		for (i = 0; i < req[n].len; i++)
			bus->trace = bus->trace * 1099511628211ULL ^ req[n].data[i];
	}
	return 0;
}

static struct mock_bus bus;
static struct device dev = {0};
static struct i2c_comm_master master = {gate, request, &bus};

static void reset(struct cxd2856er_demod *demod, struct cxd2858er_tuner *tuner)
{
	memset(&bus, 0, sizeof(bus));
	memset(demod, 0, sizeof(*demod));
	memset(tuner, 0, sizeof(*tuner));
	demod->dev = &dev;
	demod->i2c = &master;
	demod->i2c_addr.slvx = 0x64;
	demod->i2c_addr.slvt = 0x66;
	demod->state = CXD2856ER_SLEEP_STATE;
	tuner->dev = &dev;
	tuner->i2c = &master;
	tuner->i2c_addr = 0x60;
	tuner->config.xtal = 16000;
}

int main(void)
{
	struct cxd2856er_demod demod;
	struct cxd2858er_tuner tuner;
	union cxd2856er_system_params params;
	unsigned int ref, qam;
	/* J.83B 無効の全既存 C プロファイル。HEAD 版と I2C 列を比較可能な指紋。 */
	for (ref = 0; ref < 2; ref++) {
		for (qam = 64; qam <= 256; qam *= 4) {
			reset(&demod, &tuner);
			memset(&params, 0, sizeof(params));
			params.isdbc.symbol_rate = 5274;
			params.isdbc.modulation = qam;
			params.isdbc.experimental_profile = true;
			params.isdbc.reference_profile = ref != 0;
			tuner.config.isdbc_reference_profile = ref != 0;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
			CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == 0);
			printf("ISDBC ref=%u qam=%u calls=%u trace=%016llx\n", ref, qam, bus.calls, bus.trace);
		}
	}
#ifndef J83B_BASELINE
	{
		bool locked;
		unsigned int count, fail;
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		params.isdbc.experimental_j83b = true;
		tuner.config.experimental_j83b = true;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
		CHECK(demod.j83b_active);
		CHECK(bus.regs[0][0][0x17] == 0x08);
		CHECK(bus.regs[1][0x40][0x21] == 0x04);
		CHECK(bus.regs[1][0x40][0xb3] == 0x05);
		CHECK(bus.regs[1][0x40][0x1e] == 0x80);
		CHECK(bus.regs[1][0x40][0x8e] == 0x58);
		CHECK(bus.regs[1][0x41][0xcf] == 0x99);
		CHECK(bus.regs[1][0x10][0xb6] == 0x14);
		CHECK(bus.regs[1][0x10][0xb7] == 0 && bus.regs[1][0x10][0xb8] == 0);
		CHECK(bus.regs[1][0x40][0x14] == 0x10);
		CHECK(memcmp(&bus.regs[1][0x40][0x26], "\x07\x77\x07\x77", 4) == 0);
		count = bus.calls;
		CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == 0);
		CHECK(bus.regs[2][0][0x62] == 0x10);
		CHECK(bus.regs[2][0][0x69] == 0x84);
		CHECK(bus.regs[2][0][0x6e] == 0x1b && bus.regs[2][0][0x6f] == 0x02);
		tuner.config.xtal = 24000;
		CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == 0);
		CHECK(bus.regs[2][0][0x62] == 0x18);
		tuner.config.xtal = 20500;
		CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == -EINVAL);
		CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == 0 && !locked);
		bus.regs[1][0x40][0x88] = 1;
		CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == 0 && !locked);
		bus.regs[1][0x40][0x10] = 0x20;
		CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == 0 && locked);
		bus.regs[1][0x40][0x88] = 0;
		CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == 0 && !locked);
		CHECK(cxd2856er_sleep(&demod) == 0 && !demod.j83b_active);
		CHECK(bus.regs[1][0x40][0x21] == 0);
		CHECK(bus.regs[1][0x40][0xb3] == 2);
		CHECK(bus.regs[1][0x40][0x1e] == 0);
		CHECK(bus.regs[1][0x40][0x8e] == 0x0e);
		CHECK(bus.regs[1][0x41][0xcf] == 0x77);
		CHECK(bus.regs[1][0x40][0x14] == 0x1f);
		CHECK(memcmp(&bus.regs[1][0x40][0x26], "\x09\x9a\x00\xee", 4) == 0);
		/* 全 wakeup I/O 境界で失敗を注入。次の T 選局で残留状態を解除。 */
		for (fail = 1; fail <= count; fail++) {
			reset(&demod, &tuner);
			memset(&params, 0, sizeof(params));
			params.isdbc.experimental_j83b = true;
			bus.fail_at = fail;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == -EIO);
			bus.fail_at = 0;
			params.bandwidth = 6;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == 0);
			CHECK(!demod.j83b_active);
		}
		printf("J83B register/lock/restore and %u wakeup I/O failure boundaries: PASS\n", count);
		/* 既存 C への復帰と J.83B 固有 sleep の各失敗境界。 */
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		params.isdbc.experimental_j83b = true;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
		count = bus.calls;
		CHECK(cxd2856er_sleep(&demod) == 0);
		count = bus.calls - count;
		for (fail = 1; fail <= count; fail++) {
			reset(&demod, &tuner);
			memset(&params, 0, sizeof(params));
			params.isdbc.experimental_j83b = true;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
			bus.fail_at = bus.calls + fail;
			CHECK(cxd2856er_sleep(&demod) == -EIO && demod.j83b_active);
			bus.fail_at = 0;
			memset(&params, 0, sizeof(params));
			params.isdbc.symbol_rate = 5274;
			params.isdbc.modulation = 256;
			params.isdbc.experimental_profile = true;
			params.isdbc.reference_profile = true;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
			CHECK(!demod.j83b_active && bus.regs[0][0][0x17] == 4);
		}
		printf("J83B %u sleep I/O failure boundaries and C restore: PASS\n", count);
		reset(&demod, &tuner);
		tuner.config.experimental_j83b = true;
		CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == 0);
		count = bus.calls;
		for (fail = 1; fail <= count; fail++) {
			reset(&demod, &tuner);
			tuner.config.experimental_j83b = true;
			bus.fail_at = fail;
			CHECK(cxd2858er_set_params_c(&tuner, 697143, 6) == -EIO);
			CHECK(!bus.gate_open);
		}
		printf("J83B %u tuner I/O failure boundaries and gate release: PASS\n", count);
		{
			/* バースト記録用の縮小モニター。非稼働時の拒否、値の復号、freeze 解除、
			 * 全 I/O 境界での失敗時に部分的な値を返さないことを確かめる。 */
			struct cxd2856er_j83b_rs_sample sample;
			unsigned int base;

			reset(&demod, &tuner);
			memset(&sample, 0xff, sizeof(sample));
			CHECK(cxd2856er_read_j83b_rs_sample(&demod, &sample) == -EINVAL);
			CHECK(sample.rs_packet_errors == 0 && !sample.rs_valid && sample.if_agc_raw == 0);
			memset(&params, 0, sizeof(params));
			params.isdbc.experimental_j83b = true;
			CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
			bus.regs[1][0x40][0x88] = 0x35;
			bus.regs[1][0x40][0x10] = 0x20;
			bus.regs[1][0x40][0x49] = 0xf4;	/* 上位 4 bit は IF AGC に含めない */
			bus.regs[1][0x40][0x4a] = 0x88;
			bus.regs[1][0x40][0xea] = 0x01;
			bus.regs[1][0x40][0xeb] = 0x17;
			bus.regs[1][0x40][0xec] = 0x01;
			base = bus.calls;
			CHECK(cxd2856er_read_j83b_rs_sample(&demod, &sample) == 0);
			count = bus.calls - base;
			CHECK(sample.ar_status_raw == 0x35 && sample.ts_status_raw == 0x20);
			CHECK(sample.if_agc_raw == 0x488);
			CHECK(sample.rs_packet_errors == 0x117 && sample.rs_valid);
			CHECK(bus.regs[1][0x40][0x01] == 0);
			bus.regs[1][0x40][0xec] = 0x00;
			CHECK(cxd2856er_read_j83b_rs_sample(&demod, &sample) == 0 && !sample.rs_valid);
			for (fail = 1; fail <= count; fail++) {
				bus.fail_at = bus.calls + fail;
				memset(&sample, 0xff, sizeof(sample));
				CHECK(cxd2856er_read_j83b_rs_sample(&demod, &sample) == -EIO);
				CHECK(sample.rs_packet_errors == 0 && !sample.rs_valid && sample.if_agc_raw == 0);
				bus.fail_at = 0;
			}
			printf("J83B burst sample decode and %u I/O failure boundaries: PASS\n", count);
		}
		{
			/* チューナーの RSSI 読み取り。未選局での拒否、0x59/0x87/0x88 の書き戻し、
			 * 換算値、全 I/O 境界での失敗とゲートの解放を確かめる。模擬バスは 0x5b の
			 * 切り替えを再現しないため、IF AGC と RF AGC は同じ値になる。 */
			struct cxd2858er_rssi rssi;
			unsigned int base;

			reset(&demod, &tuner);
			CHECK(cxd2858er_read_rssi_t(&tuner, 327250, &rssi) == -EINVAL);
			tuner.config.experimental_j83b = true;
			CHECK(cxd2858er_set_params_c(&tuner, 327250, 6) == 0);
			bus.regs[2][0][0x59] = 0x2a;
			bus.regs[2][0][0x5b] = 60;
			CHECK(bus.regs[2][0][0x87] == 0xc0 && bus.regs[2][0][0x88] == 0x00);
			CHECK((bus.regs[2][0][0x69] & 0x0f) == 0x04);
			base = bus.calls;
			CHECK(cxd2858er_read_rssi_t(&tuner, 327250, &rssi) == 0);
			count = bus.calls - base;
			CHECK(rssi.if_agc == 60 && rssi.rf_agc == 60 && rssi.if_bpf_gc == 4);
			/* ifgain = 870 + 2*100 + (769*(9945-8400)+1275)/2550 = 1536,
			 * rfgain = 4110 - 154 = 3956 (320-350 MHz の最大利得) */
			CHECK(rssi.rssi_x100 == -5492);
			CHECK(bus.regs[2][0][0x59] == 0x2a);
			CHECK(bus.regs[2][0][0x87] == 0xc0 && bus.regs[2][0][0x88] == 0x00);
			CHECK(!bus.gate_open);
			for (fail = 1; fail <= count; fail++) {
				bus.fail_at = bus.calls + fail;
				CHECK(cxd2858er_read_rssi_t(&tuner, 327250, &rssi) == -EIO);
				CHECK(!bus.gate_open);
				bus.fail_at = 0;
			}
			printf("Tuner RSSI decode, register restore and %u I/O failure boundaries: PASS\n", count);
		}
	}
#endif
	return 0;
}
