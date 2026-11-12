// SPDX-License-Identifier: GPL-2.0-only
// PIX-DT295 のTSリング滞留の記録 (StallHistory、StallTrigger) を確認する。USBは開かない。
#include "../src/DriverHost_PX4/dt295/stall_history.hpp"
#include <cstdio>

using px4::dt295::StallHistory;
using px4::dt295::StallTrigger;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

StallHistory::Event Service(std::int64_t time, std::uint32_t pending)
{
    return {time, 10, 1, pending, StallHistory::Kind::Service};
}

void HistoryKeepsNewestInOrder()
{
    StallHistory history(4);
    CHECK(history.Snapshot().empty());
    history.Record(Service(1, 0));
    history.Record(Service(2, 0));
    auto events = history.Snapshot();
    CHECK(events.size() == 2 && events[0].time_us == 1 && events[1].time_us == 2);
    // 容量を超えたら古い記録から上書きし、写しは古い順に並ぶ。
    for (std::int64_t t = 3; t <= 6; ++t)
        history.Record(Service(t, 0));
    events = history.Snapshot();
    CHECK(events.size() == 4);
    CHECK(events.front().time_us == 3 && events.back().time_us == 6);
    for (std::size_t i = 1; i < events.size(); ++i)
        CHECK(events[i - 1].time_us < events[i].time_us);
    history.Clear();
    CHECK(history.Snapshot().empty());
}

void TriggerWaitsPostWindowAndKeepsPeak()
{
    StallTrigger trigger(1000, 100, 50, 8);
    trigger.Reset();
    CHECK(!trigger.OnService(0, 999));
    CHECK(!trigger.OnService(10, 1000)); // 起点
    CHECK(trigger.TriggerTime() == 10);
    CHECK(!trigger.OnService(30, 3000));
    CHECK(!trigger.OnService(59, 200));
    CHECK(trigger.OnService(60, 200)); // 起点から50経過
    CHECK(trigger.Peak() == 3000 && trigger.Dumps() == 1);
    // 書き出しの後も、lowまで下がらない限り次の起点を作らない。
    CHECK(!trigger.OnService(70, 5000));
    CHECK(!trigger.OnService(200, 5000));
    CHECK(!trigger.OnService(210, 100)); // 再び有効化
    CHECK(!trigger.OnService(220, 1500));
    CHECK(trigger.TriggerTime() == 220);
    CHECK(trigger.OnService(270, 0));
    CHECK(trigger.Peak() == 1500 && trigger.Dumps() == 2);
}

void TriggerLimitsDumpsPerStream()
{
    StallTrigger trigger(1000, 100, 0, 2);
    trigger.Reset();
    unsigned dumps = 0;
    std::int64_t t = 0;
    for (int episode = 0; episode < 5; ++episode) {
        trigger.OnService(t++, 0);
        trigger.OnService(t++, 2000);
        if (trigger.OnService(t++, 2000))
            ++dumps;
    }
    CHECK(dumps == 2 && trigger.Dumps() == 2);
    // 新しい受信では上限を数え直す。
    trigger.Reset();
    CHECK(!trigger.OnService(t++, 2000));
    CHECK(trigger.OnService(t++, 0));
    // 起点の後post_usを待たずに受信が止まった場合も、停止時に1回だけ書き出す。
    StallTrigger stopping(1000, 100, 1000, 8);
    stopping.Reset();
    CHECK(!stopping.Flush());
    CHECK(!stopping.OnService(0, 1500));
    CHECK(stopping.Flush());
    CHECK(stopping.Peak() == 1500 && stopping.Dumps() == 1);
    CHECK(!stopping.Flush());
    StallTrigger disabled(1000, 100, 0, 0);
    disabled.Reset();
    CHECK(!disabled.OnService(0, 5000) && !disabled.OnService(1, 5000));
}
}

int main()
{
    HistoryKeepsNewestInOrder();
    TriggerWaitsPostWindowAndKeepsPeak();
    TriggerLimitsDumpsPerStream();
    if (failures)
        return 1;
    std::printf("PASS DT295 stall history\n");
    return 0;
}
