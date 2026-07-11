#include "Sora/Kernel/Core/ComPtr.h"
#include "Sora/Kernel/Core/EventPort.h"

#include <cassert>
#include <print>

namespace Concept = Sora::Kernel::Concept;
namespace Traits = Sora::Kernel::Traits;

using Sora::Kernel::AllEvents;
using Sora::Kernel::AnyEmitter;
using Sora::Kernel::AnyEvent;
using Sora::Kernel::BaseUnknown;
using Sora::Kernel::DefaultCallbackTag;
using Sora::Kernel::Drop;
using Sora::Kernel::Emit;
using Sora::Kernel::EmitOn;
using Sora::Kernel::EventContext;
using Sora::Kernel::EventFlow;
using Sora::Kernel::EventLink;
using Sora::Kernel::EventList;
using Sora::Kernel::EventOptions;
using Sora::Kernel::Events;
using Sora::Kernel::EventSpace;
using Sora::Kernel::EventTrace;
using Sora::Kernel::Iid;
using Sora::Kernel::MakeComPtr;
using Sora::Kernel::OneShot;
using Sora::Kernel::Persistent;
using Sora::Kernel::TypeOfClass;

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

        struct RepaintRequested {};

        struct OtherEvent {};

    } // namespace Event

    namespace Callback {

        struct PositionChanged {};

        struct PositionChanged2 {};

    } // namespace Callback

    class [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] EventfulObject : public BaseUnknown {
    public:
        using Emits = EventList<Event::OtherEvent>;
        using Accepts = EventList<Event::PositionChanged2>;
        using Callbacks = EventList<Callback::PositionChanged2>;
    };

    class [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] EventfulPoint : public EventfulObject {
        S_OBJECT

    public:
        using Emits = EventList<Event::PositionChanged, Event::RepaintRequested, Event::OtherEvent>;
        using Accepts = EventList<Event::PositionChanged, Event::PositionChanged2, Event::RepaintRequested>;
        using Callbacks = EventList<Callback::PositionChanged, Callback::PositionChanged2, DefaultCallbackTag>;

        void MoveTo(float x, float y) {
            x_ = x;
            y_ = y;
            Emit(*this, Event::PositionChanged{x_, y_});
        }

        void On(const Event::RepaintRequested&, BaseUnknown&) { ++defaultOnCount; }

        int defaultOnCount{};

    private:
        float x_{};
        float y_{};
    };

    class [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] UniversalReceiver : public BaseUnknown {
        S_OBJECT

    public:
        using Emits = EventList<>;
        using Accepts = EventList<AllEvents>;
        using Callbacks = EventList<DefaultCallbackTag>;
    };

    class [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] SilentPoint : public BaseUnknown {
        S_OBJECT
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
    static_assert(Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Concept::EventReceiver<EventfulPoint, Event::PositionChanged>);
    static_assert(Concept::CallbackAttachable<EventfulPoint, Callback::PositionChanged>);
    static_assert(Concept::CallbackAttachable<EventfulPoint, Callback::PositionChanged2>);
    static_assert(Sora::Traits::Contains<Traits::EmittedEventsOf<EventfulPoint>, Event::RepaintRequested>);
    static_assert(Sora::Traits::Contains<Traits::EmittedEventsOf<EventfulPoint>, Event::OtherEvent>);
    static_assert(Sora::Traits::Contains<Traits::AcceptedEventsOf<EventfulPoint>, Event::RepaintRequested>);
    static_assert(Sora::Traits::Contains<Traits::AcceptedEventsOf<EventfulPoint>, Event::PositionChanged2>);
    static_assert(Sora::Traits::Contains<Traits::CallbackTagsOf<EventfulPoint>, DefaultCallbackTag>);
    static_assert(Sora::Traits::Contains<Traits::CallbackTagsOf<EventfulPoint>, Callback::PositionChanged2>);

    auto point = MakeComPtr<EventfulPoint>();

    int traceCount = 0;
    auto trace = Events(*point).AttachTrace([&](const EventTrace& trace) {
        if (trace.eventId == Traits::EventIdOf<Event::PositionChanged>) {
            ++traceCount;
        }
    });
    assert(trace.Active());

    EventLink defaultOn = Listen(*point, *point, Event::RepaintRequested{});
    Emit<Event::RepaintRequested>(*point);
    assert(point->defaultOnCount == 1);
    Drop(defaultOn);

    int immediateCount = 0;
    auto immediate = Listen(*point, *point, Event::PositionChanged{}, Persistent,
                            [&](EventfulPoint&, const Event::PositionChanged& event) {
                                assert(event.y == 2.0f || event.y == 3.0f);
                                ++immediateCount;
                            });

    auto asyncPoint = MakeComPtr<EventfulPoint>();
    int asyncCount = 0;
    auto async = Listen(*asyncPoint, *asyncPoint, Event::PositionChanged{}, Persistent,
                        [&](const Event::PositionChanged& event, EventContext<Event::PositionChanged>& context) {
                            assert(context.receiver.Nucleus() == asyncPoint->Nucleus());
                            assert(event.y == 2.0f || event.y == 3.0f);
                            ++asyncCount;
                        });

    {
        stdexec::run_loop loop;
        EmitOn(*asyncPoint, loop.get_scheduler(), Event::PositionChanged{-1.0f, 2.0f});
        assert(asyncCount == 0);
        Run(loop);
        assert(asyncCount == 1);
    }

    Emit(*point, Event::PositionChanged{-1.0f, 2.0f});
    assert(immediateCount == 1);

    int onceCount = 0;
    auto once = Listen(*point, *point, Event::PositionChanged{}, EventOptions{.budget = OneShot, .priority = 10},
                       [&](const Event::PositionChanged&) { ++onceCount; });
    assert(once.Active());

    point->MoveTo(1.0f, 2.0f);
    assert(immediateCount == 2);
    assert(onceCount == 1);
    assert(asyncCount == 1);

    point->MoveTo(2.0f, 3.0f);
    assert(immediateCount == 3);
    assert(onceCount == 1);
    assert(asyncCount == 1);

    Drop(immediate);
    Drop(async);

    int anyCount = 0;
    auto any = Listen(*point, *point, AnyEvent, Persistent, [&](EventfulPoint&, BaseUnknown&) { ++anyCount; });
    Emit<Event::RepaintRequested>(*point);
    assert(anyCount == 1);
    Drop(any);

    auto universal = MakeComPtr<UniversalReceiver>();
    int universalCount = 0;
    auto universalLink = Listen(*universal, *point, Event::OtherEvent{}, Persistent,
                                [&](UniversalReceiver&, const Event::OtherEvent&) { ++universalCount; });
    Emit(*point, Event::OtherEvent{});
    assert(universalCount == 1);
    Drop(universalLink);

    EventSpace space;
    space.Add(*point);
    int spaceCount = 0;
    auto spaceLinks = Listen(*point, AnyEmitter{space}, Event::PositionChanged{}, Persistent,
                             [&](EventfulPoint&, const Event::PositionChanged& event) {
                                 assert(event.x == 3.0f);
                                 ++spaceCount;
                             });
    Emit(*point, Event::PositionChanged{3.0f, 3.0f});
    assert(spaceCount == 1);
    Drop(spaceLinks);

    int stoppedCount = 0;
    auto stop = Listen(*point, Event::RepaintRequested{}, EventOptions{.priority = 100},
                       [&](EventContext<Event::RepaintRequested>&) {
                           ++stoppedCount;
                           return EventFlow::Stop;
                       });
    auto skipped = Listen(*point, Event::RepaintRequested{}, Persistent, [&] { ++stoppedCount; });

    Emit<Event::RepaintRequested>(*point);
    assert(stoppedCount == 1);
    Drop(stop);
    Drop(skipped);

    stdexec::run_loop releasedLoop;
    int releasedCount = 0;
    {
        auto released = MakeComPtr<EventfulPoint>();
        auto releasedSub = Listen(*released, Event::PositionChanged{}, Persistent, [&] { ++releasedCount; });
        assert(releasedSub.Active());
        EmitOn(*released, releasedLoop.get_scheduler(), Event::PositionChanged{1.0f, 2.0f});
    }
    Run(releasedLoop);
    assert(releasedCount == 1);

    stdexec::run_loop canceledLoop;
    int canceledCount = 0;
    auto canceledSub = Listen(*point, Event::PositionChanged{}, Persistent, [&] { ++canceledCount; });
    EmitOn(*point, canceledLoop.get_scheduler(), Event::PositionChanged{1.0f, 2.0f});
    Drop(canceledSub);
    Run(canceledLoop);
    assert(canceledCount == 0);

    assert(traceCount >= 12);
    std::println("EventPort demo passed: immediate={}, once={}, async={}, any={}, space={}, trace={}", immediateCount,
                 onceCount, asyncCount, anyCount, spaceCount, traceCount);
    return 0;
}