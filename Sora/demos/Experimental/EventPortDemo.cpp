#include "Sora/Experimental/EventPort.h"

#include <cassert>
#include <memory>
#include <print>

#include <exec/start_detached.hpp>

namespace Exp = Sora::Experimental;

namespace EventPortDemo {

    namespace Event {

        struct PositionChanged {
            float x{};
            float y{};
        };

        inline int positionProbeCount = 0;

        inline void ProbeEvent(const Exp::EventProbeContext& context, const PositionChanged&) {
            if (context.phase == Exp::EventProbePhase::CallbackBegin) {
                ++positionProbeCount;
            }
        }

        struct RepaintRequested {};

        struct OtherEvent {};

    } // namespace Event

    struct EventfulObject : std::enable_shared_from_this<EventfulObject> {
        Exp::EventPort events;

        using Emits = Exp::EventList<Event::OtherEvent>;
        using Accepts = Exp::EventList<>;
        using Callbacks = Exp::EventList<Exp::DefaultCallbackTag>;
    };

    struct EventfulPoint : EventfulObject {
        using Emits = Exp::EventList<Event::PositionChanged, Event::RepaintRequested>;
        using Accepts = Exp::EventList<Event::PositionChanged, Event::RepaintRequested, Event::OtherEvent>;

        void MoveTo(float x, float y) {
            x_ = x;
            y_ = y;
            Exp::Emit(*this, Event::PositionChanged{x_, y_});
        }

        void On(const Event::RepaintRequested&) { ++repaintCount; }

        int repaintCount{};

    private:
        float x_{};
        float y_{};
    };

    struct EventReceiver : std::enable_shared_from_this<EventReceiver> {
        Exp::EventPort events;

        using Emits = Exp::EventList<>;
        using Accepts = Exp::EventList<Event::PositionChanged, Event::OtherEvent>;
        using Callbacks = Exp::EventList<Exp::DefaultCallbackTag>;

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
    static_assert(Exp::Traits::CanEmitEvent<EventfulPoint, Event::PositionChanged>);
    static_assert(Exp::Traits::CanEmitEvent<EventfulPoint, Event::OtherEvent>);
    static_assert(Exp::Traits::CanAcceptEvent<EventfulPoint, Event::RepaintRequested>);
    static_assert(Exp::Traits::CanAcceptEvent<EventfulPoint, Event::PositionChanged>);
    static_assert(Exp::Concept::EventParticipant<EventfulPoint>);
    static_assert(Exp::Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Exp::Concept::EventReceiver<EventReceiver, Event::PositionChanged>);

    auto point = std::make_shared<EventfulPoint>();
    auto receiver = std::make_shared<EventReceiver>();

    int typedCount = 0;
    auto typed = Exp::Connect(*receiver, *point, Event::PositionChanged{}, Exp::Persistent,
                              [&](EventReceiver&, const Event::PositionChanged& event, EventfulPoint&) {
                                  assert(event.x == 1.0f || event.x == 2.0f);
                                  ++typedCount;
                              });

    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 1);

    int ignoredConnectionCount = 0;
    Exp::Connect(*receiver, *point, Event::PositionChanged{}, Exp::Persistent,
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
            Exp::Connect(*transient, *point, Event::PositionChanged{}, Exp::Persistent,
                         [&](EventReceiver&, const Event::PositionChanged&, EventfulPoint&) { ++transientCount; });
        assert(transientLink.IsActive());
    }
    assert(transientWeak.expired());
    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 3);
    assert(ignoredConnectionCount == 2);
    assert(transientCount == 0);

    auto customReceiverCallback =
        Exp::Connect(*receiver, *point, Event::PositionChanged{}, Exp::OneShot, &EventReceiver::CustomOn);
    point->MoveTo(1.0f, 5.0f);
    assert(typedCount == 4);
    assert(ignoredConnectionCount == 3);
    assert(receiver->customCount == 1);
    assert(receiver->lastCustomX == 1.0f);
    assert(receiver->lastCustomY == 5.0f);
    assert(!customReceiverCallback.IsActive());

    auto outgoing = Exp::EventPortOf(*point).Connections();
    auto incoming = Exp::EventPortOf(*receiver).Connections(Exp::EventConnectionDirection::Incoming);
    assert(!outgoing.empty());
    assert(!incoming.empty());
    for (const auto& connection : incoming) {
        connection.Suspend();
        connection.Resume();
    }

    auto repaint = Exp::Connect(*point, *point, Event::RepaintRequested{});
    Exp::Emit(*point, Event::RepaintRequested{});
    assert(point->repaintCount == 1);

    stdexec::run_loop loop;
    int asyncCount = 0;
    auto async =
        Exp::Connect(*receiver, *point, Event::PositionChanged{}, Exp::Persistent,
                     [&](const Event::PositionChanged& event, Exp::EventContext<Event::PositionChanged>& context) {
                         assert(context.emitter.As<EventfulPoint>().repaintCount == 1);
                         assert(event.x == 2.0f);
                         ++asyncCount;
                     });
    exec::start_detached(Exp::EmitOn(*point, loop.get_scheduler(), Event::PositionChanged{2.0f, 4.0f}));
    assert(asyncCount == 0);
    Run(loop);
    assert(asyncCount == 1);
    assert(Event::positionProbeCount >= 3);

    Exp::Disconnect(typed);
    Exp::Disconnect(repaint);
    Exp::Disconnect(async);

    std::println("Experimental EventPort demo passed: typed={}, async={}, repaint={}, probes={}", typedCount,
                 asyncCount, point->repaintCount, Event::positionProbeCount);
    return 0;
}
