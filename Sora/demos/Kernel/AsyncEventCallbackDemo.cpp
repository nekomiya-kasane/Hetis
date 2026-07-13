#include "Sora/Kernel/Core/ComPtr.h"
#include "Sora/Kernel/Core/EventPort.h"

#include <cassert>
#include <cstdint>
#include <print>
#include <utility>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

namespace Concept = Sora::Kernel::Concept;
namespace Traits = Sora::Kernel::Traits;

using Sora::Kernel::BaseUnknown;
using Sora::Kernel::DefaultCallbackTag;
using Sora::Kernel::Drop;
using Sora::Kernel::EmitOn;
using Sora::Kernel::EventContext;
using Sora::Kernel::EventList;
using Sora::Kernel::Events;
using Sora::Kernel::EventTrace;
using Sora::Kernel::Listen;
using Sora::Kernel::MakeComPtr;
using Sora::Kernel::Persistent;
using Sora::Kernel::TypeOfClass;

namespace AsyncEventCallbackDemo {

    namespace Event {

        struct PositionChanged {
            float x{};
            float y{};
        };

    } // namespace Event

    namespace Callback {

        struct PersistPosition {};

    } // namespace Callback

    class [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] EventfulPoint : public BaseUnknown {
        S_OBJECT

    public:
        using Emits = EventList<Event::PositionChanged>;
        using Accepts = EventList<Event::PositionChanged>;
        using Callbacks = EventList<Callback::PersistPosition, DefaultCallbackTag>;

        void MoveTo(stdexec::run_loop::scheduler scheduler, float x, float y) {
            x_ = x;
            y_ = y;
            EmitOn(*this, scheduler, Event::PositionChanged{x_, y_});
        }

    private:
        float x_{};
        float y_{};
    };

    struct AuditLog {
        int eventCallbacks{};
        int persisted{};
        float lastX{};
        float lastY{};
        uint64_t lastSequence{};
    };

    void SchedulePersistPosition(stdexec::run_loop::scheduler worker, AuditLog& log, Event::PositionChanged event,
                                 uint64_t sequence) {
        auto task = stdexec::schedule(worker) | stdexec::then([&log, event, sequence] {
                        ++log.persisted;
                        log.lastX = event.x;
                        log.lastY = event.y;
                        log.lastSequence = sequence;
                    });
        exec::start_detached(std::move(task));
    }

    struct PersistPositionCallback {
        stdexec::run_loop::scheduler worker;
        AuditLog* log{};

        void operator()(const Event::PositionChanged& event, EventContext<Event::PositionChanged>& context) const {
            ++log->eventCallbacks;
            SchedulePersistPosition(worker, *log, event, context.sequence);

        }
    };

    void Run(stdexec::run_loop& loop) {
        loop.finish();
        loop.run();
    }

} // namespace AsyncEventCallbackDemo

using namespace AsyncEventCallbackDemo;

int main() {
    static_assert(Concept::EventEmitter<EventfulPoint, Event::PositionChanged>);
    static_assert(Concept::EventReceiver<EventfulPoint, Event::PositionChanged>);
    static_assert(Traits::CanAttachCallback<EventfulPoint, Callback::PersistPosition>);

    auto point = MakeComPtr<EventfulPoint>();
    stdexec::run_loop dispatchLoop;
    stdexec::run_loop workerLoop;
    AuditLog log{};
    int traceScheduled{};

    auto trace = Events(*point).AttachTrace([&](const EventTrace& trace) {
        if (trace.eventId == Traits::EventIdOf<Event::PositionChanged>) {
            traceScheduled += trace.phase == Sora::Kernel::EventTracePhase::CallbackScheduled ? 1 : 0;
        }
    });
    assert(trace.Active());

    auto link = Listen(*point, *point, Event::PositionChanged{}, Persistent,
                       PersistPositionCallback{.worker = workerLoop.get_scheduler(), .log = &log});
    assert(link.Active());

    point->MoveTo(dispatchLoop.get_scheduler(), 3.0f, 4.0f);
    assert(log.eventCallbacks == 0);
    assert(log.persisted == 0);

    Run(dispatchLoop);
    assert(log.eventCallbacks == 1);
    assert(log.persisted == 0);
    assert(traceScheduled == 1);

    Run(workerLoop);
    assert(log.persisted == 1);
    assert(log.lastX == 3.0f);
    assert(log.lastY == 4.0f);
    assert(log.lastSequence == 1);

    Drop(link);
    std::println("Async event callback demo passed: callback={}, persisted={}, sequence={}", log.eventCallbacks,
                 log.persisted, log.lastSequence);
    return 0;
}
