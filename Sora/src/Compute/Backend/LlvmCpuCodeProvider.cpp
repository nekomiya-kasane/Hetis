/**
 * @file LlvmCpuCodeProvider.cpp
 * @brief Dynamically loaded LLVM 22 native CPU code provider.
 * @ingroup Compute
 */

#include <Sora/Compute/Backend/LlvmCpuCodeProvider.h>

#include "LlvmCApi.h"
#include "LlvmKernelLowering.h"

#include <Sora/Compute/KernelProgramPackage.h>
#include <Sora/Compute/WireReader.h>
#include <Sora/Core/PAL/CpuInfo.h>
#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/Wire.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Sora::Compute {

    namespace {

        constexpr uint64_t kLlvmArtifactMagic = 0x4D564C4C41524F53ULL; // "SORALLVM" in little-endian byte order.
        constexpr uint32_t kLlvmArtifactSchemaVersion = 1;
        constexpr uint32_t kLlvmLoweringSchemaVersion = 1;
        constexpr std::string_view kOptimizationPipeline = "verify,default<O3>,verify";

        struct ProviderState;

        struct DylibRecord {
            LlvmC::OrcJITDylibRef dylib = nullptr;
            bool usable = false;
        };

        struct Materialization {
            ArtifactKey key{};
            LlvmC::OrcResourceTrackerRef tracker = nullptr;
            CpuCodeEntry entry = nullptr;
            std::vector<std::byte> object;

            Materialization(ArtifactKey artifactKey, LlvmC::OrcResourceTrackerRef resourceTracker,
                            CpuCodeEntry kernelEntry, std::vector<std::byte> objectBytes) noexcept
                : key(artifactKey), tracker(resourceTracker), entry(kernelEntry), object(std::move(objectBytes)) {}

            Materialization(const Materialization&) = delete;
            Materialization& operator=(const Materialization&) = delete;
            Materialization(Materialization&&) = delete;
            Materialization& operator=(Materialization&&) = delete;
        };

        struct ArtifactState {
            ProviderState* owner = nullptr;
            std::shared_ptr<Materialization> materialization;
        };

        struct CacheRecord {
            std::shared_ptr<Materialization> materialization;
            size_t artifactLeases = 0;
        };

        struct ExportedArtifact {
            std::vector<std::byte> bytes;
        };

        struct ProviderState {
            PAL::ModulePtr module;
            LlvmC::Api api{};
            PAL::CpuInfo cpu{};
            unsigned llvmMajor = 0;
            unsigned llvmMinor = 0;
            unsigned llvmPatch = 0;
            std::string triple;
            std::string cpuName;
            std::string cpuFeatures;
            std::string dataLayout;
            Detail::LlvmFunctionSanitizerAbi functionSanitizer{};
            LlvmC::TargetRef target = nullptr;
            LlvmC::OrcLLJITRef jit = nullptr;
            SemanticHash compilerIdentity{};
            SemanticHash driverIdentity{};
            std::mutex mutex;
            std::unordered_map<ArtifactKey, CacheRecord, ArtifactKeyHash> cache;
            std::unordered_map<ArtifactKey, DylibRecord, ArtifactKeyHash> dylibs;

            ~ProviderState() noexcept;
        };

        template<typename Opaque>
        struct LlvmDeleter {
            void (*destroy)(Opaque*) = nullptr;

            void operator()(Opaque* value) const noexcept {
                if (value != nullptr) {
                    destroy(value);
                }
            }
        };

        using UniqueContext = std::unique_ptr<LlvmC::OpaqueContext, LlvmDeleter<LlvmC::OpaqueContext>>;
        using UniqueModule = std::unique_ptr<LlvmC::OpaqueModule, LlvmDeleter<LlvmC::OpaqueModule>>;
        using UniqueMemoryBuffer = std::unique_ptr<LlvmC::OpaqueMemoryBuffer, LlvmDeleter<LlvmC::OpaqueMemoryBuffer>>;
        using UniqueTargetMachine =
            std::unique_ptr<LlvmC::OpaqueTargetMachine, LlvmDeleter<LlvmC::OpaqueTargetMachine>>;
        using UniqueTargetData = std::unique_ptr<LlvmC::OpaqueTargetData, LlvmDeleter<LlvmC::OpaqueTargetData>>;
        using UniquePassOptions =
            std::unique_ptr<LlvmC::OpaquePassBuilderOptions, LlvmDeleter<LlvmC::OpaquePassBuilderOptions>>;

        void ConsumeError(const LlvmC::Api& api, LlvmC::ErrorRef error) noexcept {
            if (error == nullptr) {
                return;
            }
            char* message = api.getErrorMessage(error);
            if (message != nullptr) {
                api.disposeErrorMessage(message);
            }
        }

        void DisposeMessage(const LlvmC::Api& api, char* message) noexcept {
            if (message != nullptr) {
                api.disposeMessage(message);
            }
        }

        ProviderState::~ProviderState() noexcept {
            assert(cache.empty());
            if (jit != nullptr) {
                ConsumeError(api, api.disposeLlJit(jit));
            }
        }

        void ReleaseTracker(ProviderState& state, LlvmC::OrcResourceTrackerRef tracker) noexcept {
            if (tracker == nullptr) {
                return;
            }
            ConsumeError(state.api, state.api.removeResourceTracker(tracker));
            state.api.releaseResourceTracker(tracker);
        }

        template<typename FunctionPointer>
        [[nodiscard]] bool Bind(const PAL::Module& module, FunctionPointer& function, std::string_view name) noexcept {
            function = module.TryFindFunction<FunctionPointer>(name);
            return function != nullptr;
        }

        [[nodiscard]] bool BindTargetInitializers(const PAL::Module& module, CpuArchitecture architecture,
                                                  LlvmC::Api& api) {
            std::string_view target;
            switch (architecture) {
                case CpuArchitecture::X86:
                case CpuArchitecture::X64:
                    target = "X86";
                    break;
                case CpuArchitecture::Arm32:
                    target = "ARM";
                    break;
                case CpuArchitecture::Arm64:
                    target = "AArch64";
                    break;
                case CpuArchitecture::RiscV32:
                case CpuArchitecture::RiscV64:
                    target = "RISCV";
                    break;
                case CpuArchitecture::PowerPC64:
                    target = "PowerPC";
                    break;
                case CpuArchitecture::LoongArch64:
                    target = "LoongArch";
                    break;
                case CpuArchitecture::S390x:
                    target = "SystemZ";
                    break;
                case CpuArchitecture::Wasm32:
                case CpuArchitecture::Wasm64:
                case CpuArchitecture::Unknown:
                    return false;
            }
            const std::string prefix = "LLVMInitialize" + std::string{target};
            return Bind(module, api.initializeTargetInfo, prefix + "TargetInfo") &&
                   Bind(module, api.initializeTarget, prefix + "Target") &&
                   Bind(module, api.initializeTargetMc, prefix + "TargetMC") &&
                   Bind(module, api.initializeAsmPrinter, prefix + "AsmPrinter");
        }

        [[nodiscard]] bool BindApi(const PAL::Module& module, LlvmC::Api& api) {
            return Bind(module, api.getVersion, "LLVMGetVersion") &&
                   Bind(module, api.contextCreate, "LLVMContextCreate") &&
                   Bind(module, api.contextDispose, "LLVMContextDispose") &&
                   Bind(module, api.createMemoryBufferCopy, "LLVMCreateMemoryBufferWithMemoryRangeCopy") &&
                   Bind(module, api.getBufferStart, "LLVMGetBufferStart") &&
                   Bind(module, api.getBufferSize, "LLVMGetBufferSize") &&
                   Bind(module, api.disposeMemoryBuffer, "LLVMDisposeMemoryBuffer") &&
                   Bind(module, api.disposeMessage, "LLVMDisposeMessage") &&
                   Bind(module, api.disposeModule, "LLVMDisposeModule") &&
                   Bind(module, api.setTarget, "LLVMSetTarget") &&
                   Bind(module, api.setDataLayout, "LLVMSetDataLayout") &&
                   Bind(module, api.parseIrInContext, "LLVMParseIRInContext2") &&
                   Bind(module, api.getDefaultTargetTriple, "LLVMGetDefaultTargetTriple") &&
                   Bind(module, api.getHostCpuName, "LLVMGetHostCPUName") &&
                   Bind(module, api.getHostCpuFeatures, "LLVMGetHostCPUFeatures") &&
                   Bind(module, api.getTargetFromTriple, "LLVMGetTargetFromTriple") &&
                   Bind(module, api.createTargetMachine, "LLVMCreateTargetMachine") &&
                   Bind(module, api.disposeTargetMachine, "LLVMDisposeTargetMachine") &&
                   Bind(module, api.createTargetDataLayout, "LLVMCreateTargetDataLayout") &&
                   Bind(module, api.disposeTargetData, "LLVMDisposeTargetData") &&
                   Bind(module, api.copyTargetDataLayout, "LLVMCopyStringRepOfTargetData") &&
                   Bind(module, api.emitToMemoryBuffer, "LLVMTargetMachineEmitToMemoryBuffer") &&
                   Bind(module, api.createPassBuilderOptions, "LLVMCreatePassBuilderOptions") &&
                   Bind(module, api.disposePassBuilderOptions, "LLVMDisposePassBuilderOptions") &&
                   Bind(module, api.runPasses, "LLVMRunPasses") &&
                   Bind(module, api.getErrorMessage, "LLVMGetErrorMessage") &&
                   Bind(module, api.disposeErrorMessage, "LLVMDisposeErrorMessage") &&
                   Bind(module, api.createLlJit, "LLVMOrcCreateLLJIT") &&
                   Bind(module, api.disposeLlJit, "LLVMOrcDisposeLLJIT") &&
                   Bind(module, api.getExecutionSession, "LLVMOrcLLJITGetExecutionSession") &&
                   Bind(module, api.getLlJitTriple, "LLVMOrcLLJITGetTripleString") &&
                   Bind(module, api.getGlobalPrefix, "LLVMOrcLLJITGetGlobalPrefix") &&
                   Bind(module, api.createBareJITDylib, "LLVMOrcExecutionSessionCreateBareJITDylib") &&
                   Bind(module, api.createProcessGenerator, "LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess") &&
                   Bind(module, api.addGenerator, "LLVMOrcJITDylibAddGenerator") &&
                   Bind(module, api.createResourceTracker, "LLVMOrcJITDylibCreateResourceTracker") &&
                   Bind(module, api.removeResourceTracker, "LLVMOrcResourceTrackerRemove") &&
                   Bind(module, api.releaseResourceTracker, "LLVMOrcReleaseResourceTracker") &&
                   Bind(module, api.addObjectFile, "LLVMOrcLLJITAddObjectFileWithRT") &&
                   Bind(module, api.mangleAndIntern, "LLVMOrcLLJITMangleAndIntern") &&
                   Bind(module, api.lookup, "LLVMOrcExecutionSessionLookup");
        }

        [[nodiscard]] std::string TakeMessage(const LlvmC::Api& api, char* message) {
            if (message == nullptr) {
                return {};
            }
            std::string result{message};
            api.disposeMessage(message);
            return result;
        }

        void CpuCodeEntryAbiWitness(void*, const CpuCodeInvocation*) noexcept {}

        [[nodiscard]] Detail::LlvmFunctionSanitizerAbi DetectFunctionSanitizerAbi() noexcept {
#if defined(__has_feature)
#    if __has_feature(undefined_behavior_sanitizer)
            static_assert(sizeof(CpuCodeEntry) == sizeof(uintptr_t));
            constexpr uint32_t kClangFunctionSanitizerSignature = 0xC105CAFEU;
            const CpuCodeEntry witness = CpuCodeEntryAbiWitness;
            const uintptr_t entry = reinterpret_cast<uintptr_t>(witness);
            if (entry < sizeof(Detail::LlvmFunctionSanitizerAbi)) {
                return {};
            }
            Detail::LlvmFunctionSanitizerAbi result{};
            std::memcpy(&result, reinterpret_cast<const void*>(entry - sizeof(result)), sizeof(result));
            if (result.signature == kClangFunctionSanitizerSignature) {
                return result;
            }
#    endif
#endif
            return {};
        }

        [[nodiscard]] SemanticHash HashIdentity(std::initializer_list<std::string_view> fields,
                                                std::span<const uint32_t> integers) {
            std::vector<std::byte> bytes;
            size_t capacity = integers.size_bytes();
            for (std::string_view field : fields) {
                capacity += sizeof(uint64_t) + field.size();
            }
            bytes.reserve(capacity);
            for (const uint32_t value : integers) {
                Wire::AppendLittleEndian(bytes, value);
            }
            for (std::string_view field : fields) {
                Wire::AppendLittleEndian(bytes, static_cast<uint64_t>(field.size()));
                const auto* first = reinterpret_cast<const std::byte*>(field.data());
                bytes.insert(bytes.end(), first, first + field.size());
            }
            return Detail::HashBytes(bytes);
        }

        [[nodiscard]] TargetIdentityStability HostIdentityStability(const PAL::CpuInfo& cpu) noexcept {
            if (cpu.architecture == CpuArchitecture::X86 || cpu.architecture == CpuArchitecture::X64) {
                return cpu.x86.vendor.front() != '\0' ? TargetIdentityStability::MachineStable
                                                      : TargetIdentityStability::ProcessStable;
            }
            return TargetIdentityStability::ProcessStable;
        }

        [[nodiscard]] bool ValidTarget(const ProviderState& state, const TargetFingerprint& target) noexcept {
            return target.backend == BackendKind::Cpu && target.device.Valid() &&
                   target.hostArchitecture == state.cpu.architecture && target.operatingSystem == kOperatingSystem &&
                   target.binaryFormat == kBinaryFormat && target.byteOrder == state.cpu.byteOrder &&
                   target.abi == kAbiFamily && target.identityStability == HostIdentityStability(state.cpu) &&
                   target.addressBits == state.cpu.addressBits && target.architectureId == state.cpu.ArchitectureId() &&
                   target.fixedVectorBits == state.cpu.MaximumFixedVectorBits() &&
                   target.scalableVectorBits == state.cpu.currentScalableVectorBits &&
                   target.backendAbiVersion == kComputeBackendAbiVersion &&
                   target.compilerIdentity == state.compilerIdentity && target.driverIdentity == state.driverIdentity;
        }

        [[nodiscard]] bool ValidCompileRequest(const ProviderState& state,
                                               const CpuCodeCompileRequest& request) noexcept {
            return ValidCpuCodeProviderAbi(request.abiVersion, request.structSize, sizeof(CpuCodeCompileRequest)) &&
                   request.programData != nullptr && request.programSize != 0 &&
                   request.programSize <= std::numeric_limits<size_t>::max() &&
                   Detail::HashBytes(request.ProgramBytes()) == request.programHash &&
                   ValidTarget(state, request.target);
        }

        [[nodiscard]] ArtifactKey ArtifactKeyFor(const CpuCodeCompileRequest& request) noexcept {
            return MakeArtifactKey(ExecutableKey{
                .lowered = request.programHash, .target = request.target, .specialization = request.specialization});
        }

        [[nodiscard]] UniqueTargetMachine CreateTargetMachine(ProviderState& state) noexcept {
            return UniqueTargetMachine{
                state.api.createTargetMachine(state.target, state.triple.c_str(), state.cpuName.c_str(),
                                              state.cpuFeatures.c_str(), LlvmC::CodeGenOptLevel::Aggressive,
                                              LlvmC::RelocMode::Pic, LlvmC::CodeModel::JitDefault),
                LlvmDeleter<LlvmC::OpaqueTargetMachine>{state.api.disposeTargetMachine}};
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, CompileError>
        CompileObject(ProviderState& state, const CpuCodeCompileRequest& request) {
            auto decoded = DecodeKernelProgramPackage<kCpuMaximumKernelArguments, kCpuMaximumScalarParameters,
                                                      kCpuMaximumInstructions>(request.ProgramBytes());
            if (!decoded) {
                return std::unexpected(
                    CompileError{CompileErrorCode::InvalidRequest, static_cast<uint64_t>(decoded.error().code)});
            }
            auto ir = Detail::LowerKernelProgramToLlvmIr(*decoded, state.functionSanitizer);
            if (!ir) {
                return std::unexpected(ir.error());
            }
            UniqueContext context{state.api.contextCreate(),
                                  LlvmDeleter<LlvmC::OpaqueContext>{state.api.contextDispose}};
            if (!context) {
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
            UniqueMemoryBuffer irBuffer{
                state.api.createMemoryBufferCopy(ir->data(), ir->size(), "Sora.KernelProgram.ll"),
                LlvmDeleter<LlvmC::OpaqueMemoryBuffer>{state.api.disposeMemoryBuffer}};
            if (!irBuffer) {
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
            LlvmC::ModuleRef rawModule = nullptr;
            char* parseMessage = nullptr;
            if (state.api.parseIrInContext(context.get(), irBuffer.get(), &rawModule, &parseMessage) != 0) {
                DisposeMessage(state.api, parseMessage);
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            UniqueModule module{rawModule, LlvmDeleter<LlvmC::OpaqueModule>{state.api.disposeModule}};
            state.api.setTarget(module.get(), state.triple.c_str());
            state.api.setDataLayout(module.get(), state.dataLayout.c_str());

            UniqueTargetMachine targetMachine = CreateTargetMachine(state);
            UniquePassOptions passOptions{
                state.api.createPassBuilderOptions(),
                LlvmDeleter<LlvmC::OpaquePassBuilderOptions>{state.api.disposePassBuilderOptions}};
            if (!targetMachine || !passOptions) {
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
            if (LlvmC::ErrorRef error = state.api.runPasses(module.get(), kOptimizationPipeline.data(),
                                                            targetMachine.get(), passOptions.get());
                error != nullptr) {
                ConsumeError(state.api, error);
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            char* emissionMessage = nullptr;
            LlvmC::MemoryBufferRef rawObject = nullptr;
            if (state.api.emitToMemoryBuffer(targetMachine.get(), module.get(), LlvmC::CodeGenFileType::Object,
                                             &emissionMessage, &rawObject) != 0) {
                DisposeMessage(state.api, emissionMessage);
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            UniqueMemoryBuffer object{rawObject, LlvmDeleter<LlvmC::OpaqueMemoryBuffer>{state.api.disposeMemoryBuffer}};
            const size_t size = state.api.getBufferSize(object.get());
            const auto* first = reinterpret_cast<const std::byte*>(state.api.getBufferStart(object.get()));
            if (first == nullptr || size == 0) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            return std::vector<std::byte>{first, first + size};
        }

        void AppendHex(std::string& output, uint64_t value) {
            constexpr std::string_view digits = "0123456789abcdef";
            for (unsigned nibble = 16; nibble-- > 0;) {
                output.push_back(digits[(value >> (nibble * 4U)) & 0xFU]);
            }
        }

        [[nodiscard]] std::string DylibName(const ArtifactKey& key) {
            std::string result{"__sora_cpu_"};
            result.reserve(result.size() + 18 + key.Words().size() * 17);
            AppendHex(result, std::to_underlying(key.Kind()));
            for (uint64_t word : key.Words()) {
                result.push_back('_');
                AppendHex(result, word);
            }
            return result;
        }

        [[nodiscard]] std::expected<LlvmC::OrcJITDylibRef, CompileError> ResolveDylibLocked(ProviderState& state,
                                                                                            const ArtifactKey& key) {
            if (auto found = state.dylibs.find(key); found != state.dylibs.end()) {
                return found->second.usable ? std::expected<LlvmC::OrcJITDylibRef, CompileError>{found->second.dylib}
                                            : std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            const std::string name = DylibName(key);
            auto [record, inserted] = state.dylibs.try_emplace(key);
            if (!inserted) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            LlvmC::OrcExecutionSessionRef session = state.api.getExecutionSession(state.jit);
            record->second.dylib = state.api.createBareJITDylib(session, name.c_str());
            if (record->second.dylib == nullptr) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            LlvmC::OrcDefinitionGeneratorRef generator = nullptr;
            if (LlvmC::ErrorRef error = state.api.createProcessGenerator(
                    &generator, state.api.getGlobalPrefix(state.jit), nullptr, nullptr);
                error != nullptr) {
                ConsumeError(state.api, error);
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            if (generator == nullptr) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            state.api.addGenerator(record->second.dylib, generator);
            record->second.usable = true;
            return record->second.dylib;
        }

        struct LookupContext {
            const LlvmC::Api* api = nullptr;
            std::mutex mutex;
            std::condition_variable completed;
            LlvmC::OrcExecutorAddress address = 0;
            bool done = false;
            bool failed = false;
        };

        void LookupCompleted(LlvmC::ErrorRef error, LlvmC::OrcSymbolMapPair* result, size_t count,
                             void* opaque) noexcept {
            auto& context = *static_cast<LookupContext*>(opaque);
            bool failed = error != nullptr || result == nullptr || count != 1 || result[0].symbol.address == 0;
            if (error != nullptr) {
                ConsumeError(*context.api, error);
            }
            {
                std::scoped_lock lock{context.mutex};
                context.failed = failed;
                context.address = failed ? 0 : result[0].symbol.address;
                context.done = true;
            }
            context.completed.notify_one();
        }

        [[nodiscard]] std::expected<CpuCodeEntry, CompileError> LookupEntryLocked(ProviderState& state,
                                                                                  LlvmC::OrcJITDylibRef dylib) {
            LlvmC::OrcSymbolStringPoolEntryRef symbol =
                state.api.mangleAndIntern(state.jit, Detail::kLlvmCpuEntrySymbol);
            if (symbol == nullptr) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            LlvmC::OrcJITDylibSearchOrderElement search{.dylib = dylib,
                                                        .flags = LlvmC::OrcJITDylibLookupFlags::MatchAllSymbols};
            LlvmC::OrcLookupSetElement lookup{.name = symbol, .flags = LlvmC::OrcSymbolLookupFlags::RequiredSymbol};
            LookupContext context{.api = &state.api};
            state.api.lookup(state.api.getExecutionSession(state.jit), LlvmC::OrcLookupKind::Static, &search, 1,
                             &lookup, 1, LookupCompleted, &context);
            std::unique_lock lock{context.mutex};
            context.completed.wait(lock, [&context] { return context.done; });
            if (context.failed || context.address > std::numeric_limits<uintptr_t>::max()) {
                return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
            }
            return reinterpret_cast<CpuCodeEntry>(static_cast<uintptr_t>(context.address));
        }

        void DestroyArtifact(void* opaque) noexcept;

        [[nodiscard]] CpuCodeArtifactPayload AcquirePayloadLocked(ProviderState& state, CacheRecord& record,
                                                                  std::unique_ptr<ArtifactState> artifact) noexcept {
            artifact->owner = &state;
            artifact->materialization = record.materialization;
            ++record.artifactLeases;
            const CpuCodeEntry entry = artifact->materialization->entry;
            return {.state = artifact.release(), .destroy = DestroyArtifact, .execute = entry};
        }

        [[nodiscard]] std::optional<CpuCodeArtifactPayload> TryAcquireCached(ProviderState& state,
                                                                             const ArtifactKey& key) {
            std::scoped_lock lock{state.mutex};
            if (auto found = state.cache.find(key); found != state.cache.end()) {
                return AcquirePayloadLocked(state, found->second, std::make_unique<ArtifactState>());
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<CpuCodeArtifactPayload, CompileError>
        Materialize(ProviderState& state, ArtifactKey key, std::vector<std::byte> object, bool requireExactCachedObject,
                    CompileErrorCode failure) {
            auto artifact = std::make_unique<ArtifactState>();
            std::unique_lock lock{state.mutex};
            if (auto found = state.cache.find(key); found != state.cache.end()) {
                if (requireExactCachedObject && !std::ranges::equal(found->second.materialization->object, object)) {
                    return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
                }
                return AcquirePayloadLocked(state, found->second, std::move(artifact));
            }
            auto dylib = ResolveDylibLocked(state, key);
            if (!dylib) {
                return std::unexpected(dylib.error());
            }
            LlvmC::OrcResourceTrackerRef tracker = state.api.createResourceTracker(*dylib);
            if (tracker == nullptr) {
                return std::unexpected(CompileError{failure, 0});
            }
            UniqueMemoryBuffer objectBuffer{
                state.api.createMemoryBufferCopy(reinterpret_cast<const char*>(object.data()), object.size(),
                                                 "Sora.KernelProgram.obj"),
                LlvmDeleter<LlvmC::OpaqueMemoryBuffer>{state.api.disposeMemoryBuffer}};
            if (!objectBuffer) {
                state.api.releaseResourceTracker(tracker);
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
            LlvmC::ErrorRef addError = state.api.addObjectFile(state.jit, tracker, objectBuffer.release());
            if (addError != nullptr) {
                ConsumeError(state.api, addError);
                ReleaseTracker(state, tracker);
                return std::unexpected(CompileError{failure, 0});
            }
            auto entry = LookupEntryLocked(state, *dylib);
            if (!entry) {
                ReleaseTracker(state, tracker);
                return std::unexpected(CompileError{failure, 0});
            }
            std::shared_ptr<Materialization> result;
            try {
                result = std::make_shared<Materialization>(key, tracker, *entry, std::move(object));
            } catch (...) {
                ReleaseTracker(state, tracker);
                throw;
            }
            try {
                auto [record, inserted] =
                    state.cache.try_emplace(key, CacheRecord{.materialization = std::move(result)});
                if (!inserted) {
                    ReleaseTracker(state, tracker);
                    return std::unexpected(CompileError{failure, 0});
                }
                return AcquirePayloadLocked(state, record->second, std::move(artifact));
            } catch (...) {
                ReleaseTracker(state, tracker);
                throw;
            }
        }

        void DestroyArtifact(void* opaque) noexcept {
            std::unique_ptr<ArtifactState> artifact{static_cast<ArtifactState*>(opaque)};
            ProviderState& state = *artifact->owner;
            std::scoped_lock lock{state.mutex};
            auto found = state.cache.find(artifact->materialization->key);
            const bool valid = found != state.cache.end() && found->second.artifactLeases != 0 &&
                               found->second.materialization == artifact->materialization;
            assert(valid);
            if (!valid) {
                return;
            }
            if (--found->second.artifactLeases != 0) {
                return;
            }
            ReleaseTracker(state, artifact->materialization->tracker);
            artifact->materialization->tracker = nullptr;
            state.cache.erase(found);
        }

        void DestroyExportedArtifact(void* opaque) noexcept {
            delete static_cast<ExportedArtifact*>(opaque);
        }

        [[nodiscard]] std::vector<std::byte> EncodeArtifact(const Materialization& materialization) {
            const std::span<const uint64_t> words = materialization.key.Words();
            const SemanticHash objectHash = Detail::HashBytes(materialization.object);
            std::vector<std::byte> bytes;
            bytes.reserve(sizeof(uint64_t) + sizeof(uint32_t) * 2 + words.size_bytes() + sizeof(uint64_t) * 3 +
                          materialization.object.size());
            Wire::AppendLittleEndian(bytes, kLlvmArtifactMagic);
            Wire::AppendLittleEndian(bytes, kLlvmArtifactSchemaVersion);
            Wire::AppendLittleEndian(bytes, static_cast<uint32_t>(words.size()));
            for (uint64_t word : words) {
                Wire::AppendLittleEndian(bytes, word);
            }
            Wire::AppendLittleEndian(bytes, static_cast<uint64_t>(materialization.object.size()));
            Wire::AppendLittleEndian(bytes, objectHash.low);
            Wire::AppendLittleEndian(bytes, objectHash.high);
            bytes.insert(bytes.end(), materialization.object.begin(), materialization.object.end());
            return bytes;
        }

        [[nodiscard]] std::expected<std::span<const std::byte>, CompileError>
        DecodeArtifact(std::span<const std::byte> bytes, const ArtifactKey& expected) noexcept {
            Detail::WireReader reader{bytes};
            auto magic = reader.Read<uint64_t>();
            auto version = reader.Read<uint32_t>();
            auto wordCount = reader.Read<uint32_t>();
            if (!magic || !version || !wordCount || *magic != kLlvmArtifactMagic ||
                *version != kLlvmArtifactSchemaVersion || *wordCount != expected.Words().size()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            for (uint64_t expectedWord : expected.Words()) {
                auto word = reader.Read<uint64_t>();
                if (!word || *word != expectedWord) {
                    return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
                }
            }
            auto objectSize = reader.Read<uint64_t>();
            auto hashLow = reader.Read<uint64_t>();
            auto hashHigh = reader.Read<uint64_t>();
            if (!objectSize || !hashLow || !hashHigh || *objectSize == 0 ||
                *objectSize > std::numeric_limits<size_t>::max()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            auto object = reader.ReadBytes(static_cast<size_t>(*objectSize));
            if (!object || !reader.AtEnd() || Detail::HashBytes(*object) != SemanticHash{*hashLow, *hashHigh}) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            return *object;
        }

        void Compile(void* opaque, const CpuCodeCompileRequest* request, CpuCodeArtifactResult* result) noexcept {
            if (result == nullptr) {
                return;
            }
            *result = {};
            auto& state = *static_cast<ProviderState*>(opaque);
            if (request == nullptr || !ValidCompileRequest(state, *request)) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            try {
                const ArtifactKey key = ArtifactKeyFor(*request);
                if (auto cached = TryAcquireCached(state, key)) {
                    result->payload = *cached;
                    result->error = {};
                    return;
                }
                auto object = CompileObject(state, *request);
                if (!object) {
                    result->error = object.error();
                    return;
                }
                auto payload = Materialize(state, key, std::move(*object), false, CompileErrorCode::CompilationFailed);
                if (!payload) {
                    result->error = payload.error();
                    return;
                }
                result->payload = std::move(*payload);
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {CompileErrorCode::OutOfMemory, 0};
            } catch (...) {
                result->error = {CompileErrorCode::CompilationFailed, 0};
            }
        }

        void Import(void* opaque, const CpuCodeImportRequest* request, CpuCodeArtifactResult* result) noexcept {
            if (result == nullptr) {
                return;
            }
            *result = {};
            auto& state = *static_cast<ProviderState*>(opaque);
            const bool valid =
                request != nullptr &&
                ValidCpuCodeProviderAbi(request->abiVersion, request->structSize, sizeof(CpuCodeImportRequest)) &&
                request->compileRequest != nullptr && ValidCompileRequest(state, *request->compileRequest) &&
                request->objectData != nullptr && request->objectSize != 0 &&
                request->objectSize <= std::numeric_limits<size_t>::max();
            if (!valid) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            try {
                const ArtifactKey key = ArtifactKeyFor(*request->compileRequest);
                auto object = DecodeArtifact(request->ObjectBytes(), key);
                if (!object) {
                    result->error = object.error();
                    return;
                }
                std::vector<std::byte> objectCopy{object->begin(), object->end()};
                auto payload =
                    Materialize(state, key, std::move(objectCopy), true, CompileErrorCode::ArtifactImportFailed);
                if (!payload) {
                    result->error = payload.error();
                    return;
                }
                result->payload = std::move(*payload);
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {CompileErrorCode::OutOfMemory, 0};
            } catch (...) {
                result->error = {CompileErrorCode::ArtifactImportFailed, 0};
            }
        }

        void Export(void* opaque, const CpuCodeExportRequest* request, CpuCodeExportResult* result) noexcept {
            if (result == nullptr) {
                return;
            }
            *result = {};
            auto& state = *static_cast<ProviderState*>(opaque);
            const bool valid =
                request != nullptr &&
                ValidCpuCodeProviderAbi(request->abiVersion, request->structSize, sizeof(CpuCodeExportRequest)) &&
                request->artifactState != nullptr;
            if (!valid) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            auto& artifact = *static_cast<ArtifactState*>(request->artifactState);
            if (artifact.owner != &state || !artifact.materialization) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            try {
                auto exported = std::make_unique<ExportedArtifact>();
                exported->bytes = EncodeArtifact(*artifact.materialization);
                result->payload = {.data = exported->bytes.data(),
                                   .size = exported->bytes.size(),
                                   .state = exported.release(),
                                   .destroy = DestroyExportedArtifact};
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {CompileErrorCode::OutOfMemory, 0};
            } catch (...) {
                result->error = {CompileErrorCode::ArtifactExportFailed, 0};
            }
        }

        void DestroyProvider(void* opaque) noexcept {
            delete static_cast<ProviderState*>(opaque);
        }

        [[nodiscard]] std::expected<std::unique_ptr<ProviderState>, CompileError>
        InitializeProvider(LlvmCpuCodeProviderConfig config) {
            PAL::ModuleLoadOptions options{.nameKind = PAL::ModuleNameKind::FileName,
                                           .candidatePolicy = PAL::ModuleCandidatePolicy::NativeDecorated,
                                           .bindMode = PAL::ModuleBindMode::Now,
                                           .visibility = PAL::ModuleVisibility::Global,
                                           .cachePolicy = PAL::ModuleCachePolicy::Shared,
                                           .unloadOnDestroy = true,
                                           .searchPaths = config.searchPaths};
            auto module = PAL::LoadModule({"LLVM-C.dll", "LLVM-C", "LLVM-22", "LLVM"}, options);
            if (!module) {
                return std::unexpected(CompileError{CompileErrorCode::BackendUnavailable, 0});
            }
            auto state = std::make_unique<ProviderState>();
            state->module = std::move(*module);
            state->cpu = PAL::DetectHostCpuInfo();
            state->functionSanitizer = DetectFunctionSanitizerAbi();
            if (!BindApi(*state->module, state->api) ||
                !BindTargetInitializers(*state->module, state->cpu.architecture, state->api)) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidApi, 0});
            }
            state->api.getVersion(&state->llvmMajor, &state->llvmMinor, &state->llvmPatch);
            if (state->llvmMajor != kLlvmCpuCodeProviderMajorVersion) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidApi, state->llvmMajor});
            }
            state->api.initializeTargetInfo();
            state->api.initializeTarget();
            state->api.initializeTargetMc();
            state->api.initializeAsmPrinter();
            state->triple = TakeMessage(state->api, state->api.getDefaultTargetTriple());
            state->cpuName = TakeMessage(state->api, state->api.getHostCpuName());
            state->cpuFeatures = TakeMessage(state->api, state->api.getHostCpuFeatures());
            if (state->triple.empty() || state->cpuName.empty()) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            char* targetMessage = nullptr;
            if (state->api.getTargetFromTriple(state->triple.c_str(), &state->target, &targetMessage) != 0 ||
                state->target == nullptr) {
                DisposeMessage(state->api, targetMessage);
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            UniqueTargetMachine targetMachine = CreateTargetMachine(*state);
            if (!targetMachine) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            UniqueTargetData targetData{state->api.createTargetDataLayout(targetMachine.get()),
                                        LlvmDeleter<LlvmC::OpaqueTargetData>{state->api.disposeTargetData}};
            if (!targetData) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            state->dataLayout = TakeMessage(state->api, state->api.copyTargetDataLayout(targetData.get()));
            if (state->dataLayout.empty()) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            if (LlvmC::ErrorRef error = state->api.createLlJit(&state->jit, nullptr); error != nullptr) {
                ConsumeError(state->api, error);
                return std::unexpected(CompileError{CompileErrorCode::BackendUnavailable, 0});
            }
            if (state->jit == nullptr || state->api.getLlJitTriple(state->jit) == nullptr ||
                state->triple != state->api.getLlJitTriple(state->jit)) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedTarget, 0});
            }
            const std::array<uint32_t, 7> compilerFields{kLlvmLoweringSchemaVersion,
                                                         kCpuCodeProviderAbiVersion,
                                                         state->llvmMajor,
                                                         state->llvmMinor,
                                                         state->llvmPatch,
                                                         state->functionSanitizer.signature,
                                                         state->functionSanitizer.typeHash};
            state->compilerIdentity = HashIdentity({"Sora.LlvmCpuCodeProvider.Compiler", state->triple, state->cpuName,
                                                    state->cpuFeatures, state->dataLayout, kOptimizationPipeline},
                                                   compilerFields);
            const std::array<uint32_t, 7> driverFields{kCpuCodeProviderAbiVersion,
                                                       state->llvmMajor,
                                                       state->llvmMinor,
                                                       state->llvmPatch,
                                                       static_cast<uint32_t>(kOperatingSystem),
                                                       static_cast<uint32_t>(kBinaryFormat),
                                                       static_cast<uint32_t>(kAbiFamily)};
            state->driverIdentity = HashIdentity({"Sora.LlvmCpuCodeProvider.OrcRuntime", state->triple}, driverFields);
            return state;
        }

    } // namespace

    std::expected<CpuCodeProvider, CompileError> CreateLlvmCpuCodeProvider(LlvmCpuCodeProviderConfig config) noexcept {
        try {
            auto state = InitializeProvider(config);
            if (!state) {
                return std::unexpected(state.error());
            }
            BackendCodeLease code = BackendCodeLease::Retain((*state)->module);
            CpuCodeProviderApiV1 api{.compilerIdentity = (*state)->compilerIdentity,
                                     .driverIdentity = (*state)->driverIdentity,
                                     .state = state->release(),
                                     .destroy = DestroyProvider,
                                     .compile = Compile,
                                     .importArtifact = Import,
                                     .exportArtifact = Export};
            return CpuCodeProvider::Adopt(api, std::move(code));
        } catch (const std::bad_alloc&) {
            return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(CompileError{CompileErrorCode::BackendUnavailable, 0});
        }
    }

} // namespace Sora::Compute
