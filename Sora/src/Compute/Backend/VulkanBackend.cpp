/**
 * @file VulkanBackend.cpp
 * @brief Slang/SPIR-V compiler and Vulkan compute-pipeline backend implementation.
 * @ingroup Compute
 */
#include <Sora/Compute/Backend/VulkanBackend.h>

#include "SlangKernelLowering.h"
#include "VulkanExecution.h"

#include <Sora/Compute/KernelProgramPackage.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>
#include <Sora/Compute/WireReader.h>
#include <Sora/Platform.h>

#include <slang-com-ptr.h>
#include <slang.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Sora::Compute {

    namespace {

        inline constexpr uint32_t kSpirvMagic = 0x07230203U;
        inline constexpr uint32_t kExecutableEnvelopeMagic = 0x58455653U;    // "SVEX"
        inline constexpr uint32_t kPipelineCacheEnvelopeMagic = 0x43505653U; // "SVPC"
        inline constexpr uint16_t kVulkanArtifactEnvelopeVersion = 2;
        inline constexpr uint32_t kPipelineCacheExportAttempts = 3;
        inline constexpr uint16_t kMaximumCompilerWorkerCount = 16;
        inline constexpr uint64_t kExecutableHandleMagic = 0x534F52415F564558ULL;
        inline constexpr uint64_t kPipelineHandleMagic = 0x534F52415F56504CULL;
        inline constexpr char kPortabilitySubsetExtension[] = "VK_KHR_portability_subset";

        struct VulkanDeviceFacts {
            VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
            DeviceId device{};
            VkPhysicalDeviceProperties properties{};
            VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceDriverProperties driverProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceSubgroupProperties subgroupProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceMaintenance3Properties maintenance3Properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
            VkPhysicalDeviceTimelineSemaphoreProperties timelineProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            VkPhysicalDeviceFeatures features{};
            VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            uint32_t computeQueueFamily = UINT32_MAX;
            uint32_t timestampValidBits = 0;
            bool dedicatedComputeQueue = false;
        };

        class SlangCompilerPool;

        struct VulkanPlatformState {
            VolkInstanceTable instanceApi{};
            VkInstance instance = VK_NULL_HANDLE;

            ~VulkanPlatformState() noexcept;
        };

        struct VulkanServiceState {
            std::shared_ptr<VulkanPlatformState> platform{};
            VolkDeviceTable deviceApi{};
            VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            VkPipelineCache pipelineCache = VK_NULL_HANDLE;
            uint32_t computeQueueFamily = UINT32_MAX;
            DeviceDescriptor descriptor{};
            TargetFingerprint target{};
            SemanticHash compilerIdentity{};
            SemanticHash pipelineIdentity{};
            SemanticHash pipelineStateIdentity{};
            std::mutex pipelineCacheMutex{};
            std::shared_ptr<SlangCompilerPool> compilerPool{};
            std::unique_ptr<Detail::VulkanExecutionState, Detail::VulkanExecutionDeleter> execution{};

            ~VulkanServiceState() noexcept;
        };

        struct VulkanExecutableHandle {
            uint64_t magic = kExecutableHandleMagic;
            VulkanServiceState* owner = nullptr;
            ArtifactKey key{};
            Detail::VulkanKernelProgram program{};
            Detail::VulkanDispatchAbi dispatchAbi{};
            std::vector<uint32_t> spirv{};
        };

        struct VulkanPipelineHandle {
            uint64_t magic = kPipelineHandleMagic;
            VulkanServiceState* owner = nullptr;
            ArtifactKey key{};
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            Detail::VulkanKernelProgram program{};
            Detail::VulkanDispatchAbi dispatchAbi{};
        };

        struct StaticVulkanBackendCode {};

        [[nodiscard]] uint64_t DriverDetail(VkResult result) noexcept {
            return static_cast<uint64_t>(static_cast<int64_t>(result));
        }

        [[nodiscard]] SemanticHash HashBytes(std::span<const std::byte> bytes) noexcept {
            return Detail::HashBytes(bytes);
        }

        [[nodiscard]] SemanticHash HashText(std::string_view text) noexcept {
            return HashBytes(std::as_bytes(std::span{text.data(), text.size()}));
        }

        [[nodiscard]] SemanticHash HashIdentity(std::string_view domain, std::string_view version) noexcept {
            Detail::SemanticHashBuilder builder;
            builder.Append(std::as_bytes(std::span{domain.data(), domain.size()}));
            builder.Append(std::byte{0});
            builder.Append(std::as_bytes(std::span{version.data(), version.size()}));
            return builder.Finish();
        }

        [[nodiscard]] DeviceId DeviceIdentity(const VkPhysicalDeviceIDProperties& ids) noexcept {
            DeviceId result{};
            std::memcpy(&result.low, ids.deviceUUID, sizeof(result.low));
            std::memcpy(&result.high, ids.deviceUUID + sizeof(result.low), sizeof(result.high));
            return result;
        }

        [[nodiscard]] uint64_t ArchitectureIdentity(const VulkanDeviceFacts& facts) noexcept {
            Detail::SemanticHashBuilder builder;
            builder.AppendLittleEndian(facts.properties.vendorID);
            builder.AppendLittleEndian(facts.properties.deviceID);
            builder.AppendLittleEndian(facts.properties.deviceType);
            builder.AppendLittleEndian(facts.subgroupProperties.subgroupSize);
            return builder.Finish().low;
        }

        [[nodiscard]] SemanticHash DriverIdentity(const VulkanDeviceFacts& facts) noexcept {
            Detail::SemanticHashBuilder builder;
            builder.Append(std::as_bytes(std::span{facts.idProperties.driverUUID, VK_UUID_SIZE}));
            builder.Append(std::as_bytes(std::span{facts.properties.pipelineCacheUUID, VK_UUID_SIZE}));
            builder.AppendLittleEndian(facts.properties.driverVersion);
            builder.AppendLittleEndian(facts.properties.apiVersion);
            builder.AppendLittleEndian(facts.driverProperties.driverID);
            return builder.Finish();
        }

        [[nodiscard]] uint64_t TotalDeviceMemory(const VkPhysicalDeviceMemoryProperties& memory) noexcept {
            uint64_t total = 0;
            for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
                if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                    total = total > UINT64_MAX - memory.memoryHeaps[heap].size ? UINT64_MAX
                                                                               : total + memory.memoryHeaps[heap].size;
                }
            }
            return total;
        }

        [[nodiscard]] DeviceFeatureMask DeviceFeatures(const VulkanDeviceFacts& facts) noexcept {
            DeviceFeatureMask features;
            const bool computeSubgroups = (facts.subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
            if (computeSubgroups) {
                features |= DeviceFeature::Subgroup;
            }
            if (facts.features.shaderFloat64 == VK_TRUE) {
                features |= DeviceFeature::Float64;
            }
            if (facts.features.shaderInt64 == VK_TRUE) {
                features |= DeviceFeature::Int64;
            }
            if (facts.features12.shaderFloat16 == VK_TRUE &&
                facts.features11.uniformAndStorageBuffer16BitAccess == VK_TRUE) {
                features |= DeviceFeature::Float16;
            }
            if (facts.features12.shaderInt8 == VK_TRUE &&
                facts.features12.uniformAndStorageBuffer8BitAccess == VK_TRUE) {
                features |= DeviceFeature::Int8;
            }
            if (facts.features.shaderInt16 == VK_TRUE &&
                facts.features11.uniformAndStorageBuffer16BitAccess == VK_TRUE) {
                features |= DeviceFeature::Int16;
            }
            if (computeSubgroups &&
                (facts.subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0) {
                features |= DeviceFeature::SubgroupShuffle;
            }
            if (facts.features12.timelineSemaphore == VK_TRUE) {
                features |= DeviceFeature::TimelineEvents;
                features |= DeviceFeature::AsynchronousCopy;
            }
            return features;
        }

        [[nodiscard]] DeviceKind DeviceType(VkPhysicalDeviceType type) noexcept {
            switch (type) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return DeviceKind::IntegratedGpu;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return DeviceKind::DiscreteGpu;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    return DeviceKind::Cpu;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                    return DeviceKind::Accelerator;
                default:
                    return DeviceKind::Accelerator;
            }
        }

        [[nodiscard]] std::expected<std::optional<VulkanDeviceFacts>, VulkanBackendError>
        QueryDeviceFacts(const VolkInstanceTable& api, VkPhysicalDevice physicalDevice,
                         bool preferDedicatedComputeQueue) noexcept {
            VulkanDeviceFacts facts{};
            facts.physicalDevice = physicalDevice;
            facts.idProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            facts.driverProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            facts.subgroupProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            facts.maintenance3Properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
            facts.idProperties.pNext = &facts.driverProperties;
            facts.driverProperties.pNext = &facts.subgroupProperties;
            facts.subgroupProperties.pNext = &facts.maintenance3Properties;
            facts.maintenance3Properties.pNext = &facts.timelineProperties;
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &facts.idProperties;
            api.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
            facts.properties = properties.properties;
            facts.device = DeviceIdentity(facts.idProperties);
            if (!facts.device.Valid()) {
                return std::optional<VulkanDeviceFacts>{};
            }

            facts.features11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            facts.features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            facts.features11.pNext = &facts.features12;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &facts.features11;
            api.vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
            facts.features = features.features;
            api.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &facts.memoryProperties);

            uint32_t queueCount = 0;
            api.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
            try {
                std::vector<VkQueueFamilyProperties> queues(queueCount);
                api.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());
                for (uint32_t queue = 0; queue < queueCount; ++queue) {
                    const bool compute = (queues[queue].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                    const bool graphics = (queues[queue].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                    const auto candidateQuality =
                        std::tuple{queues[queue].timestampValidBits != 0, preferDedicatedComputeQueue && !graphics,
                                   queues[queue].timestampValidBits};
                    const auto selectedQuality = std::tuple{facts.timestampValidBits != 0,
                                                            preferDedicatedComputeQueue && facts.dedicatedComputeQueue,
                                                            facts.timestampValidBits};
                    if (compute && (facts.computeQueueFamily == UINT32_MAX || candidateQuality > selectedQuality)) {
                        facts.computeQueueFamily = queue;
                        facts.timestampValidBits = queues[queue].timestampValidBits;
                        facts.dedicatedComputeQueue = !graphics;
                    }
                }
            } catch (const std::bad_alloc&) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, queueCount});
            } catch (...) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, queueCount});
            }
            if (facts.properties.apiVersion < VK_API_VERSION_1_2 || facts.computeQueueFamily == UINT32_MAX ||
                facts.features12.timelineSemaphore != VK_TRUE) {
                return std::optional<VulkanDeviceFacts>{};
            }
            return std::optional<VulkanDeviceFacts>{std::move(facts)};
        }

        [[nodiscard]] int64_t DeviceScore(const VulkanDeviceFacts& facts, const VulkanBackendConfig& config) noexcept {
            int64_t score = 0;
            if (facts.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += config.preferDiscreteDevice ? 1'000'000 : 100'000;
            } else if (facts.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                score += config.preferDiscreteDevice ? 100'000 : 1'000'000;
            }
            if (facts.dedicatedComputeQueue && config.preferDedicatedComputeQueue) {
                score += 10'000;
            }
            score += static_cast<int64_t>(
                std::min<uint64_t>(TotalDeviceMemory(facts.memoryProperties) >> 20U, uint64_t{50'000}));
            return score;
        }

        [[nodiscard]] auto DriverContextQuality(const VulkanDeviceFacts& facts) noexcept {
            const SemanticHash driver = DriverIdentity(facts);
            return std::tuple{facts.properties.apiVersion,
                              DeviceFeatures(facts).Count(),
                              facts.dedicatedComputeQueue,
                              facts.properties.limits.maxComputeWorkGroupInvocations,
                              TotalDeviceMemory(facts.memoryProperties),
                              driver.high,
                              driver.low};
        }

        [[nodiscard]] std::expected<std::vector<VulkanDeviceFacts>, VulkanBackendError>
        EnumerateCompatibleDevices(const VulkanPlatformState& platform, bool preferDedicatedComputeQueue) noexcept {
            try {
                std::vector<VkPhysicalDevice> physicalDevices;
                bool complete = false;
                for (uint32_t attempt = 0; attempt < 3; ++attempt) {
                    uint32_t count = 0;
                    VkResult result =
                        platform.instanceApi.vkEnumeratePhysicalDevices(platform.instance, &count, nullptr);
                    if (result != VK_SUCCESS) {
                        return std::unexpected(VulkanBackendError{
                            VulkanBackendErrorCode::PhysicalDeviceEnumerationFailed, DriverDetail(result)});
                    }
                    physicalDevices.resize(count);
                    result = platform.instanceApi.vkEnumeratePhysicalDevices(platform.instance, &count,
                                                                             physicalDevices.data());
                    if (result == VK_SUCCESS) {
                        physicalDevices.resize(count);
                        complete = true;
                        break;
                    }
                    if (result != VK_INCOMPLETE) {
                        return std::unexpected(VulkanBackendError{
                            VulkanBackendErrorCode::PhysicalDeviceEnumerationFailed, DriverDetail(result)});
                    }
                }
                if (!complete) {
                    return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::PhysicalDeviceEnumerationFailed,
                                                              DriverDetail(VK_INCOMPLETE)});
                }
                std::vector<VulkanDeviceFacts> devices;
                devices.reserve(physicalDevices.size());
                for (VkPhysicalDevice physicalDevice : physicalDevices) {
                    auto facts = QueryDeviceFacts(platform.instanceApi, physicalDevice, preferDedicatedComputeQueue);
                    if (!facts) {
                        return std::unexpected(facts.error());
                    }
                    if (*facts) {
                        devices.push_back(std::move(**facts));
                    }
                }
                std::ranges::sort(devices, [](const VulkanDeviceFacts& left, const VulkanDeviceFacts& right) {
                    if (left.device != right.device) {
                        return left.device < right.device;
                    }
                    return DriverContextQuality(left) > DriverContextQuality(right);
                });
                const auto unique = std::ranges::unique(devices, {}, &VulkanDeviceFacts::device);
                devices.erase(unique.begin(), unique.end());
                return devices;
            } catch (const std::bad_alloc&) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
            }
        }

        [[nodiscard]] std::expected<VulkanDeviceFacts, VulkanBackendError>
        SelectDevice(std::span<const VulkanDeviceFacts> devices, const VulkanBackendConfig& config) noexcept {
            const VulkanDeviceFacts* selected = nullptr;
            int64_t selectedScore = std::numeric_limits<int64_t>::min();
            for (const VulkanDeviceFacts& candidate : devices) {
                if (config.preferredDevice.Valid() && candidate.device != config.preferredDevice) {
                    continue;
                }
                const int64_t score = DeviceScore(candidate, config);
                if (selected == nullptr || score > selectedScore) {
                    selected = &candidate;
                    selectedScore = score;
                }
            }
            if (selected == nullptr) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::NoCompatibleDevice,
                                                          config.preferredDevice.Valid() ? 1U : 0U});
            }
            return *selected;
        }

        template<typename Enumerate>
        [[nodiscard]] std::expected<std::vector<VkExtensionProperties>, VkResult>
        EnumerateExtensions(Enumerate&& enumerate) {
            for (uint32_t attempt = 0; attempt < 3; ++attempt) {
                uint32_t count = 0;
                VkResult result = enumerate(&count, nullptr);
                if (result != VK_SUCCESS) {
                    return std::unexpected(result);
                }
                std::vector<VkExtensionProperties> extensions(count);
                result = enumerate(&count, extensions.data());
                if (result == VK_SUCCESS) {
                    extensions.resize(count);
                    return extensions;
                }
                if (result != VK_INCOMPLETE) {
                    return std::unexpected(result);
                }
            }
            return std::unexpected(VK_INCOMPLETE);
        }

        [[nodiscard]] bool HasExtension(std::span<const VkExtensionProperties> extensions,
                                        std::string_view name) noexcept {
            return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
                return std::string_view{extension.extensionName} == name;
            });
        }

        [[nodiscard]] VkResult CreateInstance(VulkanPlatformState& platform) {
            auto extensions = EnumerateExtensions([](uint32_t* count, VkExtensionProperties* properties) {
                return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
            });
            if (!extensions) {
                return extensions.error();
            }
            const bool portability = HasExtension(*extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            const std::array<const char*, 1> enabledExtensions{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
            const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                .pApplicationName = "Sora.Compute",
                                                .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
                                                .pEngineName = "Sora",
                                                .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
                                                .apiVersion = VK_API_VERSION_1_2};
            const VkInstanceCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                .flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : VkInstanceCreateFlags{},
                .pApplicationInfo = &application,
                .enabledExtensionCount = portability ? 1U : 0U,
                .ppEnabledExtensionNames = portability ? enabledExtensions.data() : nullptr};
            return vkCreateInstance(&createInfo, nullptr, &platform.instance);
        }

        [[nodiscard]] std::expected<std::shared_ptr<VulkanPlatformState>, VulkanBackendError>
        CreatePlatform() noexcept {
            const VkResult initialized = volkInitialize();
            if (initialized != VK_SUCCESS) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::VulkanLoaderUnavailable, DriverDetail(initialized)});
            }
            try {
                auto platform = std::make_shared<VulkanPlatformState>();
                const VkResult created = CreateInstance(*platform);
                if (created != VK_SUCCESS) {
                    return std::unexpected(
                        VulkanBackendError{VulkanBackendErrorCode::InstanceCreationFailed, DriverDetail(created)});
                }
                volkLoadInstanceTable(&platform->instanceApi, platform->instance);
                return platform;
            } catch (const std::bad_alloc&) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::InstanceCreationFailed, 0});
            }
        }

        [[nodiscard]] VkResult CreateDevice(VulkanServiceState& state, const VulkanDeviceFacts& facts) {
            auto extensions = EnumerateExtensions([&state, &facts](uint32_t* count, VkExtensionProperties* properties) {
                return state.platform->instanceApi.vkEnumerateDeviceExtensionProperties(facts.physicalDevice, nullptr,
                                                                                        count, properties);
            });
            if (!extensions) {
                return extensions.error();
            }
            const bool portability = HasExtension(*extensions, kPortabilitySubsetExtension);
            const std::array<const char*, 1> enabledExtensions{kPortabilitySubsetExtension};
            const float priority = 1.0F;
            const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = facts.computeQueueFamily,
                                                .queueCount = 1,
                                                .pQueuePriorities = &priority};
            VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            features12.storageBuffer8BitAccess = facts.features12.storageBuffer8BitAccess;
            features12.uniformAndStorageBuffer8BitAccess = facts.features12.uniformAndStorageBuffer8BitAccess;
            features12.shaderFloat16 = facts.features12.shaderFloat16;
            features12.shaderInt8 = facts.features12.shaderInt8;
            features12.timelineSemaphore = facts.features12.timelineSemaphore;
            VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            features11.pNext = &features12;
            features11.storageBuffer16BitAccess = facts.features11.storageBuffer16BitAccess;
            features11.uniformAndStorageBuffer16BitAccess = facts.features11.uniformAndStorageBuffer16BitAccess;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &features11;
            features.features.shaderFloat64 = facts.features.shaderFloat64;
            features.features.shaderInt64 = facts.features.shaderInt64;
            features.features.shaderInt16 = facts.features.shaderInt16;
            const VkDeviceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .pNext = &features,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue,
                                                .enabledExtensionCount = portability ? 1U : 0U,
                                                .ppEnabledExtensionNames =
                                                    portability ? enabledExtensions.data() : nullptr};
            return state.platform->instanceApi.vkCreateDevice(facts.physicalDevice, &createInfo, nullptr,
                                                              &state.device);
        }

        [[nodiscard]] uint64_t DiagnosticDetail(slang::IBlob* diagnostics) noexcept {
            if (diagnostics == nullptr || diagnostics->getBufferPointer() == nullptr ||
                diagnostics->getBufferSize() == 0) {
                return 0;
            }
            const auto* data = static_cast<const std::byte*>(diagnostics->getBufferPointer());
            return HashBytes({data, diagnostics->getBufferSize()}).low;
        }

        [[nodiscard]] std::expected<std::vector<uint32_t>, CompileError>
        CompileSlang(slang::IGlobalSession& globalSession, const Detail::SlangKernelSource& source,
                     FloatingPointMode floatingPoint) noexcept {
            try {
                slang::TargetDesc target{};
                target.format = SLANG_SPIRV;
                target.profile = globalSession.findProfile("spirv_1_5");
                target.flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY;
                target.floatingPointMode = floatingPoint == FloatingPointMode::Fast ? SLANG_FLOATING_POINT_MODE_FAST
                                                                                    : SLANG_FLOATING_POINT_MODE_PRECISE;
                const slang::CompilerOptionEntry preserveEntryPointName{
                    .name = slang::CompilerOptionName::VulkanUseEntryPointName, .value = {.intValue0 = 1}};
                target.compilerOptionEntries = &preserveEntryPointName;
                target.compilerOptionEntryCount = 1;
                slang::SessionDesc sessionDesc{};
                sessionDesc.targets = &target;
                sessionDesc.targetCount = 1;
                Slang::ComPtr<slang::ISession> session;
                SlangResult slangResult = globalSession.createSession(sessionDesc, session.writeRef());
                if (SLANG_FAILED(slangResult) || !session) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::BackendUnavailable, static_cast<uint64_t>(slangResult)});
                }

                std::string moduleName = "sora_";
                const SemanticHash sourceHash = HashText(source.text);
                char suffix[33]{};
                const auto low = std::to_chars(suffix, suffix + sizeof(suffix), sourceHash.low, 16);
                moduleName.append(suffix, low.ptr);
                Slang::ComPtr<slang::IBlob> diagnostics;
                Slang::ComPtr<slang::IModule> module{session->loadModuleFromSourceString(
                    moduleName.c_str(), "sora-generated.slang", source.text.c_str(), diagnostics.writeRef())};
                if (!module) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::CompilationFailed, DiagnosticDetail(diagnostics)});
                }
                Slang::ComPtr<slang::IEntryPoint> entryPoint;
                slangResult = module->findEntryPointByName(Detail::kVulkanEntryPoint, entryPoint.writeRef());
                if (SLANG_FAILED(slangResult) || !entryPoint) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::CompilationFailed, static_cast<uint64_t>(slangResult)});
                }
                std::array<slang::IComponentType*, 2> components{module.get(), entryPoint.get()};
                Slang::ComPtr<slang::IComponentType> composite;
                diagnostics.setNull();
                slangResult = session->createCompositeComponentType(components.data(), components.size(),
                                                                    composite.writeRef(), diagnostics.writeRef());
                if (SLANG_FAILED(slangResult) || !composite) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::CompilationFailed, DiagnosticDetail(diagnostics)});
                }
                Slang::ComPtr<slang::IComponentType> linked;
                diagnostics.setNull();
                slangResult = composite->link(linked.writeRef(), diagnostics.writeRef());
                if (SLANG_FAILED(slangResult) || !linked) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::CompilationFailed, DiagnosticDetail(diagnostics)});
                }
                Slang::ComPtr<slang::IBlob> code;
                diagnostics.setNull();
                slangResult = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
                if (SLANG_FAILED(slangResult) || !code || code->getBufferPointer() == nullptr ||
                    code->getBufferSize() < sizeof(uint32_t) || code->getBufferSize() % sizeof(uint32_t) != 0) {
                    return std::unexpected(
                        CompileError{CompileErrorCode::CompilationFailed, DiagnosticDetail(diagnostics)});
                }
                std::vector<uint32_t> spirv(code->getBufferSize() / sizeof(uint32_t));
                std::memcpy(spirv.data(), code->getBufferPointer(), code->getBufferSize());
                if (spirv.front() != kSpirvMagic) {
                    return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, spirv.front()});
                }
                return spirv;
            } catch (const std::bad_alloc&) {
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
        }

        class SlangCompilerPool final {
            struct Job {
                const Detail::SlangKernelSource* source = nullptr;
                FloatingPointMode floatingPoint = FloatingPointMode::Strict;
                std::expected<std::vector<uint32_t>, CompileError> result =
                    std::unexpected(CompileError{CompileErrorCode::BackendUnavailable, 0});
                std::mutex mutex{};
                std::condition_variable completed{};
                bool done = false;
            };

        public:
            SlangCompilerPool(const SlangCompilerPool&) = delete;
            SlangCompilerPool& operator=(const SlangCompilerPool&) = delete;

            ~SlangCompilerPool() noexcept {
                {
                    std::scoped_lock lock{mutex_};
                    stopping_ = true;
                }
                workAvailable_.notify_all();
                for (std::thread& worker : workers_) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
            }

            [[nodiscard]] static std::expected<std::shared_ptr<SlangCompilerPool>, VulkanBackendError>
            Create(uint16_t workerCount) noexcept {
                try {
                    auto pool = std::shared_ptr<SlangCompilerPool>{new SlangCompilerPool};
                    pool->workers_.reserve(workerCount);
                    for (uint16_t index = 0; index < workerCount; ++index) {
                        pool->workers_.emplace_back([state = pool.get()] { state->Run(); });
                    }
                    std::unique_lock lock{pool->mutex_};
                    pool->initialized_.wait(lock,
                                            [&pool, workerCount] { return pool->initializedWorkers_ == workerCount; });
                    if (pool->initializationFailure_ != 0) {
                        return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::SlangInitializationFailed,
                                                                  pool->initializationFailure_});
                    }
                    return pool;
                } catch (const std::bad_alloc&) {
                    return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, workerCount});
                } catch (...) {
                    return std::unexpected(
                        VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, workerCount});
                }
            }

            [[nodiscard]] std::expected<std::vector<uint32_t>, CompileError>
            Compile(const Detail::SlangKernelSource& source, FloatingPointMode floatingPoint) noexcept {
                Job job{.source = &source, .floatingPoint = floatingPoint};
                {
                    std::scoped_lock lock{mutex_};
                    if (stopping_) {
                        return std::unexpected(CompileError{CompileErrorCode::BackendUnavailable, 0});
                    }
                    try {
                        jobs_.push_back(&job);
                    } catch (const std::bad_alloc&) {
                        return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
                    } catch (...) {
                        return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
                    }
                }
                workAvailable_.notify_one();
                std::unique_lock lock{job.mutex};
                job.completed.wait(lock, [&job] { return job.done; });
                return std::move(job.result);
            }

        private:
            SlangCompilerPool() = default;

            void Run() noexcept {
                Slang::ComPtr<slang::IGlobalSession> globalSession;
                SlangGlobalSessionDesc descriptor{};
                const SlangResult initialized = slang::createGlobalSession(&descriptor, globalSession.writeRef());
                {
                    std::scoped_lock lock{mutex_};
                    if (SLANG_FAILED(initialized) || !globalSession) {
                        initializationFailure_ = SLANG_FAILED(initialized)
                                                     ? static_cast<uint64_t>(static_cast<int64_t>(initialized))
                                                     : UINT64_MAX;
                    }
                    ++initializedWorkers_;
                }
                initialized_.notify_one();
                if (SLANG_FAILED(initialized) || !globalSession) {
                    return;
                }

                while (true) {
                    Job* job = nullptr;
                    {
                        std::unique_lock lock{mutex_};
                        workAvailable_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                        if (jobs_.empty()) {
                            return;
                        }
                        job = jobs_.front();
                        jobs_.pop_front();
                    }
                    auto result = CompileSlang(*globalSession, *job->source, job->floatingPoint);
                    {
                        std::scoped_lock lock{job->mutex};
                        job->result = std::move(result);
                        job->done = true;
                    }
                    job->completed.notify_one();
                }
            }

            std::mutex mutex_{};
            std::condition_variable initialized_{};
            std::condition_variable workAvailable_{};
            std::deque<Job*> jobs_{};
            std::vector<std::thread> workers_{};
            uint16_t initializedWorkers_ = 0;
            uint64_t initializationFailure_ = 0;
            bool stopping_ = false;
        };

        VulkanPlatformState::~VulkanPlatformState() noexcept {
            if (instance != VK_NULL_HANDLE) {
                instanceApi.vkDestroyInstance(instance, nullptr);
            }
        }

        VulkanServiceState::~VulkanServiceState() noexcept {
            compilerPool.reset();
            if (device != VK_NULL_HANDLE) {
                static_cast<void>(deviceApi.vkDeviceWaitIdle(device));
                execution.reset();
                if (pipelineCache != VK_NULL_HANDLE) {
                    deviceApi.vkDestroyPipelineCache(device, pipelineCache, nullptr);
                }
                deviceApi.vkDestroyDevice(device, nullptr);
            }
        }

        void DestroyExecutable(void* opaque) noexcept {
            delete static_cast<VulkanExecutableHandle*>(opaque);
        }

        void DestroyPipeline(void* opaque) noexcept {
            auto* handle = static_cast<VulkanPipelineHandle*>(opaque);
            if (handle == nullptr || handle->magic != kPipelineHandleMagic || handle->owner == nullptr) {
                return;
            }
            VulkanServiceState& state = *handle->owner;
            if (handle->pipeline != VK_NULL_HANDLE) {
                state.deviceApi.vkDestroyPipeline(state.device, handle->pipeline, nullptr);
            }
            if (handle->pipelineLayout != VK_NULL_HANDLE) {
                state.deviceApi.vkDestroyPipelineLayout(state.device, handle->pipelineLayout, nullptr);
            }
            if (handle->descriptorSetLayout != VK_NULL_HANDLE) {
                state.deviceApi.vkDestroyDescriptorSetLayout(state.device, handle->descriptorSetLayout, nullptr);
            }
            delete handle;
        }

        void DestroyArtifactBytes(void* opaque) noexcept {
            delete[] static_cast<std::byte*>(opaque);
        }

        void DestroyService(void* opaque) noexcept {
            delete static_cast<VulkanServiceState*>(opaque);
        }

        template<typename Error>
        void ReturnBytes(std::span<const std::byte> bytes, BackendArtifactBytes& payload, Error& error,
                         Error outOfMemory) noexcept {
            auto storage = std::unique_ptr<std::byte[]>{new (std::nothrow) std::byte[bytes.size()]};
            if (!storage) {
                error = outOfMemory;
                return;
            }
            std::memcpy(storage.get(), bytes.data(), bytes.size());
            payload = {.data = storage.get(),
                       .size = bytes.size(),
                       .state = storage.release(),
                       .destroy = DestroyArtifactBytes};
            error = {};
        }

        void AppendLittleEndian(std::vector<std::byte>& bytes, uint64_t value, uint32_t width) {
            for (uint32_t index = 0; index < width; ++index) {
                bytes.push_back(std::byte((value >> (index * 8U)) & 0xFFU));
            }
        }

        [[nodiscard]] std::vector<std::byte> EncodeEnvelope(uint32_t magic, SemanticHash identity,
                                                            std::span<const std::byte> payload) {
            std::vector<std::byte> bytes;
            bytes.reserve(48 + payload.size());
            AppendLittleEndian(bytes, magic, 4);
            AppendLittleEndian(bytes, kVulkanArtifactEnvelopeVersion, 2);
            AppendLittleEndian(bytes, 0, 2);
            AppendLittleEndian(bytes, payload.size(), 8);
            AppendLittleEndian(bytes, identity.low, 8);
            AppendLittleEndian(bytes, identity.high, 8);
            const SemanticHash hash = HashBytes(payload);
            AppendLittleEndian(bytes, hash.low, 8);
            AppendLittleEndian(bytes, hash.high, 8);
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            return bytes;
        }

        [[nodiscard]] std::expected<std::span<const std::byte>, uint64_t>
        DecodeEnvelope(std::span<const std::byte> bytes, uint32_t expectedMagic,
                       SemanticHash expectedIdentity) noexcept {
            Detail::WireReader reader{bytes};
            const auto magic = reader.Read<uint32_t>();
            const auto version = reader.Read<uint16_t>();
            const auto reserved = reader.Read<uint16_t>();
            const auto size = reader.Read<uint64_t>();
            const auto identityLow = reader.Read<uint64_t>();
            const auto identityHigh = reader.Read<uint64_t>();
            const auto low = reader.Read<uint64_t>();
            const auto high = reader.Read<uint64_t>();
            if (!magic || !version || !reserved || !size || !identityLow || !identityHigh || !low || !high ||
                *magic != expectedMagic || *version != kVulkanArtifactEnvelopeVersion || *reserved != 0 ||
                SemanticHash{*identityLow, *identityHigh} != expectedIdentity ||
                *size > std::numeric_limits<size_t>::max()) {
                return std::unexpected(reader.Offset());
            }
            auto payload = reader.ReadBytes(static_cast<size_t>(*size));
            if (!payload || !reader.AtEnd() || HashBytes(*payload) != SemanticHash{*low, *high}) {
                return std::unexpected(reader.Offset());
            }
            return *payload;
        }

        [[nodiscard]] VulkanExecutableHandle* ResolveExecutable(VulkanServiceState& state, ArtifactView view) noexcept {
            auto* handle = static_cast<VulkanExecutableHandle*>(view.state);
            return handle != nullptr && handle->magic == kExecutableHandleMagic && handle->owner == &state &&
                           handle->key.Kind() == ArtifactKind::DeviceExecutable &&
                           ArtifactIdentity(handle->key) == view.identity
                       ? handle
                       : nullptr;
        }

        [[nodiscard]] bool ResolveDispatchPipeline(void* opaque, ArtifactView view,
                                                   Detail::VulkanDispatchPipeline& output) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            auto* handle = static_cast<VulkanPipelineHandle*>(view.state);
            if (handle == nullptr || handle->magic != kPipelineHandleMagic || handle->owner != &state ||
                view.kind != ArtifactKind::DevicePipeline || view.backend != BackendKind::Vulkan ||
                view.device != state.descriptor.id || handle->key.Kind() != ArtifactKind::DevicePipeline ||
                ArtifactIdentity(handle->key) != view.identity) {
                return false;
            }
            output = {.descriptorSetLayout = handle->descriptorSetLayout,
                      .pipelineLayout = handle->pipelineLayout,
                      .pipeline = handle->pipeline,
                      .program = &handle->program,
                      .abi = &handle->dispatchAbi};
            return true;
        }

        [[nodiscard]] std::expected<std::unique_ptr<VulkanExecutableHandle>, CompileError>
        MakeExecutable(VulkanServiceState& state, const BackendCompileRequest& request,
                       std::span<const uint32_t> importedSpirv = {}) noexcept {
            auto decoded = DecodeKernelProgramPackage<kVulkanMaximumKernelArguments, kVulkanMaximumScalarParameters,
                                                      kVulkanMaximumInstructions>(request.ModuleBytes());
            if (!decoded || !Detail::SupportsVulkanProgram(*decoded, state.descriptor.features)) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedIr,
                                                    decoded ? 0U : static_cast<uint64_t>(decoded.error().code)});
            }
            auto source = Detail::LowerKernelProgramToSlang(*decoded, state.descriptor.features);
            if (!source) {
                return std::unexpected(source.error());
            }
            std::vector<uint32_t> spirv;
            if (importedSpirv.empty()) {
                auto compiled = state.compilerPool->Compile(*source, decoded->scalar.numericalPolicy.floatingPoint);
                if (!compiled) {
                    return std::unexpected(compiled.error());
                }
                spirv = std::move(*compiled);
            } else {
                try {
                    spirv.assign(importedSpirv.begin(), importedSpirv.end());
                } catch (const std::bad_alloc&) {
                    return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
                }
            }
            auto handle = std::unique_ptr<VulkanExecutableHandle>{new (std::nothrow) VulkanExecutableHandle{
                .owner = &state,
                .key = request.ExpectedKey(),
                .program = *decoded,
                .dispatchAbi = source->dispatchAbi,
                .spirv = std::move(spirv),
            }};
            if (!handle) {
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
            return handle;
        }

        void Compile(void* opaque, const BackendCompileRequest* request, BackendCompileResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr || request->artifactKind != ArtifactKind::DeviceExecutable ||
                request->target != state.target) {
                if (result != nullptr) {
                    result->error = {CompileErrorCode::InvalidRequest, 0};
                }
                return;
            }
            auto executable = MakeExecutable(state, *request);
            if (!executable) {
                result->error = executable.error();
                return;
            }
            result->payload = {.key = request->ExpectedKey(),
                               .device = state.target.device,
                               .state = executable->release(),
                               .destroy = DestroyExecutable};
            result->error = {};
        }

        void ImportExecutable(void* opaque, const BackendArtifactImportRequest* request,
                              BackendCompileResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr || request->compileRequest == nullptr) {
                if (result != nullptr) {
                    result->error = {CompileErrorCode::InvalidRequest, 0};
                }
                return;
            }
            auto payload = DecodeEnvelope(request->ArtifactBytes(), kExecutableEnvelopeMagic,
                                          ArtifactIdentity(request->compileRequest->ExpectedKey()));
            if (!payload || payload->size() % sizeof(uint32_t) != 0 || payload->size() < sizeof(uint32_t)) {
                result->error = {CompileErrorCode::ArtifactImportFailed, payload ? payload->size() : payload.error()};
                return;
            }
            std::vector<uint32_t> spirv(payload->size() / sizeof(uint32_t));
            std::memcpy(spirv.data(), payload->data(), payload->size());
            if (spirv.front() != kSpirvMagic) {
                result->error = {CompileErrorCode::ArtifactImportFailed, spirv.front()};
                return;
            }
            auto executable = MakeExecutable(state, *request->compileRequest, spirv);
            if (!executable) {
                result->error = executable.error();
                return;
            }
            result->payload = {.key = request->compileRequest->ExpectedKey(),
                               .device = state.target.device,
                               .state = executable->release(),
                               .destroy = DestroyExecutable};
            result->error = {};
        }

        void ExportExecutable(void* opaque, const BackendArtifactExportRequest* request,
                              BackendArtifactExportResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr) {
                return;
            }
            VulkanExecutableHandle* executable = ResolveExecutable(state, request->artifact);
            if (executable == nullptr || executable->key != request->key) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            try {
                const auto spirvBytes = std::as_bytes(std::span{executable->spirv});
                const std::vector<std::byte> bytes =
                    EncodeEnvelope(kExecutableEnvelopeMagic, ArtifactIdentity(request->key), spirvBytes);
                ReturnBytes(std::span{bytes}, result->payload, result->error,
                            CompileError{CompileErrorCode::OutOfMemory, bytes.size()});
            } catch (const std::bad_alloc&) {
                result->error = {CompileErrorCode::OutOfMemory, 0};
            } catch (...) {
                result->error = {CompileErrorCode::ArtifactExportFailed, 0};
            }
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, PipelineError>
        ExportDriverPipelineCache(VulkanServiceState& state, SemanticHash artifactIdentity) noexcept {
            try {
                std::scoped_lock lock{state.pipelineCacheMutex};
                for (uint32_t attempt = 0; attempt < kPipelineCacheExportAttempts; ++attempt) {
                    size_t size = 0;
                    VkResult vkResult =
                        state.deviceApi.vkGetPipelineCacheData(state.device, state.pipelineCache, &size, nullptr);
                    if (vkResult != VK_SUCCESS || size == 0) {
                        return std::unexpected(
                            PipelineError{PipelineErrorCode::ArtifactExportFailed, DriverDetail(vkResult)});
                    }
                    std::vector<std::byte> cache(size);
                    vkResult =
                        state.deviceApi.vkGetPipelineCacheData(state.device, state.pipelineCache, &size, cache.data());
                    if (vkResult == VK_SUCCESS) {
                        cache.resize(size);
                        return EncodeEnvelope(kPipelineCacheEnvelopeMagic, artifactIdentity, cache);
                    }
                    if (vkResult != VK_INCOMPLETE) {
                        return std::unexpected(
                            PipelineError{PipelineErrorCode::ArtifactExportFailed, DriverDetail(vkResult)});
                    }
                }
                return std::unexpected(
                    PipelineError{PipelineErrorCode::ArtifactExportFailed, DriverDetail(VK_INCOMPLETE)});
            } catch (const std::bad_alloc&) {
                return std::unexpected(PipelineError{PipelineErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(PipelineError{PipelineErrorCode::ArtifactExportFailed, 0});
            }
        }

        [[nodiscard]] std::expected<void, PipelineError>
        MergeDriverPipelineCache(VulkanServiceState& state, std::span<const std::byte> envelope,
                                 SemanticHash artifactIdentity) noexcept {
            auto payload = DecodeEnvelope(envelope, kPipelineCacheEnvelopeMagic, artifactIdentity);
            if (!payload) {
                return std::unexpected(PipelineError{PipelineErrorCode::ArtifactImportFailed, payload.error()});
            }
            std::scoped_lock lock{state.pipelineCacheMutex};
            const VkPipelineCacheCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                                       .initialDataSize = payload->size(),
                                                       .pInitialData = payload->data()};
            VkPipelineCache imported = VK_NULL_HANDLE;
            VkResult result = state.deviceApi.vkCreatePipelineCache(state.device, &createInfo, nullptr, &imported);
            if (result != VK_SUCCESS) {
                return std::unexpected(PipelineError{PipelineErrorCode::ArtifactImportFailed, DriverDetail(result)});
            }
            result = state.deviceApi.vkMergePipelineCaches(state.device, state.pipelineCache, 1, &imported);
            state.deviceApi.vkDestroyPipelineCache(state.device, imported, nullptr);
            if (result != VK_SUCCESS) {
                return std::unexpected(PipelineError{PipelineErrorCode::ArtifactImportFailed, DriverDetail(result)});
            }
            return {};
        }

        [[nodiscard]] std::expected<std::unique_ptr<VulkanPipelineHandle>, PipelineError>
        MakePipeline(VulkanServiceState& state, const BackendPipelineRequest& request) noexcept {
            VulkanExecutableHandle* executable = ResolveExecutable(state, request.executable);
            if (executable == nullptr || request.pipeline.target != PipelineTarget(state.target) ||
                request.pipeline.builder != state.pipelineIdentity ||
                request.pipeline.state != state.pipelineStateIdentity ||
                request.pipeline.bindingLayout != PipelineBindingLayoutIdentity(executable->program.Arguments())) {
                return std::unexpected(PipelineError{PipelineErrorCode::UnsupportedExecutable, 0});
            }
            try {
                std::vector<VkDescriptorSetLayoutBinding> bindings;
                bindings.reserve(executable->dispatchAbi.storageBufferCount);
                for (uint32_t argument = 0; argument < executable->program.argumentCount; ++argument) {
                    if (executable->program.arguments[argument].kind == ParameterKind::Buffer) {
                        bindings.push_back({.binding = argument,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});
                    }
                }
                bindings.push_back({.binding = executable->dispatchAbi.metadataBinding,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    .descriptorCount = 1,
                                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

                VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
                VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
                VkShaderModule shader = VK_NULL_HANDLE;
                VkPipeline pipeline = VK_NULL_HANDLE;
                const auto cleanup = [&] {
                    if (pipeline != VK_NULL_HANDLE) {
                        state.deviceApi.vkDestroyPipeline(state.device, pipeline, nullptr);
                    }
                    if (shader != VK_NULL_HANDLE) {
                        state.deviceApi.vkDestroyShaderModule(state.device, shader, nullptr);
                    }
                    if (pipelineLayout != VK_NULL_HANDLE) {
                        state.deviceApi.vkDestroyPipelineLayout(state.device, pipelineLayout, nullptr);
                    }
                    if (descriptorSetLayout != VK_NULL_HANDLE) {
                        state.deviceApi.vkDestroyDescriptorSetLayout(state.device, descriptorSetLayout, nullptr);
                    }
                };
                const VkDescriptorSetLayoutCreateInfo descriptorInfo{
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                    .bindingCount = static_cast<uint32_t>(bindings.size()),
                    .pBindings = bindings.data(),
                };
                VkResult result = state.deviceApi.vkCreateDescriptorSetLayout(state.device, &descriptorInfo, nullptr,
                                                                              &descriptorSetLayout);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return std::unexpected(PipelineError{PipelineErrorCode::CreationFailed, DriverDetail(result)});
                }
                const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                            .setLayoutCount = 1,
                                                            .pSetLayouts = &descriptorSetLayout};
                result = state.deviceApi.vkCreatePipelineLayout(state.device, &layoutInfo, nullptr, &pipelineLayout);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return std::unexpected(PipelineError{PipelineErrorCode::CreationFailed, DriverDetail(result)});
                }
                const VkShaderModuleCreateInfo shaderInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                          .codeSize = executable->spirv.size() * sizeof(uint32_t),
                                                          .pCode = executable->spirv.data()};
                result = state.deviceApi.vkCreateShaderModule(state.device, &shaderInfo, nullptr, &shader);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return std::unexpected(PipelineError{PipelineErrorCode::CreationFailed, DriverDetail(result)});
                }
                const VkPipelineShaderStageCreateInfo stage{.sType =
                                                                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                            .module = shader,
                                                            .pName = Detail::kVulkanEntryPoint};
                const VkComputePipelineCreateInfo pipelineInfo{
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage, .layout = pipelineLayout};
                {
                    std::scoped_lock lock{state.pipelineCacheMutex};
                    result = state.deviceApi.vkCreateComputePipelines(state.device, state.pipelineCache, 1,
                                                                      &pipelineInfo, nullptr, &pipeline);
                }
                state.deviceApi.vkDestroyShaderModule(state.device, shader, nullptr);
                shader = VK_NULL_HANDLE;
                if (result != VK_SUCCESS) {
                    cleanup();
                    return std::unexpected(PipelineError{PipelineErrorCode::CreationFailed, DriverDetail(result)});
                }
                auto handle = std::unique_ptr<VulkanPipelineHandle>{new (std::nothrow) VulkanPipelineHandle{
                    .owner = &state,
                    .key = request.ExpectedKey(),
                    .descriptorSetLayout = descriptorSetLayout,
                    .pipelineLayout = pipelineLayout,
                    .pipeline = pipeline,
                    .program = executable->program,
                    .dispatchAbi = executable->dispatchAbi,
                }};
                if (!handle) {
                    cleanup();
                    return std::unexpected(PipelineError{PipelineErrorCode::OutOfMemory, 0});
                }
                return handle;
            } catch (const std::bad_alloc&) {
                return std::unexpected(PipelineError{PipelineErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(PipelineError{PipelineErrorCode::CreationFailed, 0});
            }
        }

        void CreatePipeline(void* opaque, const BackendPipelineRequest* request,
                            BackendPipelineResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr) {
                return;
            }
            auto pipeline = MakePipeline(state, *request);
            if (!pipeline) {
                result->error = pipeline.error();
                return;
            }
            result->payload = {.key = request->ExpectedKey(),
                               .device = state.target.device,
                               .state = pipeline->release(),
                               .destroy = DestroyPipeline};
            result->error = {};
        }

        void ImportPipeline(void* opaque, const BackendPipelineImportRequest* request,
                            BackendPipelineResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr || request->pipelineRequest == nullptr) {
                if (result != nullptr) {
                    result->error = {PipelineErrorCode::InvalidRequest, 0};
                }
                return;
            }
            if (auto merged = MergeDriverPipelineCache(state, request->ArtifactBytes(),
                                                       ArtifactIdentity(request->pipelineRequest->ExpectedKey()));
                !merged) {
                result->error = merged.error();
                return;
            }
            CreatePipeline(opaque, request->pipelineRequest, result);
        }

        void ExportPipeline(void* opaque, const BackendArtifactExportRequest* request,
                            BackendPipelineExportResult* result) noexcept {
            auto& state = *static_cast<VulkanServiceState*>(opaque);
            if (request == nullptr || result == nullptr) {
                return;
            }
            auto* pipeline = static_cast<VulkanPipelineHandle*>(request->artifact.state);
            if (pipeline == nullptr || pipeline->magic != kPipelineHandleMagic || pipeline->owner != &state ||
                pipeline->key != request->key || ArtifactIdentity(pipeline->key) != request->artifact.identity) {
                result->error = {PipelineErrorCode::InvalidRequest, 0};
                return;
            }
            auto bytes = ExportDriverPipelineCache(state, ArtifactIdentity(request->key));
            if (!bytes) {
                result->error = bytes.error();
                return;
            }
            ReturnBytes(std::span{*bytes}, result->payload, result->error,
                        PipelineError{PipelineErrorCode::OutOfMemory, bytes->size()});
        }

        [[nodiscard]] DeviceDescriptor MakeDeviceDescriptor(const VulkanDeviceFacts& facts) noexcept {
            const uint64_t totalMemory = TotalDeviceMemory(facts.memoryProperties);
            const uint64_t maximumAllocation =
                facts.maintenance3Properties.maxMemoryAllocationSize == 0
                    ? totalMemory
                    : std::min(totalMemory, facts.maintenance3Properties.maxMemoryAllocationSize);
            const bool computeSubgroups = (facts.subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
            return {.id = facts.device,
                    .backend = BackendKind::Vulkan,
                    .kind = DeviceType(facts.properties.deviceType),
                    .features = DeviceFeatures(facts),
                    .architectureId = ArchitectureIdentity(facts),
                    .totalMemoryBytes = totalMemory,
                    .maximumAllocationBytes = maximumAllocation,
                    .computeUnitCount = 0,
                    .subgroupWidthMinimum = computeSubgroups ? facts.subgroupProperties.subgroupSize : 1,
                    .subgroupWidthMaximum = computeSubgroups ? facts.subgroupProperties.subgroupSize : 1,
                    .maximumWorkgroupInvocations = facts.properties.limits.maxComputeWorkGroupInvocations};
        }

        [[nodiscard]] TargetFingerprint MakeTarget(const VulkanServiceState& state,
                                                   const VulkanDeviceFacts& facts) noexcept {
            return {.device = state.descriptor.id,
                    .backend = BackendKind::Vulkan,
                    .hostArchitecture = kCpuArchitecture,
                    .operatingSystem = kOperatingSystem,
                    .binaryFormat = kBinaryFormat,
                    .byteOrder = std::endian::native == std::endian::little ? ByteOrder::Little : ByteOrder::Big,
                    .abi = kAbiFamily,
                    .identityStability = TargetIdentityStability::MachineStable,
                    .executableDomain = ExecutableTargetDomain::BackendPortable,
                    .addressBits = static_cast<uint8_t>(sizeof(void*) * 8U),
                    .features = state.descriptor.features,
                    .architectureId = state.descriptor.architectureId,
                    .fixedVectorBits = state.descriptor.features.Contains(DeviceFeature::Subgroup)
                                           ? facts.subgroupProperties.subgroupSize * 32U
                                           : 0,
                    .compilerIdentity = state.compilerIdentity,
                    .driverIdentity = DriverIdentity(facts)};
        }

        [[nodiscard]] std::expected<uint16_t, VulkanBackendError>
        ResolveCompilerWorkerCount(uint16_t requested) noexcept {
            if (requested > kMaximumCompilerWorkerCount) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::InvalidConfiguration, requested});
            }
            const uint32_t hardwareThreads = std::max(1U, std::thread::hardware_concurrency());
            return requested == 0 ? static_cast<uint16_t>(std::min(hardwareThreads, 2U)) : requested;
        }

    } // namespace

    struct VulkanBackend::ConstructionInput {
        std::shared_ptr<VulkanPlatformState> platform{};
        std::shared_ptr<SlangCompilerPool> compilerPool{};
        VulkanDeviceFacts device{};
    };

    std::expected<VulkanBackend, VulkanBackendError> VulkanBackend::CreateForDevice(ConstructionInput input) noexcept {
        try {
            const VulkanDeviceFacts& selected = input.device;
            auto state = std::make_unique<VulkanServiceState>();
            state->platform = std::move(input.platform);
            state->compilerPool = std::move(input.compilerPool);
            state->physicalDevice = selected.physicalDevice;
            state->computeQueueFamily = selected.computeQueueFamily;
            VkResult result = CreateDevice(*state, selected);
            if (result != VK_SUCCESS) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::DeviceCreationFailed, DriverDetail(result)});
            }
            volkLoadDeviceTable(&state->deviceApi, state->device);
            const VkPipelineCacheCreateInfo cacheInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            result = state->deviceApi.vkCreatePipelineCache(state->device, &cacheInfo, nullptr, &state->pipelineCache);
            if (result != VK_SUCCESS) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::DeviceCreationFailed, DriverDetail(result)});
            }

            const char* buildTag = spGetBuildTagString();
            const std::string_view slangVersion = buildTag != nullptr ? std::string_view{buildTag} : std::string_view{};
            state->compilerIdentity = HashIdentity("Sora.Slang.SPIRV.KernelProgram.v3", slangVersion);
            state->pipelineIdentity = HashIdentity("Sora.Vulkan.ComputePipeline.v1", "descriptor-metadata-v1");
            state->pipelineStateIdentity = HashIdentity("Sora.Vulkan.DispatchPolicy.v1", "64x1x1-8x8x1-4x4x4");
            state->descriptor = MakeDeviceDescriptor(selected);
            state->target = MakeTarget(*state, selected);

            static std::atomic<uint64_t> nextServiceId = 1;
            const BackendServiceId serviceId{nextServiceId.fetch_add(1, std::memory_order_relaxed),
                                             state->descriptor.id.high ^ 0x534F52415F564B53ULL};
            auto service = BackendService::Adopt({.id = serviceId, .state = state.get(), .destroy = DestroyService},
                                                 BackendCodeLease::Retain(std::make_shared<StaticVulkanBackendCode>()));
            if (!service) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed,
                                                          static_cast<uint64_t>(service.error())});
            }
            static_cast<void>(state.release());
            auto& native = *static_cast<VulkanServiceState*>(service->State());
            auto compiler =
                BackendCompiler::Create(*service, {.identity = native.compilerIdentity,
                                                   .backend = BackendKind::Vulkan,
                                                   .executableDomain = ExecutableTargetDomain::BackendPortable,
                                                   .state = &native,
                                                   .compile = Compile,
                                                   .importArtifact = ImportExecutable,
                                                   .exportArtifact = ExportExecutable});
            auto pipelineFactory = BackendPipelineFactory::Create(*service, native.target,
                                                                  {.identity = native.pipelineIdentity,
                                                                   .state = &native,
                                                                   .create = CreatePipeline,
                                                                   .importPipeline = ImportPipeline,
                                                                   .exportPipeline = ExportPipeline});
            if (!compiler || !pipelineFactory) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
            }
            auto execution = Detail::CreateVulkanExecution(
                *service,
                {.instanceApi = &native.platform->instanceApi,
                 .deviceApi = &native.deviceApi,
                 .instance = native.platform->instance,
                 .physicalDevice = native.physicalDevice,
                 .device = native.device,
                 .queueFamily = native.computeQueueFamily,
                 .timestampValidBits = selected.timestampValidBits,
                 .properties = selected.properties,
                 .memoryProperties = selected.memoryProperties,
                 .maximumTimelineValueDifference = selected.timelineProperties.maxTimelineSemaphoreValueDifference,
                 .descriptor = native.descriptor,
                 .pipelineState = &native,
                 .resolvePipeline = ResolveDispatchPipeline});
            if (!execution) {
                return std::unexpected(execution.error());
            }
            native.execution = std::move(execution->state);
            auto observer = DeviceObserver::Create(
                *service, {.device = native.descriptor.id,
                           .backend = BackendKind::Vulkan,
                           .state = &native,
                           .observe = [](void* opaque, DeviceSnapshot* snapshot) noexcept {
                               const auto& service = *static_cast<const VulkanServiceState*>(opaque);
                               *snapshot = service.execution ? Detail::QueryVulkanExecution(*service.execution)
                                                             : DeviceSnapshot{.id = service.descriptor.id,
                                                                              .state = DeviceState::Unavailable,
                                                                              .generation = 1};
                           }});
            if (!observer) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
            }
            return VulkanBackend{std::move(*service),
                                 std::move(*observer),
                                 native.descriptor,
                                 native.target,
                                 execution->deviceLocation,
                                 execution->hostLocation,
                                 std::move(execution->deviceAllocator),
                                 std::move(execution->hostAllocator),
                                 std::move(execution->hostAccessor),
                                 std::move(execution->transfers),
                                 std::move(*compiler),
                                 std::move(*pipelineFactory),
                                 std::move(execution->queue),
                                 native.pipelineStateIdentity};
        } catch (const std::bad_alloc&) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
        }
    }

    std::expected<VulkanBackend, VulkanBackendError> VulkanBackend::Create(VulkanBackendConfig config) noexcept {
        auto workerCount = ResolveCompilerWorkerCount(config.compilerWorkerCount);
        if (!workerCount) {
            return std::unexpected(workerCount.error());
        }
        auto platform = CreatePlatform();
        if (!platform) {
            return std::unexpected(platform.error());
        }
        auto devices = EnumerateCompatibleDevices(**platform, config.preferDedicatedComputeQueue);
        if (!devices) {
            return std::unexpected(devices.error());
        }
        auto selected = SelectDevice(*devices, config);
        if (!selected) {
            return std::unexpected(selected.error());
        }
        auto compilerPool = SlangCompilerPool::Create(*workerCount);
        if (!compilerPool) {
            return std::unexpected(compilerPool.error());
        }
        return CreateForDevice(
            {.platform = std::move(*platform), .compilerPool = std::move(*compilerPool), .device = *selected});
    }

    std::expected<std::vector<VulkanBackend>, VulkanBackendError>
    VulkanBackend::CreateAll(VulkanBackendSetConfig config) noexcept {
        auto workerCount = ResolveCompilerWorkerCount(config.compilerWorkerCount);
        if (!workerCount) {
            return std::unexpected(workerCount.error());
        }
        auto platform = CreatePlatform();
        if (!platform) {
            return std::unexpected(platform.error());
        }
        auto devices = EnumerateCompatibleDevices(**platform, config.preferDedicatedComputeQueue);
        if (!devices) {
            return std::unexpected(devices.error());
        }
        if (devices->empty()) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::NoCompatibleDevice, 0});
        }
        auto compilerPool = SlangCompilerPool::Create(*workerCount);
        if (!compilerPool) {
            return std::unexpected(compilerPool.error());
        }
        try {
            std::vector<VulkanBackend> backends;
            backends.reserve(devices->size());
            for (VulkanDeviceFacts& device : *devices) {
                auto backend = CreateForDevice(
                    {.platform = *platform, .compilerPool = *compilerPool, .device = std::move(device)});
                if (!backend) {
                    return std::unexpected(backend.error());
                }
                backends.push_back(std::move(*backend));
            }
            return backends;
        } catch (const std::bad_alloc&) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, devices->size()});
        } catch (...) {
            return std::unexpected(
                VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, devices->size()});
        }
    }

    DeviceSnapshot VulkanBackend::Snapshot() const noexcept {
        auto snapshot = observer_.Observe();
        return snapshot ? *snapshot
                        : DeviceSnapshot{.id = device_.id, .state = DeviceState::Unavailable, .generation = 1};
    }

    std::expected<std::span<std::byte>, VulkanBackendError>
    VulkanBackend::Map(const Allocation& allocation, uint64_t offset, uint64_t sizeBytes) const noexcept {
        if (!service_) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
        }
        const auto& native = *static_cast<const VulkanServiceState*>(service_.State());
        if (!native.execution) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
        }
        return Detail::MapVulkanAllocation(*native.execution, service_.Id(), allocation, offset, sizeBytes);
    }

} // namespace Sora::Compute
