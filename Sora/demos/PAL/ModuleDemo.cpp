#include "Sora/Core/PAL/Module.h"

#include <Windows.h>

#include <iostream>

using namespace Sora;
using namespace Sora::PAL;

int main() {
    Result<ModulePtr> module = LoadModule({"user32"});
    auto win = module.value()->TryFindFunction<decltype(GetWindowDC)>("GetWindowDC");

    std::cout << "GetWindowDC: " << std::hex << std::bit_cast<uint64_t>(win) << std::endl;
    return 0;
}
