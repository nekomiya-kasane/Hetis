/**
 * @file SchedulerTrace.cpp
 * @brief Implement normalized real-time scheduler tracing over the native platform transport.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/SchedulerTrace.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Platform.h>

#include "SchedulerTraceDecoder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Sora::PAL {

    struct SchedulerTraceSession::State {
        const SchedulerTraceSystemAPI* api = nullptr;
        SchedulerTraceSystemAPI::SessionHandle nativeSession = nullptr;
        std::jthread consumer{};
        SchedulerTraceClock clock{};
        std::vector<SchedulerEvent> queue{};
        size_t queueBegin = 0;
        size_t queueSize = 0;
        mutable std::mutex mutex{};
        std::condition_variable available{};
        std::atomic_bool running{false};
        std::atomic_bool stopping{false};
        std::atomic_uint64_t nativeEventsLost{};
        std::atomic_uint64_t nativeLogBuffersLost{};
        std::atomic_uint64_t nativeRealtimeBuffersLost{};
        std::atomic_uint64_t consumerEventsDropped{};
        std::atomic<ErrorCode> consumerError{ErrorCode::Ok};

        static void StoreMaximum(std::atomic_uint64_t& destination, uint64_t value) noexcept {
            uint64_t previous = destination.load(std::memory_order_relaxed);
            while (previous < value &&
                   !destination.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
            }
        }

        static void LossCallback(uint64_t eventsLost, void* userData) noexcept {
            if (userData != nullptr) {
                auto& state = *static_cast<State*>(userData);
                StoreMaximum(state.nativeEventsLost, eventsLost);
            }
        }

        static void EventCallback(const NativeSchedulerTraceRecord& record, void* userData) noexcept {
            if (userData != nullptr) {
                static_cast<State*>(userData)->Consume(record);
            }
        }

        void Consume(const NativeSchedulerTraceRecord& record) noexcept {
            const auto decoded = Detail::DecodeSchedulerTraceRecord(record);
            if (!decoded) {
                return;
            }

            std::scoped_lock lock(mutex);
            if (queueSize == queue.size()) {
                consumerEventsDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue[(queueBegin + queueSize) % queue.size()] = *decoded;
            ++queueSize;
            available.notify_one();
        }

        [[nodiscard]] SchedulerTraceLossCounters Losses() const noexcept {
            return {
                .nativeEventsLost = nativeEventsLost.load(std::memory_order_relaxed),
                .nativeLogBuffersLost = nativeLogBuffersLost.load(std::memory_order_relaxed),
                .nativeRealtimeBuffersLost = nativeRealtimeBuffersLost.load(std::memory_order_relaxed),
                .consumerEventsDropped = consumerEventsDropped.load(std::memory_order_relaxed),
            };
        }
    };

    SchedulerTraceSession::SchedulerTraceSession() noexcept = default;

    SchedulerTraceSession::SchedulerTraceSession(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}

    SchedulerTraceSession::SchedulerTraceSession(SchedulerTraceSession&&) noexcept = default;

    SchedulerTraceSession& SchedulerTraceSession::operator=(SchedulerTraceSession&& other) noexcept {
        if (this != &other) {
            (void)Stop();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    SchedulerTraceSession::~SchedulerTraceSession() noexcept {
        (void)Stop();
    }

    Result<SchedulerTraceSession> SchedulerTraceSession::Start(const SchedulerTraceOptions& options) {
        if (options.eventQueueCapacity == 0 || options.bufferSizeKilobytes == 0 || options.minimumBuffers == 0 ||
            options.maximumBuffers < options.minimumBuffers) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        const SchedulerTraceSystemAPI& api = LoadSchedulerTraceSystemAPI();
        if (!EnsureSystemAPIs(api.start, api.process, api.stop, api.close)) {
            return std::unexpected(ErrorCode::NotSupported);
        }

        std::unique_ptr<State> state;
        try {
            state = std::make_unique<State>();
            state->queue.resize(options.eventQueueCapacity);
        } catch (const std::bad_alloc&) {
            return std::unexpected(ErrorCode::OutOfMemory);
        } catch (const std::length_error&) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        state->api = &api;
        const NativeSchedulerTraceOptions nativeOptions{
            .bufferSizeKilobytes = options.bufferSizeKilobytes,
            .minimumBuffers = options.minimumBuffers,
            .maximumBuffers = options.maximumBuffers,
        };
        const ErrorCode startError =
            api.start(nativeOptions, &State::EventCallback, &State::LossCallback, state.get(), &state->nativeSession,
                      &state->clock.ticksPerSecond);
        if (startError != ErrorCode::Ok) {
            return std::unexpected(startError);
        }

        state->running.store(true, std::memory_order_release);
        State* consumerState = state.get();
        ErrorCode consumerStartError = ErrorCode::Ok;
        try {
            state->consumer = std::jthread{[consumerState] {
                const ErrorCode error = consumerState->api->process(consumerState->nativeSession);
                consumerState->consumerError.store(error, std::memory_order_release);
                consumerState->running.store(false, std::memory_order_release);
                consumerState->available.notify_all();
            }};
        } catch (const std::bad_alloc&) {
            consumerStartError = ErrorCode::OutOfMemory;
        } catch (const std::system_error&) {
            consumerStartError = ErrorCode::ResourceExhausted;
        } catch (...) {
            consumerStartError = ErrorCode::TraceCaptureFailed;
        }
        if (consumerStartError != ErrorCode::Ok) {
            NativeSchedulerTraceLosses losses{};
            (void)api.stop(state->nativeSession, &losses);
            api.close(state->nativeSession);
            state->nativeSession = nullptr;
            return std::unexpected(consumerStartError);
        }
        return SchedulerTraceSession{std::move(state)};
    }

    SchedulerTraceClock SchedulerTraceSession::Clock() const noexcept {
        return state_ != nullptr ? state_->clock : SchedulerTraceClock{};
    }

    Result<SchedulerTraceRead> SchedulerTraceSession::Read(std::span<SchedulerEvent> destination,
                                                           std::chrono::milliseconds timeout) {
        if (state_ == nullptr) {
            return std::unexpected(ErrorCode::InvalidState);
        }
        if (destination.empty() || timeout < std::chrono::milliseconds::zero()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::unique_lock lock(state_->mutex);
        if (state_->queueSize == 0 && timeout > std::chrono::milliseconds::zero()) {
            state_->available.wait_for(lock, timeout, [this] {
                return state_->queueSize != 0 || !state_->running.load(std::memory_order_acquire);
            });
        }
        const ErrorCode consumerError = state_->consumerError.load(std::memory_order_acquire);
        if (state_->queueSize == 0 && !state_->running.load(std::memory_order_acquire) &&
            consumerError != ErrorCode::Ok) {
            return std::unexpected(consumerError);
        }
        const size_t count = std::min(destination.size(), state_->queueSize);
        for (size_t index = 0; index < count; ++index) {
            destination[index] = state_->queue[(state_->queueBegin + index) % state_->queue.size()];
        }
        state_->queueBegin = (state_->queueBegin + count) % state_->queue.size();
        state_->queueSize -= count;
        lock.unlock();
        return SchedulerTraceRead{
            .eventCount = count,
            .running = state_->running.load(std::memory_order_acquire),
            .losses = state_->Losses(),
        };
    }

    SchedulerTraceLossCounters SchedulerTraceSession::LossCounters() const noexcept {
        return state_ != nullptr ? state_->Losses() : SchedulerTraceLossCounters{};
    }

    VoidResult SchedulerTraceSession::Stop() noexcept {
        if (state_ == nullptr || state_->stopping.exchange(true, std::memory_order_acq_rel)) {
            return {};
        }

        NativeSchedulerTraceLosses nativeLosses{};
        const ErrorCode stopError = state_->api->stop(state_->nativeSession, &nativeLosses);
        State::StoreMaximum(state_->nativeEventsLost, nativeLosses.eventsLost);
        State::StoreMaximum(state_->nativeLogBuffersLost, nativeLosses.logBuffersLost);
        State::StoreMaximum(state_->nativeRealtimeBuffersLost, nativeLosses.realtimeBuffersLost);
        if (state_->consumer.joinable()) {
            state_->consumer.join();
        }
        state_->api->close(state_->nativeSession);
        state_->nativeSession = nullptr;
        state_->running.store(false, std::memory_order_release);
        state_->available.notify_all();
        if (stopError != ErrorCode::Ok) {
            return std::unexpected(stopError);
        }
        const ErrorCode consumerError = state_->consumerError.load(std::memory_order_acquire);
        if (consumerError != ErrorCode::Ok) {
            return std::unexpected(consumerError);
        }
        return {};
    }

    SchedulerTraceSession::operator bool() const noexcept {
        return state_ != nullptr && state_->running.load(std::memory_order_acquire);
    }

} // namespace Sora::PAL
