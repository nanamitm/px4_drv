// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace px4::dt295 {
// mAgicVISION 0.1.0のSBC2コンテナーから、source-fr用の復調器・RF初期化列を組み立てる。
// 出力はDTP1バンドルの初期化セクションと同じ形式（op, 長さ, データ。op 255は待機ms）。
// コンテナーのSHA-256は呼出し側が照合する。構造が想定と異なる場合は-EINVALを返す。
int BuildFrontendInit(const uint8_t *container, size_t size, std::vector<uint8_t> &init);

// BuildFrontendInitが受け付けるコンテナーのSHA-256（mAgicVISION 0.1.0）。
constexpr char FrontendInitContainerSha256[] = "d9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f";
}
