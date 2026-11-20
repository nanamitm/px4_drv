// SPDX-License-Identifier: GPL-2.0-only
// px4-dt295d: PIX-DT295 を libusb で動かし、受信とカードのデバイスファイルを CUSE で公開するデーモン。
// 設計は docs/20261120_dt295_linux_cuse_design.md を参照。
#include "cuse_session.hpp"
#include "device.hpp"
#include "msg.h"
#include <libusb.h>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace px4::cuse {
namespace {
constexpr std::uint16_t Vendor = 0x06b8, Product = 0x103b;

// 機器の追加と削除、デバイスファイルの解放、終了の要求を、1 つのスレッドで順に処理する。
// libusb のホットプラグ通知と CUSE の要求の処理から長い処理 (初期化、セッションの停止) を外すためのもの。
class Manager {
public:
    explicit Manager(libusb_context *) {}

    void Post(std::function<void()> work)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(work));
        }
        changed_.notify_one();
    }

    void RequestExit()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exit_ = true;
        }
        changed_.notify_one();
    }

    void Run()
    {
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return exit_ || !queue_.empty(); });
                if (exit_)
                    break;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            work();
        }
        for (auto &device : devices_) {
            device->Shutdown();
            device->StopSessions();
            Close(*device);
        }
        devices_.clear();
        WriteReaderMap();
    }

    void Arrived(libusb_device *usb)
    {
        libusb_device_handle *handle = nullptr;
        const int bus = libusb_get_bus_number(usb), address = libusb_get_device_address(usb);
        if (int rc = libusb_open(usb, &handle)) {
            msg_err("PIX-DT295 %03d:%03d: open failed (%s)\n", bus, address, libusb_error_name(rc));
            return;
        }
        if (int rc = libusb_claim_interface(handle, 0)) {
            msg_err("PIX-DT295 %03d:%03d: claim failed (%s)\n", bus, address, libusb_error_name(rc));
            libusb_close(handle);
            return;
        }
        std::shared_ptr<Device> device;
        try {
            device = std::make_shared<Device>(handle, static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(address),
                                              [this, usb] { Post([this, usb] { Released(usb); }); });
        } catch (...) {
            libusb_release_interface(handle, 0);
            libusb_close(handle);
            return;
        }
        if (const int rc = device->Init()) {
            msg_err("PIX-DT295 %03d:%03d: firmware bundle result=%d\n", bus, address, rc);
            Close(*device);
            return;
        }
        if (const int rc = device->StartVideo(FreeIndex())) {
            msg_err("PIX-DT295 %03d:%03d: video device not started result=%d\n", bus, address, rc);
            Close(*device);
            return;
        }
        devices_.push_back(device);
        // カードのデバイスファイルを作れなくても受信は続ける (WinUSB 版のカードサーバーと同じ扱い)。
        StartCard(*device);
    }

    void Left(libusb_device *usb)
    {
        auto device = Find(usb);
        if (!device || device->Removed())
            return;
        msg_info("PIX-DT295 %03u:%03u: removed\n", device->Bus(), device->Address());
        device->Remove();
        // 開かれていないセッションはすぐに消す。受信のデバイスファイルが開かれたままでも、使われていない
        // カードのデバイスファイルを残すと、挿し直した機器が同じ ID のデバイスファイルを作れないため。
        const std::string card_id = device->CardId();
        if (device->StopUnusedSessions())
            CardReleased(card_id);
        WriteReaderMap();
        Released(usb);
    }

    // 切断済みの機器のデバイスファイルが、すべて閉じられた。
    void Released(libusb_device *usb)
    {
        auto device = Find(usb);
        if (!device || !device->Removed() || !device->Idle())
            return;
        const std::string card_id = device->HasCard() ? device->CardId() : std::string();
        device->StopSessions();
        devices_.erase(std::find(devices_.begin(), devices_.end(), device));
        Close(*device);
        if (!card_id.empty())
            CardReleased(card_id);
        WriteReaderMap();
    }

private:
    std::shared_ptr<Device> Find(libusb_device *usb)
    {
        for (auto &device : devices_)
            if (device->UsbDevice() == usb)
                return device;
        return nullptr;
    }

    // セッションと Bulk IN の転送をすべて止めた後に呼ぶ。以降、device は USB の操作を行わない。
    static void Close(Device &device)
    {
        libusb_release_interface(device.Handle(), 0);
        libusb_close(device.Handle());
    }

    // 受信のデバイスファイルは、カーネルモジュールと同じく空いている最小の番号にする。
    // 切断後に開かれたまま残るセッションの番号も使用中として扱う。
    unsigned FreeIndex() const
    {
        std::set<unsigned> used;
        for (const auto &device : devices_)
            if (device->HasVideo())
                used.insert(device->VideoIndex());
        unsigned index = 0;
        while (used.count(index))
            ++index;
        return index;
    }

    // 同じ機器を挿し直すとリーダーの ID が同じになる。pcscd が古いカードのデバイスファイルを閉じるまでは
    // 同じ名前の CUSE のデバイスを作れないため、古いセッションが終わるまで作成を保留する。
    void StartCard(Device &device)
    {
        if (device.CardId().empty() || device.HasCard())
            return;
        for (const auto &other : devices_)
            if (other.get() != &device && other->HasCard() && other->CardId() == device.CardId()) {
                msg_info("PIX-DT295 %03u:%03u: card device deferred until /dev/px4card/%s is released\n",
                         device.Bus(), device.Address(), device.CardId().c_str());
                return;
            }
        if (const int rc = device.StartCard())
            msg_err("PIX-DT295 %03u:%03u: card device not started result=%d\n", device.Bus(), device.Address(), rc);
        WriteReaderMap();
    }

    void CardReleased(const std::string &id)
    {
        for (auto &device : devices_)
            if (!device->Removed() && device->CardId() == id)
                StartCard(*device);
    }

    // IFD ハンドラーへ渡す、USB 機器とカードのデバイスファイルの対応表。
    // CUSE のデバイスは USB 機器を親に持たず、sysfs からたどれないため、この表で対応を示す。
    void WriteReaderMap()
    {
        const char *dir = std::getenv("RUNTIME_DIRECTORY");
        const std::string base = dir && *dir ? std::string(dir) : std::string("/run/px4-dt295");
        const std::string path = base + "/readers", temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            if (!out)
                return;
            out << "# bus device path\n";
            for (const auto &device : devices_)
                if (device->HasCard() && !device->Removed())
                    out << static_cast<unsigned>(device->Bus()) << ' ' << static_cast<unsigned>(device->Address())
                        << " /dev/px4card/" << device->CardId() << '\n';
        }
        if (std::rename(temp.c_str(), path.c_str()))
            std::remove(temp.c_str());
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
    bool exit_ = false;
    std::vector<std::shared_ptr<Device>> devices_;
};

int LIBUSB_CALL Hotplug(libusb_context *, libusb_device *usb, libusb_hotplug_event event, void *data)
{
    auto &manager = *static_cast<Manager *>(data);
    // 通知の処理は libusb のイベントのスレッドで動くため、機器の初期化は Manager のスレッドへ移す。
    // 処理するまで libusb_device を解放させないよう参照を取る。
    libusb_ref_device(usb);
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        manager.Post([&manager, usb] {
            manager.Arrived(usb);
            libusb_unref_device(usb);
        });
    else
        manager.Post([&manager, usb] {
            manager.Left(usb);
            libusb_unref_device(usb);
        });
    return 0;
}
}
}

int main()
{
    using px4::cuse::Manager;
    // 終了のシグナルは全スレッドで遮断し、専用のスレッドだけが sigwait で受ける。
    // CUSE のループのスレッドへ届くと、そのスレッドの read だけが中断されて終了の処理が進まないため。
    // libusb と CUSE のスレッドはこの設定を引き継ぐため、スレッドを作る前に行う。
    sigset_t exit_signals;
    sigemptyset(&exit_signals);
    sigaddset(&exit_signals, SIGTERM);
    sigaddset(&exit_signals, SIGINT);
    sigaddset(&exit_signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &exit_signals, nullptr);
    // 利用者が読み出しをやめた CUSE の応答で、プロセスが終了しないようにする。
    std::signal(SIGPIPE, SIG_IGN);
    px4::cuse::CuseSession::InstallWakeHandler();

    libusb_context *usb = nullptr;
    if (int rc = libusb_init(&usb)) {
        msg_err("libusb_init failed (%s)\n", libusb_error_name(rc));
        return 1;
    }
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        msg_err("libusb hotplug is not supported on this system\n");
        libusb_exit(usb);
        return 1;
    }
    Manager manager(usb);
    std::atomic_bool events_run{true};
    std::thread events([usb, &events_run] {
        while (events_run) {
            timeval tv{0, 100000};
            libusb_handle_events_timeout_completed(usb, &tv, nullptr);
        }
    });
    libusb_hotplug_callback_handle hotplug = 0;
    if (int rc = libusb_hotplug_register_callback(
            usb, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_ENUMERATE, px4::cuse::Vendor, px4::cuse::Product, LIBUSB_HOTPLUG_MATCH_ANY,
            px4::cuse::Hotplug, &manager, &hotplug)) {
        msg_err("libusb hotplug registration failed (%s)\n", libusb_error_name(rc));
        events_run = false;
        events.join();
        libusb_exit(usb);
        return 1;
    }
    std::thread signals([&exit_signals, &manager] {
        int signal = 0;
        sigwait(&exit_signals, &signal);
        msg_info("exit requested by signal %d\n", signal);
        manager.RequestExit();
    });
    msg_info("px4-dt295d started\n");
    manager.Run();
    libusb_hotplug_deregister_callback(usb, hotplug);
    events_run = false;
    events.join();
    // シグナル以外の理由で Run() を抜けた場合も、sigwait のスレッドを回収できるようにする。
    pthread_kill(signals.native_handle(), SIGTERM);
    signals.join();
    libusb_exit(usb);
    msg_info("px4-dt295d stopped\n");
    return 0;
}
