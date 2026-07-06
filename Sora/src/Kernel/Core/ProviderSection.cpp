/**
 * @file ProviderSection.cpp
 * @brief Runtime folding for provider records emitted into the kernel section.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/ProviderSection.h"

#include <mutex>

namespace Sora::Kernel {

    namespace {

#if defined(_WIN32)
        extern "C" {
        __declspec(allocate("kernel$a")) const ProviderSectionBlockHeader* const kSoraKernelSectionBegin = nullptr;
        __declspec(allocate("kernel$z")) const ProviderSectionBlockHeader* const kSoraKernelSectionEnd = nullptr;
        }

        void RegisterLinkedProviderBlocks() {
            const auto* begin = &kSoraKernelSectionBegin + 1;
            const auto* end = &kSoraKernelSectionEnd;
            for (const auto* current = begin; current != end; ++current) {
                RegisterProviderSectionBlock(*current);
            }
        }
#elif defined(__APPLE__)
        void RegisterLinkedProviderBlocks() {}
#else
        extern "C" {
        extern const ProviderSectionBlockHeader* const __start_kernel[] __attribute__((weak));
        extern const ProviderSectionBlockHeader* const __stop_kernel[] __attribute__((weak));
        }

        void RegisterLinkedProviderBlocks() {
            if (!__start_kernel || !__stop_kernel) {
                return;
            }
            for (auto current = __start_kernel; current != __stop_kernel; ++current) {
                RegisterProviderSectionBlock(*current);
            }
        }
#endif

    } // namespace

    void RegisterProviderSectionBlock(const ProviderSectionBlockHeader* header) {
        if (!header || header->magic != 0x4b505253u || header->version != 1 ||
            header->headerSize != sizeof(ProviderSectionBlockHeader)) {
            return;
        }

        const auto* records = reinterpret_cast<const ProviderSectionRecord*>(header + 1);
        for (uint32_t i = 0; i < header->count; ++i) {
            if (records[i].registerProvider) {
                records[i].registerProvider();
            }
        }
    }

    void EnsureProviderSectionsRegistered() {
        static std::once_flag once;
        std::call_once(once, [] { RegisterLinkedProviderBlocks(); });
    }

} // namespace Sora::Kernel
