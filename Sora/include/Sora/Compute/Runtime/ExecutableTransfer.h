/**
 * @file ExecutableTransfer.h
 * @brief Move-only validated transfer description submitted only as part of physical graph execution.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/TransferApi.h>

#include <cstdint>
#include <expected>
#include <new>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Owned inputs for one not-yet-submitted asynchronous physical transfer. */
    struct ExecutableTransferStorage {
        TransferEngine engine;
        Allocation source{};
        Allocation destination{};
        std::vector<TransferRegion> regions{};
        std::vector<Event> waits{};
    };

    /** @brief One validated asynchronous transfer retaining every resource until exactly one submission. */
    class ExecutableTransfer final {
    public:
        ExecutableTransfer(const ExecutableTransfer&) = delete;
        ExecutableTransfer& operator=(const ExecutableTransfer&) = delete;
        ExecutableTransfer(ExecutableTransfer&&) noexcept = default;
        ExecutableTransfer& operator=(ExecutableTransfer&&) noexcept = default;

        /** @brief Validate topology, physical ranges, and prerequisite event compatibility without submitting. */
        [[nodiscard]] static std::expected<ExecutableTransfer, TransferError>
        Create(ExecutableTransferStorage storage) noexcept {
            if (storage.engine.Kind() == TransferKind::CoherentAlias || !storage.source || !storage.destination ||
                storage.source.Location() != storage.engine.Source() ||
                storage.destination.Location() != storage.engine.Destination() ||
                !ValidTransferRegions(storage.source.View(), storage.destination.View(), storage.regions) ||
                storage.regions.size() > UINT32_MAX || storage.waits.size() > UINT32_MAX ||
                !ValidWaits(storage.waits)) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
            }
            return ExecutableTransfer{std::move(storage)};
        }

        /** @brief Return the backend that owns prerequisite and completion event handles. */
        [[nodiscard]] BackendKind CompletionBackend() const noexcept { return engine_.CompletionBackend(); }

        /** @brief Return the device that owns prerequisite and completion event handles. */
        [[nodiscard]] DeviceId CompletionDevice() const noexcept { return engine_.CompletionDevice(); }

        /** @brief Inject graph predecessor events and submit exactly once. */
        [[nodiscard]] std::expected<TransferSubmission, TransferError>
        Submit(std::vector<Event> dynamicWaits = {}) && noexcept {
            if (dynamicWaits.size() > UINT32_MAX - waits_.size() || !CompatibleWaits(engine_, dynamicWaits)) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
            }
            try {
                waits_.reserve(waits_.size() + dynamicWaits.size());
                for (Event& wait : dynamicWaits) {
                    waits_.push_back(std::move(wait));
                }
            } catch (const std::bad_alloc&) {
                return std::unexpected(TransferError{TransferErrorCode::OutOfMemory, 0});
            }
            return engine_.Submit(std::move(source_), std::move(destination_), regions_, std::move(waits_));
        }

    private:
        explicit ExecutableTransfer(ExecutableTransferStorage storage) noexcept
            : engine_(storage.engine),
              source_(std::move(storage.source)),
              destination_(std::move(storage.destination)),
              regions_(std::move(storage.regions)),
              waits_(std::move(storage.waits)) {}

        [[nodiscard]] static bool ValidWaits(const std::vector<Event>& waits) noexcept {
            for (const Event& wait : waits) {
                if (!wait) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] static bool CompatibleWaits(const TransferEngine& engine,
                                                  const std::vector<Event>& waits) noexcept {
            for (const Event& wait : waits) {
                const EventView view = wait ? wait.View() : EventView{};
                if (!view || view.backend != engine.CompletionBackend() || view.device != engine.CompletionDevice()) {
                    return false;
                }
            }
            return true;
        }

        TransferEngine engine_;
        Allocation source_{};
        Allocation destination_{};
        std::vector<TransferRegion> regions_{};
        std::vector<Event> waits_{};
    };

} // namespace Sora::Compute
