// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "msg.h"
#include <cerrno>
#include <cstdio>

namespace px4::cuse {
namespace {
std::string MakeLabel(const char *model, std::uint8_t bus, std::uint8_t address)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%s %03u:%03u", model, bus, address);
    return text;
}
}

Device::Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, const char *model,
               const char *prefix, std::function<void()> changed)
    : handle_(handle), bus_(bus), address_(address), label_(MakeLabel(model, bus, address)), prefix_(prefix),
      video_(*this, available_, label_, changed), card_(*this, available_, label_, changed)
{
}

Device::~Device()
{
    StopSessions();
}

int Device::Init()
{
    if (const int rc = InitModel())
        return rc;
    return video_.Init();
}

int Device::StartVideo(unsigned index)
{
    const int rc = video_.Start(prefix_ + std::to_string(index));
    if (!rc)
        video_index_ = index;
    return rc;
}

int Device::StartCard()
{
    if (card_id_.empty())
        return -ENODEV;
    return card_.Start(card_id_);
}

void Device::StopSessions()
{
    card_.Stop();
    video_.Stop();
}

bool Device::StopUnusedSessions()
{
    // 切断後の open は ENODEV で断るため、判定の後に開かれることはない。
    if (!video_.InUse())
        video_.Stop();
    if (card_.Started() && !card_.InUse()) {
        card_.Stop();
        return true;
    }
    return false;
}

bool Device::Idle()
{
    return !video_.InUse() && !card_.InUse();
}

void Device::Remove()
{
    available_ = false;
    removal_.Set();
    // 機種ごとの状態の破棄は、受信のデバイスファイルの操作と混ざらないよう、その状態のロックの下で行う。
    video_.Remove([this] { OnRemove(); });
    card_.Remove();
}

void Device::Shutdown()
{
    video_.Shutdown([this] { OnShutdown(); });
    card_.Shutdown();
}
}
