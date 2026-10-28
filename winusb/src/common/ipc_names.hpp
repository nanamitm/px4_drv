// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// 実機試験用ビルドだけ名前を分離し、既存の視聴・録画プロセスとの接続を防ぐ。
#ifdef PX4_ISOLATED_TEST_HOST
#define PX4_IPC_SUFFIX L"_RTLSDR_TEST"
#else
#define PX4_IPC_SUFFIX L""
#endif
namespace px4::ipc {
inline constexpr auto Mutex=L"Global\\DriverHost_PX4_Mutex" PX4_IPC_SUFFIX;
inline constexpr auto Startup=L"Global\\DriverHost_PX4_StartupEvent" PX4_IPC_SUFFIX;
inline constexpr auto Control=L"px4_ctrl_pipe" PX4_IPC_SUFFIX;
inline constexpr auto Data=L"px4_data_pipe" PX4_IPC_SUFFIX;
inline constexpr auto Card=L"px4_card_pipe" PX4_IPC_SUFFIX;
}
#undef PX4_IPC_SUFFIX
