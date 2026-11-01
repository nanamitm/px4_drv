// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace px4 {
// koreapyj/px4_drv の差分更新と共有ラッチの直列化を参考にした WinUSB 用実装。
// Writer は (register, bytes, length) を受け、成功時0、失敗時は負のerrnoを返す。
class HardwarePidFilter final {
public:
    enum class Mode { Off, Pass, Block };
    static constexpr std::size_t Capacity = 64;

    // 電源再初期化や従来のnullフィルター設定の後は、キャッシュから実機を推測できない。
    void Invalidate(unsigned port) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (port < states_.size()) states_[port].dirty = true;
    }

    template<class Writer>
    int Apply(unsigned port, Mode mode, const std::uint16_t* pids, std::size_t count, Writer write) {
        if (port >= states_.size() || count > Capacity || (count && !pids) ||
            (mode != Mode::Off && mode != Mode::Pass && mode != Mode::Block) ||
            (mode == Mode::Off && count) || (mode == Mode::Pass && !count)) return -EINVAL;
        std::array<int, Capacity> wanted; wanted.fill(-1);
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (pids[i] > 0x1fff) return -EINVAL;
            if (std::find(wanted.begin(), wanted.begin() + n, pids[i]) == wanted.begin() + n)
                wanted[n++] = pids[i];
        }
        // PID/enable/index はポート共通のラッチを使うため、1回のレジスター操作だけの
        // 排他では足りない。別ポートも含め、設定全体を1つのロックで守る。
        std::lock_guard<std::mutex> guard(mutex_);
        auto& s = states_[port];
        auto byte = [&](std::uint32_t reg, std::uint8_t value) { return write(reg, &value, 1); };
        auto set_mode = [&](Mode m) {
            static constexpr std::uint32_t regs[] = {0xda13,0xda25,0xda29,0xda2d,0xda7f};
            // 解除時は先にremap自体を止め、Passリストが一瞬Blockになるのを避ける。
            if (m == Mode::Off) {
                int e = byte(0xda73 + port, 1);
                return e ? e : byte(regs[port], 0);
            }
            int e = byte(regs[port], m == Mode::Pass ? 2 : 0);
            const std::uint8_t zero[2]{};
            if (!e) e = write(0xda81 + port * 2, zero, 2);
            if (!e) e = byte(0xda73 + port, 3);
            return e;
        };
        auto slot = [&](unsigned index, int pid) {
            static constexpr std::uint32_t regs[] = {0xda15,0xda26,0xda2a,0xda2e,0xda80};
            const std::uint16_t value = pid < 0 ? 0 : static_cast<std::uint16_t>(pid);
            const std::uint8_t data[] = {static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value >> 8)};
            int e = write(0xda16, data, 2);
            if (!e) e = byte(0xda14, pid < 0 ? 0 : 1);
            if (!e) e = byte(regs[port], static_cast<std::uint8_t>(index));
            return e;
        };
        auto fail = [&](int e) {
            // 途中まで書いたスロットを成功済みと見なさず、次回は全スロットを再構成する。
            // 可能なら選別を解除するが、解除自体のI/O失敗もあるため成功には変換しない。
            s.dirty = true;
            (void)set_mode(Mode::Off);
            return e;
        };
        auto current = s.slots;
        int e = 0;
        if (s.dirty) {
            if ((e = set_mode(Mode::Off))) return fail(e);
            for (unsigned i = 0; i < Capacity; ++i)
                if ((e = slot(i, -1))) return fail(e);
            current.fill(-1);
        }
        // PassとBlockを切り替える途中で、新しいリストを逆の意味で適用しない。
        // 切替中は全PIDを通し、完成したリストへ最後に切り替える。
        if (!s.dirty && s.mode != Mode::Off && s.mode != mode) {
            if ((e = set_mode(Mode::Off))) return fail(e);
        }
        if (mode == Mode::Off) {
            if ((s.dirty || s.mode != mode) && (e = set_mode(mode))) return fail(e);
            s.slots = current; s.mode = mode; s.dirty = false;
            return 0;
        }
        auto needed = [&](int pid) { return pid >= 0 && std::find(wanted.begin(), wanted.begin()+n, pid) != wanted.begin()+n; };
        // 空きがあれば新PIDを先に追加し、共通PIDを更新の途中でも残す。
        auto add = [&]() {
            for (std::size_t i = 0; i < n; ++i) {
                if (std::find(current.begin(),current.end(),wanted[i]) != current.end()) continue;
                auto free = std::find(current.begin(),current.end(),-1);
                if (free == current.end()) return 0;
                unsigned index = static_cast<unsigned>(free-current.begin());
                int result = slot(index,wanted[i]); if (result) return result;
                *free = wanted[i];
            }
            return 0;
        };
        if ((e = add())) return fail(e);
        for (unsigned i = 0; i < Capacity; ++i) {
            if (current[i] >= 0 && !needed(current[i])) {
                if ((e = slot(i,-1))) return fail(e);
                current[i] = -1;
            }
        }
        if ((e = add())) return fail(e);
        if ((s.dirty || s.mode != mode) && (e = set_mode(mode))) return fail(e);
        s.slots = current; s.mode = mode; s.dirty = false;
        return 0;
    }
private:
    struct State {
        State() { slots.fill(-1); }
        std::array<int, Capacity> slots;
        Mode mode = Mode::Off;
        bool dirty = true;
    };
    std::mutex mutex_;
    std::array<State,5> states_;
};
} // namespace px4
