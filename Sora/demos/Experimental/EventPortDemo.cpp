#include "Sora/Experimental/EventPort.h"

#include <cassert>
#include <memory>
#include <print>

namespace Exp = Sora::Experimental;

namespace EventPortDemo {

    namespace Event {

        struct PositionChanged {
            float x{};
            float y{};
        };

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
    static_assert(Exp::Concept::EventParticipant<EventfulPoint>);
    static_assert(Exp::Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Exp::Concept::EventReceiver<EventReceiver, Event::PositionChanged>);

    auto point = std::make_shared<EventfulPoint>();
    auto receiver = std::make_shared<EventReceiver>();

    int typedCount = 0;
    auto typed = Exp::Listen(*receiver, *point, Event::PositionChanged{}, Exp::Persistent,
                             [&](EventReceiver&, const Event::PositionChanged& event, EventfulPoint&) {
                                 assert(event.x == 1.0f || event.x == 2.0f);
                                 ++typedCount;
                             });

    point->MoveTo(1.0f, 3.0f);
    assert(typedCount == 1);

    auto repaint = Exp::Listen(*point, *point, Event::RepaintRequested{});
    Exp::Emit(*point, Event::RepaintRequested{});
    assert(point->repaintCount == 1);

    stdexec::run_loop loop;
    int asyncCount = 0;
    auto async = Exp::Listen(*receiver, *point, Event::PositionChanged{}, Exp::Persistent,
                             [&](const Event::PositionChanged& event,
                                 Exp::EventContext<Event::PositionChanged>& context) {
                                 assert(context.emitter.As<EventfulPoint>().repaintCount == 1);
                                 assert(event.x == 2.0f);
                                 ++asyncCount;
                             });
    Exp::EmitOn(*point, loop.get_scheduler(), Event::PositionChanged{2.0f, 4.0f});
    assert(asyncCount == 0);
    Run(loop);
    assert(asyncCount == 1);

    Exp::Drop(typed);
    Exp::Drop(repaint);
    Exp::Drop(async);

    std::println("Experimental EventPort demo passed: typed={}, async={}, repaint={}", typedCount, asyncCount,
                 point->repaintCount);
    return 0;
}
