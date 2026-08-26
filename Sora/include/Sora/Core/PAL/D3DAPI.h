/**
 * @file D3DAPI.h
 * @brief Self-contained Direct3D, DXGI, DXCore, legacy compiler, and DXC ABI declarations and export tables.
 * @ingroup PAL
 * @details Native structures and COM interfaces are generated from a pinned Windows SDK snapshot. COM methods remain
 * virtual interface calls; only actual DLL exports appear in the runtime tables.
 */
#pragma once

#include <Sora/Core/PAL/SystemAPI.h>

#include <type_traits>

#include <Sora/Core/PAL/Detail/D3DNativeAPI.inc>

namespace Sora::PAL::D3D {

#if defined(PLATFORM_WINDOWS)
    template<typename Function>
    [[nodiscard]] Function ResolveD3DExport(auto&& resolver, const char* name) noexcept {
        static_assert(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>);
        return reinterpret_cast<Function>(resolver(name));
    }

    /** @brief Process-wide exports from d3d12.dll; device and resource operations remain COM methods. */
    struct D3D12API {
        decltype(&Detail::D3D12CreateDevice) createDevice = nullptr;
        decltype(&Detail::D3D12GetDebugInterface) getDebugInterface = nullptr;
        decltype(&Detail::D3D12EnableExperimentalFeatures) enableExperimentalFeatures = nullptr;
        decltype(&Detail::D3D12GetInterface) getInterface = nullptr;
        decltype(&Detail::D3D12SerializeRootSignature) serializeRootSignature = nullptr;
        decltype(&Detail::D3D12CreateRootSignatureDeserializer) createRootSignatureDeserializer = nullptr;
        decltype(&Detail::D3D12SerializeVersionedRootSignature) serializeVersionedRootSignature = nullptr;
        decltype(&Detail::D3D12CreateVersionedRootSignatureDeserializer) createVersionedRootSignatureDeserializer =
            nullptr;
        decltype(&Detail::D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary)
            createVersionedRootSignatureDeserializerFromSubobjectInLibrary = nullptr;

        void Resolve(auto&& resolver) noexcept {
            createDevice = ResolveD3DExport<decltype(createDevice)>(resolver, "D3D12CreateDevice");
            getDebugInterface = ResolveD3DExport<decltype(getDebugInterface)>(resolver, "D3D12GetDebugInterface");
            enableExperimentalFeatures =
                ResolveD3DExport<decltype(enableExperimentalFeatures)>(resolver, "D3D12EnableExperimentalFeatures");
            getInterface = ResolveD3DExport<decltype(getInterface)>(resolver, "D3D12GetInterface");
            serializeRootSignature =
                ResolveD3DExport<decltype(serializeRootSignature)>(resolver, "D3D12SerializeRootSignature");
            createRootSignatureDeserializer = ResolveD3DExport<decltype(createRootSignatureDeserializer)>(
                resolver, "D3D12CreateRootSignatureDeserializer");
            serializeVersionedRootSignature = ResolveD3DExport<decltype(serializeVersionedRootSignature)>(
                resolver, "D3D12SerializeVersionedRootSignature");
            createVersionedRootSignatureDeserializer =
                ResolveD3DExport<decltype(createVersionedRootSignatureDeserializer)>(
                    resolver, "D3D12CreateVersionedRootSignatureDeserializer");
            createVersionedRootSignatureDeserializerFromSubobjectInLibrary =
                ResolveD3DExport<decltype(createVersionedRootSignatureDeserializerFromSubobjectInLibrary)>(
                    resolver, "D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary");
        }
    };

    /** @brief Process-wide DXGI factory, diagnostics, and adapter-removal exports. */
    struct DXGIAPI {
        decltype(&Detail::CreateDXGIFactory) createFactory = nullptr;
        decltype(&Detail::CreateDXGIFactory1) createFactory1 = nullptr;
        decltype(&Detail::CreateDXGIFactory2) createFactory2 = nullptr;
        decltype(&Detail::DXGIGetDebugInterface1) getDebugInterface1 = nullptr;
        decltype(&Detail::DXGIDeclareAdapterRemovalSupport) declareAdapterRemovalSupport = nullptr;

        void Resolve(auto&& resolver, auto&& debugResolver) noexcept {
            createFactory = ResolveD3DExport<decltype(createFactory)>(resolver, "CreateDXGIFactory");
            createFactory1 = ResolveD3DExport<decltype(createFactory1)>(resolver, "CreateDXGIFactory1");
            createFactory2 = ResolveD3DExport<decltype(createFactory2)>(resolver, "CreateDXGIFactory2");
            declareAdapterRemovalSupport =
                ResolveD3DExport<decltype(declareAdapterRemovalSupport)>(resolver, "DXGIDeclareAdapterRemovalSupport");
            getDebugInterface1 =
                ResolveD3DExport<decltype(getDebugInterface1)>(debugResolver, "DXGIGetDebugInterface1");
        }
    };

    /** @brief Process-wide DXCore adapter-factory export. */
    struct DXCoreAPI {
        using CreateAdapterFactoryFunction = WindowsSystem::HResult(__stdcall*)(WindowsSystem::RefInterfaceId, void**);
        CreateAdapterFactoryFunction createAdapterFactory = nullptr;

        void Resolve(auto&& resolver) noexcept {
            createAdapterFactory =
                ResolveD3DExport<decltype(createAdapterFactory)>(resolver, "DXCoreCreateAdapterFactory");
        }
    };

    /** @brief Legacy d3dcompiler_47.dll shader compilation, reflection, transformation, and blob exports. */
    struct CompilerAPI {
#    define SORA_D3D_COMPILER_EXPORT(Member, Symbol) decltype(&Detail::Symbol) Member = nullptr;
        SORA_D3D_COMPILER_EXPORT(readFileToBlob, D3DReadFileToBlob)
        SORA_D3D_COMPILER_EXPORT(writeBlobToFile, D3DWriteBlobToFile)
        SORA_D3D_COMPILER_EXPORT(compile, D3DCompile)
        SORA_D3D_COMPILER_EXPORT(compile2, D3DCompile2)
        SORA_D3D_COMPILER_EXPORT(compileFromFile, D3DCompileFromFile)
        SORA_D3D_COMPILER_EXPORT(preprocess, D3DPreprocess)
        SORA_D3D_COMPILER_EXPORT(getDebugInfo, D3DGetDebugInfo)
        SORA_D3D_COMPILER_EXPORT(reflect, D3DReflect)
        SORA_D3D_COMPILER_EXPORT(reflectLibrary, D3DReflectLibrary)
        SORA_D3D_COMPILER_EXPORT(disassemble, D3DDisassemble)
        SORA_D3D_COMPILER_EXPORT(disassembleRegion, D3DDisassembleRegion)
        SORA_D3D_COMPILER_EXPORT(createLinker, D3DCreateLinker)
        SORA_D3D_COMPILER_EXPORT(loadModule, D3DLoadModule)
        SORA_D3D_COMPILER_EXPORT(createFunctionLinkingGraph, D3DCreateFunctionLinkingGraph)
        SORA_D3D_COMPILER_EXPORT(getTraceInstructionOffsets, D3DGetTraceInstructionOffsets)
        SORA_D3D_COMPILER_EXPORT(getInputSignatureBlob, D3DGetInputSignatureBlob)
        SORA_D3D_COMPILER_EXPORT(getOutputSignatureBlob, D3DGetOutputSignatureBlob)
        SORA_D3D_COMPILER_EXPORT(getInputAndOutputSignatureBlob, D3DGetInputAndOutputSignatureBlob)
        SORA_D3D_COMPILER_EXPORT(stripShader, D3DStripShader)
        SORA_D3D_COMPILER_EXPORT(getBlobPart, D3DGetBlobPart)
        SORA_D3D_COMPILER_EXPORT(setBlobPart, D3DSetBlobPart)
        SORA_D3D_COMPILER_EXPORT(createBlob, D3DCreateBlob)
        SORA_D3D_COMPILER_EXPORT(compressShaders, D3DCompressShaders)
        SORA_D3D_COMPILER_EXPORT(decompressShaders, D3DDecompressShaders)
        SORA_D3D_COMPILER_EXPORT(disassemble10Effect, D3DDisassemble10Effect)
#    undef SORA_D3D_COMPILER_EXPORT

        void Resolve(auto&& resolver) noexcept {
#    define SORA_D3D_COMPILER_EXPORT(Member, Symbol) Member = ResolveD3DExport<decltype(Member)>(resolver, #Symbol);
            SORA_D3D_COMPILER_EXPORT(readFileToBlob, D3DReadFileToBlob)
            SORA_D3D_COMPILER_EXPORT(writeBlobToFile, D3DWriteBlobToFile)
            SORA_D3D_COMPILER_EXPORT(compile, D3DCompile)
            SORA_D3D_COMPILER_EXPORT(compile2, D3DCompile2)
            SORA_D3D_COMPILER_EXPORT(compileFromFile, D3DCompileFromFile)
            SORA_D3D_COMPILER_EXPORT(preprocess, D3DPreprocess)
            SORA_D3D_COMPILER_EXPORT(getDebugInfo, D3DGetDebugInfo)
            SORA_D3D_COMPILER_EXPORT(reflect, D3DReflect)
            SORA_D3D_COMPILER_EXPORT(reflectLibrary, D3DReflectLibrary)
            SORA_D3D_COMPILER_EXPORT(disassemble, D3DDisassemble)
            SORA_D3D_COMPILER_EXPORT(disassembleRegion, D3DDisassembleRegion)
            SORA_D3D_COMPILER_EXPORT(createLinker, D3DCreateLinker)
            SORA_D3D_COMPILER_EXPORT(loadModule, D3DLoadModule)
            SORA_D3D_COMPILER_EXPORT(createFunctionLinkingGraph, D3DCreateFunctionLinkingGraph)
            SORA_D3D_COMPILER_EXPORT(getTraceInstructionOffsets, D3DGetTraceInstructionOffsets)
            SORA_D3D_COMPILER_EXPORT(getInputSignatureBlob, D3DGetInputSignatureBlob)
            SORA_D3D_COMPILER_EXPORT(getOutputSignatureBlob, D3DGetOutputSignatureBlob)
            SORA_D3D_COMPILER_EXPORT(getInputAndOutputSignatureBlob, D3DGetInputAndOutputSignatureBlob)
            SORA_D3D_COMPILER_EXPORT(stripShader, D3DStripShader)
            SORA_D3D_COMPILER_EXPORT(getBlobPart, D3DGetBlobPart)
            SORA_D3D_COMPILER_EXPORT(setBlobPart, D3DSetBlobPart)
            SORA_D3D_COMPILER_EXPORT(createBlob, D3DCreateBlob)
            SORA_D3D_COMPILER_EXPORT(compressShaders, D3DCompressShaders)
            SORA_D3D_COMPILER_EXPORT(decompressShaders, D3DDecompressShaders)
            SORA_D3D_COMPILER_EXPORT(disassemble10Effect, D3DDisassemble10Effect)
#    undef SORA_D3D_COMPILER_EXPORT
        }
    };

    /** @brief Process-wide dxcompiler.dll component-factory exports; compilation is performed through DXC COM APIs. */
    struct DXCAPI {
        decltype(&Detail::DxcCreateInstance) createInstance = nullptr;
        decltype(&Detail::DxcCreateInstance2) createInstance2 = nullptr;

        void Resolve(auto&& resolver) noexcept {
            createInstance = ResolveD3DExport<decltype(createInstance)>(resolver, "DxcCreateInstance");
            createInstance2 = ResolveD3DExport<decltype(createInstance2)>(resolver, "DxcCreateInstance2");
        }
    };
#else
    struct D3D12API {};
    struct DXGIAPI {};
    struct DXCoreAPI {};
    struct CompilerAPI {};
    struct DXCAPI {};
#endif

    [[nodiscard]] const D3D12API& LoadD3D12API() noexcept;
    [[nodiscard]] const DXGIAPI& LoadDXGIAPI() noexcept;
    [[nodiscard]] const DXCoreAPI& LoadDXCoreAPI() noexcept;
    [[nodiscard]] const CompilerAPI& LoadD3DCompilerAPI() noexcept;
    [[nodiscard]] const DXCAPI& LoadDXCAPI() noexcept;

} // namespace Sora::PAL::D3D
