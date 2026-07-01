#include "Sora/Core/ABI.h"

#include <iostream>

int func(int a, double b) {
    return static_cast<int>(a + b);
}

int main() {
    // Example usage of the ABI demangling functions
    std::string mangledName = "_Z3fooi"; // Example mangled name
    auto demangled = Sora::ABI::TryDemangle(mangledName);
    if (demangled) {
        std::cout << "Demangled name: " << *demangled << std::endl;
    } else {
        std::cout << "Failed to demangle: " << mangledName << std::endl;
    }

    std::string_view mangledFuncMSVC = Sora::ABI::Mangle<Sora::ABI::Kind::MSVC>(^^func);
    std::cout << "Mangled function name (MSVC): " << mangledFuncMSVC << std::endl;
    std::string_view mangledFuncItanium = Sora::ABI::Mangle<Sora::ABI::Kind::Itanium>(^^func);
    std::cout << "Mangled function name (Itanium): " << mangledFuncItanium << std::endl;

    return 0;
}