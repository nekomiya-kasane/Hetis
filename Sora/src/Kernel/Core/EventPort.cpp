#include "Sora/Kernel/Core/EventPort.h"
#include "Sora/Kernel/Core/KernelSection.h"
#include "Sora/Kernel/Core/Query.h"

#include <algorithm>
#include <exception>
#include <ranges>

namespace {

    inline constexpr auto kSoraCoreKernelClasses = [] consteval {
        return Sora::Kernel::KernelManifest{
            .classes = {^^Sora::Kernel::EventPort},
        };
    }();

    consteval {
        Sora::Kernel::ValidateKernelManifest<kSoraCoreKernelClasses>();
    }

    [[maybe_unused]] constinit auto const& kSoraCoreKernelSection =
        Sora::Kernel::KernelSection<kSoraCoreKernelClasses>::anchor;

} // namespace

namespace Sora::Kernel {

    EventPort::EventPort() noexcept
        : subscriptions_(std::make_shared<Detail::SubscriptionSnapshot>()),
          traces_(std::make_shared<Detail::TraceSnapshot>()) {}

    EventPort::~EventPort() noexcept {
        CancelAll();
    }

    EventLink EventPort::AddSubscription(Detail::SubscriptionRecord record) {
        record.id = nextSubscription_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.order = record.id;
        auto state = record.state;
        auto stored = std::shared_ptr<Detail::SubscriptionRecord>{new Detail::SubscriptionRecord(std::move(record))};

        {
            std::scoped_lock lock(mutex_);
            auto next = std::make_shared<Detail::SubscriptionSnapshot>(*subscriptions_);
            next->records.push_back(std::move(stored));
            std::ranges::stable_sort(next->records, [](const auto& lhs, const auto& rhs) noexcept {
                if (lhs->priority != rhs->priority) {
                    return lhs->priority > rhs->priority;
                }
                return lhs->order < rhs->order;
            });
            subscriptions_ = std::move(next);
        }

        return EventLink{state};
    }

    EventTraceHook EventPort::AddTrace(Detail::TraceRecord record) {
        record.id = nextTrace_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.order = record.id;
        auto state = record.state;
        auto stored = std::shared_ptr<Detail::TraceRecord>{new Detail::TraceRecord(std::move(record))};

        {
            std::scoped_lock lock(mutex_);
            auto next = std::make_shared<Detail::TraceSnapshot>(*traces_);
            next->records.push_back(std::move(stored));
            std::ranges::stable_sort(next->records, [](const auto& lhs, const auto& rhs) noexcept {
                return lhs->order < rhs->order;
            });
            traces_ = std::move(next);
        }

        return EventTraceHook{std::move(state)};
    }

    std::shared_ptr<const Detail::SubscriptionSnapshot> EventPort::Subscriptions() const {
        std::scoped_lock lock(mutex_);
        return subscriptions_;
    }

    std::shared_ptr<const Detail::TraceSnapshot> EventPort::Traces() const {
        std::scoped_lock lock(mutex_);
        return traces_;
    }

    void EventPort::AttachInbound(std::weak_ptr<Detail::SubscriptionState> state) {
        std::scoped_lock lock(mutex_);
        std::erase_if(inbound_,
                      [](const std::weak_ptr<Detail::SubscriptionState>& relation) { return relation.expired(); });
        inbound_.push_back(std::move(state));
    }

    void EventPort::Trace(const EventTrace& trace) const {
        Trace(Traces(), trace);
    }

    void EventPort::Trace(std::shared_ptr<const Detail::TraceSnapshot> traces, const EventTrace& trace) {
        if (!traces) {
            return;
        }
        for (const auto& hook : traces->records) {
            if (hook && hook->state->active.load(std::memory_order_acquire)) {
                hook->callback(trace);
            }
        }
    }

    void EventPort::CancelAll() noexcept {
        std::scoped_lock lock(mutex_);
        for (const auto& subscription : subscriptions_->records) {
            if (subscription) {
                subscription->state->active.store(false, std::memory_order_release);
                subscription->state->canceled.store(true, std::memory_order_release);
            }
        }
        std::erase_if(inbound_, [](const std::weak_ptr<Detail::SubscriptionState>& relation) {
            auto state = relation.lock();
            if (!state) {
                return true;
            }
            state->active.store(false, std::memory_order_release);
            state->canceled.store(true, std::memory_order_release);
            return false;
        });
        for (const auto& trace : traces_->records) {
            if (trace) {
                trace->state->active.store(false, std::memory_order_release);
            }
        }
    }

    EventPort& Events(BaseUnknown& object) {
        EventPort* hub = QueryInterface<EventPort>(object);
        assert(hub != nullptr && "EventPort must be registered as a BaseUnknown data extension.");
        if (!hub) [[unlikely]] {
            std::terminate();
        }
        return *hub;
    }

    EventPort& Events(BaseUnknown* object) {
        assert(object != nullptr);
        if (!object) [[unlikely]] {
            std::terminate();
        }
        return Events(*object);
    }

} // namespace Sora::Kernel
