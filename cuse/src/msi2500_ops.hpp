// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <libusb.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// MSi2500 (Msi2500Device) のうち、USB の機器を使わずに試験できる処理。
// helper の ops の実行と出力の解釈、等時転送の完了したパケットからのブロックの取り出しを、
// 模擬の helper と組み立てた libusb_transfer で試験する (cuse/tests/msi2500_ops_test.cpp)
namespace px4::cuse::msi2500 {
// 1 回の制御転送 (bmRequestType 0x40、wLength 0)
struct ControlOp {
    std::uint8_t request = 0;
    std::uint16_t value = 0, index = 0;
};

// helper の生ブロックの単位。等時転送のパケットはこの長さの倍数だけを helper へ渡す
constexpr std::size_t Block = 1024;

// helper の ops の標準出力 (1 行に "setup|stop <request> <value> <index>"、16 進数) を、取り込みの前に送る列 (setup) と
// 停止要求の後に送る列 (stop) へ分ける。不正な行、または setup が空の場合は false を返し、理由を error へ入れる
bool ParseOps(const std::string &text, std::vector<ControlOp> &setup, std::vector<ControlOp> &stop, std::string &error);

// helper の ops を起動し、標準出力を limit まで読んで、終了を回収する。
// 終了コード 0 で終わったら 0、起動できなければ -errno (spawned は false)、それ以外は -EIO を返す。
// status は waitpid の状態で、回収できなかった場合は -1
int RunOps(const std::vector<std::string> &args, std::chrono::milliseconds limit, std::string &text, bool &spawned,
           int &status);

// 1 回の等時転送のパケットの内訳
struct PacketCounts {
    std::uint64_t packets = 0, bad = 0, empty = 0, odd = 0;
};

// 完了した等時転送から、状態が完了で空でないパケットの 1024 バイトのブロックだけを blocks へ集める。
// 1 パケットの通信エラーで転送ごと捨てると欠落が転送のパケット数倍に広がるため、パケットごとに除く。
// 除いたブロックは helper の OFDM がブロック番号の欠落として扱う。端数のあるパケットは端数を捨てて odd に数える
void CollectBlocks(const libusb_transfer &transfer, std::vector<std::uint8_t> &blocks, PacketCounts &counts);
}
