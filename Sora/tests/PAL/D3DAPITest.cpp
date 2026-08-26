/**
 * @file D3DAPITest.cpp
 * @brief Verify Direct3D ABI declarations, exact export resolution, and process-lifetime module tables.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/D3DAPI.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <type_traits>

namespace D3D = Sora::PAL::D3D;
namespace WindowsSystem = Sora::PAL::WindowsSystem;

#if defined(PLATFORM_WINDOWS)
namespace {

    WindowsSystem::HResult __stdcall StubCreateFactory(WindowsSystem::RefInterfaceId, void**) {
        return 0;
    }

    static_assert(std::is_base_of_v<WindowsSystem::Unknown, D3D::D3D12::ID3D12Object>);
    static_assert(std::is_base_of_v<WindowsSystem::Unknown, D3D::DXGI::IDXGIFactory>);
    static_assert(std::is_base_of_v<WindowsSystem::Unknown, D3D::DXC::IDxcCompiler3>);
    static_assert(std::is_same_v<WindowsSystem::Rect, Sora::PAL::DisplayRect>);
    static_assert(std::is_same_v<WindowsSystem::Point, Sora::PAL::DisplayPoint>);
    static_assert(D3D::D3D12::D3D12_RESOURCE_DIMENSION_BUFFER == 1);
    static_assert(D3D::DXGI::DXGI_FORMAT_R8G8B8A8_UNORM == 28);
    static_assert(sizeof(D3D::D3D12::D3D12_CPU_DESCRIPTOR_HANDLE) == sizeof(uintptr_t));

} // namespace

TEST_CASE("DXGI export table resolves exact DLL symbol spellings", "[Sora.PAL.D3DAPI]") {
    D3D::DXGIAPI api{};
    api.Resolve(
        [](const char* name) noexcept -> void* {
            return std::string_view{name} == "CreateDXGIFactory" ? reinterpret_cast<void*>(&StubCreateFactory)
                                                                 : nullptr;
        },
        [](const char*) noexcept -> void* { return nullptr; });

    REQUIRE(api.createFactory == reinterpret_cast<decltype(api.createFactory)>(&StubCreateFactory));
    REQUIRE(api.createFactory1 == nullptr);
    REQUIRE(api.getDebugInterface1 == nullptr);
}

TEST_CASE("Windows Direct3D export tables expose installed system runtimes", "[Sora.PAL.D3DAPI]") {
    REQUIRE(&D3D::LoadD3D12API() == &D3D::LoadD3D12API());
    REQUIRE(&D3D::LoadDXGIAPI() == &D3D::LoadDXGIAPI());
    REQUIRE(&D3D::LoadDXCoreAPI() == &D3D::LoadDXCoreAPI());
    REQUIRE(&D3D::LoadD3DCompilerAPI() == &D3D::LoadD3DCompilerAPI());
    REQUIRE(&D3D::LoadDXCAPI() == &D3D::LoadDXCAPI());

    REQUIRE(D3D::LoadD3D12API().createDevice != nullptr);
    REQUIRE(D3D::LoadDXGIAPI().createFactory1 != nullptr);
    REQUIRE(D3D::LoadD3DCompilerAPI().compile != nullptr);
}
#else
TEST_CASE("Direct3D PAL remains syntactically available off Windows", "[Sora.PAL.D3DAPI]") {
    REQUIRE(&D3D::LoadD3D12API() == &D3D::LoadD3D12API());
}
#endif
