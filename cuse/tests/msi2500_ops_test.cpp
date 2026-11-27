// SPDX-License-Identifier: GPL-2.0-only
// MSi2500 の USB の機器を使わない処理 (msi2500_ops.hpp) を試す。
// helper の ops の出力の解釈 (不正な行と範囲外の値の拒否)、模擬の helper (fake_msi2500_ops.sh) による ops の実行
// (正常終了、EOF の後に回収できるまで間がある正常終了、異常終了、期限を過ぎた helper の強制終了、起動の失敗)、
// 組み立てた等時転送からのブロックの取り出し (失敗したパケット、空のパケット、端数のあるパケット) を確かめる。
#include "msi2500_ops.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <vector>

using namespace px4::cuse::msi2500;
namespace {
std::string helper;
void Require(bool ok, const char *what) {if (!ok) {std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1);}}

bool Same(const ControlOp &op, std::uint8_t request, std::uint16_t value, std::uint16_t index)
{
    return op.request == request && op.value == value && op.index == index;
}

bool Rejected(const char *text)
{
    std::vector<ControlOp> setup, stop;
    std::string error;
    return !ParseOps(text, setup, stop, error) && !error.empty();
}

void TestParse()
{
    // 前回の結果が残っていても、解釈し直した列だけになる
    std::vector<ControlOp> setup{ControlOp{1, 1, 1}}, stop{ControlOp{2, 2, 2}};
    std::string error;
    Require(ParseOps("setup 43 0 0\nsetup ff ffff ffff\nstop 45 0 0\n", setup, stop, error), "valid ops");
    Require(setup.size() == 2 && stop.size() == 1, "ops are split by stage");
    Require(Same(setup[0], 0x43, 0, 0) && Same(setup[1], 0xff, 0xffff, 0xffff) && Same(stop[0], 0x45, 0, 0),
            "ops values are hexadecimal");
    Require(Rejected("setup 100 0 0\n"), "request above 0xff");
    Require(Rejected("setup 1 10000 0\n"), "value above 0xffff");
    Require(Rejected("setup 1 0 10000\n"), "index above 0xffff");
    Require(Rejected("setup 1 2\n"), "missing field");
    Require(Rejected("setup 1 2 x\n"), "non-hexadecimal field");
    Require(Rejected("start 1 2 3\n"), "unknown stage");
    Require(Rejected("setup 1 2 3\n\nstop 4 5 6\n"), "blank line");
    Require(Rejected(""), "empty output");
    Require(Rejected("stop 45 0 0\n"), "no setup");
    std::printf("MSi2500 ops parsing: stages, hexadecimal values, range limits, malformed lines and missing setup passed\n");
}

int Run(const char *mode, std::chrono::milliseconds limit, std::string &text, bool &spawned, int &status, long long &ms)
{
    setenv("FAKE_MSI2500_OPS", mode, 1);
    const auto start = std::chrono::steady_clock::now();
    const int rc = RunOps({helper, "ops", "--frequency", "473142857", "--gain", "40"}, limit, text, spawned, status);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    return rc;
}

void TestRun()
{
    std::string text;
    bool spawned = false;
    int status = 0;
    long long ms = 0;
    const std::string expected = "setup 43 0 0\nsetup 9 abcd 1234\nstop 45 0 0\n";
    const auto limit = std::chrono::milliseconds(1000);

    Require(Run("normal", limit, text, spawned, status, ms) == 0 && spawned, "normal exit succeeds");
    Require(text == expected && WIFEXITED(status) && !WEXITSTATUS(status), "normal output and status");
    std::vector<ControlOp> setup, stop;
    std::string error;
    Require(ParseOps(text, setup, stop, error) && setup.size() == 2 && stop.size() == 1, "helper output parses");

    // 標準出力を閉じてから終わるまで間がある正常終了を、強制終了して失敗にしない
    for (int i = 0; i < 5; ++i) {
        Require(Run("linger", limit, text, spawned, status, ms) == 0, "exit after EOF succeeds");
        Require(text == expected && WIFEXITED(status) && !WEXITSTATUS(status) && ms >= 250, "waited for the exit after EOF");
    }

    Require(Run("fail", limit, text, spawned, status, ms) == -EIO && spawned, "nonzero exit fails");
    Require(WIFEXITED(status) && WEXITSTATUS(status) == 1 && text == expected, "nonzero exit status");

    Require(Run("hang", limit, text, spawned, status, ms) == -EIO, "hung helper fails");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && ms >= 900 && ms < 3000, "hung helper killed at the deadline");

    Require(Run("closed_hang", limit, text, spawned, status, ms) == -EIO, "helper that never exits after EOF fails");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && ms >= 900 && ms < 3000 && text == expected,
            "helper killed at the deadline after EOF");

    const int rc = RunOps({"/nonexistent/msi2500_frontend", "ops"}, limit, text, spawned, status);
    Require(rc == -ENOENT && !spawned && status == -1, "missing helper is not spawned");
    std::printf("MSi2500 ops helper: normal exit, exit after EOF, failure status, deadline kill and missing executable passed\n");
}

void TestBlocks()
{
    constexpr int packets = 5;
    constexpr int packet_size = 3 * static_cast<int>(Block);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(packets * packet_size));
    for (std::size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = static_cast<std::uint8_t>((i / packet_size) * 31 + i % 251);
    libusb_transfer *transfer = libusb_alloc_transfer(packets);
    Require(transfer != nullptr, "allocate transfer");
    transfer->buffer = buffer.data();
    transfer->length = static_cast<int>(buffer.size());
    transfer->num_iso_packets = packets;
    // 0: 完全なパケット、1: 失敗、2: 空、3: 端数 (2 ブロックと 452 バイト)、4: 1 ブロックだけ
    const int actual[packets] = {packet_size, packet_size, 0, 2500, static_cast<int>(Block)};
    const libusb_transfer_status states[packets] = {LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_ERROR,
        LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED};
    for (int i = 0; i < packets; ++i) {
        transfer->iso_packet_desc[i].length = static_cast<unsigned>(packet_size);
        transfer->iso_packet_desc[i].actual_length = static_cast<unsigned>(actual[i]);
        transfer->iso_packet_desc[i].status = states[i];
    }
    std::vector<std::uint8_t> expected(buffer.begin(), buffer.begin() + packet_size);
    expected.insert(expected.end(), buffer.begin() + 3 * packet_size, buffer.begin() + 3 * packet_size + 2 * Block);
    expected.insert(expected.end(), buffer.begin() + 4 * packet_size, buffer.begin() + 4 * packet_size + Block);

    // 前回の転送のブロックが残っていても、今回のパケットのブロックだけになる
    std::vector<std::uint8_t> blocks(7, 0xaa);
    PacketCounts counts;
    CollectBlocks(*transfer, blocks, counts);
    Require(blocks == expected, "only whole blocks of completed packets in order");
    Require(counts.packets == 5 && counts.bad == 1 && counts.empty == 1 && counts.odd == 1, "packet counts");

    // 全パケットが失敗した転送はブロックを渡さない。計数は前回の続きへ足す
    for (int i = 0; i < packets; ++i)
        transfer->iso_packet_desc[i].status = LIBUSB_TRANSFER_STALL;
    CollectBlocks(*transfer, blocks, counts);
    Require(blocks.empty() && counts.packets == 10 && counts.bad == 6, "failed transfer yields no blocks");
    transfer->buffer = nullptr;
    libusb_free_transfer(transfer);
    std::printf("MSi2500 isochronous blocks: whole blocks, failed, empty and odd packets passed\n");
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FAKE_MSI2500_OPS\n", argv[0]);
        return 2;
    }
    helper = argv[1];
    TestParse();
    TestRun();
    TestBlocks();
    return 0;
}
