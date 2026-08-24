#include <Sora/Core/Memory/Allocator.h>

#include <mimalloc.h>

#include <algorithm>
#include <limits>

namespace Sora {

    namespace {

        [[nodiscard]] constexpr size_t NormalizeMimallocAlignment(Align alignment) noexcept {
            if (alignment.Value() > std::numeric_limits<size_t>::max()) [[unlikely]] {
                return 0;
            }
            return std::max(static_cast<size_t>(alignment.Value()), alignof(void*));
        }

    } // namespace

    void* AllocateMemory(size_t bytes, Align alignment) noexcept {
        const size_t normalizedAlignment = NormalizeMimallocAlignment(alignment);
        return normalizedAlignment == 0 ? nullptr : mi_malloc_aligned(bytes, normalizedAlignment);
    }

    void DeallocateMemory(void* allocation) noexcept {
        mi_free(allocation);
    }

    void DeallocateMemory(void* allocation, size_t bytes, Align alignment) noexcept {
        const size_t normalizedAlignment = NormalizeMimallocAlignment(alignment);
        if (normalizedAlignment == 0) [[unlikely]] {
            mi_free(allocation);
            return;
        }
        mi_free_size_aligned(allocation, bytes, normalizedAlignment);
    }

} // namespace Sora
