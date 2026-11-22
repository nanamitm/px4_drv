// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "pxbcud/pat_gate.hpp"
#include "pxbcud/plain_framer.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace px4::cuse {
// PX-BCUD の isochronous のパケットを 188 バイトの TS へ組み立て、選んだ TSID の PAT を確かめるまで配信しない。
// WinUSB 版の Receiver::RunStream と同じ処理を、libusb の完了の通知から 1 転送ずつ呼べる形にしたもの。
// libusb のイベントのスレッドだけから使う (受信の開始と再開は、転送がすべて回収された後に行う)。
class PxBcudStream {
public:
    // WinUSB 版で実測した alternate setting 1 の endpoint 0x84 の最大長。
    static constexpr std::size_t PacketSize = 940;

    explicit PxBcudStream(std::uint16_t tsid) : gate_(tsid) {}

    // 同じ選局の受信の再開 (自動復旧)。確認済みの PAT は保持し、TS の端数と PAT の断片を捨てる。
    // WinUSB 版も再開ごとに PlainFramer を作り直すため、計数も受信の区間ごとに数え直す。
    void Restart()
    {
        gate_.Restart();
        framer_ = pxbcud::PlainFramer{};
        iso_errors = empty_packets = startup_discarded = 0;
        output_.clear();
    }

    // 1 パケット分を渡す。ok は libusb のパケットの status が成功か。
    // 長さが endpoint の最大長を超える場合は false を返す (WinUSB 版の packet_descriptor の失敗)。
    bool Packet(bool ok, const std::uint8_t *data, std::size_t length)
    {
        if (!ok) {
            // 欠けたマイクロフレームの前後を連結すると、TS の境界がずれた 188 バイトを配信してしまう。
            ++iso_errors;
            framer_.Gap();
            return true;
        }
        if (!length) {
            ++empty_packets;
            return true;
        }
        if (length > PacketSize)
            return false;
        framer_.Feed(data, length, output_);
        return true;
    }

    // 1 転送分の TS を deliver へ渡す。PAT を確かめる前のパケットは捨てて startup_discarded に数える。
    // 開いた後は残り全体を 1 回で渡す (WinUSB 版と同じく、配信のロックの取得を 1 転送に 1 回にする)。
    template <class Deliver>
    std::size_t Flush(Deliver &&deliver)
    {
        std::size_t begin = 0;
        while (begin < output_.size() && !gate_.Accept(output_.data() + begin)) {
            ++startup_discarded;
            begin += 188;
        }
        const std::size_t size = output_.size() - begin;
        if (size)
            deliver(output_.data() + begin, size);
        output_.clear();
        return size;
    }

    bool Ready() const { return gate_.Ready(); }
    std::uint64_t Packets() const { return framer_.packets; }
    std::uint64_t Tei() const { return framer_.tei; }
    std::uint64_t DiscardedBytes() const { return framer_.discarded; }

    std::uint64_t iso_errors = 0, empty_packets = 0, startup_discarded = 0;

private:
    pxbcud::PatGate gate_;
    pxbcud::PlainFramer framer_;
    std::vector<std::uint8_t> output_;
};
}
