/**
 * @file CpuBackendBenchmark.cpp
 * @brief CPU backend crossover, layout, and end-to-end pipeline performance baselines.
 */

#include <BenchmarkHarness.h>
#include <Sora/Core/PAL/CpuInfo.h>
#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Backend/LlvmCpuCodeProvider.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <expected>
#include <limits>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    using namespace std::chrono_literals;

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::ValueType kVec3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr size_t kSampleCount = 7;
    constexpr auto kMinimumSampleTime = 40ms;
    constexpr float kAlpha = 1.75F;

    enum class Operation : uint8_t {
        Saxpy,
        Vec3Add,
    };

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        auto module = std::move(scalar).Finish(scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeVec3AddProgram(Sora::Compute::LayoutKind layout) {
        Sora::Compute::KernelBuilder<2, 3> scalar;
        const auto x = scalar.AddValueParameter(kVec3);
        const auto y = scalar.AddValueParameter(kVec3);
        auto module = std::move(scalar).Finish(scalar.Binary(Sora::Compute::OpCode::Add, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<3, 2, 3> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto xArgument =
            program.AddBufferArgument(Sora::Compute::AccessMode::Read, kVec3, domain.rank, layout, 64, {1});
        const auto yArgument =
            program.AddBufferArgument(Sora::Compute::AccessMode::Read, kVec3, domain.rank, layout, 64, {2});
        const auto output =
            program.AddBufferArgument(Sora::Compute::AccessMode::Write, kVec3, domain.rank, layout, 64, {3});
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kAosAdd = MakeVec3AddProgram(Sora::Compute::LayoutKind::Contiguous);
    constexpr auto kSoaAdd = MakeVec3AddProgram(Sora::Compute::LayoutKind::StructureOfArrays);

    template<typename T, typename E>
    [[nodiscard]] T Take(std::expected<T, E>&& result, std::string_view stage) {
        if (!result) {
            std::println(stderr, "{} failed: code={}, detail={}", stage, std::to_underlying(result.error().code),
                         result.error().detail);
            std::exit(2);
        }
        return std::move(*result);
    }

    [[nodiscard]] Sora::Compute::Allocation Allocate(const Sora::Compute::MemoryAllocator& allocator,
                                                     Sora::Compute::MemoryLocation location, uint64_t sizeBytes) {
        return Take(allocator.Allocate({.location = location,
                                        .requiredProperties = Sora::Compute::MemoryProperty::HostVisible |
                                                              Sora::Compute::MemoryProperty::HostCoherent,
                                        .preferredProperties = Sora::Compute::MemoryProperty::Unified,
                                        .sizeBytes = sizeBytes,
                                        .alignment = 64}),
                    "allocate");
    }

    template<size_t ArgumentCapacity, size_t ParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] Sora::Compute::Artifact
    Compile(Sora::Compute::CpuBackend& backend,
            const Sora::Compute::KernelProgram<ArgumentCapacity, ParameterCapacity, InstructionCapacity>& program) {
        const auto bytes = program.Serialize();
        return Take(backend.Compiler().Compile({.moduleData = bytes.Bytes().data(),
                                                .moduleSize = bytes.Bytes().size(),
                                                .moduleHash = program.Hash(),
                                                .target = backend.Target(),
                                                .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable}),
                    "compile");
    }

    [[nodiscard]] const Sora::Compute::TransferEngine& FindTransfer(const Sora::Compute::CpuBackend& backend,
                                                                    Sora::Compute::MemoryLocation source,
                                                                    Sora::Compute::MemoryLocation destination) {
        for (const Sora::Compute::TransferEngine& engine : backend.TransferEngines()) {
            if (engine.Source() == source && engine.Destination() == destination &&
                engine.Kind() == Sora::Compute::TransferKind::Copy) {
                return engine;
            }
        }
        std::println(stderr, "required CPU copy engine is unavailable");
        std::exit(2);
    }

    [[nodiscard]] Sora::Compute::BackendBinding MakeBinding(const Sora::Compute::Allocation& allocation,
                                                            uint32_t binding, Sora::Compute::AccessMode access,
                                                            Sora::Compute::ValueType type,
                                                            Sora::Compute::LayoutKind layout, uint64_t count) {
        const uint64_t scalarBytes = Sora::Compute::ScalarByteSize(type.scalar);
        const bool soa = layout == Sora::Compute::LayoutKind::StructureOfArrays;
        return {.allocation = allocation.View(),
                .offset = 0,
                .size = count * type.ByteSize(),
                .binding = binding,
                .access = access,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = type,
                .layout = layout,
                .rank = 1,
                .extents = {count},
                .byteStrides = {soa ? scalarBytes : type.ByteSize()},
                .componentStrideBytes = soa ? count * scalarBytes : scalarBytes};
    }

    template<typename T>
    [[nodiscard]] std::span<T> As(std::span<std::byte> bytes) noexcept {
        return {reinterpret_cast<T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    class KernelWorkload final {
    public:
        KernelWorkload(const KernelWorkload&) = delete;
        KernelWorkload& operator=(const KernelWorkload&) = delete;
        KernelWorkload(KernelWorkload&&) = delete;
        KernelWorkload& operator=(KernelWorkload&&) = delete;

        [[nodiscard]] static std::unique_ptr<KernelWorkload>
        Create(Sora::Compute::CpuBackendConfig config, uint64_t count, Operation operation,
               Sora::Compute::LayoutKind layout = Sora::Compute::LayoutKind::Contiguous,
               Sora::Compute::CpuCodeProvider provider = {}) {
            return std::unique_ptr<KernelWorkload>{
                new KernelWorkload(config, count, operation, layout, std::move(provider))};
        }

        void RunDispatch() noexcept {
            auto submitted = backend_.ExecutionQueue().Submit(request_);
            if (!submitted || submitted->WaitUntilTerminal().status != Sora::Compute::EventStatus::Completed) {
                std::terminate();
            }
        }

        void RunRoundTrip() noexcept {
            const std::array regions{
                Sora::Compute::TransferRegion{.sourceOffset = 0, .targetOffset = 0, .sizeBytes = sizeBytes_}};
            auto uploadX = upload_->Submit(xHost_, xDevice_, regions);
            auto uploadY = upload_->Submit(yHost_, yDevice_, regions);
            if (!uploadX || !uploadY) {
                std::terminate();
            }
            const Sora::Compute::Event xReady = uploadX->Completion();
            const Sora::Compute::Event yReady = uploadY->Completion();
            const std::array waits{xReady.View(), yReady.View()};
            auto dispatchRequest = request_;
            dispatchRequest.waits = waits.data();
            dispatchRequest.waitCount = static_cast<uint32_t>(waits.size());
            auto dispatched = backend_.ExecutionQueue().Submit(dispatchRequest);
            if (!dispatched) {
                std::terminate();
            }
            auto downloaded = download_->Submit(outputDevice_, outputHost_, regions, {*dispatched});
            if (!downloaded || downloaded->WaitUntilTerminal().status != Sora::Compute::EventStatus::Completed) {
                std::terminate();
            }
        }

        void Validate(bool hostOutput = false) {
            if (hostOutput) {
                RunRoundTrip();
            } else {
                RunDispatch();
            }
            auto outputBytes = Take(backend_.Map(hostOutput ? outputHost_ : outputDevice_), "map output");
            const auto output = As<float>(outputBytes);
            const auto x = As<float>(Take(backend_.Map(hostOutput ? xHost_ : xDevice_), "map x"));
            const auto y = As<float>(Take(backend_.Map(hostOutput ? yHost_ : yDevice_), "map y"));
            float maximumError = 0.0F;
            for (uint64_t index = 0; index < count_; ++index) {
                for (uint8_t component = 0; component < valueType_.lanes; ++component) {
                    const uint64_t offset = Offset(index, component);
                    const float expected =
                        operation_ == Operation::Saxpy ? std::fma(kAlpha, x[offset], y[offset]) : x[offset] + y[offset];
                    const float scale = std::max({1.0F, std::abs(expected), std::abs(output[offset])});
                    maximumError = std::max(maximumError, std::abs(expected - output[offset]) / scale);
                }
            }
            if (maximumError > 2.0e-6F) {
                std::println(stderr, "kernel result mismatch: relative error={}", maximumError);
                std::exit(2);
            }
        }

        void ResetStatistics() noexcept { backend_.ResetStatistics(); }

        [[nodiscard]] Sora::Compute::CpuBackendStatistics Statistics() const noexcept { return backend_.Statistics(); }

        [[nodiscard]] uint32_t WorkerCount() const noexcept { return backend_.Device().computeUnitCount; }

        [[nodiscard]] double Checksum(bool hostOutput = false) {
            const auto output = As<float>(Take(backend_.Map(hostOutput ? outputHost_ : outputDevice_), "checksum"));
            return static_cast<double>(output[count_ / 3]) + static_cast<double>(output[output.size() / 2]);
        }

    private:
        KernelWorkload(Sora::Compute::CpuBackendConfig config, uint64_t count, Operation operation,
                       Sora::Compute::LayoutKind layout, Sora::Compute::CpuCodeProvider provider)
            : backend_(Take(Sora::Compute::CpuBackend::Create(config, std::move(provider)), "create CPU backend")),
              count_(count),
              operation_(operation),
              layout_(layout),
              valueType_(operation == Operation::Saxpy ? kFloat : kVec3),
              sizeBytes_(count * valueType_.ByteSize()),
              xDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), sizeBytes_)),
              yDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), sizeBytes_)),
              outputDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), sizeBytes_)),
              xHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), sizeBytes_)),
              yHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), sizeBytes_)),
              outputHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), sizeBytes_)) {
            InitializeInputs(xDevice_, yDevice_);
            InitializeInputs(xHost_, yHost_);
            artifact_ =
                operation == Operation::Saxpy
                    ? Compile(backend_, kSaxpy)
                    : Compile(backend_, layout == Sora::Compute::LayoutKind::StructureOfArrays ? kSoaAdd : kAosAdd);
            const uint32_t firstInputBinding = operation == Operation::Saxpy ? 1U : 0U;
            bindings_ = {
                MakeBinding(xDevice_, firstInputBinding, Sora::Compute::AccessMode::Read, valueType_, layout_, count_),
                MakeBinding(yDevice_, firstInputBinding + 1U, Sora::Compute::AccessMode::Read, valueType_, layout_,
                            count_),
                MakeBinding(outputDevice_, firstInputBinding + 2U, Sora::Compute::AccessMode::Write, valueType_,
                            layout_, count_),
            };
            alphaData_ = std::bit_cast<std::array<std::byte, sizeof(float)>>(kAlpha);
            scalarBinding_ = {.dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat};
            command_ = {.kind = Sora::Compute::BackendCommandKind::Dispatch,
                        .bindingCount = static_cast<uint32_t>(bindings_.size()),
                        .scalarBindingCount = operation == Operation::Saxpy ? 1U : 0U,
                        .artifact = artifact_.View(),
                        .workgroupCount = {1, 1, 1}};
            request_ = {.commands = &command_,
                        .commandCount = 1,
                        .bindings = bindings_.data(),
                        .bindingCount = static_cast<uint32_t>(bindings_.size()),
                        .scalarData = operation == Operation::Saxpy ? alphaData_.data() : nullptr,
                        .scalarDataSize = operation == Operation::Saxpy ? alphaData_.size() : 0,
                        .scalarBindings = operation == Operation::Saxpy ? &scalarBinding_ : nullptr,
                        .scalarBindingCount = operation == Operation::Saxpy ? 1U : 0U};
            upload_ = &FindTransfer(backend_, backend_.HostLocation(), backend_.DeviceLocation());
            download_ = &FindTransfer(backend_, backend_.DeviceLocation(), backend_.HostLocation());
        }

        void InitializeInputs(Sora::Compute::Allocation& xAllocation, Sora::Compute::Allocation& yAllocation) {
            auto x = As<float>(Take(backend_.Map(xAllocation), "map x initialization"));
            auto y = As<float>(Take(backend_.Map(yAllocation), "map y initialization"));
            for (uint64_t index = 0; index < count_; ++index) {
                for (uint8_t component = 0; component < valueType_.lanes; ++component) {
                    const uint64_t offset = Offset(index, component);
                    x[offset] = static_cast<float>(index % 4096U) * 0.25F + static_cast<float>(component);
                    y[offset] =
                        1000.0F - static_cast<float>(index % 2048U) * 0.125F + static_cast<float>(component) * 0.5F;
                }
            }
        }

        [[nodiscard]] uint64_t Offset(uint64_t index, uint8_t component) const noexcept {
            return layout_ == Sora::Compute::LayoutKind::StructureOfArrays ? component * count_ + index
                                                                           : index * valueType_.lanes + component;
        }

        Sora::Compute::CpuBackend backend_;
        uint64_t count_ = 0;
        Operation operation_ = Operation::Saxpy;
        Sora::Compute::LayoutKind layout_ = Sora::Compute::LayoutKind::Contiguous;
        Sora::Compute::ValueType valueType_{};
        uint64_t sizeBytes_ = 0;
        Sora::Compute::Allocation xDevice_;
        Sora::Compute::Allocation yDevice_;
        Sora::Compute::Allocation outputDevice_;
        Sora::Compute::Allocation xHost_;
        Sora::Compute::Allocation yHost_;
        Sora::Compute::Allocation outputHost_;
        Sora::Compute::Artifact artifact_;
        std::array<Sora::Compute::BackendBinding, 3> bindings_{};
        std::array<std::byte, sizeof(float)> alphaData_{};
        Sora::Compute::BackendScalarBinding scalarBinding_{};
        Sora::Compute::BackendCommand command_{};
        Sora::Compute::BackendSubmitRequest request_{};
        const Sora::Compute::TransferEngine* upload_ = nullptr;
        const Sora::Compute::TransferEngine* download_ = nullptr;
    };

    [[nodiscard]] Sora::Compute::CpuCodeProvider CreateLlvmProvider() {
#if defined(_WIN32)
        const std::array searchPaths{std::filesystem::path{LR"(C:\Program Files\LLVM\bin)"}};
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider(
            {.searchPaths = std::span<const std::filesystem::path>{searchPaths}});
#else
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider();
#endif
        return Take(std::move(created), "create LLVM CPU provider");
    }

    constexpr Sora::Compute::CpuBackendConfig SerialConfig(bool simd) noexcept {
        return {.workerCount = 1,
                .minimumParallelWorkItems = std::numeric_limits<uint64_t>::max(),
                .minimumSimdWorkItems = 1,
                .enableSimd = simd};
    }

    constexpr Sora::Compute::CpuBackendConfig ParallelConfig(bool simd) noexcept {
        return {.workerCount = 0, .minimumParallelWorkItems = 1, .minimumSimdWorkItems = 1, .enableSimd = simd};
    }

    void MeasureCrossover(uint64_t count) {
        auto scalarSerial = KernelWorkload::Create(SerialConfig(false), count, Operation::Saxpy);
        auto simdSerial = KernelWorkload::Create(SerialConfig(true), count, Operation::Saxpy);
        auto scalarParallel = KernelWorkload::Create(ParallelConfig(false), count, Operation::Saxpy);
        auto simdParallel = KernelWorkload::Create(ParallelConfig(true), count, Operation::Saxpy);
        auto adaptive = KernelWorkload::Create({}, count, Operation::Saxpy);
        auto llvmJit = KernelWorkload::Create(SerialConfig(false), count, Operation::Saxpy,
                                              Sora::Compute::LayoutKind::Contiguous, CreateLlvmProvider());
        scalarSerial->Validate();
        simdSerial->Validate();
        scalarParallel->Validate();
        simdParallel->Validate();
        adaptive->Validate();
        llvmJit->Validate();
        scalarSerial->ResetStatistics();
        simdSerial->ResetStatistics();
        scalarParallel->ResetStatistics();
        simdParallel->ResetStatistics();
        adaptive->ResetStatistics();
        llvmJit->ResetStatistics();

        auto runScalarSerial = [&]() noexcept { scalarSerial->RunDispatch(); };
        auto runSimdSerial = [&]() noexcept { simdSerial->RunDispatch(); };
        auto runScalarParallel = [&]() noexcept { scalarParallel->RunDispatch(); };
        auto runSimdParallel = [&]() noexcept { simdParallel->RunDispatch(); };
        auto runAdaptive = [&]() noexcept { adaptive->RunDispatch(); };
        auto runLlvmJit = [&]() noexcept { llvmJit->RunDispatch(); };
        const std::array cases{
            Sora::Benchmark::MakeCase("scalar_serial", runScalarSerial),
            Sora::Benchmark::MakeCase("simd_serial", runSimdSerial),
            Sora::Benchmark::MakeCase("scalar_parallel", runScalarParallel),
            Sora::Benchmark::MakeCase("simd_parallel", runSimdParallel),
            Sora::Benchmark::MakeCase("adaptive", runAdaptive),
            Sora::Benchmark::MakeCase("llvm_jit", runLlvmJit),
        };
        const auto timing =
            Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, count, kMinimumSampleTime);
        const auto scalarStats = scalarSerial->Statistics();
        const auto simdStats = simdSerial->Statistics();
        const auto scalarParallelStats = scalarParallel->Statistics();
        const auto simdParallelStats = simdParallel->Statistics();
        const auto adaptiveStats = adaptive->Statistics();
        const auto llvmJitStats = llvmJit->Statistics();
        const uint64_t adaptiveThreshold = static_cast<uint64_t>(adaptive->WorkerCount()) * 32U * 1024U;
        const bool adaptiveShouldParallelize = adaptive->WorkerCount() > 1 && count >= adaptiveThreshold;
        if (scalarStats.scalarWorkItems == 0 || simdStats.simdWorkItems == 0 ||
            scalarParallelStats.parallelDispatches == 0 || simdParallelStats.parallelDispatches == 0 ||
            simdParallelStats.simdWorkItems == 0 ||
            (adaptiveStats.parallelDispatches != 0) != adaptiveShouldParallelize || llvmJitStats.nativeWorkItems == 0) {
            std::println(stderr, "benchmark path verification failed at count={}", count);
            std::exit(2);
        }
        constexpr std::array names{"scalar_serial", "simd_serial", "scalar_parallel",
                                   "simd_parallel", "adaptive",    "llvm_jit"};
        for (size_t index = 0; index < names.size(); ++index) {
            std::println("saxpy_dispatch,{},{},{:.6f},ns/element", count, names[index], timing.MedianTime(index));
        }
        std::println("saxpy_ratio,{},simd_speedup_serial,{:.6f},ratio", count, timing.PairedRatio(0, 1));
        std::println("saxpy_ratio,{},parallel_speedup_scalar,{:.6f},ratio", count, timing.PairedRatio(0, 2));
        std::println("saxpy_ratio,{},parallel_speedup_simd,{:.6f},ratio", count, timing.PairedRatio(1, 3));
        std::println("saxpy_ratio,{},adaptive_speedup_over_simd_serial,{:.6f},ratio", count, timing.PairedRatio(1, 4));
        std::println("saxpy_ratio,{},llvm_jit_speedup_over_simd_serial,{:.6f},ratio", count, timing.PairedRatio(1, 5));
    }

    void MeasureLayout() {
        constexpr uint64_t count = 1U << 18U;
        auto aosScalar = KernelWorkload::Create(SerialConfig(false), count, Operation::Vec3Add);
        auto aosSimd = KernelWorkload::Create(SerialConfig(true), count, Operation::Vec3Add);
        auto soaScalar = KernelWorkload::Create(SerialConfig(false), count, Operation::Vec3Add,
                                                Sora::Compute::LayoutKind::StructureOfArrays);
        auto soaSimd = KernelWorkload::Create(SerialConfig(true), count, Operation::Vec3Add,
                                              Sora::Compute::LayoutKind::StructureOfArrays);
        aosScalar->Validate();
        aosSimd->Validate();
        soaScalar->Validate();
        soaSimd->Validate();
        auto runAosScalar = [&]() noexcept { aosScalar->RunDispatch(); };
        auto runAosSimd = [&]() noexcept { aosSimd->RunDispatch(); };
        auto runSoaScalar = [&]() noexcept { soaScalar->RunDispatch(); };
        auto runSoaSimd = [&]() noexcept { soaSimd->RunDispatch(); };
        const std::array cases{
            Sora::Benchmark::MakeCase("aos_scalar", runAosScalar),
            Sora::Benchmark::MakeCase("aos_simd", runAosSimd),
            Sora::Benchmark::MakeCase("soa_scalar", runSoaScalar),
            Sora::Benchmark::MakeCase("soa_simd", runSoaSimd),
        };
        const auto timing =
            Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, count, kMinimumSampleTime);
        constexpr std::array names{"aos_scalar", "aos_simd", "soa_scalar", "soa_simd"};
        for (size_t index = 0; index < names.size(); ++index) {
            std::println("vec3_layout,{},{},{:.6f},ns/element", count, names[index], timing.MedianTime(index));
        }
        std::println("vec3_ratio,{},aos_simd_speedup,{:.6f},ratio", count, timing.PairedRatio(0, 1));
        std::println("vec3_ratio,{},soa_simd_speedup,{:.6f},ratio", count, timing.PairedRatio(2, 3));
        std::println("vec3_ratio,{},soa_simd_over_aos_scalar,{:.6f},ratio", count, timing.PairedRatio(0, 3));
        std::println("vec3_ratio,{},soa_over_aos_simd,{:.6f},ratio", count, timing.PairedRatio(1, 3));
    }

    void MeasureRoundTrip() {
        constexpr uint64_t count = 1U << 18U;
        auto workload = KernelWorkload::Create(ParallelConfig(true), count, Operation::Saxpy);
        workload->Validate();
        workload->Validate(true);
        auto dispatch = [&]() noexcept { workload->RunDispatch(); };
        auto roundTrip = [&]() noexcept { workload->RunRoundTrip(); };
        const std::array cases{Sora::Benchmark::MakeCase("dispatch", dispatch),
                               Sora::Benchmark::MakeCase("round_trip", roundTrip)};
        const auto timing =
            Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, count, kMinimumSampleTime);
        std::println("pipeline,{},{},{:.6f},ns/element", count, "dispatch", timing.MedianTime(0));
        std::println("pipeline,{},{},{:.6f},ns/element", count, "round_trip", timing.MedianTime(1));
        std::println("pipeline_ratio,{},round_trip_over_dispatch,{:.6f},ratio", count, timing.PairedRatio(1, 0));
        std::println("checksum,{},pipeline,{:.9f},value", count, workload->Checksum(true));
    }

} // namespace

int main() {
    std::println("family,elements,mode,value,unit");
    const Sora::PAL::CpuInfo cpu = Sora::PAL::DetectHostCpuInfo();
    std::println("cpu_metadata,0,logical_processors,{},count", cpu.logicalProcessorCount);
    std::println("cpu_metadata,0,maximum_vector_bits,{},bits", cpu.MaximumFixedVectorBits());
    std::println("cpu_metadata,0,architecture_id,{},id", cpu.ArchitectureId());
    constexpr std::array<uint64_t, 8> counts{64, 512, 4096, 32768, 131072, 262144, 524288, 1048576};
    for (const uint64_t count : counts) {
        MeasureCrossover(count);
    }
    MeasureLayout();
    MeasureRoundTrip();
}
