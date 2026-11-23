// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "msg.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace px4::cuse {
std::string UsbPortPath(libusb_device *usb)
{
    std::uint8_t ports[8] = {};
    const int n = libusb_get_port_numbers(usb, ports, sizeof(ports));
    std::string path = std::to_string(libusb_get_bus_number(usb));
    for (int i = 0; i < n; ++i)
        path += (i ? "." : "-") + std::to_string(ports[i]);
    return path;
}

bool UsbPortListed(libusb_device *usb, const char *variable, const char *model)
{
    const std::string port = UsbPortPath(usb);
    const char *list = std::getenv(variable);
    std::istringstream in(list ? list : "");
    for (std::string item; std::getline(in, item, ',');) {
        item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); }), item.end());
        if (!item.empty() && item == port)
            return true;
    }
    msg_info("%s %s: not listed in %s; left untouched\n", model, port.c_str(), variable);
    return false;
}

bool PortCardId(libusb_device_handle *handle, const char *usb_id, std::string &id)
{
    libusb_device *usb = libusb_get_device(handle);
    std::uint8_t ports[8];
    const int count = libusb_get_port_numbers(usb, ports, sizeof(ports));
    if (count <= 0)
        return false;
    std::string path = std::string(usb_id) + "@" + std::to_string(libusb_get_bus_number(usb)) + "-";
    for (int i = 0; i < count; ++i)
        path += (i ? "." : "") + std::to_string(ports[i]);
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : path) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX", static_cast<unsigned long long>(hash));
    id = out;
    return true;
}

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
