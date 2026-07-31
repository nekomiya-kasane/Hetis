/**
 * @file LlvmCApi.h
 * @brief Private LLVM-C 22 ABI subset used by the dynamically loaded CPU code provider.
 * @ingroup Compute
 *
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Sora::Compute::LlvmC {

    using Bool = int;
    struct OpaqueContext;
    struct OpaqueModule;
    struct OpaqueMemoryBuffer;
    struct OpaqueError;
    struct OpaqueTarget;
    struct OpaqueTargetMachine;
    struct OpaqueTargetData;
    struct OpaquePassBuilderOptions;
    struct OpaqueOrcLLJIT;
    struct OpaqueOrcExecutionSession;
    struct OpaqueOrcJITDylib;
    struct OpaqueOrcResourceTracker;
    struct OpaqueOrcDefinitionGenerator;
    struct OpaqueOrcSymbolStringPoolEntry;

    using ContextRef = OpaqueContext*;
    using ModuleRef = OpaqueModule*;
    using MemoryBufferRef = OpaqueMemoryBuffer*;
    using ErrorRef = OpaqueError*;
    using TargetRef = OpaqueTarget*;
    using TargetMachineRef = OpaqueTargetMachine*;
    using TargetDataRef = OpaqueTargetData*;
    using PassBuilderOptionsRef = OpaquePassBuilderOptions*;
    using OrcLLJITRef = OpaqueOrcLLJIT*;
    using OrcExecutionSessionRef = OpaqueOrcExecutionSession*;
    using OrcJITDylibRef = OpaqueOrcJITDylib*;
    using OrcResourceTrackerRef = OpaqueOrcResourceTracker*;
    using OrcDefinitionGeneratorRef = OpaqueOrcDefinitionGenerator*;
    using OrcSymbolStringPoolEntryRef = OpaqueOrcSymbolStringPoolEntry*;
    using OrcExecutorAddress = uint64_t;

    enum class CodeGenOptLevel : int {
        None,
        Less,
        Default,
        Aggressive,
    };

    enum class RelocMode : int {
        Default,
        Static,
        Pic,
        DynamicNoPic,
        Ropi,
        Rwpi,
        RopiRwpi,
    };

    enum class CodeModel : int {
        Default,
        JitDefault,
        Tiny,
        Small,
        Kernel,
        Medium,
        Large,
    };

    enum class CodeGenFileType : int {
        Assembly,
        Object,
    };

    enum class OrcLookupKind : int {
        Static,
        DynamicSymbol,
    };

    enum class OrcJITDylibLookupFlags : int {
        MatchExportedSymbolsOnly,
        MatchAllSymbols,
    };

    enum class OrcSymbolLookupFlags : int {
        RequiredSymbol,
        WeaklyReferencedSymbol,
    };

    struct JITSymbolFlags {
        uint8_t genericFlags = 0;
        uint8_t targetFlags = 0;
    };

    struct JITEvaluatedSymbol {
        OrcExecutorAddress address = 0;
        JITSymbolFlags flags{};
    };

    struct OrcSymbolMapPair {
        OrcSymbolStringPoolEntryRef name = nullptr;
        JITEvaluatedSymbol symbol{};
    };

    struct OrcJITDylibSearchOrderElement {
        OrcJITDylibRef dylib = nullptr;
        OrcJITDylibLookupFlags flags = OrcJITDylibLookupFlags::MatchExportedSymbolsOnly;
    };

    struct OrcLookupSetElement {
        OrcSymbolStringPoolEntryRef name = nullptr;
        OrcSymbolLookupFlags flags = OrcSymbolLookupFlags::RequiredSymbol;
    };

    using OrcLookupHandler = void (*)(ErrorRef error, OrcSymbolMapPair* result, size_t count, void* context);
    using OrcSymbolPredicate = int (*)(void* context, OrcSymbolStringPoolEntryRef symbol);

    struct Api {
        void (*getVersion)(unsigned*, unsigned*, unsigned*) = nullptr;
        ContextRef (*contextCreate)() = nullptr;
        void (*contextDispose)(ContextRef) = nullptr;
        MemoryBufferRef (*createMemoryBufferCopy)(const char*, size_t, const char*) = nullptr;
        const char* (*getBufferStart)(MemoryBufferRef) = nullptr;
        size_t (*getBufferSize)(MemoryBufferRef) = nullptr;
        void (*disposeMemoryBuffer)(MemoryBufferRef) = nullptr;
        void (*disposeMessage)(char*) = nullptr;
        void (*disposeModule)(ModuleRef) = nullptr;
        void (*setTarget)(ModuleRef, const char*) = nullptr;
        void (*setDataLayout)(ModuleRef, const char*) = nullptr;
        Bool (*parseIrInContext)(ContextRef, MemoryBufferRef, ModuleRef*, char**) = nullptr;

        void (*initializeTargetInfo)() = nullptr;
        void (*initializeTarget)() = nullptr;
        void (*initializeTargetMc)() = nullptr;
        void (*initializeAsmPrinter)() = nullptr;
        char* (*getDefaultTargetTriple)() = nullptr;
        char* (*getHostCpuName)() = nullptr;
        char* (*getHostCpuFeatures)() = nullptr;
        Bool (*getTargetFromTriple)(const char*, TargetRef*, char**) = nullptr;
        TargetMachineRef (*createTargetMachine)(TargetRef, const char*, const char*, const char*, CodeGenOptLevel,
                                                RelocMode, CodeModel) = nullptr;
        void (*disposeTargetMachine)(TargetMachineRef) = nullptr;
        TargetDataRef (*createTargetDataLayout)(TargetMachineRef) = nullptr;
        void (*disposeTargetData)(TargetDataRef) = nullptr;
        char* (*copyTargetDataLayout)(TargetDataRef) = nullptr;
        Bool (*emitToMemoryBuffer)(TargetMachineRef, ModuleRef, CodeGenFileType, char**, MemoryBufferRef*) = nullptr;

        PassBuilderOptionsRef (*createPassBuilderOptions)() = nullptr;
        void (*disposePassBuilderOptions)(PassBuilderOptionsRef) = nullptr;
        ErrorRef (*runPasses)(ModuleRef, const char*, TargetMachineRef, PassBuilderOptionsRef) = nullptr;
        char* (*getErrorMessage)(ErrorRef) = nullptr;
        void (*disposeErrorMessage)(char*) = nullptr;

        ErrorRef (*createLlJit)(OrcLLJITRef*, void*) = nullptr;
        ErrorRef (*disposeLlJit)(OrcLLJITRef) = nullptr;
        OrcExecutionSessionRef (*getExecutionSession)(OrcLLJITRef) = nullptr;
        const char* (*getLlJitTriple)(OrcLLJITRef) = nullptr;
        char (*getGlobalPrefix)(OrcLLJITRef) = nullptr;
        OrcJITDylibRef (*createBareJITDylib)(OrcExecutionSessionRef, const char*) = nullptr;
        ErrorRef (*createProcessGenerator)(OrcDefinitionGeneratorRef*, char, OrcSymbolPredicate, void*) = nullptr;
        void (*addGenerator)(OrcJITDylibRef, OrcDefinitionGeneratorRef) = nullptr;
        OrcResourceTrackerRef (*createResourceTracker)(OrcJITDylibRef) = nullptr;
        ErrorRef (*removeResourceTracker)(OrcResourceTrackerRef) = nullptr;
        void (*releaseResourceTracker)(OrcResourceTrackerRef) = nullptr;
        ErrorRef (*addObjectFile)(OrcLLJITRef, OrcResourceTrackerRef, MemoryBufferRef) = nullptr;
        OrcSymbolStringPoolEntryRef (*mangleAndIntern)(OrcLLJITRef, const char*) = nullptr;
        void (*lookup)(OrcExecutionSessionRef, OrcLookupKind, OrcJITDylibSearchOrderElement*, size_t,
                       OrcLookupSetElement*, size_t, OrcLookupHandler, void*) = nullptr;
    };

    static_assert(sizeof(JITSymbolFlags) == 2);

} // namespace Sora::Compute::LlvmC
