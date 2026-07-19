/**
 * @file ABI.cpp
 * @brief Runtime name demangling through lazily loaded ABI demangler backends.
 * @ingroup Core
 *
 * @details Implements @ref Sora::Meta::ABI::TryDemangle and @ref Sora::Meta::ABI::Demangle. Compile-time mangling is
 * header-only @c consteval code; this translation unit contains the runtime inverse for symbol strings that may come
 * from a different ABI than the host compiler.
 *
 * Runtime loading is delegated to PAL system API tables and @ref Sora::PAL::LoadModule. This keeps platform loader
 * mechanics in PAL, while this file only owns ABI recognition and backend symbol use.
 */

#include "Sora/Core/ABI.h"

#include "Sora/Core/PAL/Module.h"
#include "Sora/Core/PAL/SystemAPI.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
// #include <cxxabi.h> // todo

namespace Sora::Meta::ABI {

    namespace {

        /** @brief Signature of the Itanium ABI demangler exported by libc++abi/libstdc++ runtimes. */
        using CxaDemangleFn = char*(const char*, char*, size_t*, int*);

        /** @brief A loaded module kept alive with a resolved function pointer. */
        template<typename Fn>
        struct LoadedFunction {
            PAL::ModulePtr module;
            Fn* function = nullptr;
        };

        /** @brief Load the first module containing @p symbol. */
        template<typename Fn>
        [[nodiscard]] LoadedFunction<Fn> LoadFunction(std::initializer_list<std::string_view> modules,
                                                      std::string_view symbol) noexcept {
            try {
                for (std::string_view moduleName : modules) {
                    auto module = PAL::LoadModule({moduleName});
                    if (!module) {
                        continue;
                    }
                    if (auto* function = (*module)->template TryFindFunction<Fn>(symbol)) {
                        return {.module = std::move(*module), .function = function};
                    }
                }
            } catch (...) {
                return {};
            }
            return {};
        }

        /** @brief Lazily resolve @c __cxa_demangle from C++ ABI runtimes. */
        [[nodiscard]] CxaDemangleFn* LoadCxaDemangle() noexcept {
            static LoadedFunction<CxaDemangleFn> loaded = LoadFunction<CxaDemangleFn>(
                {"libc++abi.so.1", "libc++abi.so", "libstdc++.so.6", "libstdc++.so", "libc++abi.dll", "cxxabi.dll",
                 "libc++.dll", "c++.dll", "c++abi", "stdc++", "c++"},
                "__cxa_demangle");
            return loaded.function;
        }

        /** @brief Try the lazily available Itanium ABI demangler. */
        [[nodiscard]] std::optional<std::string> TryDemangleItanium(std::string_view mangled) noexcept {
            try {
                if (mangled.empty()) {
                    return std::nullopt;
                }
                CxaDemangleFn* demangle = LoadCxaDemangle();
                if (demangle == nullptr) {
                    return std::nullopt;
                }

                std::string input{mangled.starts_with("__Z") ? mangled.substr(1) : mangled};
                int status = 0;
                std::unique_ptr<char, decltype(&std::free)> demangled{
                    demangle(input.c_str(), nullptr, nullptr, &status), &std::free};
                if (status != 0 || demangled == nullptr) {
                    return std::nullopt;
                }
                return std::string{demangled.get()};
            } catch (...) {
                return std::nullopt;
            }
        }

#if defined(PLATFORM_WINDOWS)
        /** @brief Try the lazily available MSVC decorated-name demangler. */
        [[nodiscard]] std::optional<std::string> TryUndecorateMsvc(std::string_view mangled) noexcept {
            if (mangled.empty()) {
                return std::nullopt;
            }
            try {
                PAL::LockedDbgHelpSystemAPI dbgHelp = PAL::LockDbgHelpSystemAPI();
                const PAL::DbgHelpSystemAPI::UndecorateSymbolNameFunction undecorate = dbgHelp->undecorateSymbolName;
                if (undecorate == nullptr) {
                    return std::nullopt;
                }

                std::string_view sym = mangled;
                if (sym.front() == '.') {
                    sym.remove_prefix(1);
                }
                if (sym.empty() || sym.front() != '?') {
                    return std::nullopt;
                }

                std::string input{sym};
                char output[1024];
                unsigned long n = undecorate(input.c_str(), output, static_cast<unsigned long>(sizeof(output)), 0);
                if (n == 0) {
                    return std::nullopt;
                }
                return std::string(output, n);
            } catch (...) {
                return std::nullopt;
            }
        }
#else
        /** @brief No MSVC decorated-name demangler is available on this target. */
        [[nodiscard]] std::optional<std::string> TryUndecorateMsvc(std::string_view) noexcept {
            return std::nullopt;
        }
#endif

        /** @brief Try demanglers in the order implied by the symbol spelling, then use fallback probing. */
        [[nodiscard]] std::optional<std::string> Undecorate(std::string_view mangled) noexcept {
            // MSVC decorated names
            if (mangled.starts_with("?") || mangled.starts_with(".?")) {
                if (auto result = TryUndecorateMsvc(mangled)) {
                    return result;
                }
                return TryDemangleItanium(mangled);
            }
            // Itanium external names
            if (auto result = TryDemangleItanium(mangled)) {
                return result;
            }
            return TryUndecorateMsvc(mangled);
        }

    } // namespace

    std::optional<std::string> TryDemangle(std::string_view mangled) noexcept {
        return Undecorate(mangled);
    }

    std::string Demangle(std::string_view mangled) {
        if (auto r = Undecorate(mangled)) {
            return *r;
        }
        return std::string{mangled};
    }

} // namespace Sora::Meta::ABI
