// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "runtime.h"

namespace px4::dt295 {
// PIX-DT295 の制御転送の経路。WinUSB版 (winusb_transport.hpp) とLinux版のTransportが実装し、
// 共有のControlChannelから使う。失敗した要求は再送せず、Failed()が0以外の間は後続の転送を始めない。
class ControlTransport {
public:
    virtual ~ControlTransport() = default;
    // 以降の転送全体の絶対期限を設定する。
    virtual void Begin(unsigned milliseconds) = 0;
    // 失敗を解除して期限を設定する。保存済みの前像を戻す復元だけが使う。
    virtual void BeginRestore(unsigned milliseconds) = 0;
    virtual int Failed() const = 0;
    // カード操作のように、失敗した要求の結果へ後続が依存しない操作の終わりに、呼び出し元が失敗を解除する。
    virtual void ClearFailure() = 0;
    virtual dt295_io Callbacks() = 0;
    virtual void ReportTiming(const char *stage) = 0;
    // ログへ添えるOSのエラー番号。持たない実装は0を返す。
    virtual unsigned long LastError() const = 0;
};
}
