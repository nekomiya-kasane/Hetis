/**
 * @file D3DAPI.h
 * @brief Self-contained Direct3D, DXGI, DXCore, legacy compiler, and DXC ABI declarations and export tables.
 * @ingroup PAL
 * @details Native structures and COM interfaces are generated from a pinned Windows SDK snapshot. COM methods remain
 * virtual interface calls; only actual DLL exports appear in the runtime tables.
 *
 * @par Example
 * @code{.cpp}
 * const auto& d3d12 = Sora::PAL::D3D::LoadD3D12API();
 * if (d3d12.createDevice != nullptr) {
 *     // Resolve the DLL entry point first; the returned device is then used through its COM vtable.
 *     Sora::PAL::WindowsSystem::Handle adapter{};
 *     Sora::PAL::WindowsSystem::Guid iid{};
 *     void* device = nullptr;
 *     auto hr = d3d12.createDevice(adapter, 0, iid, &device);
 * }
 * @endcode
 *
 * @note The example is schematic: @p adapter, @p featureLevel, and @p device are application-owned native values.
 * Every function-table member is optional and must be checked before invocation.
 */
#pragma once

#include <Sora/Core/PAL/SystemAPI.h>

#include <type_traits>

#include <Sora/Core/PAL/Detail/D3DNativeAPI.inc>

namespace Sora::PAL::D3D {

#if defined(PLATFORM_WINDOWS)
    /**
     * @brief Resolve one exported Direct3D function through a typed symbol resolver.
     * @tparam Function Function-pointer type to reconstruct from the native address.
     * @param[in] resolver Callable receiving an ASCII export name and returning its address.
     * @param[in] name Exact DLL export name.
     * @return Typed function pointer, or null when the resolver cannot find @p name.
     */
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

        /**
         * @brief Resolve all D3D12 process-wide exports represented by this table.
         * @param[in] resolver Callable receiving an export name and returning its address.
         */
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

        /**
         * @brief Resolve DXGI exports from the normal and debug DLLs.
         * @param[in] resolver Resolver for dxgi.dll.
         * @param[in] debugResolver Resolver for dxgidebug.dll.
         */
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

        /**
         * @brief Resolve the DXCore adapter-factory export.
         * @param[in] resolver Callable receiving an export name and returning its address.
         */
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

        /**
         * @brief Resolve all legacy compiler exports represented by this table.
         * @param[in] resolver Callable receiving an export name and returning its address.
         */
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

        /**
         * @brief Resolve the DXC component-factory exports.
         * @param[in] resolver Callable receiving an export name and returning its address.
         */
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

    /** @brief Return the immutable process-wide D3D12 export table. */
    [[nodiscard]] const D3D12API& LoadD3D12API() noexcept;
    /** @brief Return the immutable process-wide DXGI export table. */
    [[nodiscard]] const DXGIAPI& LoadDXGIAPI() noexcept;
    /** @brief Return the immutable process-wide DXCore export table. */
    [[nodiscard]] const DXCoreAPI& LoadDXCoreAPI() noexcept;
    /** @brief Return the immutable legacy d3dcompiler export table. */
    [[nodiscard]] const CompilerAPI& LoadD3DCompilerAPI() noexcept;
    /** @brief Return the immutable dxcompiler export table. */
    [[nodiscard]] const DXCAPI& LoadDXCAPI() noexcept;

} // namespace Sora::PAL::D3D
