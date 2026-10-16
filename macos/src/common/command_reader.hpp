#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "pipe.hpp"
#include "command.hpp"

namespace px4 {

// SOCK_STREAM はメッセージ境界を持たないため、必要な長さだけを蓄積する。
inline bool ReadExact(Pipe &pipe, void *buffer, std::size_t size, HANDLE cancel)
{
 auto *p = static_cast<std::uint8_t *>(buffer);
 while (size) {
  std::size_t received = 0;
  if (!pipe.Read(p, size, received, cancel) || !received) return false;
  p += received;
  size -= received;
 }
 return true;
}

inline bool ReadControlCommand(Pipe &pipe, std::uint8_t *buffer,
 std::size_t capacity, std::size_t &length, HANDLE cancel)
{
 using namespace command;
 length = sizeof(CtrlCmdHeader);
 if (capacity < length || !ReadExact(pipe, buffer, length, cancel)) return false;
 CtrlCmdHeader header;
 std::memcpy(&header, buffer, sizeof(header));
 std::size_t fixed = length, element = 0, count_offset = 0;
 switch (header.cmd) {
 case CtrlCmdCode::GET_VERSION: fixed = sizeof(CtrlVersionCmd); break;
 case CtrlCmdCode::OPEN: fixed = sizeof(CtrlOpenCmd); break;
 case CtrlCmdCode::GET_INFO: fixed = sizeof(CtrlReceiverInfoCmd); break;
 case CtrlCmdCode::SET_CAPTURE: fixed = sizeof(CtrlCaptureCmd); break;
 case CtrlCmdCode::TUNE: fixed = sizeof(CtrlTuneCmd); break;
 case CtrlCmdCode::CHECK_LOCK: fixed = sizeof(CtrlCheckLockCmd); break;
 case CtrlCmdCode::SET_LNB_VOLTAGE: fixed = sizeof(CtrlLnbVoltageCmd); break;
 case CtrlCmdCode::GET_PARAMS:
 case CtrlCmdCode::SET_PARAMS:
  fixed = sizeof(CtrlParamsCmd); element = sizeof(Parameter);
  count_offset = sizeof(CtrlCmdHeader) + offsetof(ParameterSet, num); break;
 case CtrlCmdCode::READ_STATS:
  fixed = sizeof(CtrlStatsCmd); element = sizeof(Stat);
  count_offset = sizeof(CtrlCmdHeader) + offsetof(StatSet, num); break;
 default: break;
 }
 if (fixed > capacity || !ReadExact(pipe, buffer + length, fixed - length, cancel))
  return false;
 length = fixed;
 if (element) {
  std::uint32_t count;
  std::memcpy(&count, buffer + count_offset, sizeof(count));
  // 乗算前に容量から上限を求め、異常な要素数で受信先を越えないようにする。
  if (count && (count - 1) > (capacity - fixed) / element) return false;
  const std::size_t extra = count ? (count - 1) * element : 0;
  if (!ReadExact(pipe, buffer + fixed, extra, cancel)) return false;
  length += extra;
 }
 return true;
}

} // namespace px4
