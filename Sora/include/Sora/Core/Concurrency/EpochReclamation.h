/**
 * @file EpochReclamation.h
 * @brief Three-generation epoch-based reclamation for affine participants.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/Detail/Reclamation.h>
#include <Sora/Core/Concurrency/StampedValue.h>
#include <Sora/Core/Concurrency/Protection.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /** @brief Epoch domain whose participants delimit short, nestable read-side regions. */
    class EpochDomain {
        struct alignas(Platform::kCacheLineSize) Record {
            std::atomic<std::uint64_t> announced{0};
            std::atomic<std::size_t> depth{0};
            std::atomic<bool> registered{false};

            [[nodiscard]] bool TryRegister() noexcept {
                if (depth.load(std::memory_order_relaxed) != 0) {
                    return false;
                }
                bool expected = false;
                if (!registered.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
                    return false;
                }
                if (depth.load(std::memory_order_acquire) == 0) {
                    return true;
                }
                registered.store(false, std::memory_order_release);
                return false;
            }
        };
        static_assert(sizeof(Record) % Platform::kCacheLineSize == 0);

    public:
        class Participant;

        /** @brief One nested read-side region on an affine epoch participant. */
        class ReadGuard {
        public:
            ReadGuard() noexcept = default;
            ReadGuard(const ReadGuard&) = delete;
            ReadGuard& operator=(const ReadGuard&) = delete;
            ReadGuard(ReadGuard&& other) noexcept : record_(std::exchange(other.record_, nullptr)) {}
            ReadGuard& operator=(ReadGuard&& other) noexcept {
                if (this != &other) {
                    Reset();
                    record_ = std::exchange(other.record_, nullptr);
                }
                return *this;
            }
            ~ReadGuard() { Reset(); }

            template<typename T>
            [[nodiscard]] T* Load(const std::atomic<T*>& source) const noexcept {
                assert(record_ != nullptr);
                return source.load(std::memory_order_acquire);
            }
            template<typename Stamped>
                requires std::is_pointer_v<typename Stamped::ValueType>
            [[nodiscard]] Stamped Load(const AtomicStampedValue<Stamped>& source) const noexcept {
                assert(record_ != nullptr);
                return source.Load(std::memory_order_acquire);
            }
            void Reset() noexcept {
                if (record_ == nullptr) {
                    return;
                }
                const std::size_t depth = record_->depth.load(std::memory_order_relaxed);
                assert(depth != 0);
                record_->depth.store(depth - 1, std::memory_order_release);
                record_ = nullptr;
            }
            [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

        private:
            friend class Participant;
            explicit ReadGuard(Record* record) noexcept : record_(record) {}
            Record* record_ = nullptr;
        };

        /** @brief Affine registration; calls through it and its guards must not overlap between threads. */
        class Participant {
        public:
            explicit Participant(EpochDomain& domain)
                : epoch_(std::addressof(domain.epoch_)), record_(domain.records_.Register()) {}
            Participant(const Participant&) = delete;
            Participant& operator=(const Participant&) = delete;
            Participant(Participant&& other) noexcept
                : epoch_(std::exchange(other.epoch_, nullptr)), record_(std::exchange(other.record_, nullptr)) {}
            Participant& operator=(Participant&& other) noexcept {
                if (this != &other) {
                    Release();
                    epoch_ = std::exchange(other.epoch_, nullptr);
                    record_ = std::exchange(other.record_, nullptr);
                }
                return *this;
            }
            ~Participant() { Release(); }

            [[nodiscard]] ReadGuard Enter() noexcept {
                assert(record_ != nullptr);
                const std::size_t depth = record_->depth.load(std::memory_order_relaxed);
                if (depth != 0) {
                    record_->depth.store(depth + 1, std::memory_order_release);
                    return ReadGuard(record_);
                }

                for (;;) {
                    const std::uint64_t observed = epoch_->load(std::memory_order_acquire);
                    record_->announced.store(observed, std::memory_order_relaxed);
                    record_->depth.store(1, std::memory_order_seq_cst);
                    if (epoch_->load(std::memory_order_seq_cst) == observed) {
                        return ReadGuard(record_);
                    }
                    record_->depth.store(0, std::memory_order_release);
                }
            }

            template<typename T>
            [[nodiscard]] ProtectedPointer<T, ReadGuard> Protect(const std::atomic<T*>& source) noexcept {
                ReadGuard guard = Enter();
                T* pointer = guard.Load(source);
                return ProtectedPointer<T, ReadGuard>(pointer, std::move(guard));
            }
            template<typename Stamped>
                requires std::is_pointer_v<typename Stamped::ValueType>
            [[nodiscard]] ProtectedStampedPointer<Stamped, ReadGuard>
            Protect(const AtomicStampedValue<Stamped>& source) noexcept {
                ReadGuard guard = Enter();
                Stamped snapshot = guard.Load(source);
                return ProtectedStampedPointer<Stamped, ReadGuard>(snapshot, std::move(guard));
            }

        private:
            void Release() noexcept {
                if (record_ == nullptr) {
                    return;
                }
                record_->registered.store(false, std::memory_order_release);
                record_ = nullptr;
                epoch_ = nullptr;
            }

            std::atomic<std::uint64_t>* epoch_ = nullptr;
            Record* record_ = nullptr;
        };

        EpochDomain() = default;
        EpochDomain(const EpochDomain&) = delete;
        EpochDomain& operator=(const EpochDomain&) = delete;
        ~EpochDomain() {
            records_.Visit([](const Record& record) {
                assert(!record.registered.load(std::memory_order_relaxed) &&
                       record.depth.load(std::memory_order_relaxed) == 0 &&
                       "epoch domain must outlive all participants and read guards");
            });
            retired_.Drain();
        }

        [[nodiscard]] Participant Register() { return Participant(*this); }

        template<typename T>
        void Retire(T* pointer) {
            if (pointer != nullptr) {
                retired_.Retire(pointer, CaptureEpoch());
            }
        }
        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter) {
            if (pointer != nullptr) {
                retired_.Retire(pointer, std::forward<Deleter>(deleter), CaptureEpoch());
            }
        }
        template<typename Pointer, std::size_t Extent>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*>
        void Retire(std::span<Pointer, Extent> pointers) {
            if (!pointers.empty()) {
                retired_.Retire(pointers, CaptureEpoch());
            }
        }
        template<typename Pointer, std::size_t Extent, typename Deleter>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_copy_constructible_v<Deleter> && std::is_nothrow_move_constructible_v<Deleter> &&
                     std::is_nothrow_invocable_v<Deleter&, std::remove_cv_t<Pointer>>
        void Retire(std::span<Pointer, Extent> pointers, Deleter deleter) {
            if (!pointers.empty()) {
                retired_.Retire(pointers, std::move(deleter), CaptureEpoch());
            }
        }

        /** @brief Attempt one epoch advance; active readers from an older generation defer it. */
        [[nodiscard]] bool TryAdvance() noexcept {
            std::uint64_t current = epoch_.load(std::memory_order_acquire);
            bool mayAdvance = true;
            records_.Visit([&](const Record& record) {
                if (record.depth.load(std::memory_order_seq_cst) != 0 &&
                    record.announced.load(std::memory_order_relaxed) != current) {
                    mayAdvance = false;
                }
            });
            if (!mayAdvance) {
                return false;
            }
            return epoch_.compare_exchange_strong(current, current + 1, std::memory_order_seq_cst,
                                                  std::memory_order_seq_cst);
        }

        /** @brief Reclaim objects at least two completed epoch transitions behind the current generation. */
        [[nodiscard]] std::size_t Collect() {
            auto candidates = retired_.DetachAll();
            const std::uint64_t current = epoch_.load(std::memory_order_acquire);
            return Detail::ReclaimDetached(retired_, std::move(candidates),
                                           [&](const Detail::RetiredObject& object) noexcept {
                                               return Detail::GenerationReached(current, object.generation + 2);
                                           });
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.Size(); }

    private:
        /** @brief Read the immediate predecessor in epoch modification order, avoiding a stale retirement epoch. */
        [[nodiscard]] std::uint64_t CaptureEpoch() noexcept { return epoch_.fetch_add(0, std::memory_order_acquire); }

        std::atomic<std::uint64_t> epoch_{1};
        Detail::StableRecordRegistry<Record> records_;
        Detail::RetiredList retired_;
    };

} // namespace Sora::Concurrency
