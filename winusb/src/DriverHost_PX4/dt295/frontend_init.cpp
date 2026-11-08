// SPDX-License-Identifier: GPL-2.0-only
#include "frontend_init.hpp"
#include <cerrno>
#include <new>

namespace px4::dt295 {
namespace {
// 値はリポジトリへ収録せず、配布物に同梱するmAgicVISION 0.1.0の常駐コードから読む。
// 常駐コードの初期化スクリプトは0x803ACB8Cから992 bytes・134レコード。
// 各レコードはop, 長さ, 後続待機ms (16bit BE), データの順。
constexpr uint32_t ResidentVa = 0x80380000, ScriptVa = 0x803ACB8C, ScriptBytes = 992, ScriptRecords = 134;
// 地デジ初期化の末尾で、表の先頭にある3レジスターを同じ値で書き直す。
// pix-userlandの地デジ初期化手順と同じ順序で、実機検証した初期化列と一致させる。
constexpr uint8_t TerrestrialRewrite[] = {0x25, 0x23, 0x03};
// DriverHostの復調器リセット後の20ms・5msと同じ待機。検証済みの初期化列と一致させるため残す。
constexpr uint8_t LeadingDelays[] = {20, 5};
constexpr uint8_t OpDelay = 255;

uint32_t Be32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
        static_cast<uint32_t>(p[2]) << 8 | p[3];
}

struct Record { uint8_t op, length; uint16_t delay; const uint8_t *data; };
}

int BuildFrontendInit(const uint8_t *container, size_t size, std::vector<uint8_t> &init)
{
    init.clear();
    if (!container || size < 48)
        return -EINVAL;
    const auto offset = Be32(container + 40), length = Be32(container + 44);
    if (offset > size || length > size - offset || length < ScriptVa - ResidentVa + ScriptBytes)
        return -EINVAL;
    const auto *script = container + offset + (ScriptVa - ResidentVa);

    std::vector<Record> records;
    try {
        records.reserve(ScriptRecords);
        for (size_t cursor = 0; cursor < ScriptBytes;) {
            if (ScriptBytes - cursor < 4)
                return -EINVAL;
            const Record r{script[cursor], script[cursor + 1],
                static_cast<uint16_t>(script[cursor + 2] << 8 | script[cursor + 3]), script + cursor + 4};
            if ((r.op != 0 && r.op != 2 && r.op != 128) || r.length < 1 || r.length > 8 ||
                r.length > ScriptBytes - cursor - 4 || r.delay > 255 || (r.op == 128 && r.length != 4))
                return -EINVAL;
            records.push_back(r);
            cursor += 4u + r.length;
        }
        if (records.size() != ScriptRecords)
            return -EINVAL;

        size_t first_satellite = records.size();
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].op == 2 && first_satellite == records.size())
                first_satellite = i;
            else if (records[i].op != 2 && first_satellite != records.size())
                return -EINVAL;
        }
        if (first_satellite == records.size())
            return -EINVAL;

        int values[256];
        for (auto &value : values)
            value = -1;
        for (size_t i = 0; i < first_satellite; ++i)
            if (records[i].op == 0 && records[i].length == 2 && values[records[i].data[0]] < 0)
                values[records[i].data[0]] = records[i].data[1];
        for (auto reg : TerrestrialRewrite)
            if (values[reg] < 0)
                return -EINVAL;

        for (auto ms : LeadingDelays)
            init.insert(init.end(), {OpDelay, 1, ms});
        for (size_t i = 0; i < records.size(); ++i) {
            if (i == first_satellite)
                for (auto reg : TerrestrialRewrite)
                    init.insert(init.end(), {0, 2, reg, static_cast<uint8_t>(values[reg])});
            init.push_back(records[i].op);
            init.push_back(records[i].length);
            init.insert(init.end(), records[i].data, records[i].data + records[i].length);
            if (records[i].delay)
                init.insert(init.end(), {OpDelay, 1, static_cast<uint8_t>(records[i].delay)});
        }
    } catch (const std::bad_alloc &) {
        init.clear();
        return -ENOMEM;
    }
    return 0;
}
}
