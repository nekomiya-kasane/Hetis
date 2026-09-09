#include "Sora/Core/PAL/Module.h"
#include "Sora/Core/PAL/SystemAPI.h"
#include "Sora/Platform.h"

#include <bit>
#include <cstdint>
#include <iostream>

int main() {
#if defined(PLATFORM_WINDOWS)
    using GetWindowDeviceContext = Sora::PAL::WindowsSystem::DeviceContextHandle(__stdcall*)(
        Sora::PAL::WindowsSystem::WindowHandle);
    Sora::Result<Sora::PAL::ModulePtr> module = Sora::PAL::LoadModule({"user32"});
    const auto getWindowDeviceContext = module.value()->TryFindFunction<GetWindowDeviceContext>("GetWindowDC");

    std::cout << "GetWindowDC: " << std::hex << std::bit_cast<uint64_t>(getWindowDeviceContext) << '\n';
#else
    std::cout << "GetWindowDC is only available on Windows\n";
#endif
    return 0;
}
