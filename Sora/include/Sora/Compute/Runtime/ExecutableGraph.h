/**
 * @file ExecutableGraph.h
 * @brief Move-only physical segment graph with device waits and host completion dependencies.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/DeviceObserver.h>
#include <Sora/Compute/Runtime/CompletionTransaction.h>
#include <Sora/Compute/Runtime/PhysicalSegment.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Dense identity of one topologically ordered physical batch segment. */
    struct ExecutableNodeId {
        uint32_t index = UINT32_MAX;

        /** @brief Return whether this identity can denote a segment. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return index != UINT32_MAX; }

        friend constexpr bool operator==(ExecutableNodeId, ExecutableNodeId) noexcept = default;
        friend constexpr auto operator<=>(ExecutableNodeId, ExecutableNodeId) noexcept = default;
    };

    /** @brief Synchronization mechanism joining two physical segments. */
    enum class ExecutableDependencyKind : uint8_t {
        DeviceWait,
        HostCompletion,
    };

    /** @brief One topologically forward dependency between physical segments. */
    struct ExecutableEdge {
        ExecutableNodeId predecessor{};
        ExecutableNodeId successor{};
        ExecutableDependencyKind kind = ExecutableDependencyKind::DeviceWait;

        friend constexpr bool operator==(ExecutableEdge, ExecutableEdge) noexcept = default;
    };

    /** @brief Stable graph-construction failure before any backend work is accepted. */
    enum class ExecutableGraphErrorCode : uint8_t {
        EmptyGraph,
        CapacityExceeded,
        InvalidEdge,
        IncompatibleDeviceWait,
        InvalidCompletionTransaction,
        InvalidDeviceLease,
    };

    /** @brief Graph-construction failure with an optional edge identity. */
    struct ExecutableGraphError {
        ExecutableGraphErrorCode code = ExecutableGraphErrorCode::EmptyGraph;
        uint32_t predecessor = UINT32_MAX;
        uint32_t successor = UINT32_MAX;

        friend constexpr bool operator==(ExecutableGraphError, ExecutableGraphError) noexcept = default;
    };

    /** @brief Submission failure after zero or more predecessor segments may have been accepted and drained. */
    struct GraphSubmissionError {
        uint32_t node = UINT32_MAX;
        SubmissionError error{};

        friend constexpr bool operator==(GraphSubmissionError, GraphSubmissionError) noexcept = default;
    };

    /** @brief Owned physical graph inputs before topology validation and canonicalization. */
    struct ExecutableGraphStorage {
        std::vector<DeviceLease> deviceLeases{};
        std::vector<PhysicalSegment> segments{};
        std::vector<ExecutableEdge> edges{};
        std::optional<CompletionTransaction> completion{};
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidExecutableDependencyKind(ExecutableDependencyKind kind) noexcept {
            return kind == ExecutableDependencyKind::DeviceWait || kind == ExecutableDependencyKind::HostCompletion;
        }

        [[nodiscard]] constexpr bool ExecutableEdgeLess(const ExecutableEdge& left,
                                                        const ExecutableEdge& right) noexcept {
            if (left.successor != right.successor) {
                return left.successor < right.successor;
            }
            if (left.predecessor != right.predecessor) {
                return left.predecessor < right.predecessor;
            }
            return std::to_underlying(left.kind) < std::to_underlying(right.kind);
        }

        [[nodiscard]] constexpr SubmissionError ObservationError(EventObservation observation) noexcept {
            if (observation.error.code != SubmissionErrorCode::None) {
                return observation.error;
            }
            return {SubmissionErrorCode::SubmissionFailed, 0};
        }

        inline void DrainAccepted(std::span<const PhysicalSubmission> submissions) noexcept {
            for (const PhysicalSubmission& submission : submissions) {
                (void)submission.WaitUntilTerminal();
            }
        }

    } // namespace Detail

    /** @brief Accepted graph work retaining every segment submission until graph completion. */
    class GraphSubmission {
    public:
        GraphSubmission(const GraphSubmission&) = delete;
        GraphSubmission& operator=(const GraphSubmission&) = delete;
        GraphSubmission(GraphSubmission&& other) noexcept
            : deviceLeases_(std::move(other.deviceLeases_)),
              submissions_(std::move(other.submissions_)),
              completion_(std::move(other.completion_)),
              finalization_(std::move(other.finalization_)) {
            other.completion_.reset();
        }
        GraphSubmission& operator=(GraphSubmission&& other) noexcept {
            if (this != &other) {
                (void)WaitUntilTerminal();
                deviceLeases_ = std::move(other.deviceLeases_);
                submissions_ = std::move(other.submissions_);
                completion_ = std::move(other.completion_);
                finalization_ = std::move(other.finalization_);
                other.completion_.reset();
            }
            return *this;
        }

        /** @brief Drain accepted work and publish successful results before releasing retained resources. */
        ~GraphSubmission() noexcept { (void)WaitUntilTerminal(); }

        /** @brief Observe every segment; remain pending until all accepted work is terminal. */
        [[nodiscard]] EventObservation Query() noexcept {
            if (finalization_) {
                return *finalization_;
            }
            bool pending = false;
            std::optional<EventObservation> firstFailure;
            for (const PhysicalSubmission& submission : submissions_) {
                const EventObservation observation = submission.Query();
                if (observation.status == EventStatus::Pending) {
                    pending = true;
                } else if (observation.status != EventStatus::Completed && !firstFailure) {
                    firstFailure = observation;
                }
            }
            if (pending) {
                return EventObservation{.status = EventStatus::Pending};
            }
            return firstFailure ? FinalizeFailure(*firstFailure) : FinalizeSuccess();
        }

        /** @brief Wait for every segment within one total timeout budget. */
        [[nodiscard]] EventObservation Wait(uint64_t timeoutNanoseconds) noexcept {
            if (finalization_) {
                return *finalization_;
            }
            if (timeoutNanoseconds == UINT64_MAX) {
                return WaitUntilTerminal();
            }

            using Clock = std::chrono::steady_clock;
            std::optional<EventObservation> firstFailure;
            const auto start = Clock::now();
            for (const PhysicalSubmission& submission : submissions_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
                const uint64_t elapsedCount = elapsed.count() < 0 ? 0 : static_cast<uint64_t>(elapsed.count());
                const uint64_t remaining = elapsedCount < timeoutNanoseconds ? timeoutNanoseconds - elapsedCount : 0;
                const EventObservation observation = submission.Wait(remaining);
                if (observation.status == EventStatus::Pending) {
                    return observation;
                }
                if (observation.status != EventStatus::Completed && !firstFailure) {
                    firstFailure = observation;
                }
            }
            return firstFailure ? FinalizeFailure(*firstFailure) : FinalizeSuccess();
        }

        /** @brief Wait without a deadline for every segment and finalize publication exactly once. */
        [[nodiscard]] EventObservation WaitUntilTerminal() noexcept {
            if (finalization_) {
                return *finalization_;
            }
            std::optional<EventObservation> firstFailure;
            for (const PhysicalSubmission& submission : submissions_) {
                const EventObservation observation = submission.WaitUntilTerminal();
                if (observation.status != EventStatus::Completed && !firstFailure) {
                    firstFailure = observation;
                }
            }
            return firstFailure ? FinalizeFailure(*firstFailure) : FinalizeSuccess();
        }

        /** @brief Return the accepted physical segment count. */
        [[nodiscard]] size_t SegmentCount() const noexcept { return submissions_.size(); }

    private:
        friend class ExecutableGraph;

        explicit GraphSubmission(std::vector<DeviceLease> deviceLeases, std::vector<PhysicalSubmission> submissions,
                                 std::optional<CompletionTransaction> completion) noexcept
            : deviceLeases_(std::move(deviceLeases)),
              submissions_(std::move(submissions)),
              completion_(std::move(completion)) {}

        [[nodiscard]] EventObservation FinalizeFailure(EventObservation observation) noexcept {
            completion_.reset();
            finalization_ = observation;
            return observation;
        }

        [[nodiscard]] EventObservation FinalizeSuccess() noexcept {
            for (const DeviceLease& lease : deviceLeases_) {
                const auto current = lease.Validate();
                if (!current) {
                    completion_.reset();
                    finalization_ = EventObservation{
                        .status = EventStatus::Failed,
                        .error = {SubmissionErrorCode::DeviceLost,
                                  static_cast<uint64_t>(std::to_underlying(current.error()))},
                    };
                    return *finalization_;
                }
            }
            if (completion_) {
                auto committed = std::move(*completion_).Commit();
                completion_.reset();
                if (!committed) {
                    const PublicationError error = committed.error();
                    const uint64_t detail = (static_cast<uint64_t>(std::to_underlying(error.code)) << 32) |
                                            static_cast<uint64_t>(error.action);
                    finalization_ = EventObservation{
                        .status = EventStatus::Failed,
                        .error = {SubmissionErrorCode::PublicationFailed, detail},
                    };
                    return *finalization_;
                }
            }
            finalization_ = EventObservation{.status = EventStatus::Completed};
            return *finalization_;
        }

        std::vector<DeviceLease> deviceLeases_{};
        std::vector<PhysicalSubmission> submissions_{};
        std::optional<CompletionTransaction> completion_{};
        std::optional<EventObservation> finalization_{};
    };

    /** @brief Immutable-topology, move-only graph of queue-local physical command batches. */
    class ExecutableGraph {
    public:
        ExecutableGraph(const ExecutableGraph&) = delete;
        ExecutableGraph& operator=(const ExecutableGraph&) = delete;
        ExecutableGraph(ExecutableGraph&&) noexcept = default;
        ExecutableGraph& operator=(ExecutableGraph&&) noexcept = default;

        /** @brief Validate topology and device-wait compatibility without submitting backend work. */
        [[nodiscard]] static std::expected<ExecutableGraph, ExecutableGraphError>
        Create(ExecutableGraphStorage storage) noexcept {
            if (storage.segments.empty()) {
                return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::EmptyGraph});
            }
            if (storage.segments.size() > UINT32_MAX) {
                return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::CapacityExceeded});
            }
            if (storage.completion && storage.completion->ActionCount() == 0) {
                return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::InvalidCompletionTransaction});
            }
            if (std::ranges::any_of(storage.deviceLeases, [](const DeviceLease& lease) { return !lease; })) {
                return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::InvalidDeviceLease});
            }
            for (const ExecutableEdge& edge : storage.edges) {
                if (!edge.predecessor.Valid() || !edge.successor.Valid() ||
                    edge.predecessor.index >= storage.segments.size() ||
                    edge.successor.index >= storage.segments.size() || edge.predecessor.index >= edge.successor.index ||
                    !Detail::ValidExecutableDependencyKind(edge.kind)) {
                    return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::InvalidEdge,
                                                                edge.predecessor.index, edge.successor.index});
                }
                if (edge.kind == ExecutableDependencyKind::DeviceWait) {
                    const PhysicalSynchronizationDomain predecessor =
                        storage.segments[edge.predecessor.index].SynchronizationDomain();
                    const PhysicalSynchronizationDomain successor =
                        storage.segments[edge.successor.index].SynchronizationDomain();
                    if (predecessor != successor) {
                        return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::IncompatibleDeviceWait,
                                                                    edge.predecessor.index, edge.successor.index});
                    }
                }
            }
            std::ranges::sort(storage.edges, Detail::ExecutableEdgeLess);
            for (size_t index = 1; index < storage.edges.size(); ++index) {
                const ExecutableEdge& previous = storage.edges[index - 1];
                const ExecutableEdge& current = storage.edges[index];
                if (previous.predecessor == current.predecessor && previous.successor == current.successor &&
                    previous.kind != current.kind) {
                    return std::unexpected(ExecutableGraphError{ExecutableGraphErrorCode::InvalidEdge,
                                                                current.predecessor.index, current.successor.index});
                }
            }
            storage.edges.erase(std::ranges::unique(storage.edges).begin(), storage.edges.end());
            return ExecutableGraph{std::move(storage.deviceLeases), std::move(storage.segments),
                                   std::move(storage.edges), std::move(storage.completion)};
        }

        /** @brief Return the queue-local physical segment count. */
        [[nodiscard]] size_t SegmentCount() const noexcept { return segments_.size(); }

        /**
         * @brief Submit all topologically ordered segments and drain already accepted work on any later failure.
         * @details DeviceWait edges inject predecessor events into the successor queue request. HostCompletion edges
         * wait before successor submission and therefore never cross an incompatible backend event ABI.
         */
        [[nodiscard]] std::expected<GraphSubmission, GraphSubmissionError> Submit() && noexcept {
            for (const DeviceLease& lease : deviceLeases_) {
                const auto current = lease.Validate();
                if (!current) {
                    const SubmissionErrorCode code = current.error() == DeviceObservationError::Unavailable
                                                         ? SubmissionErrorCode::QueueUnavailable
                                                         : SubmissionErrorCode::DeviceLost;
                    return std::unexpected(GraphSubmissionError{
                        UINT32_MAX, {code, static_cast<uint64_t>(std::to_underlying(current.error()))}});
                }
            }
            std::vector<PhysicalSubmission> accepted;
            try {
                accepted.reserve(segments_.size());
                size_t firstEdge = 0;
                for (uint32_t node = 0; node < segments_.size(); ++node) {
                    std::vector<Event> deviceWaits;
                    size_t lastEdge = firstEdge;
                    while (lastEdge < edges_.size() && edges_[lastEdge].successor.index == node) {
                        ++lastEdge;
                    }
                    for (size_t edgeIndex = firstEdge; edgeIndex < lastEdge; ++edgeIndex) {
                        const ExecutableEdge& edge = edges_[edgeIndex];
                        const PhysicalSubmission& predecessor = accepted[edge.predecessor.index];
                        if (edge.kind == ExecutableDependencyKind::DeviceWait) {
                            deviceWaits.push_back(predecessor.Completion());
                            continue;
                        }
                        const EventObservation observation = predecessor.WaitUntilTerminal();
                        if (observation.status != EventStatus::Completed) {
                            Detail::DrainAccepted(accepted);
                            return std::unexpected(GraphSubmissionError{node, Detail::ObservationError(observation)});
                        }
                    }
                    auto submitted = std::move(segments_[node]).Submit(std::move(deviceWaits));
                    if (!submitted) {
                        Detail::DrainAccepted(accepted);
                        return std::unexpected(GraphSubmissionError{node, submitted.error()});
                    }
                    accepted.push_back(std::move(*submitted));
                    firstEdge = lastEdge;
                }
                return GraphSubmission{std::move(deviceLeases_), std::move(accepted), std::move(completion_)};
            } catch (const std::bad_alloc&) {
                Detail::DrainAccepted(accepted);
                return std::unexpected(GraphSubmissionError{static_cast<uint32_t>(accepted.size()),
                                                            {SubmissionErrorCode::OutOfMemory, 0}});
            }
        }

    private:
        ExecutableGraph(std::vector<DeviceLease> deviceLeases, std::vector<PhysicalSegment> segments,
                        std::vector<ExecutableEdge> edges, std::optional<CompletionTransaction> completion) noexcept
            : deviceLeases_(std::move(deviceLeases)),
              segments_(std::move(segments)),
              edges_(std::move(edges)),
              completion_(std::move(completion)) {}

        std::vector<DeviceLease> deviceLeases_{};
        std::vector<PhysicalSegment> segments_{};
        std::vector<ExecutableEdge> edges_{};
        std::optional<CompletionTransaction> completion_{};
    };

    static_assert(std::is_trivially_copyable_v<ExecutableNodeId>);
    static_assert(std::is_trivially_copyable_v<ExecutableEdge>);

} // namespace Sora::Compute
