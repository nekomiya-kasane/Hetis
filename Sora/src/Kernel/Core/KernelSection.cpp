/**
 * @file KernelSection.cpp
 * @brief Runtime folding for object-model class records emitted into the kernel section.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/KernelSection.h"

#include <mutex>

namespace Sora::Kernel {

    namespace {

#if defined(_WIN32)
        extern "C" {
        __declspec(allocate("kernel$a")) const KernelSectionBlockHeader* const kSoraKernelSectionBegin = nullptr;
        __declspec(allocate("kernel$z")) const KernelSectionBlockHeader* const kSoraKernelSectionEnd = nullptr;
        }

        void RegisterLinkedKernelBlocks() {
            const auto* begin = &kSoraKernelSectionBegin + 1;
            const auto* end = &kSoraKernelSectionEnd;
            for (const auto* current = begin; current != end; ++current) {
                RegisterKernelSectionBlock(*current);
            }
        }
#elif defined(__APPLE__)
        void RegisterLinkedKernelBlocks() {}
#else
        extern "C" {
        extern const KernelSectionBlockHeader* const __start_kernel[] __attribute__((weak));
        extern const KernelSectionBlockHeader* const __stop_kernel[] __attribute__((weak));
        }

        void RegisterLinkedKernelBlocks() {
            if (!__start_kernel || !__stop_kernel) {
                return;
            }
            for (auto current = __start_kernel; current != __stop_kernel; ++current) {
                RegisterKernelSectionBlock(*current);
            }
        }
#endif

    } // namespace

    void RegisterKernelSectionBlock(const KernelSectionBlockHeader* header) {
        if (!header || header->magic != 0x4b534f53u || header->version != 1 ||
            header->headerSize != sizeof(KernelSectionBlockHeader)) {
            return;
        }

        const auto* records = reinterpret_cast<const KernelSectionRecord*>(header + 1);
        for (uint32_t i = 0; i < header->count; ++i) {
            if (records[i].registerClass) {
                records[i].registerClass();
            }
        }
    }

    void EnsureKernelSectionsRegistered() {
        static std::once_flag once;
        std::call_once(once, [] { RegisterLinkedKernelBlocks(); });
    }

} // namespace Sora::Kernel
