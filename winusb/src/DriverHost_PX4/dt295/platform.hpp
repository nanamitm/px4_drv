// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// SourceFirmwareがOSに依存する2つの処理。Windows版はplatform_win32.cpp (EXEのリソースとBCrypt)、
// Linux版はデーモン側の実装 (実行ファイルへの埋め込みと移植可能なSHA-256) を結合する。
namespace px4::dt295::platform {
// 実行ファイルへ組み込んだファームウェアのバンドル。direct-frとsource-frで別のバンドルを持つ。
// 見つからない場合はfalseを返す。返した領域はプロセスの終了まで有効。
bool EmbeddedBundle(bool direct, const std::uint8_t *&data, std::size_t &size);
// ブートROMの内容の確認に使う。計算できない場合はfalseを返す。
bool Sha256(const std::uint8_t *data, std::size_t size, std::array<std::uint8_t, 32> &digest);
}
