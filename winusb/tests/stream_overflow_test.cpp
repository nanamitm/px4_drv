// SPDX-License-Identifier: GPL-2.0-only
// 受信バッファが満杯のとき、入りきらない TS をパケット単位で捨て、読み出し側の境界を保つことを確かめる。
#include "receiver_base.hpp"
#include "ringbuffer.hpp"
#include "stream_overflow.hpp"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

static std::vector<std::string> messages;
extern "C" int msg_printf(const char *format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	messages.emplace_back(text);
	return 0;
}

static void Check(bool ok, const char *reason)
{
	if (!ok) {
		std::cerr << reason << "\n";
		ExitProcess(1);
	}
}

// 番号を埋め込んだ TS パケットを count 個並べる。
static std::vector<std::uint8_t> Packets(unsigned first, unsigned count)
{
	std::vector<std::uint8_t> bytes(188 * count);
	for (unsigned i = 0; i < count; ++i) {
		std::uint8_t *p = bytes.data() + 188 * i;
		p[0] = 0x47;
		std::memset(p + 1, static_cast<int>(first + i), 187);
	}
	return bytes;
}

static void CheckPackets(const std::vector<std::uint8_t> &bytes, unsigned first, unsigned count, const char *reason)
{
	Check(bytes.size() == 188 * count, reason);
	for (unsigned i = 0; i < count; ++i) {
		const std::uint8_t *p = bytes.data() + 188 * i;
		Check(p[0] == 0x47, reason);
		for (unsigned j = 1; j < 188; ++j)
			Check(p[j] == static_cast<std::uint8_t>(first + i), reason);
	}
}

// 容量が188の倍数でなくても、満杯のときはパケットの途中まで書かない。
static void RingBufferTests()
{
	px4::RingBuffer ring(188 * 4 + 100);
	ring.Start();
	auto first = Packets(0, 4);
	std::size_t size = first.size();
	Check(ring.Write(first.data(), size, 188) && size == first.size(), "ring accepts packets that fit");

	auto next = Packets(4, 1);
	size = next.size();
	Check(!ring.Write(next.data(), size, 188) && size == 0, "ring wrote part of a packet into 100 free bytes");

	std::vector<std::uint8_t> read(188);
	size = read.size();
	Check(ring.Read(read.data(), size) && size == 188, "ring read one packet");
	CheckPackets(read, 0, 1, "ring first packet");

	// 空きは288バイト。2パケットのうち1つだけが入る。
	auto two = Packets(4, 2);
	size = two.size();
	Check(!ring.Write(two.data(), size, 188) && size == 188, "ring did not trim to whole packets");

	read.assign(188 * 4, 0);
	size = read.size();
	Check(ring.Read(read.data(), size) && size == 188 * 4, "ring read remaining packets");
	CheckPackets(read, 1, 4, "ring packet order after overflow");

	// 既定の単位では従来どおり空きいっぱいまで書く。
	px4::RingBuffer bytes(100);
	bytes.Start();
	auto one = Packets(0, 1);
	size = one.size();
	Check(!bytes.Write(one.data(), size) && size == 100, "default unit changed partial write");
}

// 複数パケットをまとめて書いても、StreamBuffer は入りきるパケットだけを渡し、残りを失ったバイトとして数える。
static void StreamBufferTests()
{
	px4::ReceiverBase::StreamBuffer stream;
	Check(stream.Alloc(188 * 8 + 50), "stream alloc");
	stream.Start();

	auto head = Packets(0, 6);
	std::size_t size = head.size();
	Check(stream.Write(head.data(), size) && size == head.size(), "stream accepts packets that fit");

	auto tail = Packets(6, 4);
	size = tail.size();
	Check(!stream.Write(tail.data(), size) && size == 188 * 2, "stream did not trim bulk write to whole packets");
	const auto diagnostics = stream.GetWriteDiagnostics();
	Check(diagnostics.rejected_bytes == 188 * 2 && diagnostics.rejected_writes == 1, "stream rejected byte count");

	std::vector<std::uint8_t> received;
	stream.HandleRead(188 * 3, [&](const void *buf, std::size_t length) {
		const auto *p = static_cast<const std::uint8_t *>(buf);
		received.insert(received.end(), p, p + length);
		return received.size() < 188 * 8;
	});
	CheckPackets(received, 0, 8, "stream packet boundary after overflow");
	stream.Stop();
}

// 満杯の開始と解消だけを記録し、利用者の Purge による短縮は満杯として扱わない。
static void OverflowLogTests()
{
	messages.clear();
	px4::StreamOverflowLog log("test");
	log.Record(true, 188, 0);
	Check(messages.empty(), "purge discard logged as overflow");
	log.Record(false, 188 * 3, 188);
	log.Record(false, 188, 0);
	Check(messages.size() == 1 && messages[0].find("TS buffer full") != std::string::npos, "overflow start logged once");
	log.Record(true, 188, 0);
	Check(messages.size() == 1, "purge during overflow ended it");
	log.Record(true, 188, 188);
	Check(messages.size() == 2 && messages[1].find("dropped_bytes=564") != std::string::npos, "overflow end and dropped bytes");
	log.Record(true, 188, 188);
	Check(messages.size() == 2, "normal write logged");
}

int main()
{
	RingBufferTests();
	StreamBufferTests();
	OverflowLogTests();
	std::cout << "stream overflow tests passed\n";
	return 0;
}
