#include "Sora/Core/EventPort.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <print>
#include <utility>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

namespace Concept = Sora::Concept;
namespace Traits = Sora::Traits;

using Sora::Connect;
using Sora::DefaultCallbackTag;
using Sora::Disconnect;
using Sora::EmitOn;
using Sora::EventContext;
using Sora::EventList;
using Sora::EventOptions;
using Sora::EventPort;
using Sora::EventProbeContext;
using Sora::EventProbePhase;
using Sora::Persistent;

namespace AsyncEventCallbackDemo {

    namespace Event {

        struct PositionChanged {
            float x{};
            float y{};
        };

        inline int callbackScheduled{};

        void ProbeEvent(const EventProbeContext& context, const PositionChanged&) {
            if (context.phase == EventProbePhase::CallbackScheduled) {
                ++callbackScheduled;
            }
        }

    } // namespace Event

    namespace Callback {

        struct PersistPosition {};

    } // namespace Callback

    struct EventfulPoint : std::enable_shared_from_this<EventfulPoint> {
        EventPort events;

        using Emits = EventList<Event::PositionChanged>;
        using Accepts = EventList<Event::PositionChanged>;
        using Callbacks = EventList<Callback::PersistPosition, DefaultCallbackTag>;

        void MoveTo(stdexec::scheduler auto scheduler, float x, float y) {
            x_ = x;
            y_ = y;
            exec::start_detached(EmitOn(*this, std::move(scheduler), Event::PositionChanged{x_, y_}));
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

    auto point = std::make_shared<EventfulPoint>();
    stdexec::run_loop dispatchLoop;
    stdexec::run_loop workerLoop;
    AuditLog log{};

    auto link =
        Connect(*point, *point, Event::PositionChanged{},
                EventOptions{.budget = Persistent, .callbackId = Traits::EventIdOf<Callback::PersistPosition>},
                PersistPositionCallback{.worker = workerLoop.get_scheduler(), .log = &log});
    assert(link.IsActive());

    point->MoveTo(dispatchLoop.get_scheduler(), 3.0f, 4.0f);
    assert(log.eventCallbacks == 0);
    assert(log.persisted == 0);

    Run(dispatchLoop);
    assert(log.eventCallbacks == 1);
    assert(log.persisted == 0);
    assert(Event::callbackScheduled == 1);

    Run(workerLoop);
    assert(log.persisted == 1);
    assert(log.lastX == 3.0f);
    assert(log.lastY == 4.0f);
    assert(log.lastSequence == 1);

    Disconnect(link);
    std::println("Core async event callback demo passed: callback={}, persisted={}, sequence={}", log.eventCallbacks,
                 log.persisted, log.lastSequence);
    return 0;
}
