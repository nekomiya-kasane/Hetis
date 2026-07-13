#include "Sora/Core/EventPort.h"

#include <cassert>
#include <memory>
#include <print>

#include <exec/start_detached.hpp>

using namespace Sora;

namespace EventPortDemo {

    namespace Event {

        struct PositionChanged {
            float x{};
            float y{};
        };

        struct PositionChanged2 {
            float x{};
            float y{};
            float z{};
        };

        inline int positionProbeCount = 0;
        inline int positionScheduledProbeCount = 0;

        inline void ProbeEvent(const EventProbeContext& context, const PositionChanged&) {
            if (context.phase == EventProbePhase::CallbackBegin) {
                ++positionProbeCount;
            } else if (context.phase == EventProbePhase::CallbackScheduled) {
                ++positionScheduledProbeCount;
            }
        }

        struct RepaintRequested {};

        struct OtherEvent {};

    } // namespace Event

    namespace Callback {

        struct PositionChanged {};

        struct PositionChanged2 {};

    } // namespace Callback

    struct EventfulObject : std::enable_shared_from_this<EventfulObject> {
        EventPort events;

        using Emits = EventList<Event::OtherEvent>;
        using Accepts = EventList<Event::PositionChanged2>;
        using Callbacks = EventList<Callback::PositionChanged2>;
    };

    struct EventfulPoint : EventfulObject {
        using Emits = EventList<Event::PositionChanged, Event::RepaintRequested>;
        using Accepts = EventList<Event::PositionChanged, Event::RepaintRequested, Event::OtherEvent>;
        using Callbacks = EventList<Callback::PositionChanged, DefaultCallbackTag>;

        void MoveTo(float x, float y) {
            x_ = x;
            y_ = y;
            Emit(*this, Event::PositionChanged{x_, y_});
        }

        void On(const Event::RepaintRequested&, EventfulPoint&) { ++repaintCount; }

        int repaintCount{};

    private:
        float x_{};
        float y_{};
    };

    struct UniversalReceiver : std::enable_shared_from_this<UniversalReceiver> {
        EventPort events;

        using Emits = EventList<>;
        using Accepts = EventList<AllEvents>;
        using Callbacks = EventList<DefaultCallbackTag>;
    };

    struct SilentPoint {
        EventPort events;
    };

    struct EventReceiver : std::enable_shared_from_this<EventReceiver> {
        EventPort events;

        using Emits = EventList<>;
        using Accepts = EventList<Event::PositionChanged, Event::OtherEvent>;
        using Callbacks = EventList<DefaultCallbackTag>;

        void CustomOn(const Event::PositionChanged& event) {
            lastCustomX = event.x;
            lastCustomY = event.y;
            ++customCount;
        }

        int customCount{};
        float lastCustomX{};
        float lastCustomY{};
    };

    void Run(stdexec::run_loop& loop) {
        loop.finish();
        loop.run();
    }

} // namespace EventPortDemo

using namespace EventPortDemo;

int main() {
    static_assert(Traits::CanEmitEvent<EventfulPoint, Event::PositionChanged>);
    static_assert(Traits::CanEmitEvent<EventfulPoint, Event::OtherEvent>);
    static_assert(Traits::CanAcceptEvent<EventfulPoint, Event::RepaintRequested>);
    static_assert(Traits::CanAcceptEvent<EventfulPoint, Event::PositionChanged>);
    static_assert(Traits::CanAcceptEvent<EventfulPoint, Event::PositionChanged2>);
    static_assert(Traits::CanAcceptEvent<UniversalReceiver, Event::PositionChanged>);
    static_assert(Traits::CanAcceptEvent<UniversalReceiver, Event::RepaintRequested>);
    static_assert(!Traits::CanEmitEvent<UniversalReceiver, Event::PositionChanged>);
    static_assert(!Concept::EventPayload<AllEvents>);
    static_assert(Traits::CanAttachCallback<EventfulPoint, Callback::PositionChanged>);
    static_assert(Traits::CanAttachCallback<EventfulPoint, Callback::PositionChanged2>);
    static_assert(!Traits::CanEmitEvent<SilentPoint, Event::PositionChanged>);
    static_assert(!Traits::CanAcceptEvent<SilentPoint, Event::PositionChanged>);
    static_assert(!Traits::CanAttachCallback<SilentPoint, DefaultCallbackTag>);
    static_assert(Concept::EventParticipant<EventfulPoint>);
    static_assert(Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Concept::CallbackAttachable<EventfulPoint, Callback::PositionChanged>);
    static_assert(Concept::CallbackAttachable<EventfulPoint, Callback::PositionChanged2>);
    static_assert(Concept::EventReceiver<EventReceiver, Event::PositionChanged>);
    static_assert(Sora::Traits::Contains<Traits::EmittedEventsOf<EventfulPoint>, Event::RepaintRequested>);
    static_assert(Sora::Traits::Contains<Traits::EmittedEventsOf<EventfulPoint>, Event::OtherEvent>);
    static_assert(Sora::Traits::Contains<Traits::AcceptedEventsOf<EventfulPoint>, Event::PositionChanged2>);
    static_assert(Sora::Traits::Contains<Traits::CallbackTagsOf<EventfulPoint>, DefaultCallbackTag>);
    static_assert(Sora::Traits::Contains<Traits::CallbackTagsOf<EventfulPoint>, Callback::PositionChanged2>);

    auto point = std::make_shared<EventfulPoint>();
    auto receiver = std::make_shared<EventReceiver>();

    int typedCount = 0;
    auto typed = Connect(*receiver, *point, Event::PositionChanged{}, Persistent,
                              [&](EventReceiver&, const Event::PositionChanged& event, EventfulPoint&) {
                                  assert(event.x == 1.0f || event.x == 2.0f);
                                  ++typedCount;
                              });

    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 1);

    int ignoredConnectionCount = 0;
    Connect(*receiver, *point, Event::PositionChanged{}, Persistent,
                 [&](EventReceiver&, const Event::PositionChanged&, EventfulPoint&) {
                     ++ignoredConnectionCount;
                 });
    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 2);
    assert(ignoredConnectionCount == 1);

    int transientCount = 0;
    std::weak_ptr<EventReceiver> transientWeak;
    {
        auto transient = std::make_shared<EventReceiver>();
        transientWeak = transient;
        auto transientLink =
            Connect(*transient, *point, Event::PositionChanged{}, Persistent,
                         [&](EventReceiver&, const Event::PositionChanged&, EventfulPoint&) { ++transientCount; });
        assert(transientLink.IsActive());
    }
    assert(transientWeak.expired());
    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 3);
    assert(ignoredConnectionCount == 2);
    assert(transientCount == 0);

    auto customReceiverCallback =
        Connect(*receiver, *point, Event::PositionChanged{}, OneShot, &EventReceiver::CustomOn);
    point->MoveTo(1.0f, 5.0f);
    assert(typedCount == 4);
    assert(ignoredConnectionCount == 3);
    assert(receiver->customCount == 1);
    assert(receiver->lastCustomX == 1.0f);
    assert(receiver->lastCustomY == 5.0f);
    assert(!customReceiverCallback.IsActive());

    int onceCount = 0;
    auto once = Connect(*receiver, *point, Event::PositionChanged{}, EventOptions{.budget = OneShot, .priority = 10},
                        [&](const Event::PositionChanged& event) {
                            assert(event.y == 2.0f || event.y == 3.0f);
                            ++onceCount;
                        });
    assert(once.IsActive());

    point->MoveTo(1.0f, 2.0f);
    assert(typedCount == 5);
    assert(ignoredConnectionCount == 4);
    assert(onceCount == 1);
    assert(!once.IsActive());

    point->MoveTo(2.0f, 3.0f);
    assert(typedCount == 6);
    assert(ignoredConnectionCount == 5);
    assert(onceCount == 1);

    auto outgoing = EventPortOf(*point).Connections();
    auto incoming = EventPortOf(*receiver).Connections(EventConnectionDirection::Incoming);
    assert(!outgoing.empty());
    assert(!incoming.empty());
    for (const auto& connection : incoming) {
        connection.Suspend();
        connection.Resume();
    }

    auto repaint = Connect(*point, *point, Event::RepaintRequested{});
    Emit(*point, Event::RepaintRequested{});
    assert(point->repaintCount == 1);

    int anyCount = 0;
    auto any = Connect(*point, *point, AnyEvent, EventOptions{.budget = Persistent}, [&] { ++anyCount; });
    Emit(*point, Event::RepaintRequested{});
    assert(anyCount == 1);
    Disconnect(any);

    auto universal = std::make_shared<UniversalReceiver>();
    int universalCount = 0;
    auto universalLink = Connect(*universal, *point, Event::OtherEvent{}, EventOptions{.budget = Persistent},
                                 [&](UniversalReceiver&, const Event::OtherEvent&) { ++universalCount; });
    Emit(*point, Event::OtherEvent{});
    assert(universalCount == 1);
    Disconnect(universalLink);

    int stoppedCount = 0;
    auto stop = Connect(*point, *point, Event::RepaintRequested{}, EventOptions{.priority = 100},
                        [&](EventContext<Event::RepaintRequested>&) {
                            ++stoppedCount;
                            return EventFlow::Stop;
                        });
    auto skipped =
        Connect(*point, *point, Event::RepaintRequested{}, EventOptions{.budget = Persistent}, [&] { ++stoppedCount; });
    Emit(*point, Event::RepaintRequested{});
    assert(stoppedCount == 1);
    Disconnect(stop);
    Disconnect(skipped);

    stdexec::run_loop loop;
    int asyncCount = 0;
    const int expectedRepaintCount = point->repaintCount;
    const int scheduledProbeCountBeforeAsync = Event::positionScheduledProbeCount;
    auto async =
        Connect(*receiver, *point, Event::PositionChanged{}, Persistent,
                     [&](const Event::PositionChanged& event, EventContext<Event::PositionChanged>& context) {
                         assert(context.emitter.As<EventfulPoint>().repaintCount == expectedRepaintCount);
                         assert(event.x == 2.0f);
                         ++asyncCount;
                     });
    exec::start_detached(EmitOn(*point, loop.get_scheduler(), Event::PositionChanged{2.0f, 4.0f}));
    assert(asyncCount == 0);
    Run(loop);
    assert(asyncCount == 1);
    assert(Event::positionProbeCount >= 3);
    assert(Event::positionScheduledProbeCount > scheduledProbeCountBeforeAsync);
    Disconnect(async);

    stdexec::run_loop releasedLoop;
    int releasedCount = 0;
    {
        auto released = std::make_shared<EventfulPoint>();
        auto releasedSub = Connect(*released, *released, Event::PositionChanged{}, Persistent,
                                   [&] { ++releasedCount; });
        assert(releasedSub.IsActive());
        exec::start_detached(EmitOn(*released, releasedLoop.get_scheduler(), Event::PositionChanged{1.0f, 2.0f}));
    }
    Run(releasedLoop);
    assert(releasedCount == 1);

    stdexec::run_loop canceledLoop;
    int canceledCount = 0;
    auto canceledSub =
        Connect(*receiver, *point, Event::PositionChanged{}, Persistent, [&] { ++canceledCount; });
    exec::start_detached(EmitOn(*point, canceledLoop.get_scheduler(), Event::PositionChanged{1.0f, 2.0f}));
    Disconnect(canceledSub);
    Run(canceledLoop);
    assert(canceledCount == 0);

    Disconnect(typed);
    Disconnect(repaint);

    std::println("Core EventPort demo passed: typed={}, once={}, async={}, any={}, repaint={}, probes={}", typedCount,
                 onceCount, asyncCount, anyCount, point->repaintCount, Event::positionProbeCount);
    return 0;
}
