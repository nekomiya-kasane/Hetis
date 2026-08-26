/**
 * @file D3DPALABIProbe.cpp
 * @brief Export generated PAL layout probes without co-declaring native C-linkage SDK symbols.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/D3DAPI.h>
#include <Sora/Platform.h>

#include <cstddef>

namespace D3D = Sora::PAL::D3D;

#if defined(PLATFORM_WINDOWS)
static_assert(std::is_base_of_v<D3D::D3D12::ID3D12Device, D3D::D3D12::ID3D12Device14>);
static_assert(std::is_base_of_v<D3D::DXGI::IDXGIFactory6, D3D::DXGI::IDXGIFactory7>);

extern "C" size_t SoraD3DResourceDescSize() {
    return sizeof(D3D::D3D12::D3D12_RESOURCE_DESC);
}
extern "C" size_t SoraD3DResourceDescAlignment() {
    return alignof(D3D::D3D12::D3D12_RESOURCE_DESC);
}
extern "C" size_t SoraD3DGraphicsPipelineDescSize() {
    return sizeof(D3D::D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC);
}
extern "C" size_t SoraD3DGraphicsPipelineDescAlignment() {
    return alignof(D3D::D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC);
}
extern "C" size_t SoraDXGISwapChainDescSize() {
    return sizeof(D3D::DXGI::DXGI_SWAP_CHAIN_DESC1);
}
extern "C" size_t SoraDXGISwapChainDescAlignment() {
    return alignof(D3D::DXGI::DXGI_SWAP_CHAIN_DESC1);
}
extern "C" size_t SoraDXGIAdapterDescSize() {
    return sizeof(D3D::DXGI::DXGI_ADAPTER_DESC3);
}
extern "C" size_t SoraDXCBufferSize() {
    return sizeof(D3D::DXC::DxcBuffer);
}
extern "C" size_t SoraDXCBufferAlignment() {
    return alignof(D3D::DXC::DxcBuffer);
}
#endif
