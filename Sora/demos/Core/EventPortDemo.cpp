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

        inline int positionProbeCount = 0;

        inline void ProbeEvent(const EventProbeContext& context, const PositionChanged&) {
            if (context.phase == EventProbePhase::CallbackBegin) {
                ++positionProbeCount;
            }
        }

        struct RepaintRequested {};

        struct OtherEvent {};

    } // namespace Event

    struct EventfulObject : std::enable_shared_from_this<EventfulObject> {
        EventPort events;

        using Emits = EventList<Event::OtherEvent>;
        using Accepts = EventList<>;
        using Callbacks = EventList<DefaultCallbackTag>;
    };

    struct EventfulPoint : EventfulObject {
        using Emits = EventList<Event::PositionChanged, Event::RepaintRequested>;
        using Accepts = EventList<Event::PositionChanged, Event::RepaintRequested, Event::OtherEvent>;

        void MoveTo(float x, float y) {
            x_ = x;
            y_ = y;
            Emit(*this, Event::PositionChanged{x_, y_});
        }

        void On(const Event::RepaintRequested&) { ++repaintCount; }

        int repaintCount{};

    private:
        float x_{};
        float y_{};
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
    static_assert(Concept::EventParticipant<EventfulPoint>);
    static_assert(Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Concept::EventReceiver<EventReceiver, Event::PositionChanged>);

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

    stdexec::run_loop loop;
    int asyncCount = 0;
    auto async =
        Connect(*receiver, *point, Event::PositionChanged{}, Persistent,
                     [&](const Event::PositionChanged& event, EventContext<Event::PositionChanged>& context) {
                         assert(context.emitter.As<EventfulPoint>().repaintCount == 1);
                         assert(event.x == 2.0f);
                         ++asyncCount;
                     });
    exec::start_detached(EmitOn(*point, loop.get_scheduler(), Event::PositionChanged{2.0f, 4.0f}));
    assert(asyncCount == 0);
    Run(loop);
    assert(asyncCount == 1);
    assert(Event::positionProbeCount >= 3);

    Disconnect(typed);
    Disconnect(repaint);
    Disconnect(async);

    std::println("Experimental EventPort demo passed: typed={}, async={}, repaint={}, probes={}", typedCount,
                 asyncCount, point->repaintCount, Event::positionProbeCount);
    return 0;
}
