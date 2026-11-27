// SPDX-License-Identifier: GPL-2.0-only
/* 模擬 I2C で ISDB-C・J.83B の設定・停止と失敗時の復帰を検証する
 * PX-MLT 系のチューナー (CXD2858ER) と、XIT-SQR100 のチューナー (CXD6866AER) と TS クロックを対象にする
 * 信号ロック、USB 負荷、実際の TS 品質はこの試験の確認範囲外
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cxd2856er.h"
#include "cxd2858er.h"
#include "cxd6866.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct mock_bus {
	u8 bank[128];
	u8 regs[3][256][256];
	unsigned int calls;
	unsigned int fail_at;
	bool frozen;
	bool gate_open;
	/* CXD2858ER の ADC がつないだ AGC の値。0x59 へ書いた接続先に応じて 0x5B の読み出し値を変える */
	u8 tuner_if_agc;
	u8 tuner_rf_agc;
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

	bus->calls++;
	if (bus->calls == bus->fail_at)
		return -EIO;
	for (n = 0; n < num; n++) {
		int addr = req[n].addr;
		int chip = addr == 0x64 ? 0 : addr == 0x66 ? 1 : 2;
		u8 reg = req[n].data[0];

		if (req[n].req == I2C_READ_REQUEST) {
			CHECK(n > 0);
			reg = req[n - 1].data[0];
			for (i = 0; i < req[n].len; i++)
				req[n].data[i] = bus->regs[chip][bus->bank[addr]][(u8)(reg + i)];
		} else {
			for (i = 1; i < req[n].len; i++) {
				u8 at = (u8)(reg + i - 1);
				if (chip != 2 && at == 0)
					bus->bank[addr] = req[n].data[i];
				else
					bus->regs[chip][bus->bank[addr]][at] = req[n].data[i];
				if (addr == 0x66 && at == 1)
					bus->frozen = req[n].data[i] != 0;
				if (chip == 2 && at == 0x59)
					bus->regs[2][0][0x5b] = (req[n].data[i] == 0x05) ? bus->tuner_if_agc
							      : (req[n].data[i] == 0x03) ? bus->tuner_rf_agc : 0;
			}
		}
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

static void reset_freia(struct cxd6866_tuner *freia)
{
	memset(&bus, 0, sizeof(bus));
	memset(freia, 0, sizeof(*freia));
	freia->dev = &dev;
	freia->i2c = &master;
	freia->i2c_addr = 0x60;
	freia->config.xtal = 24000;
	freia->config.loop_filter_internal = true;
	freia->config.refout_enable = true;
	/* CXD6866AER のチップ ID (0x7F & 0xFC == 0xF8) */
	bus.regs[2][0][0x7f] = 0xf8;
}

/* 地上波の動作モードの選局で、方式ごとに異なるレジスターの値 */
static void check_freia_ter(bool j83b, u32 freq)
{
	u8 *r = bus.regs[2][0];

	/* Sony の freia_tune() は、ケーブルの方式だけ OVLD のレジスターを別の値にする */
	CHECK(memcmp(&r[0x3c], j83b ? "\xfc\x87\xcf" : "\xfc\x9c\x8f", 3) == 0);
	CHECK(r[0x52] == (j83b ? 0x78 : 0x00));
	CHECK(r[0x8b] == (j83b ? 0x05 : 0x75));
	/* g_terr_param_table_freia の SONY_FREIA_DTV_SKP_OPT と SONY_FREIA_DTV_ISDBT_6 の行 */
	CHECK(r[0x69] == (j83b ? 0x83 : 0x86));
	CHECK(r[0x6b] == (j83b ? 0x0b : 0x0e));
	CHECK(r[0x6c] == 0x33);
	CHECK(r[0x6d] == 0x00);
	CHECK(r[0x6e] == (j83b ? 0x1b : 0x17));
	CHECK(r[0x6f] == (j83b ? 0x02 : 0x1b));
	CHECK(r[0x70] == (u8)freq && r[0x71] == (u8)(freq >> 8) && r[0x72] == (u8)((freq >> 16) & 0x1f));
	CHECK(r[0x73] == 0xff);
	CHECK(memcmp(&r[0x91], "\x0a\x0c", 2) == 0);
	/* 選局の後は CPU と内部の論理回路を待機させる */
	CHECK(r[0x88] == 0x00 && r[0x87] == 0x80);
}

/* CXD2858ER の入力レベル (dBm × 1000)。期待値は Sony の sony_helene_ReadRssi() の式を手で計算した値で、
 * RF 利得の6区間と IF 利得の2区間、地上波とケーブルの補正を通す
 * 実際の電波の強さと合うかは、この試験の確認範囲外 */
static void test_rf_level(void)
{
	static const struct {
		enum cxd2858er_system system;
		u32 freq;
		u8 if_agc, rf_agc, if_bpf_gc;
		s32 level;
	} cases[] = {
		/* RF の AGC が IF より大きく、RF 利得は2つ目の傾きの区間、IF 利得は AGC が 0.39 V を超える区間 */
		{ CXD2858ER_ISDB_T_SYSTEM, 557143, 100, 150, 0x0a, -33930 },
		/* RF 利得は 70 × 2.2 だけ下げる平らな区間、IF 利得は AGC に比例する区間、ケーブルの補正は -1.5 dB */
		{ CXD2858ER_ISDB_C_SYSTEM, 93143, 50, 20, 0x04, -61340 },
		/* RF 利得は 57 dB/V の区間 */
		{ CXD2858ER_ISDB_T_SYSTEM, 767143, 200, 200, 0x00, -2160 },
		/* RF 利得は 160 dB/V の区間、利得の番号は上位4ビットを無視する */
		{ CXD2858ER_ISDB_C_SYSTEM, 50000, 255, 0, 0xff, 22020 },
		/* RF 利得は最初の傾きの区間 */
		{ CXD2858ER_ISDB_T_SYSTEM, 300000, 37, 37, 0x00, -69890 },
		/* RF 利得は最大値のまま */
		{ CXD2858ER_ISDB_T_SYSTEM, 300000, 30, 0, 0x00, -73630 },
	};
	struct cxd2856er_demod demod;
	struct cxd2858er_tuner tuner;
	unsigned int i, calls;
	s32 level, expected;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		level = 0;
		CHECK(cxd2858er_calc_rf_level(cases[i].system, cases[i].freq, cases[i].if_agc, cases[i].rf_agc, cases[i].if_bpf_gc, &level) == 0);
		if (level != cases[i].level) {
			fprintf(stderr, "case %u: level %d, expected %d\n", i, (int)level, (int)cases[i].level);
			exit(1);
		}
	}

	/* 衛星と未選局は dBm で求められないため失敗させる */
	CHECK(cxd2858er_calc_rf_level(CXD2858ER_ISDB_S_SYSTEM, 1049480, 100, 100, 0, &level) == -EINVAL);
	CHECK(cxd2858er_calc_rf_level(CXD2858ER_UNSPECIFIED_SYSTEM, 557143, 100, 100, 0, &level) == -EINVAL);
	puts("Tuner RF level calculation: PASS");

	/* 地上波の選局後に ADC の接続を切り替えて IF と RF の AGC を読み、選局後と同じ待機の設定へ戻す */
	reset(&demod, &tuner);
	CHECK(cxd2858er_init(&tuner) == 0);
	CHECK(cxd2858er_read_rf_level(&tuner, &level) == -EINVAL && !bus.gate_open);
	CHECK(cxd2858er_set_params_t(&tuner, CXD2858ER_ISDB_T_SYSTEM, 557143, 6) == 0);
	bus.tuner_if_agc = 100;
	bus.tuner_rf_agc = 150;
	/* 利得の番号 (0x69 の下位4ビット) は選局で書いた値を読む */
	CHECK(cxd2858er_calc_rf_level(CXD2858ER_ISDB_T_SYSTEM, 557143, 100, 150, bus.regs[2][0][0x69], &expected) == 0);
	bus.calls = 0;
	CHECK(cxd2858er_read_rf_level(&tuner, &level) == 0 && level == expected);
	calls = bus.calls;
	CHECK(calls == 9 && !bus.gate_open);
	CHECK(bus.regs[2][0][0x59] == 0x04 && bus.regs[2][0][0x88] == 0x00 && bus.regs[2][0][0x87] == 0xc0);
	CHECK(tuner.system == CXD2858ER_ISDB_T_SYSTEM);

	/* どの I2C 転送で失敗してもゲートを閉じ、失敗していない待機の設定は書き戻し、選局の状態は変えない */
	for (i = 1; i <= calls; i++) {
		bus.regs[2][0][0x87] = 0xc0;
		bus.regs[2][0][0x88] = 0x00;
		bus.calls = 0;
		bus.fail_at = i;
		CHECK(cxd2858er_read_rf_level(&tuner, &level) == -EIO && !bus.gate_open);
		CHECK(i == 8 || bus.regs[2][0][0x87] == 0xc0);
		CHECK(i == 7 || bus.regs[2][0][0x88] == 0x00);
		CHECK(tuner.system == CXD2858ER_ISDB_T_SYSTEM);
	}
	bus.fail_at = 0;

	/* J.83B もケーブルの補正で求め、周波数は最後の選局の値を使う */
	CHECK(cxd2858er_set_params_c(&tuner, 697143, true) == 0);
	CHECK(cxd2858er_calc_rf_level(CXD2858ER_ISDB_C_SYSTEM, 697143, 100, 150, bus.regs[2][0][0x69], &expected) == 0);
	CHECK(cxd2858er_read_rf_level(&tuner, &level) == 0 && level == expected);

	/* 衛星の選局中と停止後は読まない */
	CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	bus.calls = 0;
	CHECK(cxd2858er_read_rf_level(&tuner, &level) == -EINVAL && bus.calls == 0);
	CHECK(cxd2858er_stop(&tuner) == 0);
	bus.calls = 0;
	CHECK(cxd2858er_read_rf_level(&tuner, &level) == -EINVAL && bus.calls == 0);
	printf("Tuner RF level read with %u I2C failure boundaries: PASS\n", calls);
}

static void test_xit_sqr100(void)
{
	struct cxd6866_tuner freia;
	struct cxd2856er_demod demod;
	struct cxd2858er_tuner unused_tuner;
	union cxd2856er_system_params params;
	unsigned int count, fail, i;
	static const struct {
		enum cxd2856er_system system;
		u8 c4, d1, d9, ck, d3;
	} clocks[] = {
		/* Sony の連続クロックの表の、高い周波数・ハーフレート (地デジ) と中間の周波数・フルレート (BS/CS・ケーブル) */
		{ CXD2856ER_ISDB_T_SYSTEM, 0x02, 0x02, 0x10, 0x00, 0x01 },
		{ CXD2856ER_ISDB_S_SYSTEM, 0x01, 0x01, 0x08, 0x01, 0x01 },
		{ CXD2856ER_ISDB_C_SYSTEM, 0x01, 0x01, 0x08, 0x01, 0x01 },
		/* J.83B だけパケット間の隙間を挿入する (OTSRATECTRLOFF = 0) */
		{ CXD2856ER_J83B_SYSTEM, 0x01, 0x01, 0x08, 0x01, 0x00 },
	};

	/* J.83B (5.6Msps) と ISDB-T の地上波の動作モードの値 */
	reset_freia(&freia);
	CHECK(cxd6866_init(&freia) == 0 && !bus.gate_open);
	CHECK(bus.regs[2][0][0x81] == 0x18 && bus.regs[2][0][0x84] == 0x83);
	CHECK(cxd6866_set_params_j83b(&freia, 597250) == 0);
	CHECK(freia.system == CXD6866_J83B_SYSTEM && !bus.gate_open);
	check_freia_ter(true, 597250);
	CHECK(cxd6866_set_params_t(&freia, CXD6866_ISDB_T_SYSTEM, 557143, 6) == 0);
	CHECK(freia.system == CXD6866_ISDB_T_SYSTEM && !bus.gate_open);
	check_freia_ter(false, 557143);
	CHECK(cxd6866_set_params_j83b(&freia, 99250) == 0);
	check_freia_ter(true, 99250);
	puts("CXD6866 J83B 5.6Msps and ISDB-T terrestrial settings: PASS");

	/* 地上波 (J.83B) と衛星の動作モードの切り替えでは、先に前のモードの回路を止める */
	reset_freia(&freia);
	CHECK(cxd6866_init(&freia) == 0);
	CHECK(cxd6866_set_params_j83b(&freia, 597250) == 0);
	CHECK(cxd6866_set_params_s(&freia, CXD6866_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	CHECK(freia.system == CXD6866_ISDB_S_SYSTEM);
	/* TER_fin で OVLD のレジスターを戻し、地上波の回路を待機させている */
	CHECK(bus.regs[2][0][0x3d] == 0 && bus.regs[2][0][0x3e] == 0 && bus.regs[2][0][0x5e] == 0x15);
	CHECK(bus.regs[2][0][0x01] == 0x01);
	CHECK(cxd6866_set_params_j83b(&freia, 597250) == 0);
	CHECK(freia.system == CXD6866_J83B_SYSTEM);
	/* SAT_fin で衛星の回路を待機させてから地上波のモードへ戻している */
	CHECK(bus.regs[2][0][0x0c] == 0x14 && bus.regs[2][0][0x01] == 0x00);
	check_freia_ter(true, 597250);
	CHECK(cxd6866_term(&freia) == 0);
	CHECK(freia.system == CXD6866_UNSPECIFIED_SYSTEM && !bus.gate_open);
	CHECK(bus.regs[2][0][0x3d] == 0 && bus.regs[2][0][0x74] == 0x02 && bus.regs[2][0][0x5e] == 0x15);
	puts("CXD6866 J83B <-> ISDB-S switching and term: PASS");

	/* 範囲外の周波数はハードウェアへ触れずに失敗する */
	reset_freia(&freia);
	CHECK(cxd6866_set_params_j83b(&freia, 999) == -ERANGE);
	CHECK(cxd6866_set_params_j83b(&freia, 1200001) == -ERANGE);
	CHECK(bus.calls == 0 && freia.system == CXD6866_UNSPECIFIED_SYSTEM);
	puts("CXD6866 J83B frequency range: PASS");

	/* 選局の途中で I2C が失敗したときは、状態を不明に戻してゲートを閉じる */
	reset_freia(&freia);
	CHECK(cxd6866_set_params_j83b(&freia, 597250) == 0);
	count = bus.calls;
	for (fail = 1; fail <= count; fail++) {
		reset_freia(&freia);
		freia.system = CXD6866_ISDB_S_SYSTEM;
		bus.fail_at = fail;
		CHECK(cxd6866_set_params_j83b(&freia, 597250) == -EIO);
		CHECK(freia.system == CXD6866_UNSPECIFIED_SYSTEM && !bus.gate_open);
	}
	printf("CXD6866 J83B %u tuner I/O failure boundaries and gate release: PASS\n", count);

	/* XIT-SQR100 のシリアルの連続クロック (Windows ドライバーと同じ地デジ・BS/CS の値) と、ケーブルの方式の値 */
	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		reset(&demod, &unused_tuner);
		demod.config.serial_ts_clock = true;
		memset(&params, 0, sizeof(params));
		params.bandwidth = 6;
		bus.regs[1][0][0xd1] = 0x03;
		CHECK(cxd2856er_wakeup(&demod, clocks[i].system, &params) == 0);
		CHECK((bus.regs[1][0][0xc4] & 0x03) == clocks[i].c4);
		CHECK((bus.regs[1][0][0xd1] & 0x03) == clocks[i].d1);
		CHECK(bus.regs[1][0][0xd9] == clocks[i].d9);
		CHECK((bus.regs[1][0][0x33] & 0x03) == clocks[i].ck);
		CHECK((bus.regs[1][0][0xd3] & 0x01) == clocks[i].d3);
		CHECK((bus.regs[1][0][0x32] & 0x01) == 0x01);
	}
	puts("XIT-SQR100 serial continuous TS clock for ISDB-T/S/C and J83B: PASS");
}

int main(void)
{
	struct cxd2856er_demod demod;
	struct cxd2858er_tuner tuner;
	union cxd2856er_system_params params;
	enum cxd2856er_system system;
	unsigned int xtal, mode, sync, ts, count, fail;
	bool locked;
	u16 cnr;

	for (xtal = 16000; xtal <= 24000; xtal += 8000) {
		for (mode = 0; mode < 2; mode++) {
			reset(&demod, &tuner);
			memset(&params, 0, sizeof(params));
			system = mode ? CXD2856ER_J83B_SYSTEM : CXD2856ER_ISDB_C_SYSTEM;
			bus.regs[1][0][0xd1] = 0x01;
			bus.regs[1][0x49][0x36] = 0x13;
			tuner.config.xtal = xtal;
			CHECK(cxd2856er_wakeup(&demod, system, &params) == 0);
			CHECK(cxd2858er_set_params_c(&tuner, 697143, mode != 0) == 0);
			/* チューナーは sony_helene.c の CXD2856 世代の DVB-C 6 MHz と J.83B 5.6 Msps の設定 */
			CHECK(bus.regs[2][0][0x62] == (xtal == 16000 ? 0x10 : 0x18));
			CHECK(bus.regs[2][0][0x69] == 0x84);
			CHECK(bus.regs[2][0][0x6e] == (mode ? 0x1b : 0x1a));
			CHECK(bus.regs[2][0][0x6f] == (mode ? 0x02 : 0x1c));
			/* TS クロックは PLEX の CXD2856.c が PX-MLT 系に設定するゲート式・ハーフレート・高い周波数 */
			CHECK((bus.regs[1][0][0xc4] & 0x03) == 0);
			CHECK((bus.regs[1][0][0xd1] & 0x03) == 2);
			CHECK(bus.regs[1][0][0xd9] == 0x10);
			CHECK((bus.regs[1][0][0x33] & 0x03) == 0);
			CHECK((bus.regs[1][0][0xd3] & 0x01) == (mode ? 0 : 1));
			if (!mode) {
				/* sony_demod_isdbc.c の SetTSMFSetting() と SLtoAIC() の値 */
				CHECK(bus.regs[1][0x49][0x36] == 0x03);
				CHECK(bus.regs[0][0][0x17] == 0x09);
				CHECK(bus.regs[1][0x11][0x6a] == 0x48);
				CHECK(memcmp(&bus.regs[1][0x11][0x33], "\x00\x03\x3b", 3) == 0);
				CHECK(bus.regs[1][0x48][0x2c] == 0x03);
				CHECK(memcmp(&bus.regs[1][0x10][0xa6], "\x31\xa8\x29\x9b\x27\x9c\x28\x9e\x29\xa4\x29\xa2\x29\xa8", 14) == 0);
				CHECK(memcmp(&bus.regs[1][0x10][0xb6], "\x13\xbb\xbc", 3) == 0);
				CHECK(bus.regs[1][0x11][0xa3] == 0x14);
				CHECK(bus.regs[1][0x40][0x14] == 0x14);
				CHECK(bus.regs[1][0x40][0x1e] == 0x80);
				CHECK(memcmp(&bus.regs[1][0x40][0x26], "\x07\x08\x07\x08", 4) == 0);
			}
			CHECK(cxd2856er_sleep(&demod) == 0);
			if (!mode) {
				/* sony_demod_isdbc.c の AICtoSL() で既定値へ戻す値 */
				CHECK(bus.regs[1][0x11][0xa3] == 0x00);
				CHECK(bus.regs[1][0x48][0x2c] == 0x01);
				CHECK(bus.regs[1][0x40][0x14] == 0x1f);
				CHECK(bus.regs[1][0x40][0x1e] == 0x00);
				CHECK(bus.regs[0][0][0x18] == 0x01 && bus.regs[0][0][0x17] == 0x01);
				CHECK(bus.regs[1][0][0x49] == 0x33 && bus.regs[1][0][0x4b] == 0x21 && bus.regs[1][0][0x2c] == 0x00);
			}
			CHECK(cxd2858er_stop(&tuner) == 0 && !bus.gate_open);
		}
	}
	puts("ISDBC, J83B and 16/24 MHz REF_R: PASS");

	/* ISDB-C の起動が途中で失敗しても、地上波の起動の前に ISDB-C の設定を既定値へ戻す */
	reset(&demod, &tuner);
	memset(&params, 0, sizeof(params));
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
	count = bus.calls;
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		bus.fail_at = fail;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == -EIO);
		CHECK(demod.state == CXD2856ER_ACTIVE_STATE && demod.system == CXD2856ER_ISDB_C_SYSTEM);
		bus.fail_at = 0;
		bus.regs[1][0x48][0x2c] = 0x03;
		bus.regs[1][0x40][0x14] = 0x14;
		params.bandwidth = 6;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == 0);
		CHECK(bus.regs[1][0x48][0x2c] == 0x01 && bus.regs[1][0x40][0x14] == 0x1f);
	}
	printf("ISDBC %u wakeup I/O failure boundaries and restore before ISDB-T: PASS\n", count);

	/* ISDB-C の設定を戻せなかったときは、地上波を起動せずに ISDB-C の状態を残す */
	reset(&demod, &tuner);
	memset(&params, 0, sizeof(params));
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
	count = bus.calls;
	CHECK(cxd2856er_sleep(&demod) == 0);
	count = bus.calls - count;
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
		bus.fail_at = bus.calls + fail;
		params.bandwidth = 6;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == -EIO);
		CHECK(demod.state == CXD2856ER_ACTIVE_STATE && demod.system == CXD2856ER_ISDB_C_SYSTEM);
		bus.fail_at = 0;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == 0);
		CHECK(demod.system == CXD2856ER_ISDB_T_SYSTEM && bus.regs[1][0x48][0x2c] == 0x01);
	}
	printf("ISDBC %u sleep I/O failure boundaries before ISDB-T: PASS\n", count);

	for (mode = 0; mode < 2; mode++) {
		for (sync = 0; sync < 2; sync++) {
			for (ts = 0; ts < 2; ts++) {
				reset(&demod, &tuner);
				demod.system = mode ? CXD2856ER_J83B_SYSTEM : CXD2856ER_ISDB_C_SYSTEM;
				bus.regs[1][0x40][0x88] = (u8)sync;
				bus.regs[1][0x40][0x10] = ts ? 0x20 : 0;
				CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == 0);
				CHECK(locked == (sync && ts));
				CHECK(bus.calls == (sync ? 3U : 2U));
			}
		}
		for (fail = 1; fail <= 3; fail++) {
			reset(&demod, &tuner);
			demod.system = mode ? CXD2856ER_J83B_SYSTEM : CXD2856ER_ISDB_C_SYSTEM;
			bus.regs[1][0x40][0x88] = 1;
			bus.regs[1][0x40][0x10] = 0x20;
			bus.fail_at = fail;
			locked = true;
			CHECK(cxd2856er_is_ts_locked_isdbc(&demod, &locked) == -EIO);
			CHECK(!locked && bus.calls == fail);
		}
	}
	puts("ISDBC/J83B QAM and TS lock states and I/O failures: PASS");

	for (fail = 0; fail <= 4; fail++) {
		reset(&demod, &tuner);
		bus.regs[1][0x40][0x4c] = 0xe1;
		bus.regs[1][0x40][0x4d] = 0x23;
		bus.fail_at = fail;
		CHECK(cxd2856er_read_cnr_raw_isdbc(&demod, &cnr) == (fail ? -EIO : 0));
		CHECK(bus.frozen == (fail == 4));
		if (!fail)
			CHECK(cnr == 0x123);
	}
	puts("ISDBC C/N raw mask and unfreeze on I/O failure: PASS");

	reset(&demod, &tuner);
	tuner.system = CXD2858ER_ISDB_C_SYSTEM;
	CHECK(cxd2858er_stop(&tuner) == 0);
	CHECK(tuner.system == CXD2858ER_UNSPECIFIED_SYSTEM && !bus.gate_open);
	CHECK(bus.regs[2][0][0x5e] == 0x15 && bus.regs[2][0][0x87] == 0xc0);
	count = bus.calls;
	reset(&demod, &tuner);
	tuner.system = CXD2858ER_ISDB_C_SYSTEM;
	CHECK(cxd2858er_term(&tuner) == 0);
	CHECK(tuner.system == CXD2858ER_UNSPECIFIED_SYSTEM && !bus.gate_open);
	reset(&demod, &tuner);
	tuner.system = CXD2858ER_ISDB_C_SYSTEM;
	CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	CHECK(tuner.system == CXD2858ER_ISDB_S_SYSTEM);
	CHECK(bus.regs[2][0][0x5e] == 0x15 && !bus.gate_open);
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		tuner.system = CXD2858ER_ISDB_C_SYSTEM;
		bus.fail_at = fail;
		CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == -EIO);
		CHECK(tuner.system == CXD2858ER_ISDB_C_SYSTEM && !bus.gate_open);
		CHECK(bus.regs[2][0][0x15] == 0);
	}
	puts("Cable tuner stop, term, C to S and stop I/O failure boundaries: PASS");

	reset(&demod, &tuner);
	memset(&params, 0, sizeof(params));
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
	CHECK(demod.system == CXD2856ER_J83B_SYSTEM);
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
	CHECK(cxd2856er_sleep(&demod) == 0 && demod.system == CXD2856ER_UNSPECIFIED_SYSTEM);
	CHECK(bus.regs[1][0x40][0x21] == 0);
	CHECK(bus.regs[1][0x40][0xb3] == 2);
	CHECK(bus.regs[1][0x40][0x1e] == 0);
	CHECK(bus.regs[1][0x40][0x8e] == 0x0e);
	CHECK(bus.regs[1][0x41][0xcf] == 0x77);
	CHECK(bus.regs[1][0x40][0x14] == 0x1f);
	CHECK(memcmp(&bus.regs[1][0x40][0x26], "\x09\x9a\x00\xee", 4) == 0);

	/* 起動途中で失敗した場合も、地上波選局時に J.83B 固有の設定を解除する必要がある */
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		bus.fail_at = fail;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == -EIO);
		bus.fail_at = 0;
		params.bandwidth = 6;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == 0);
		CHECK(demod.system == CXD2856ER_ISDB_T_SYSTEM && bus.regs[1][0x40][0x21] == 0);
	}
	printf("J83B register/restore and %u wakeup I/O failure boundaries: PASS\n", count);

	reset(&demod, &tuner);
	memset(&params, 0, sizeof(params));
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
	count = bus.calls;
	CHECK(cxd2856er_sleep(&demod) == 0);
	count = bus.calls - count;
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
		bus.fail_at = bus.calls + fail;
		CHECK(cxd2856er_sleep(&demod) == -EIO && demod.system == CXD2856ER_J83B_SYSTEM);
		bus.fail_at = 0;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_C_SYSTEM, &params) == 0);
		CHECK(demod.system == CXD2856ER_ISDB_C_SYSTEM && bus.regs[0][0][0x17] == 0x09);
	}
	printf("J83B %u sleep I/O failure boundaries and C restore: PASS\n", count);

	/* Sony の ACCtoACC() と同じく、J.83B のまま選局し直すときは停止せず、TS の出力を止めて帯域の設定だけを書き直す */
	reset(&demod, &tuner);
	memset(&params, 0, sizeof(params));
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
	bus.regs[1][0][0xc3] = 0;
	bus.regs[1][0x10][0xb6] = 0;
	bus.regs[1][0x40][0x14] = 0;
	memset(&bus.regs[1][0x40][0x26], 0, 4);
	count = bus.calls;
	CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
	count = bus.calls - count;
	CHECK(demod.system == CXD2856ER_J83B_SYSTEM && demod.state == CXD2856ER_ACTIVE_STATE);
	CHECK(bus.regs[1][0][0xc3] == 0x01);
	CHECK(bus.regs[1][0x10][0xb6] == 0x14);
	CHECK(bus.regs[1][0x40][0x14] == 0x10);
	CHECK(memcmp(&bus.regs[1][0x40][0x26], "\x07\x77\x07\x77", 4) == 0);
	/* 停止してから起動し直しても同じ値になるため、ACCtoACC() と同じ7回の書き込み (バンクの選択を含む) だけで済んだことで、停止していないと判断する */
	CHECK(count == 7);
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == 0);
		bus.fail_at = bus.calls + fail;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_J83B_SYSTEM, &params) == -EIO);
		bus.fail_at = 0;
		/* 失敗した後も J.83B 固有の設定を解除できる */
		params.bandwidth = 6;
		CHECK(cxd2856er_wakeup(&demod, CXD2856ER_ISDB_T_SYSTEM, &params) == 0);
		CHECK(demod.system == CXD2856ER_ISDB_T_SYSTEM && bus.regs[1][0x40][0x21] == 0);
	}
	printf("J83B retune without sleep and %u I/O failure boundaries: PASS\n", count);

	/* 起動の途中で I2C が失敗した後は、同じ方式の選局でも選局し直しで済ませず、停止してから完全に起動し直す */
	for (mode = 0; mode < 2; mode++) {
		unsigned int full, retry;

		reset(&demod, &tuner);
		memset(&params, 0, sizeof(params));
		system = mode ? CXD2856ER_J83B_SYSTEM : CXD2856ER_ISDB_C_SYSTEM;
		CHECK(cxd2856er_wakeup(&demod, system, &params) == 0);
		full = bus.calls;
		CHECK(!demod.cable_wakeup_incomplete);
		for (fail = 1; fail <= full; fail++) {
			reset(&demod, &tuner);
			memset(&params, 0, sizeof(params));
			bus.fail_at = fail;
			CHECK(cxd2856er_wakeup(&demod, system, &params) == -EIO);
			CHECK(demod.cable_wakeup_incomplete && demod.state == CXD2856ER_ACTIVE_STATE);
			bus.fail_at = 0;
			retry = bus.calls;
			CHECK(cxd2856er_wakeup(&demod, system, &params) == 0);
			retry = bus.calls - retry;
			/* 停止の分を加えて、起動の書き込みをすべて通している */
			CHECK(retry > full);
			CHECK(!demod.cable_wakeup_incomplete && demod.system == system);
		}
	}
	puts("ISDBC/J83B full wakeup after interrupted wakeup: PASS");

	reset(&demod, &tuner);
	CHECK(cxd2858er_set_params_c(&tuner, 697143, true) == 0);
	count = bus.calls;
	for (fail = 1; fail <= count; fail++) {
		reset(&demod, &tuner);
		bus.fail_at = fail;
		CHECK(cxd2858er_set_params_c(&tuner, 697143, true) == -EIO);
		CHECK(!bus.gate_open);
	}
	printf("J83B %u tuner I/O failure boundaries and gate release: PASS\n", count);

	/* 既定の待機状態 (PLEX の公開ドライバーと同じ) では、地デジと BS/CS の利用者のチューナーは待機中も RF 入力の整合を保たない (0x67 bit1 が 0) */
	reset(&demod, &tuner);
	CHECK(cxd2858er_init(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x00 && bus.regs[2][0][0x5e] == 0x15);
	bus.regs[2][0][0x67] |= 0x01;
	CHECK(cxd2858er_set_params_t(&tuner, CXD2858ER_ISDB_T_SYSTEM, 557143, 6) == 0);
	CHECK(cxd2858er_stop(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x01);
	CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	CHECK(cxd2858er_stop(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x01);

	/* rfin_matching のときは待機中も RF 入力の整合を保ち (0x67 bit1)、動作中は同じビットの LT_AMP_EN を 0 にし、bit0 の RF_EXT は保持する */
	reset(&demod, &tuner);
	tuner.config.ter.rfin_matching = true;
	CHECK(cxd2858er_init(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x02 && bus.regs[2][0][0x5e] == 0x15);
	bus.regs[2][0][0x67] |= 0x01;
	CHECK(cxd2858er_set_params_c(&tuner, 697143, true) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x01);
	CHECK(cxd2858er_stop(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x03 && bus.regs[2][0][0x5e] == 0x15);
	CHECK(cxd2858er_set_params_t(&tuner, CXD2858ER_ISDB_T_SYSTEM, 557143, 6) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x01);
	CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x03);

	/* 衛星の受信中に rfin_matching になったチューナーも、待機へ戻るときに整合を保つ状態にそろえる */
	reset(&demod, &tuner);
	CHECK(cxd2858er_init(&tuner) == 0);
	CHECK(cxd2858er_set_params_s(&tuner, CXD2858ER_ISDB_S_SYSTEM, 1049480, 28860) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x00);
	tuner.config.ter.rfin_matching = true;
	CHECK(cxd2858er_stop(&tuner) == 0);
	CHECK(bus.regs[2][0][0x67] == 0x02);
	/* 開いただけのチューナーに整合が要求された場合は、RF_EXT を保持して待機中に反映する。 */
	reset(&demod, &tuner);
	CHECK(cxd2858er_init(&tuner) == 0);
	bus.regs[2][0][0x67] = 0x01;
	tuner.config.ter.rfin_matching = true;
	bus.fail_at = bus.calls + 2;
	CHECK(cxd2858er_apply_standby_rfin_matching(&tuner) == -EIO);
	CHECK(!tuner.standby_rfin_matching && !bus.gate_open);
	CHECK(bus.regs[2][0][0x67] == 0x01);

	/* 選局前に反映できなかった場合は、未選局のまま閉じるときにも再試行する。 */
	CHECK(cxd2858er_term(&tuner) == 0);
	CHECK(tuner.standby_rfin_matching && !bus.gate_open);
	CHECK(bus.regs[2][0][0x67] == 0x03);
	count = bus.calls;
	CHECK(cxd2858er_apply_standby_rfin_matching(&tuner) == 0);
	CHECK(bus.calls == count);

	CHECK(cxd2858er_set_params_t(&tuner, CXD2858ER_ISDB_T_SYSTEM, 557143, 6) == 0);
	count = bus.calls;
	CHECK(cxd2858er_apply_standby_rfin_matching(&tuner) == 0);
	CHECK(bus.calls == count && bus.regs[2][0][0x67] == 0x01);
	puts("Tuner RF input matching in standby: PASS");

	test_rf_level();
	test_xit_sqr100();
	return 0;
}
