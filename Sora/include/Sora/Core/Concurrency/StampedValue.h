/**
 * @file StampedValue.h
 * @brief Canonical single-word tagged and versioned values.
 * @ingroup Core
 */
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Concurrency {

    namespace Concept {

        /** @brief Codec contract for one payload and a disjoint stamp encoded in an unsigned atomic word. */
        template<typename Codec>
        concept StampedWordCodec = std::unsigned_integral<typename Codec::Word> &&
                                   requires(typename Codec::Value value, typename Codec::Word word) {
                                       { Codec::Encode(value) } noexcept -> std::same_as<typename Codec::Word>;
                                       { Codec::Decode(word) } noexcept -> std::same_as<typename Codec::Value>;
                                       { Codec::PayloadMask } -> std::convertible_to<typename Codec::Word>;
                                       { Codec::StampMask } -> std::convertible_to<typename Codec::Word>;
                                       { Codec::StampShift } -> std::convertible_to<std::size_t>;
                                   };

        /** @brief Canonical unsigned-word value accepted by atomic stamped publication. */
        template<typename Stamped>
        concept StampedWord = requires(Stamped value, typename Stamped::Word word) {
            requires std::unsigned_integral<typename Stamped::Word>;
            { value.Representation() } noexcept -> std::same_as<typename Stamped::Word>;
            { Stamped::FromRepresentation(word) } noexcept -> std::same_as<Stamped>;
        };

    } // namespace Concept

    namespace Detail {

        struct FromRepresentationTag {};

        /** @brief Encode an aligned pointer as payload bits and use its zero low bits for a stamp. */
        template<typename T, std::size_t StampBits>
        struct PointerLowBitsCodec {
            using Value = T*;
            using Word = std::uintptr_t;

            static_assert(StampBits > 0 && StampBits < std::numeric_limits<Word>::digits);
            static_assert(alignof(T) >= (Word{1} << StampBits),
                          "the pointed-to type does not provide enough low stamp bits");

            static constexpr Word StampMask = (Word{1} << StampBits) - 1;
            static constexpr Word PayloadMask = ~StampMask;
            static constexpr std::size_t StampShift = 0;

            [[nodiscard]] static Word Encode(Value value) noexcept {
                const Word word = reinterpret_cast<Word>(value);
                assert((word & StampMask) == 0 && "pointer does not satisfy its declared alignment");
                return word;
            }
            [[nodiscard]] static Value Decode(Word word) noexcept {
                return reinterpret_cast<Value>(word & PayloadMask);
            }
        };

        /** @brief Encode a bounded unsigned payload and a stamp in disjoint fields of one unsigned word. */
        template<std::unsigned_integral UInt, std::size_t PayloadBits, std::size_t StampBits>
        struct BoundedUnsignedCodec {
            using Value = UInt;
            using Word = UInt;

            static_assert(PayloadBits > 0 && StampBits > 0);
            static_assert(PayloadBits + StampBits <= std::numeric_limits<Word>::digits);

            static constexpr Word PayloadMask = (Word{1} << PayloadBits) - 1;
            static constexpr Word StampMask = ((Word{1} << StampBits) - 1) << PayloadBits;
            static constexpr std::size_t StampShift = PayloadBits;

            [[nodiscard]] static constexpr Word Encode(Value value) noexcept {
                assert((value & ~PayloadMask) == 0 && "unsigned payload exceeds its declared bit width");
                return value;
            }
            [[nodiscard]] static constexpr Value Decode(Word word) noexcept { return word & PayloadMask; }
        };

        template<Concept::StampedWordCodec Codec>
        class StampedValueStorage {
        public:
            using ValueType = typename Codec::Value;
            using Word = typename Codec::Word;

            static constexpr Word RepresentationMask = Codec::PayloadMask | Codec::StampMask;
            static_assert((Codec::PayloadMask & Codec::StampMask) == 0, "payload and stamp fields must not overlap");

            constexpr StampedValueStorage() noexcept = default;
            explicit constexpr StampedValueStorage(ValueType value, Word stamp = 0) noexcept
                : word_(Pack(value, stamp)) {}

            [[nodiscard]] constexpr ValueType Value() const noexcept { return Codec::Decode(word_); }
            [[nodiscard]] constexpr Word Stamp() const noexcept {
                return (word_ & Codec::StampMask) >> Codec::StampShift;
            }
            [[nodiscard]] constexpr Word Representation() const noexcept { return word_; }
            [[nodiscard]] static constexpr Word MaxStamp() noexcept { return Codec::StampMask >> Codec::StampShift; }

        protected:
            explicit constexpr StampedValueStorage(FromRepresentationTag, Word word) noexcept
                : word_(word & RepresentationMask) {}
            [[nodiscard]] static constexpr Word Pack(ValueType value, Word stamp) noexcept {
                assert(stamp <= MaxStamp() && "stamp exceeds its declared bit width");
                return (Codec::Encode(value) & Codec::PayloadMask) | ((stamp & MaxStamp()) << Codec::StampShift);
            }

        private:
            Word word_ = 0;
        };

    } // namespace Detail

    /** @brief A value coupled to an application-controlled tag in one canonical unsigned word. */
    template<Concept::StampedWordCodec Codec>
    class TaggedValue : private Detail::StampedValueStorage<Codec> {
        using Base = Detail::StampedValueStorage<Codec>;

    public:
        using Base::Base;
        using Base::Representation;
        using Base::Value;
        using typename Base::ValueType;
        using typename Base::Word;
        constexpr TaggedValue() noexcept = default;

        [[nodiscard]] constexpr Word Tag() const noexcept { return Base::Stamp(); }
        [[nodiscard]] static constexpr Word MaxTag() noexcept { return Base::MaxStamp(); }
        [[nodiscard]] constexpr TaggedValue WithTag(Word tag) const noexcept {
            return FromRepresentation(Base::Pack(Base::Value(), tag));
        }
        [[nodiscard]] static constexpr TaggedValue FromRepresentation(Word word) noexcept {
            return TaggedValue(Detail::FromRepresentationTag{}, word);
        }
        friend constexpr bool operator==(TaggedValue left, TaggedValue right) noexcept {
            return left.Representation() == right.Representation();
        }

    private:
        explicit constexpr TaggedValue(Detail::FromRepresentationTag tag, Word word) noexcept : Base(tag, word) {}
    };

    /** @brief A value whose publication identity advances modulo the encoded version width. */
    template<Concept::StampedWordCodec Codec>
    class VersionedValue : private Detail::StampedValueStorage<Codec> {
        using Base = Detail::StampedValueStorage<Codec>;

    public:
        using Base::Base;
        using Base::Representation;
        using Base::Value;
        using typename Base::ValueType;
        using typename Base::Word;
        constexpr VersionedValue() noexcept = default;

        [[nodiscard]] constexpr Word Version() const noexcept { return Base::Stamp(); }
        [[nodiscard]] static constexpr Word MaxVersion() noexcept { return Base::MaxStamp(); }
        [[nodiscard]] constexpr VersionedValue NextVersion(ValueType value) const noexcept {
            return FromRepresentation(Base::Pack(value, (Version() + 1) & Base::MaxStamp()));
        }
        [[nodiscard]] constexpr VersionedValue NextVersion() const noexcept { return NextVersion(Base::Value()); }
        [[nodiscard]] static constexpr VersionedValue FromRepresentation(Word word) noexcept {
            return VersionedValue(Detail::FromRepresentationTag{}, word);
        }
        friend constexpr bool operator==(VersionedValue left, VersionedValue right) noexcept {
            return left.Representation() == right.Representation();
        }

    private:
        explicit constexpr VersionedValue(Detail::FromRepresentationTag tag, Word word) noexcept : Base(tag, word) {}
    };

    /** @brief Lock-free atomic publication for a canonical single-word tagged or versioned value. */
    template<Concept::StampedWord Stamped>
    class AtomicStampedValue {
    public:
        using ValueType = Stamped;
        using Word = typename Stamped::Word;

        static_assert(std::atomic<Word>::is_always_lock_free,
                      "AtomicStampedValue requires an always-lock-free word on the target ABI");

        explicit constexpr AtomicStampedValue(Stamped initial = {}) noexcept : word_(initial.Representation()) {}
        AtomicStampedValue(const AtomicStampedValue&) = delete;
        AtomicStampedValue& operator=(const AtomicStampedValue&) = delete;

        [[nodiscard]] Stamped Load(std::memory_order order = std::memory_order_acquire) const noexcept {
            return Stamped::FromRepresentation(word_.load(order));
        }
        void Store(Stamped value, std::memory_order order = std::memory_order_release) noexcept {
            word_.store(value.Representation(), order);
        }
        [[nodiscard]] Stamped Exchange(Stamped value, std::memory_order order = std::memory_order_acq_rel) noexcept {
            return Stamped::FromRepresentation(word_.exchange(value.Representation(), order));
        }
        [[nodiscard]] bool CompareExchangeWeak(Stamped& expected, Stamped desired,
                                               std::memory_order success = std::memory_order_acq_rel,
                                               std::memory_order failure = std::memory_order_acquire) noexcept {
            return CompareExchange(expected, desired, true, success, failure);
        }
        [[nodiscard]] bool CompareExchangeStrong(Stamped& expected, Stamped desired,
                                                 std::memory_order success = std::memory_order_acq_rel,
                                                 std::memory_order failure = std::memory_order_acquire) noexcept {
            return CompareExchange(expected, desired, false, success, failure);
        }
        [[nodiscard]] static consteval bool IsAlwaysLockFree() noexcept {
            return std::atomic<Word>::is_always_lock_free;
        }
        [[nodiscard]] bool IsLockFree() const noexcept { return word_.is_lock_free(); }

    private:
        [[nodiscard]] bool CompareExchange(Stamped& expected, Stamped desired, bool weak, std::memory_order success,
                                           std::memory_order failure) noexcept {
            Word expectedWord = expected.Representation();
            const bool exchanged =
                weak ? word_.compare_exchange_weak(expectedWord, desired.Representation(), success, failure)
                     : word_.compare_exchange_strong(expectedWord, desired.Representation(), success, failure);
            if (!exchanged) {
                expected = Stamped::FromRepresentation(expectedWord);
            }
            return exchanged;
        }

        std::atomic<Word> word_;
    };

    template<typename T, std::size_t TagBits>
    using TaggedPointer = TaggedValue<Detail::PointerLowBitsCodec<T, TagBits>>;
    template<typename T, std::size_t VersionBits>
    using VersionedPointer = VersionedValue<Detail::PointerLowBitsCodec<T, VersionBits>>;
    template<std::unsigned_integral UInt, std::size_t PayloadBits, std::size_t TagBits>
    using TaggedUnsigned = TaggedValue<Detail::BoundedUnsignedCodec<UInt, PayloadBits, TagBits>>;
    template<std::unsigned_integral UInt, std::size_t PayloadBits, std::size_t VersionBits>
    using VersionedUnsigned = VersionedValue<Detail::BoundedUnsignedCodec<UInt, PayloadBits, VersionBits>>;
    template<typename T, std::size_t TagBits>
    using AtomicTaggedPointer = AtomicStampedValue<TaggedPointer<T, TagBits>>;
    template<typename T, std::size_t VersionBits>
    using AtomicVersionedPointer = AtomicStampedValue<VersionedPointer<T, VersionBits>>;
    template<std::unsigned_integral UInt, std::size_t PayloadBits, std::size_t TagBits>
    using AtomicTaggedUnsigned = AtomicStampedValue<TaggedUnsigned<UInt, PayloadBits, TagBits>>;
    template<std::unsigned_integral UInt, std::size_t PayloadBits, std::size_t VersionBits>
    using AtomicVersionedUnsigned = AtomicStampedValue<VersionedUnsigned<UInt, PayloadBits, VersionBits>>;

} // namespace Sora::Concurrency
