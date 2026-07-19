/**
 * @file DirectBuffer.cpp
 * @brief Implement ownership of runtime-aligned byte storage.
 * @ingroup Core
 */

#include <Sora/Core/Memory/DirectBuffer.h>

#include <algorithm>
#include <new>
#include <utility>

namespace Sora {

    namespace {

        inline constexpr Align kDefaultNewAlignment = Align::Constant<__STDCPP_DEFAULT_NEW_ALIGNMENT__>();

    } // namespace

    DirectBuffer::~DirectBuffer() {
        Reset();
    }

    DirectBuffer::DirectBuffer(DirectBuffer&& other) noexcept
        : data_{std::exchange(other.data_, nullptr)},
          size_{std::exchange(other.size_, 0)},
          alignment_{std::exchange(other.alignment_, Align{})} {}

    DirectBuffer& DirectBuffer::operator=(DirectBuffer&& other) noexcept {
        if (this != &other) {
            Reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            alignment_ = std::exchange(other.alignment_, Align{});
        }
        return *this;
    }

    Result<DirectBuffer> DirectBuffer::Allocate(size_t size, Align alignment) noexcept {
        const Align actualAlignment = std::max(alignment, kDefaultNewAlignment);
        if (size == 0) {
            return DirectBuffer{nullptr, 0, actualAlignment};
        }

        try {
            std::byte* data = static_cast<std::byte*>(
                actualAlignment == kDefaultNewAlignment
                    ? ::operator new(size)
                    : ::operator new(size, std::align_val_t{static_cast<size_t>(actualAlignment.Value())}));
            return DirectBuffer{data, size, actualAlignment};
        } catch (const std::bad_alloc&) {
            return std::unexpected{ErrorCode::OutOfMemory};
        }
    }

    Result<DirectBuffer> DirectBuffer::Allocate(size_t size, size_t alignment) noexcept {
        if (!IsValidAlignment(alignment)) {
            return std::unexpected{ErrorCode::InvalidArgument};
        }
        return Allocate(size, Align{alignment});
    }

    void DirectBuffer::Reset() noexcept {
        if (data_ != nullptr) {
            if (alignment_ == kDefaultNewAlignment) {
                ::operator delete(data_);
            } else {
                ::operator delete(data_, std::align_val_t{static_cast<size_t>(alignment_.Value())});
            }
        }
        data_ = nullptr;
        size_ = 0;
        alignment_ = Align{};
    }

    void DirectBuffer::Swap(DirectBuffer& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(alignment_, other.alignment_);
    }

} // namespace Sora
