#include <cstdio>
#include <vector>

#include "ts_sync.h"
#include "ts_plain_framer.h"

namespace {

bool Check(const char *name, bool actual, bool expected)
{
	if (actual == expected)
		return true;
	std::fprintf(stderr, "%s failed. actual: %d, expected: %d\n",
		name, actual ? 1 : 0, expected ? 1 : 0);
	return false;
}

void Collect(void *ctx, const unsigned char *data, std::size_t size)
{
	auto &out = *static_cast<std::vector<unsigned char> *>(ctx);
	out.insert(out.end(), data, data + size);
}

bool TestPlainFramer()
{
	std::vector<unsigned char> ts(12 * PX4_TS_PACKET_SIZE, 0xff), out;
	for (std::size_t i = 0; i < 12; ++i) {
		ts[i * PX4_TS_PACKET_SIZE] = 0x47;
		ts[i * PX4_TS_PACKET_SIZE + 1] = static_cast<unsigned char>(i);
	}
	px4_ts_plain_framer framer{};
	for (std::size_t split = 1; split < ts.size(); ++split) {
		out.clear();
		px4_ts_plain_reset(&framer);
		px4_ts_plain_feed(&framer, ts.data(), split, Collect, &out);
		px4_ts_plain_feed(&framer, ts.data() + split, ts.size() - split, Collect, &out);
		if (out != ts)
			return false;
	}

	// 1バイトずつ届く場合も、破損した同期の後に正常なパケットだけを出力する。
	auto damaged = ts;
	damaged[5 * PX4_TS_PACKET_SIZE] = 0x46;
	auto expected = ts;
	expected.erase(expected.begin() + 5 * PX4_TS_PACKET_SIZE, expected.begin() + 6 * PX4_TS_PACKET_SIZE);
	out.clear();
	px4_ts_plain_reset(&framer);
	for (const auto byte : damaged)
		px4_ts_plain_feed(&framer, &byte, 1, Collect, &out);
	if (out != expected)
		return false;

	out.clear();
	px4_ts_plain_feed(&framer, ts.data(), 100, Collect, &out);
	px4_ts_plain_reset(&framer);
	px4_ts_plain_feed(&framer, ts.data(), ts.size(), Collect, &out);
	return out == ts;
}

} // namespace

int main()
{
	bool succeeded = true;
	succeeded = Check("plain framing across USB splits, sync loss and reset", TestPlainFramer(), true) && succeeded;
	/* 通常 TS は0x47だけを同期バイトとして受理する */
	succeeded = Check("plain valid", px4_ts_has_plain_sync(0x47) != 0, true) && succeeded;
	succeeded = Check("plain corrupted", px4_ts_has_plain_sync(0x46) != 0, false) && succeeded;

	/* 多重 TS は有効なチューナータグを残し、同期値の破損を拒否する */
	succeeded = Check("tagged receiver 1", px4_ts_has_tagged_sync(0x17) != 0, true) && succeeded;
	succeeded = Check("tagged receiver 4", px4_ts_has_tagged_sync(0x47) != 0, true) && succeeded;
	succeeded = Check("tagged receiver 7", px4_ts_has_tagged_sync(0x77) != 0, true) && succeeded;
	succeeded = Check("tagged corrupted", px4_ts_has_tagged_sync(0x46) != 0, false) && succeeded;
	succeeded = Check("tagged error bit", px4_ts_has_tagged_sync(0x87) != 0, false) && succeeded;

	if (succeeded)
		std::puts("TS sync condition test: passed");
	return succeeded ? 0 : 1;
}
