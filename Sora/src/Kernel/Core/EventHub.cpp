#include "Sora/Kernel/Core/EventHub.h"

#include <algorithm>
#include <ranges>

namespace Sora::Kernel {

    EventHub::EventHub() noexcept
        : subscriptions_(std::make_shared<Detail::SubscriptionSnapshot>()),
          traces_(std::make_shared<Detail::TraceSnapshot>()) {}

    EventHub::~EventHub() noexcept {
        CancelAll();
    }

    EventLink EventHub::AddSubscription(Detail::SubscriptionRecord record) {
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

        return EventLink{std::move(state)};
    }

    EventTraceHook EventHub::AddTrace(Detail::TraceRecord record) {
        record.id = nextTrace_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.order = record.id;
        auto state = record.state;
        auto stored = std::shared_ptr<Detail::TraceRecord>{new Detail::TraceRecord(std::move(record))};

        {
            std::scoped_lock lock(mutex_);
            auto next = std::make_shared<Detail::TraceSnapshot>(*traces_);
            next->records.push_back(std::move(stored));
            std::ranges::stable_sort(next->records,
                                     [](const auto& lhs, const auto& rhs) noexcept { return lhs->order < rhs->order; });
            traces_ = std::move(next);
        }

        return EventTraceHook{std::move(state)};
    }

    std::shared_ptr<const Detail::SubscriptionSnapshot> EventHub::Subscriptions() const {
        std::scoped_lock lock(mutex_);
        return subscriptions_;
    }

    std::shared_ptr<const Detail::TraceSnapshot> EventHub::Traces() const {
        std::scoped_lock lock(mutex_);
        return traces_;
    }

    void EventHub::AttachInbound(std::weak_ptr<Detail::SubscriptionState> state) {
        std::scoped_lock lock(mutex_);
        std::erase_if(inbound_,
                      [](const std::weak_ptr<Detail::SubscriptionState>& relation) { return relation.expired(); });
        inbound_.push_back(std::move(state));
    }

    void EventHub::Trace(const EventTrace& trace) const {
        Trace(Traces(), trace);
    }

    void EventHub::Trace(std::shared_ptr<const Detail::TraceSnapshot> traces, const EventTrace& trace) {
        if (!traces) {
            return;
        }
        for (const auto& hook : traces->records) {
            if (hook && hook->state->active.load(std::memory_order_acquire)) {
                hook->callback(trace);
            }
        }
    }

    void EventHub::CancelAll() noexcept {
        std::scoped_lock lock(mutex_);
        for (const auto& subscription : subscriptions_->records) {
            if (subscription) {
                subscription->state->active.store(false, std::memory_order_release);
            }
        }
        std::erase_if(inbound_, [](const std::weak_ptr<Detail::SubscriptionState>& relation) {
            auto state = relation.lock();
            if (!state) {
                return true;
            }
            state->active.store(false, std::memory_order_release);
            return false;
        });
        for (const auto& trace : traces_->records) {
            if (trace) {
                trace->state->active.store(false, std::memory_order_release);
            }
        }
    }

    EventHub& Events(BaseUnknown& object) {
        using Detail::BaseUnknownInternal;

        BaseUnknown* owner = object.Nucleus();
        assert(owner != nullptr);

        const Iid hubIid = Traits::IidOf<EventHub>;
        if (BaseUnknown* existing = BaseUnknownInternal::FindExtensionNode(owner, hubIid)) {
            return *static_cast<EventHub*>(existing);
        }

        EventHub* hub = new EventHub;
        hub->ownerWeak_ = owner->GetComponentWeakRef();
        if (BaseUnknownInternal::AdoptExtensionNode(owner, hub)) {
            return *hub;
        }

        Release(hub);
        BaseUnknown* existing = BaseUnknownInternal::FindExtensionNode(owner, hubIid);
        assert(existing != nullptr);
        return *static_cast<EventHub*>(existing);
    }

    EventHub& Events(BaseUnknown* object) {
        assert(object != nullptr);
        return Events(*object);
    }

} // namespace Sora::Kernel
