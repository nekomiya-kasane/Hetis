/**
 * @file D3DNativeABITest.cpp
 * @brief Compare generated Direct3D declarations with the active Windows SDK ABI.
 * @ingroup Testing
 */

#include <Sora/Platform.h>

#if defined(PLATFORM_WINDOWS)
#    include <Windows.h>
#    include <d3d12.h>
#    include <dxcapi.h>
#    include <dxcore.h>
#    include <dxgi1_6.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#if defined(PLATFORM_WINDOWS)
extern "C" size_t SoraD3DResourceDescSize();
extern "C" size_t SoraD3DResourceDescAlignment();
extern "C" size_t SoraD3DGraphicsPipelineDescSize();
extern "C" size_t SoraD3DGraphicsPipelineDescAlignment();
extern "C" size_t SoraDXGISwapChainDescSize();
extern "C" size_t SoraDXGISwapChainDescAlignment();
extern "C" size_t SoraDXGIAdapterDescSize();
extern "C" size_t SoraDXCBufferSize();
extern "C" size_t SoraDXCBufferAlignment();
#endif

TEST_CASE("Direct3D PAL structures match the active native SDK ABI", "[Sora.PAL.D3DAPI]") {
#if defined(PLATFORM_WINDOWS)
    REQUIRE(SoraD3DResourceDescSize() == sizeof(::D3D12_RESOURCE_DESC));
    REQUIRE(SoraD3DResourceDescAlignment() == alignof(::D3D12_RESOURCE_DESC));
    REQUIRE(SoraD3DGraphicsPipelineDescSize() == sizeof(::D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    REQUIRE(SoraD3DGraphicsPipelineDescAlignment() == alignof(::D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    REQUIRE(SoraDXGISwapChainDescSize() == sizeof(::DXGI_SWAP_CHAIN_DESC1));
    REQUIRE(SoraDXGISwapChainDescAlignment() == alignof(::DXGI_SWAP_CHAIN_DESC1));
    REQUIRE(SoraDXGIAdapterDescSize() == sizeof(::DXGI_ADAPTER_DESC3));
    REQUIRE(SoraDXCBufferSize() == sizeof(::DxcBuffer));
    REQUIRE(SoraDXCBufferAlignment() == alignof(::DxcBuffer));
#else
    SUCCEED();
#endif
}
