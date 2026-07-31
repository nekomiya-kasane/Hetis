/**
 * @file RuntimeEpoch.cpp
 * @brief Validation and observation for immutable heterogeneous runtime epochs.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <algorithm>
#include <new>
#include <ranges>
#include <tuple>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr auto DeviceKey(const RuntimeDeviceBinding& device) noexcept {
            return std::tuple{std::to_underlying(device.descriptor.backend), device.descriptor.id, device.service};
        }

        [[nodiscard]] constexpr BackendServiceId ServiceKey(const RuntimeDeviceBinding& device) noexcept {
            return device.service;
        }

        [[nodiscard]] constexpr RuntimeEpochError
        Failure(RuntimeEpochErrorCode code, uint32_t device = UINT32_MAX,
                DeviceObservationError cause = DeviceObservationError::InvalidApi) noexcept {
            return {.code = code, .device = device, .cause = cause};
        }

        [[nodiscard]] std::expected<DeviceSnapshot, RuntimeEpochError>
        ObserveBinding(const RuntimeDeviceBinding& binding, uint32_t index,
                       const PhysicalResourceCatalog& resources) noexcept {
            const DeviceObserver* observer = resources.FindDeviceObserver(binding.service);
            if (observer == nullptr) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::MissingDeviceObserver, index));
            }
            if (observer->Backend() != binding.descriptor.backend || observer->Device() != binding.descriptor.id) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::IncompatibleDeviceObserver, index));
            }
            auto snapshot = observer->Observe();
            if (!snapshot) {
                return std::unexpected(
                    Failure(RuntimeEpochErrorCode::DeviceObservationFailed, index, snapshot.error()));
            }
            if (snapshot->state == DeviceState::Lost) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::DeviceLost, index, DeviceObservationError::Lost));
            }
            return *snapshot;
        }

    } // namespace

    std::expected<RuntimeEpoch, RuntimeEpochError>
    RuntimeEpoch::Create(uint64_t sequence, PhysicalResourceCatalog resources,
                         std::vector<RuntimeDeviceBinding> devices) noexcept {
        if (sequence == 0) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::InvalidSequence));
        }
        if (devices.empty()) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::EmptyDeviceSet));
        }
        if (devices.size() > UINT32_MAX) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::CapacityExceeded));
        }
        for (uint32_t index = 0; index < devices.size(); ++index) {
            const RuntimeDeviceBinding& device = devices[index];
            if (!device.service.Valid() || !ValidDeviceDescriptor(device.descriptor) || !device.target.Valid() ||
                device.target.device != device.descriptor.id || device.target.backend != device.descriptor.backend ||
                device.target.features != device.descriptor.features ||
                device.target.architectureId != device.descriptor.architectureId) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::InvalidDeviceBinding, index));
            }
        }
        std::ranges::sort(devices, {}, ServiceKey);
        for (uint32_t index = 1; index < devices.size(); ++index) {
            if (devices[index - 1].service == devices[index].service) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::DuplicateService, index));
            }
        }
        std::ranges::sort(devices, {}, DeviceKey);
        for (uint32_t index = 1; index < devices.size(); ++index) {
            if (devices[index - 1].descriptor.backend == devices[index].descriptor.backend &&
                devices[index - 1].descriptor.id == devices[index].descriptor.id) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::DuplicateBackendDevice, index));
            }
        }
        for (uint32_t index = 0; index < devices.size(); ++index) {
            const DeviceObserver* observer = resources.FindDeviceObserver(devices[index].service);
            if (observer == nullptr) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::MissingDeviceObserver, index));
            }
            if (observer->Backend() != devices[index].descriptor.backend ||
                observer->Device() != devices[index].descriptor.id) {
                return std::unexpected(Failure(RuntimeEpochErrorCode::IncompatibleDeviceObserver, index));
            }
        }
        if (resources.DeviceObserverCount() != devices.size()) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::DeviceSetMismatch));
        }

        try {
            std::vector<DeviceSnapshot> observed;
            observed.reserve(devices.size());
            for (uint32_t index = 0; index < devices.size(); ++index) {
                auto snapshot = ObserveBinding(devices[index], index, resources);
                if (!snapshot) {
                    return std::unexpected(snapshot.error());
                }
                observed.push_back(*snapshot);
            }
            return RuntimeEpoch{sequence, std::move(resources), std::move(devices), std::move(observed)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::OutOfMemory));
        }
    }

    std::expected<std::vector<DeviceSnapshot>, RuntimeEpochError>
    RuntimeEpoch::RefreshDeviceSnapshots() const noexcept {
        try {
            std::vector<DeviceSnapshot> refreshed;
            refreshed.reserve(devices_.size());
            for (uint32_t index = 0; index < devices_.size(); ++index) {
                auto snapshot = ObserveBinding(devices_[index], index, resources_);
                if (!snapshot) {
                    return std::unexpected(snapshot.error());
                }
                if (snapshot->generation != snapshots_[index].generation) {
                    return std::unexpected(Failure(RuntimeEpochErrorCode::DeviceGenerationChanged, index,
                                                   DeviceObservationError::GenerationChanged));
                }
                refreshed.push_back(*snapshot);
            }
            return refreshed;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeEpochErrorCode::OutOfMemory));
        }
    }

    std::expected<RuntimeEpochDiff, RuntimeEpochDiffError> DiffRuntimeEpochs(const RuntimeEpoch& previous,
                                                                             const RuntimeEpoch& replacement) noexcept {
        try {
            RuntimeEpochDiff result;
            result.retainedServices.reserve(std::min(previous.Devices().size(), replacement.Devices().size()));
            result.removedServices.reserve(previous.Devices().size());
            result.addedServices.reserve(replacement.Devices().size());
            result.replacements.reserve(std::min(previous.Devices().size(), replacement.Devices().size()));

            std::vector<const RuntimeDeviceBinding*> previousByService;
            std::vector<const RuntimeDeviceBinding*> replacementByService;
            previousByService.reserve(previous.Devices().size());
            replacementByService.reserve(replacement.Devices().size());
            for (const RuntimeDeviceBinding& binding : previous.Devices()) {
                previousByService.push_back(std::addressof(binding));
            }
            for (const RuntimeDeviceBinding& binding : replacement.Devices()) {
                replacementByService.push_back(std::addressof(binding));
            }
            const auto service = [](const RuntimeDeviceBinding* binding) { return binding->service; };
            std::ranges::sort(previousByService, {}, service);
            std::ranges::sort(replacementByService, {}, service);
            size_t previousIndex = 0;
            size_t replacementIndex = 0;
            while (previousIndex < previousByService.size() && replacementIndex < replacementByService.size()) {
                const RuntimeDeviceBinding& oldBinding = *previousByService[previousIndex];
                const RuntimeDeviceBinding& newBinding = *replacementByService[replacementIndex];
                if (oldBinding.service < newBinding.service) {
                    result.removedServices.push_back(oldBinding.service);
                    ++previousIndex;
                } else if (newBinding.service < oldBinding.service) {
                    result.addedServices.push_back(newBinding.service);
                    ++replacementIndex;
                } else {
                    if (oldBinding != newBinding) {
                        return std::unexpected(RuntimeEpochDiffError{RuntimeEpochDiffErrorCode::ServiceIdentityConflict,
                                                                     oldBinding.service});
                    }
                    result.retainedServices.push_back(oldBinding.service);
                    ++previousIndex;
                    ++replacementIndex;
                }
            }
            for (; previousIndex < previousByService.size(); ++previousIndex) {
                result.removedServices.push_back(previousByService[previousIndex]->service);
            }
            for (; replacementIndex < replacementByService.size(); ++replacementIndex) {
                result.addedServices.push_back(replacementByService[replacementIndex]->service);
            }

            const auto coordinate = [](const RuntimeDeviceBinding& binding) {
                return std::tuple{std::to_underlying(binding.descriptor.backend), binding.descriptor.id};
            };
            size_t oldCoordinate = 0;
            size_t newCoordinate = 0;
            const auto oldDevices = previous.Devices();
            const auto newDevices = replacement.Devices();
            while (oldCoordinate < oldDevices.size() && newCoordinate < newDevices.size()) {
                const auto oldKey = coordinate(oldDevices[oldCoordinate]);
                const auto newKey = coordinate(newDevices[newCoordinate]);
                if (oldKey < newKey) {
                    ++oldCoordinate;
                } else if (newKey < oldKey) {
                    ++newCoordinate;
                } else {
                    const RuntimeDeviceBinding& oldBinding = oldDevices[oldCoordinate];
                    const RuntimeDeviceBinding& newBinding = newDevices[newCoordinate];
                    if (oldBinding.service != newBinding.service) {
                        result.replacements.push_back({.backend = oldBinding.descriptor.backend,
                                                       .device = oldBinding.descriptor.id,
                                                       .previousService = oldBinding.service,
                                                       .replacementService = newBinding.service,
                                                       .targetChanged = oldBinding.target != newBinding.target});
                    }
                    ++oldCoordinate;
                    ++newCoordinate;
                }
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(RuntimeEpochDiffError{RuntimeEpochDiffErrorCode::OutOfMemory, {}});
        }
    }

} // namespace Sora::Compute
