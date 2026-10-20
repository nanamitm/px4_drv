// SPDX-License-Identifier: GPL-2.0-only
/* J.83B 録画中の受信機へ、同じデバイスの別受信機の Open・選局・Close が
 * 与える影響を再現するための実機試験ツール。
 *
 *   victim    録画役。J.83B を受信し続け、CC エラーを時刻付きで記録する。
 *   aggressor 操作役。1 つのシナリオ (Open・選局・Close) を繰り返し、各操作の時刻を記録する。
 *   analyze   録画役と操作役の記録を照合し、操作ごとの CC エラーを集計する。
 *   suite     録画役を子プロセスで起動し、既定のシナリオを順に実行して集計まで行う。
 *
 * 受信機の固定は BonDriver の ini (ReceiverDefinition の DeviceGUID と Index) で行う。
 * 実機の受信機を占有するため、録画予約のない時間帯に実行する前提である。
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "IBonDriver2.h"

namespace {

/* 録画役を早めに止めるための名前付きイベント。suite と手動停止の両方で使う。 */
const wchar_t kStopEventName[] = L"Local\\px4_j83b_interference_stop";

std::atomic_bool g_stop(false);

BOOL WINAPI ConsoleHandler(DWORD)
{
	g_stop = true;
	return TRUE;
}

/* 別プロセスの記録と DriverHost のログを照合できるよう、壁時計の時刻を使う。 */
std::int64_t NowUnixMs()
{
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	ULARGE_INTEGER u;
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return static_cast<std::int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

/* DriverHost_PX4.log と同じ「年/月/日 時:分:秒.ミリ秒」のローカル時刻。 */
std::string LocalStamp(std::int64_t unix_ms)
{
	ULARGE_INTEGER u;
	u.QuadPart = static_cast<ULONGLONG>(unix_ms) * 10000ULL + 116444736000000000ULL;
	FILETIME ft;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;
	SYSTEMTIME utc, local;
	FileTimeToSystemTime(&ft, &utc);
	SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04u/%02u/%02u %02u:%02u:%02u.%03u",
		local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
	return buf;
}

class BonDriverHandle final {
public:
	BonDriverHandle() = default;
	~BonDriverHandle() { Unload(); }
	BonDriverHandle(const BonDriverHandle &) = delete;
	BonDriverHandle &operator=(const BonDriverHandle &) = delete;

	bool Load(const wchar_t *path)
	{
		module_ = LoadLibraryW(path);
		if (!module_) {
			std::fprintf(stderr, "Failed to load BonDriver. error: %lu\n", GetLastError());
			return false;
		}
		auto create = reinterpret_cast<IBonDriver *(*)()>(GetProcAddress(module_, "CreateBonDriver"));
		if (!create) {
			std::fprintf(stderr, "CreateBonDriver was not found.\n");
			return false;
		}
		bon_ = static_cast<IBonDriver2 *>(create());
		return bon_ != nullptr;
	}

	void Unload()
	{
		if (bon_) {
			bon_->CloseTuner();
			bon_->Release();
			bon_ = nullptr;
		}
		if (module_) {
			FreeLibrary(module_);
			module_ = nullptr;
		}
	}

	IBonDriver2 *operator->() const { return bon_; }
	IBonDriver2 *Get() const { return bon_; }

private:
	HMODULE module_ = nullptr;
	IBonDriver2 *bon_ = nullptr;
};

/* ---- 録画役 -------------------------------------------------------------- */

int RunVictim(const wchar_t *dll, DWORD space, DWORD channel, unsigned int seconds, const wchar_t *csv_path)
{
	FILE *csv = nullptr;
	if (_wfopen_s(&csv, csv_path, L"w") || !csv) {
		std::fprintf(stderr, "Cannot open %ls\n", csv_path);
		return 2;
	}
	std::fprintf(csv, "type,unix_ms,local_time,a,b,c,d\n");

	BonDriverHandle bon;
	if (!bon.Load(dll) || !bon->OpenTuner()) {
		std::fprintf(stderr, "victim: failed to open tuner.\n");
		std::fclose(csv);
		return 3;
	}
	if (!bon->SetChannel(space, channel)) {
		std::fprintf(stderr, "victim: failed to tune. space=%lu channel=%lu\n", space, channel);
		std::fclose(csv);
		return 4;
	}
	bon->PurgeTsStream();

	HANDLE stop_event = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
	const std::int64_t begin = NowUnixMs();
	const std::int64_t end = seconds ? begin + static_cast<std::int64_t>(seconds) * 1000 : INT64_MAX;
	std::fprintf(stderr, "victim: recording space=%lu channel=%lu from %s\n", space, channel, LocalStamp(begin).c_str());

	std::array<int, 8192> last_counter;
	last_counter.fill(-1);
	std::uint64_t total_packets = 0, total_sync = 0, total_tei = 0, total_cc = 0;
	std::uint64_t tick_packets = 0, tick_sync = 0, tick_tei = 0, tick_cc = 0;
	std::int64_t tick_begin = begin;
	unsigned int empty_ticks = 0;

	/* J83B_VICTIM_SIGNAL_MS (20-10000) を指定した場合だけ、GetSignalLevel の値を
	 * その間隔で sig 行として残す。地デジ・BS のように誤りが出ない方式でも、他受信機の
	 * 操作で C/N が下がるかを比べるため。読み出しは共有制御経路の I/O を増やすため既定は無効。 */
	unsigned int signal_ms = 0;
	{
		wchar_t value[16] = {};
		if (GetEnvironmentVariableW(L"J83B_VICTIM_SIGNAL_MS", value, 16) > 0) {
			const unsigned long v = std::wcstoul(value, nullptr, 10);
			if (v >= 20 && v <= 10000)
				signal_ms = static_cast<unsigned int>(v);
		}
	}
	std::int64_t signal_next = begin;
	if (signal_ms)
		std::fprintf(stderr, "victim: signal level every %u ms\n", signal_ms);

	while (!g_stop) {
		const std::int64_t now = NowUnixMs();
		if (now >= end)
			break;
		if (stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
			break;

		bon->WaitTsStream(50);
		while (bon->GetReadyCount()) {
			BYTE *buffer = nullptr;
			DWORD size = 0, remain = 0;
			if (!bon->GetTsStream(&buffer, &size, &remain) || !buffer)
				break;
			const std::int64_t t = NowUnixMs();

			/* channel_switch_stress と同じ判定。範囲外を読まずに 188 バイト単位で検査する。 */
			for (DWORD offset = 0; offset + 188 <= size; offset += 188) {
				const BYTE *packet = buffer + offset;
				tick_packets++;
				if (packet[0] != 0x47) {
					tick_sync++;
					continue;
				}
				if (packet[1] & 0x80)
					tick_tei++;
				const std::uint16_t pid = static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
				const std::uint8_t adaptation = (packet[3] >> 4) & 0x03;
				const std::uint8_t counter = packet[3] & 0x0f;
				if (!adaptation || ((adaptation & 0x02) && packet[4] >= 184)) {
					tick_sync++;
					continue;
				}
				if ((adaptation & 0x02) && packet[4] >= 1 && (packet[5] & 0x80))
					last_counter[pid] = -1;
				if ((adaptation & 0x01) && pid != 0x1fff) {
					const int expected = (last_counter[pid] + 1) & 0x0f;
					if (last_counter[pid] >= 0 && counter != expected) {
						tick_cc++;
						std::fprintf(csv, "cc,%lld,%s,%u,%d,%u,0\n",
							static_cast<long long>(t), LocalStamp(t).c_str(), pid, expected, counter);
					}
					last_counter[pid] = counter;
				}
			}
		}

		/* 100 ms ごとに受信量と誤りを残し、空受信の区間も分かるようにする。 */
		const std::int64_t t = NowUnixMs();
		if (signal_ms && t >= signal_next) {
			/* 値は 0.01 dB 単位の整数で残す。読み出しにかかった時間も d 列に記録する。 */
			const float level = bon->GetSignalLevel();
			const std::int64_t t2 = NowUnixMs();
			std::fprintf(csv, "sig,%lld,%s,%lld,0,0,%lld\n", static_cast<long long>(t), LocalStamp(t).c_str(),
				static_cast<long long>(level * 100.0f + (level >= 0 ? 0.5f : -0.5f)), static_cast<long long>(t2 - t));
			signal_next = t + signal_ms;
		}
		if (t - tick_begin >= 100) {
			std::fprintf(csv, "tick,%lld,%s,%llu,%llu,%llu,%llu\n",
				static_cast<long long>(t), LocalStamp(t).c_str(),
				static_cast<unsigned long long>(tick_packets), static_cast<unsigned long long>(tick_sync),
				static_cast<unsigned long long>(tick_tei), static_cast<unsigned long long>(tick_cc));
			if (!tick_packets)
				empty_ticks++;
			total_packets += tick_packets;
			total_sync += tick_sync;
			total_tei += tick_tei;
			total_cc += tick_cc;
			tick_packets = tick_sync = tick_tei = tick_cc = 0;
			tick_begin = t;
		}
	}

	/* 最後の 100 ms 未満の区間も合計に含める。 */
	total_packets += tick_packets;
	total_sync += tick_sync;
	total_tei += tick_tei;
	total_cc += tick_cc;

	std::fflush(csv);
	std::fclose(csv);
	if (stop_event)
		CloseHandle(stop_event);
	std::printf("victim: packets=%llu sync_errors=%llu transport_errors=%llu continuity_errors=%llu empty_100ms=%u\n",
		static_cast<unsigned long long>(total_packets), static_cast<unsigned long long>(total_sync),
		static_cast<unsigned long long>(total_tei), static_cast<unsigned long long>(total_cc), empty_ticks);
	return 0;
}

/* ---- 操作役 -------------------------------------------------------------- */

struct ScenarioSpec {
	std::string name;
	enum class Kind { OpenOnly, OpenTune, TuneOnly } kind;
	DWORD space;
	DWORD channel;
	DWORD alt_channel;
};

/* 1 回の操作を実行し、開始・終了の時刻を記録する。 */
template <class F>
bool Act(FILE *csv, const std::string &label, F &&f)
{
	const std::int64_t b = NowUnixMs();
	const bool ok = f();
	const std::int64_t e = NowUnixMs();
	std::fprintf(csv, "%s,%lld,%lld,%s,%d,\n", label.c_str(), static_cast<long long>(b), static_cast<long long>(e),
		LocalStamp(b).c_str(), ok ? 1 : 0);
	std::fflush(csv);
	std::printf("%s %s %s (%lld ms)\n", LocalStamp(b).c_str(), label.c_str(), ok ? "ok" : "FAILED",
		static_cast<long long>(e - b));
	return ok;
}

void SleepInterruptible(unsigned int ms)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!g_stop && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

/* 各操作の後に hold_ms 待ち、次の繰り返しまで gap_ms 空ける。gap_ms は録画役の
 * 影響が収まるまでの間隔で、集計の窓 (analyze の window) より長くする。 */
unsigned int RunScenario(IBonDriver2 *bon, FILE *csv, const ScenarioSpec &s, unsigned int repeats,
			 unsigned int hold_ms, unsigned int gap_ms)
{
	unsigned int failures = 0;
	const std::string p = s.name + ":";

	if (s.kind == ScenarioSpec::Kind::TuneOnly) {
		if (!Act(csv, p + "setup-open", [&] { return bon->OpenTuner() != FALSE; }))
			return repeats;
		Act(csv, p + "setup-tune", [&] { return bon->SetChannel(s.space, s.channel) != FALSE; });
		SleepInterruptible(gap_ms);
	}

	for (unsigned int r = 0; r < repeats && !g_stop; r++) {
		switch (s.kind) {
		case ScenarioSpec::Kind::OpenOnly:
			if (!Act(csv, p + "open", [&] { return bon->OpenTuner() != FALSE; })) {
				failures++;
				break;
			}
			SleepInterruptible(hold_ms);
			Act(csv, p + "close", [&] { bon->CloseTuner(); return true; });
			break;

		case ScenarioSpec::Kind::OpenTune:
			if (!Act(csv, p + "open", [&] { return bon->OpenTuner() != FALSE; })) {
				failures++;
				break;
			}
			if (!Act(csv, p + "tune", [&] { return bon->SetChannel(s.space, s.channel) != FALSE; }))
				failures++;
			SleepInterruptible(hold_ms);
			Act(csv, p + "close", [&] { bon->CloseTuner(); return true; });
			break;

		case ScenarioSpec::Kind::TuneOnly:
		{
			/* 開いたまま 2 つの周波数を交互に選局し、Open を伴わない選局だけを見る。 */
			const DWORD ch = (r % 2) ? s.channel : s.alt_channel;
			if (!Act(csv, p + ((r % 2) ? "tune-a" : "tune-b"), [&] { return bon->SetChannel(s.space, ch) != FALSE; }))
				failures++;
			SleepInterruptible(hold_ms);
			break;
		}
		}
		SleepInterruptible(gap_ms);
	}

	if (s.kind == ScenarioSpec::Kind::TuneOnly)
		Act(csv, p + "setup-close", [&] { bon->CloseTuner(); return true; });
	return failures;
}

bool ParseKind(const wchar_t *s, ScenarioSpec::Kind &kind)
{
	if (!wcscmp(s, L"open"))
		kind = ScenarioSpec::Kind::OpenOnly;
	else if (!wcscmp(s, L"open-tune"))
		kind = ScenarioSpec::Kind::OpenTune;
	else if (!wcscmp(s, L"tune"))
		kind = ScenarioSpec::Kind::TuneOnly;
	else
		return false;
	return true;
}

/* ---- 集計 ---------------------------------------------------------------- */

struct CcEvent { std::int64_t t; unsigned int pid; };
struct Action { std::string label; std::int64_t begin; std::int64_t end; bool ok; };

bool LoadVictim(const wchar_t *path, std::vector<CcEvent> &cc, std::int64_t &first, std::int64_t &last,
		std::vector<std::pair<std::int64_t, std::uint64_t>> &ticks,
		std::vector<std::pair<std::int64_t, double>> &sig)
{
	FILE *f = nullptr;
	if (_wfopen_s(&f, path, L"r") || !f)
		return false;
	char line[512];
	first = INT64_MAX;
	last = 0;
	while (std::fgets(line, sizeof(line), f)) {
		char type[16] = {};
		long long t = 0;
		char stamp[32] = {};
		unsigned long long a = 0, b = 0, c = 0, d = 0;
		if (sscanf_s(line, "%15[^,],%lld,%31[^,],%llu,%llu,%llu,%llu", type, static_cast<unsigned>(sizeof(type)), &t,
			stamp, static_cast<unsigned>(sizeof(stamp)), &a, &b, &c, &d) < 4)
			continue;
		first = std::min<std::int64_t>(first, t);
		last = std::max<std::int64_t>(last, t);
		if (!std::strcmp(type, "cc"))
			cc.push_back({ t, static_cast<unsigned int>(a) });
		else if (!std::strcmp(type, "tick"))
			ticks.emplace_back(t, a);
		else if (!std::strcmp(type, "sig"))
			sig.emplace_back(t, static_cast<double>(static_cast<long long>(a)) / 100.0);
	}
	std::fclose(f);
	return true;
}

bool LoadActions(const wchar_t *path, std::vector<Action> &actions)
{
	FILE *f = nullptr;
	if (_wfopen_s(&f, path, L"r") || !f)
		return false;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		char label[128] = {};
		long long b = 0, e = 0;
		char stamp[32] = {};
		int ok = 0;
		if (sscanf_s(line, "%127[^,],%lld,%lld,%31[^,],%d", label, static_cast<unsigned>(sizeof(label)), &b, &e,
			stamp, static_cast<unsigned>(sizeof(stamp)), &ok) == 5)
			actions.push_back({ label, b, e, ok != 0 });
	}
	std::fclose(f);
	return true;
}

/* 各操作の開始から window_ms 以内の CC エラーをその操作に数える。どの操作の窓にも
 * 入らない区間を基準とし、同じ長さの窓あたりの期待値と比べる。 */
int Analyze(const wchar_t *victim_csv, const wchar_t *aggressor_csv, unsigned int window_ms, FILE *out)
{
	std::vector<CcEvent> cc;
	std::vector<std::pair<std::int64_t, std::uint64_t>> ticks;
	std::vector<std::pair<std::int64_t, double>> sig;
	std::vector<Action> actions;
	std::int64_t first = 0, last = 0;
	if (!LoadVictim(victim_csv, cc, first, last, ticks, sig) || !LoadActions(aggressor_csv, actions)) {
		std::fprintf(stderr, "Cannot read input files.\n");
		return 2;
	}
	if (ticks.empty()) {
		std::fprintf(stderr, "Victim log has no samples.\n");
		return 2;
	}

	struct Row { unsigned int trials = 0, failed = 0, hit = 0; std::uint64_t cc = 0, cc_max = 0; std::vector<std::int64_t> first_delay; };
	std::map<std::string, Row> rows;
	std::vector<bool> attributed(cc.size(), false);
	std::int64_t covered_ms = 0;
	std::int64_t covered_until = 0;

	for (const Action &a : actions) {
		/* 準備の操作も記録には残すが、窓には含めて基準から除外する。 */
		Row &row = rows[a.label];
		row.trials++;
		if (!a.ok)
			row.failed++;
		const std::int64_t wb = a.begin, we = a.begin + window_ms;
		std::uint64_t n = 0;
		std::int64_t first_t = -1;
		for (std::size_t i = 0; i < cc.size(); i++) {
			if (cc[i].t >= wb && cc[i].t < we) {
				n++;
				attributed[i] = true;
				if (first_t < 0)
					first_t = cc[i].t - wb;
			}
		}
		if (n) {
			row.hit++;
			row.cc += n;
			row.cc_max = std::max<std::uint64_t>(row.cc_max, n);
			row.first_delay.push_back(first_t);
		}
		const std::int64_t s = std::max(wb, covered_until);
		if (we > s)
			covered_ms += we - s;
		covered_until = std::max(covered_until, we);
	}

	std::uint64_t baseline_cc = 0;
	for (std::size_t i = 0; i < cc.size(); i++)
		if (!attributed[i])
			baseline_cc++;
	const std::int64_t total_ms = std::max<std::int64_t>(1, last - first);
	const std::int64_t baseline_ms = std::max<std::int64_t>(1, total_ms - covered_ms);
	unsigned int empty_ticks = 0;
	for (const auto &t : ticks)
		if (!t.second)
			empty_ticks++;

	std::fprintf(out, "victim span: %s - %s (%lld s), cc_errors=%zu, empty_100ms=%u\n",
		LocalStamp(first).c_str(), LocalStamp(last).c_str(), static_cast<long long>(total_ms / 1000), cc.size(), empty_ticks);
	std::fprintf(out, "window: %u ms after each action begin\n", window_ms);
	std::fprintf(out, "baseline (outside all windows): %llu cc errors in %lld s, expected per window %.3f\n\n",
		static_cast<unsigned long long>(baseline_cc), static_cast<long long>(baseline_ms / 1000),
		static_cast<double>(baseline_cc) * window_ms / static_cast<double>(baseline_ms));
	std::fprintf(out, "%-32s %6s %6s %6s %8s %6s %14s\n", "action", "trials", "failed", "hit", "cc_total", "cc_max", "first_cc_ms");
	for (auto &kv : rows) {
		Row &r = kv.second;
		std::string delay = "-";
		if (!r.first_delay.empty()) {
			std::sort(r.first_delay.begin(), r.first_delay.end());
			delay = std::to_string(r.first_delay.front()) + ".." + std::to_string(r.first_delay.back());
		}
		std::fprintf(out, "%-32s %6u %6u %6u %8llu %6llu %14s\n", kv.first.c_str(), r.trials, r.failed, r.hit,
			static_cast<unsigned long long>(r.cc), static_cast<unsigned long long>(r.cc_max), delay.c_str());
	}

	if (sig.empty())
		return 0;

	/* 信号レベルの落ち込み: 窓の開始前 1 秒の中央値から、窓内の最小値までの差 (dB)。
	 * 操作の窓に入らない時刻を起点にした同じ計算を基準として並べる。 */
	auto drop_at = [&sig, window_ms](std::int64_t wb, double &drop) {
		std::vector<double> pre;
		double low = 1e9;
		for (const auto &s : sig) {
			if (s.first >= wb - 1000 && s.first < wb)
				pre.push_back(s.second);
			else if (s.first >= wb && s.first < wb + window_ms)
				low = std::min(low, s.second);
		}
		if (pre.empty() || low > 1e8)
			return false;
		std::sort(pre.begin(), pre.end());
		drop = pre[pre.size() / 2] - low;
		return true;
	};
	auto in_window = [&actions, window_ms](std::int64_t t) {
		for (const Action &a : actions)
			if (t >= a.begin - 1000 && t < a.begin + window_ms)
				return true;
		return false;
	};

	std::vector<double> all;
	for (const auto &s : sig)
		all.push_back(s.second);
	std::sort(all.begin(), all.end());

	std::vector<double> base;
	for (const auto &s : sig) {
		double d = 0;
		if (!in_window(s.first) && !in_window(s.first + window_ms) && drop_at(s.first, d))
			base.push_back(d);
	}
	std::sort(base.begin(), base.end());
	std::fprintf(out, "\nsignal level: samples=%zu median=%.2f min=%.2f max=%.2f\n",
		all.size(), all[all.size() / 2], all.front(), all.back());
	if (!base.empty())
		std::fprintf(out, "baseline drop (no action): windows=%zu median=%.2f p95=%.2f max=%.2f dB\n",
			base.size(), base[base.size() / 2], base[std::min(base.size() - 1, base.size() * 95 / 100)], base.back());

	struct SigRow { std::vector<double> drops; };
	std::map<std::string, SigRow> srows;
	for (const Action &a : actions) {
		double d = 0;
		if (drop_at(a.begin, d))
			srows[a.label].drops.push_back(d);
	}
	std::fprintf(out, "%-32s %6s %9s %9s %9s\n", "action", "n", "drop_avg", "drop_max", "n>=1dB");
	for (auto &kv : srows) {
		auto &v = kv.second.drops;
		double sum = 0, mx = 0;
		unsigned int over = 0;
		for (double d : v) {
			sum += d;
			mx = std::max(mx, d);
			if (d >= 1.0)
				over++;
		}
		std::fprintf(out, "%-32s %6zu %9.2f %9.2f %9u\n", kv.first.c_str(), v.size(), sum / v.size(), mx, over);
	}
	return 0;
}

/* ---- 一括実行 ------------------------------------------------------------ */

std::wstring JoinPath(const std::wstring &dir, const wchar_t *name)
{
	std::wstring p = dir;
	if (!p.empty() && p.back() != L'\\')
		p += L'\\';
	return p + name;
}

int RunSuite(int argc, wchar_t **argv)
{
	/* suite <victim.dll> <aggressor.dll> <space-c> <victim-ch> <other-ch> <space-t|-1> <t-ch> <repeats> <outdir> */
	if (argc != 11) {
		std::fprintf(stderr, "suite: wrong arguments.\n");
		return 2;
	}
	const wchar_t *victim_dll = argv[2], *aggressor_dll = argv[3];
	const DWORD space_c = wcstoul(argv[4], nullptr, 10);
	const DWORD victim_ch = wcstoul(argv[5], nullptr, 10);
	const DWORD other_ch = wcstoul(argv[6], nullptr, 10);
	const long space_t = wcstol(argv[7], nullptr, 10);
	const DWORD t_ch = wcstoul(argv[8], nullptr, 10);
	const unsigned int repeats = wcstoul(argv[9], nullptr, 10);
	const std::wstring outdir = argv[10];
	const unsigned int hold_ms = 5000, gap_ms = 10000, window_ms = 3000;

	CreateDirectoryW(outdir.c_str(), nullptr);
	const std::wstring victim_csv = JoinPath(outdir, L"victim.csv");
	const std::wstring aggressor_csv = JoinPath(outdir, L"aggressor.csv");
	const std::wstring summary_txt = JoinPath(outdir, L"summary.txt");

	HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
	if (!stop_event) {
		std::fprintf(stderr, "suite: cannot create stop event.\n");
		return 3;
	}
	ResetEvent(stop_event);

	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	/* 録画役の BonDriver は ISDB-C だけを扱うため、J.83B は常に空間 0 になる。 */
	std::wstring cmd = L"\"" + std::wstring(self) + L"\" victim \"" + victim_dll + L"\" 0 " +
		std::to_wstring(victim_ch) + L" 0 \"" + victim_csv + L"\"";
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
	cmdbuf.push_back(L'\0');
	if (!CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		std::fprintf(stderr, "suite: cannot start victim. error: %lu\n", GetLastError());
		CloseHandle(stop_event);
		return 3;
	}

	/* 録画役の選局が落ち着き、基準となる区間が取れるまで待つ。 */
	std::printf("suite: victim started (pid %lu); warming up 15 s\n", pi.dwProcessId);
	SleepInterruptible(15000);

	int ret = 0;
	if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
		std::fprintf(stderr, "suite: victim exited early.\n");
		ret = 4;
	} else {
		FILE *csv = nullptr;
		BonDriverHandle bon;
		if (_wfopen_s(&csv, aggressor_csv.c_str(), L"w") || !csv || !bon.Load(aggressor_dll)) {
			std::fprintf(stderr, "suite: cannot prepare aggressor.\n");
			ret = 5;
		} else {
			std::fprintf(csv, "type,begin_ms,end_ms,local_time,ok,detail\n");
			std::vector<ScenarioSpec> scenarios = {
				{ "c-open", ScenarioSpec::Kind::OpenOnly, space_c, other_ch, other_ch },
				{ "c-open-tune-same", ScenarioSpec::Kind::OpenTune, space_c, victim_ch, victim_ch },
				{ "c-open-tune-other", ScenarioSpec::Kind::OpenTune, space_c, other_ch, other_ch },
				{ "c-tune", ScenarioSpec::Kind::TuneOnly, space_c, victim_ch, other_ch },
			};
			if (space_t >= 0) {
				scenarios.push_back({ "t-open-tune", ScenarioSpec::Kind::OpenTune, static_cast<DWORD>(space_t), t_ch, t_ch });
			}
			for (const auto &s : scenarios) {
				if (g_stop)
					break;
				std::printf("suite: scenario %s x%u\n", s.name.c_str(), repeats);
				RunScenario(bon.Get(), csv, s, repeats, hold_ms, gap_ms);
			}
			std::fclose(csv);
		}
	}

	SetEvent(stop_event);
	if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0)
		std::fprintf(stderr, "suite: victim did not stop within 30 s.\n");
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(stop_event);

	if (ret)
		return ret;

	FILE *summary = nullptr;
	if (!_wfopen_s(&summary, summary_txt.c_str(), L"w") && summary) {
		Analyze(victim_csv.c_str(), aggressor_csv.c_str(), window_ms, summary);
		std::fclose(summary);
	}
	return Analyze(victim_csv.c_str(), aggressor_csv.c_str(), window_ms, stdout);
}

void Usage()
{
	std::fprintf(stderr,
		"Usage:\n"
		"  j83b_interference.exe victim <BonDriver.dll> <space> <channel> <seconds|0> <victim.csv>\n"
		"  j83b_interference.exe aggressor <BonDriver.dll> <open|open-tune|tune> <space> <channel> <alt-channel>\n"
		"                        <repeats> <hold-ms> <gap-ms> <aggressor.csv>\n"
		"  j83b_interference.exe analyze <victim.csv> <aggressor.csv> <window-ms>\n"
		"  j83b_interference.exe suite <victim.dll> <aggressor.dll> <space-c> <victim-ch> <other-ch>\n"
		"                        <space-t|-1> <t-ch> <repeats> <outdir>\n"
		"  victim stops at <seconds>, Ctrl+C, or when the suite signals " "Local\\px4_j83b_interference_stop.\n");
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	if (argc < 2) {
		Usage();
		return 2;
	}
	const std::wstring mode = argv[1];

	if (mode == L"victim" && argc == 7)
		return RunVictim(argv[2], wcstoul(argv[3], nullptr, 10), wcstoul(argv[4], nullptr, 10),
			wcstoul(argv[5], nullptr, 10), argv[6]);

	if (mode == L"aggressor" && argc == 11) {
		ScenarioSpec s;
		if (!ParseKind(argv[3], s.kind)) {
			Usage();
			return 2;
		}
		char name[64];
		std::snprintf(name, sizeof(name), "%ls", argv[3]);
		s.name = name;
		s.space = wcstoul(argv[4], nullptr, 10);
		s.channel = wcstoul(argv[5], nullptr, 10);
		s.alt_channel = wcstoul(argv[6], nullptr, 10);
		FILE *csv = nullptr;
		if (_wfopen_s(&csv, argv[10], L"w") || !csv) {
			std::fprintf(stderr, "Cannot open %ls\n", argv[10]);
			return 2;
		}
		std::fprintf(csv, "type,begin_ms,end_ms,local_time,ok,detail\n");
		BonDriverHandle bon;
		if (!bon.Load(argv[2])) {
			std::fclose(csv);
			return 3;
		}
		const unsigned int failures = RunScenario(bon.Get(), csv, s, wcstoul(argv[7], nullptr, 10),
			wcstoul(argv[8], nullptr, 10), wcstoul(argv[9], nullptr, 10));
		std::fclose(csv);
		return failures ? 1 : 0;
	}

	if (mode == L"analyze" && argc == 5)
		return Analyze(argv[2], argv[3], wcstoul(argv[4], nullptr, 10), stdout);

	if (mode == L"suite")
		return RunSuite(argc, argv);

	Usage();
	return 2;
}
