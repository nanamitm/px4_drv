// SPDX-License-Identifier: GPL-2.0-only
// px4-cused: libusb で機器を動かし、受信とカードのデバイスファイルを CUSE で公開するデーモン。
// 対応機種は PIX-DT295 (docs/20261120_dt295_linux_cuse_design.md) と
// Sony torne CECH-ZD1J (docs/20261121_torne_linux_cuse_design.md)、I-O DATA GV-MVP/XZ3
// (docs/20261121_gv_mvp_xz3_linux_cuse_design.md)、PLEX PX-BCUD (docs/20261122_px_bcud_linux_cuse_design.md)、
// PLEX PX-S1UD (docs/20261122_px_s1ud_linux_cuse_design.md)、RTL-SDR のワンセグ (docs/20261122_rtlsdr_linux_cuse_design.md)、
// MSi2500 (RSP1 互換機) のフルセグ (docs/20261123_msi2500_linux_cuse_design.md)。
#include "cuse_session.hpp"
#include "dt295_device.hpp"
#include "msi2500_device.hpp"
#include "pxbcud_device.hpp"
#include "pxs1ud_device.hpp"
#include "rtlsdr_device.hpp"
#include "torne_device.hpp"
#include "xz3_device.hpp"
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
// 対応機種。受信のデバイスファイルの名前と番号の割り当ては機種ごとに独立する。
struct Model {
    std::uint16_t vendor, product;
    const char *name;
    std::shared_ptr<Device> (*make)(libusb_device_handle *, std::uint8_t, std::uint8_t, std::function<void()>);
    // Linux の本体のドライバーが既定で interface 0 を取る機種。開くときに外し、閉じるときに戻す。
    bool detach_kernel_driver;
    // 開く前に機器を選ぶ機種 (ほかの用途と取り合う汎用の機器)。nullptr ならすべて開く
    bool (*select)(libusb_device *);
};

template <class T>
std::shared_ptr<Device> Make(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address,
                             std::function<void()> changed)
{
    return std::make_shared<T>(handle, bus, address, std::move(changed));
}

const Model Models[] = {
    {Dt295Device::Vendor, Dt295Device::Product, "PIX-DT295", Make<Dt295Device>, false, nullptr},
    {TorneDevice::Vendor, TorneDevice::Product, "CECH-ZD1J", Make<TorneDevice>, false, nullptr},
    {Xz3Device::Vendor, Xz3Device::Product, "GV-MVP/XZ3", Make<Xz3Device>, false, nullptr},
    // PX-BCUD は Linux の本体の em28xx (DVB) が対応機種として取る (docs/20261122_px_bcud_linux_cuse_design.md)。
    {PxBcudDevice::Vendor, PxBcudDevice::Product, "PX-BCUD", Make<PxBcudDevice>, true, nullptr},
    // PX-S1UD は Linux の本体の smsusb (Siano) が対応機種として取る (docs/20261122_px_s1ud_linux_cuse_design.md)。
    {PxS1udDevice::Vendor, PxS1udDevice::Product, "PX-S1UD", Make<PxS1udDevice>, true, nullptr},
    // RTL-SDR は Linux の本体の dvb_usb_rtl28xxu が取り、ほかの用途 (SDR) とも取り合う汎用の機器のため、
    // PX4_CUSED_RTLSDR_PORTS で指定した接続位置の機器だけを開く (docs/20261122_rtlsdr_linux_cuse_design.md)。
    {RtlSdrDevice::Vendor, RtlSdrDevice::Product, "RTL-SDR", Make<RtlSdrDevice>, true, RtlSdrPortSelected},
    // MSi2500 (RSP1 互換機) は Linux の本体の msi2500 (V4L2 の SDR) が取り、ほかの用途とも取り合う汎用の機器のため、
    // PX4_CUSED_MSI2500_PORTS で指定した接続位置の機器だけを開く (docs/20261123_msi2500_linux_cuse_design.md)。
    {Msi2500Device::Vendor, Msi2500Device::Product, "MSi2500", Make<Msi2500Device>, true, Msi2500PortSelected},
};

// 機器の追加と削除、デバイスファイルの解放、終了の要求を、1 つのスレッドで順に処理する。
// libusb のホットプラグ通知と CUSE の要求の処理から長い処理 (初期化、セッションの停止) を外すためのもの。
class Manager {
public:
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

    void Arrived(libusb_device *usb, const Model &model)
    {
        libusb_device_handle *handle = nullptr;
        const int bus = libusb_get_bus_number(usb), address = libusb_get_device_address(usb);
        // 選ばない機器は開かず、制御転送も送らない (ほかのプロセスが使っている可能性があるため)
        if (model.select && !model.select(usb))
            return;
        if (int rc = libusb_open(usb, &handle)) {
            msg_err("%s %03d:%03d: open failed (%s)\n", model.name, bus, address, libusb_error_name(rc));
            return;
        }
        bool detached = false;
        if (model.detach_kernel_driver && libusb_kernel_driver_active(handle, 0) == 1) {
            if (int rc = libusb_detach_kernel_driver(handle, 0))
                msg_err("%s %03d:%03d: kernel driver detach failed (%s)\n", model.name, bus, address,
                        libusb_error_name(rc));
            else {
                detached = true;
                msg_info("%s %03d:%03d: kernel driver detached\n", model.name, bus, address);
            }
        }
        if (int rc = libusb_claim_interface(handle, 0)) {
            // カーネルのドライバや別のプロセスが機器を使っている場合 (torne_drv のカーネルモジュールなど)。
            msg_err("%s %03d:%03d: claim failed (%s)\n", model.name, bus, address, libusb_error_name(rc));
            if (detached)
                libusb_attach_kernel_driver(handle, 0);
            libusb_close(handle);
            return;
        }
        std::shared_ptr<Device> device;
        try {
            device = model.make(handle, static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(address),
                                [this, usb] { Post([this, usb] { Released(usb); }); });
        } catch (...) {
            libusb_release_interface(handle, 0);
            if (detached)
                libusb_attach_kernel_driver(handle, 0);
            libusb_close(handle);
            return;
        }
        device->kernel_driver_detached = detached;
        if (const int rc = device->Init()) {
            msg_err("%s: initialization result=%d\n", device->Label().c_str(), rc);
            Close(*device);
            return;
        }
        if (const int rc = device->StartVideo(FreeIndex(device->VideoPrefix()))) {
            msg_err("%s: video device not started result=%d\n", device->Label().c_str(), rc);
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
        msg_info("%s: removed\n", device->Label().c_str());
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
        // 外したカーネルのドライバーを戻し、デーモンを止めた後は DVB などのほかの使い方ができる状態にする。
        if (device.kernel_driver_detached && !device.Removed()) {
            const int rc = libusb_attach_kernel_driver(device.Handle(), 0);
            if (rc)
                msg_err("%s: kernel driver reattach failed (%s)\n", device.Label().c_str(), libusb_error_name(rc));
            else
                msg_info("%s: kernel driver reattached\n", device.Label().c_str());
        }
        libusb_close(device.Handle());
    }

    // 受信のデバイスファイルは、カーネルモジュールと同じく機種ごとに空いている最小の番号にする。
    // 切断後に開かれたまま残るセッションの番号も使用中として扱う。
    unsigned FreeIndex(const std::string &prefix) const
    {
        std::set<unsigned> used;
        for (const auto &device : devices_)
            if (device->HasVideo() && device->VideoPrefix() == prefix)
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
                msg_info("%s: card device deferred until /dev/px4card/%s is released\n", device.Label().c_str(),
                         device.CardId().c_str());
                return;
            }
        if (const int rc = device.StartCard())
            msg_err("%s: card device not started result=%d\n", device.Label().c_str(), rc);
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
        const std::string base = dir && *dir ? std::string(dir) : std::string("/run/px4-cused");
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

struct HotplugContext {
    Manager *manager;
    const Model *model;
};

int LIBUSB_CALL Hotplug(libusb_context *, libusb_device *usb, libusb_hotplug_event event, void *data)
{
    auto &context = *static_cast<HotplugContext *>(data);
    auto &manager = *context.manager;
    const Model &model = *context.model;
    // 通知の処理は libusb のイベントのスレッドで動くため、機器の初期化は Manager のスレッドへ移す。
    // 処理するまで libusb_device を解放させないよう参照を取る。
    libusb_ref_device(usb);
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        manager.Post([&manager, &model, usb] {
            manager.Arrived(usb, model);
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
    using namespace px4::cuse;
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
    CuseSession::InstallWakeHandler();

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
    Manager manager;
    std::atomic_bool events_run{true};
    std::thread events([usb, &events_run] {
        while (events_run) {
            timeval tv{0, 100000};
            libusb_handle_events_timeout_completed(usb, &tv, nullptr);
        }
    });
    std::vector<HotplugContext> contexts;
    for (const auto &model : Models)
        contexts.push_back({&manager, &model});
    std::vector<libusb_hotplug_callback_handle> hotplugs;
    for (auto &context : contexts) {
        libusb_hotplug_callback_handle hotplug = 0;
        if (int rc = libusb_hotplug_register_callback(
                usb, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
                LIBUSB_HOTPLUG_ENUMERATE, context.model->vendor, context.model->product, LIBUSB_HOTPLUG_MATCH_ANY,
                Hotplug, &context, &hotplug)) {
            msg_err("libusb hotplug registration failed for %s (%s)\n", context.model->name, libusb_error_name(rc));
            for (const auto registered : hotplugs)
                libusb_hotplug_deregister_callback(usb, registered);
            events_run = false;
            events.join();
            libusb_exit(usb);
            return 1;
        }
        hotplugs.push_back(hotplug);
    }
    std::thread signals([&exit_signals, &manager] {
        int signal = 0;
        sigwait(&exit_signals, &signal);
        msg_info("exit requested by signal %d\n", signal);
        manager.RequestExit();
    });
    msg_info("px4-cused started\n");
    manager.Run();
    for (const auto hotplug : hotplugs)
        libusb_hotplug_deregister_callback(usb, hotplug);
    events_run = false;
    events.join();
    // シグナル以外の理由で Run() を抜けた場合も、sigwait のスレッドを回収できるようにする。
    pthread_kill(signals.native_handle(), SIGTERM);
    signals.join();
    libusb_exit(usb);
    msg_info("px4-cused stopped\n");
    return 0;
}
