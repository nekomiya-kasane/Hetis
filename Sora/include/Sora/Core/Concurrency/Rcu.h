/**
 * @file Rcu.h
 * @brief Phase-based read-copy-update grace periods and deferred reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/Detail/Reclamation.h>
#include <Sora/Core/Concurrency/StampedValue.h>
#include <Sora/Core/Concurrency/Protection.h>
#include <Sora/Core/Guard.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /**
     * @brief RCU domain that waits only for read-side regions preceding a grace-period flip.
     *
     * @details Access protected objects only inside a participant's read-side region. Remove an object from every
     * source before @ref Retire; queue handoff publishes that unlink before the grace-period phase flip. Participants
     * and guards are affine, and the domain must outlive all of them.
     */
    class RcuDomain {
        struct alignas(Platform::kCacheLineSize) Record {
            std::atomic<unsigned> announcedPhase{0};
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
        static_assert(std::atomic<unsigned>::is_always_lock_free && std::atomic<std::size_t>::is_always_lock_free &&
                          std::atomic<bool>::is_always_lock_free,
                      "RcuDomain requires lock-free phase, depth, and registration atomics");

    public:
        class Participant;

        /** @brief One nestable read-side region on an affine RCU participant. */
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
            explicit Participant(RcuDomain& domain)
                : phase_(std::addressof(domain.phase_)), record_(domain.records_.Register()) {}
            Participant(const Participant&) = delete;
            Participant& operator=(const Participant&) = delete;
            Participant(Participant&& other) noexcept
                : phase_(std::exchange(other.phase_, nullptr)), record_(std::exchange(other.record_, nullptr)) {}
            Participant& operator=(Participant&& other) noexcept {
                if (this != &other) {
                    Release();
                    phase_ = std::exchange(other.phase_, nullptr);
                    record_ = std::exchange(other.record_, nullptr);
                }
                return *this;
            }
            ~Participant() { Release(); }

            [[nodiscard]] ReadGuard Read() noexcept {
                assert(record_ != nullptr);
                const std::size_t depth = record_->depth.load(std::memory_order_relaxed);
                if (depth != 0) {
                    record_->depth.store(depth + 1, std::memory_order_release);
                    return ReadGuard(record_);
                }

                for (;;) {
                    const unsigned observed = phase_->load(std::memory_order_acquire);
                    record_->announcedPhase.store(observed, std::memory_order_release);
                    record_->depth.store(1, std::memory_order_seq_cst);
                    if (phase_->load(std::memory_order_seq_cst) == observed) {
                        return ReadGuard(record_);
                    }
                    record_->depth.store(0, std::memory_order_release);
                }
            }

            template<typename T>
            [[nodiscard]] ProtectedPointer<T, ReadGuard> Protect(const std::atomic<T*>& source) noexcept {
                ReadGuard guard = Read();
                T* pointer = guard.Load(source);
                return ProtectedPointer<T, ReadGuard>(pointer, std::move(guard));
            }
            template<typename Stamped>
                requires std::is_pointer_v<typename Stamped::ValueType>
            [[nodiscard]] ProtectedStampedPointer<Stamped, ReadGuard>
            Protect(const AtomicStampedValue<Stamped>& source) noexcept {
                ReadGuard guard = Read();
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
                phase_ = nullptr;
            }

            std::atomic<unsigned>* phase_ = nullptr;
            Record* record_ = nullptr;
        };

        RcuDomain() = default;
        RcuDomain(const RcuDomain&) = delete;
        RcuDomain& operator=(const RcuDomain&) = delete;
        ~RcuDomain() {
            records_.Visit([](const Record& record) {
                assert(!record.registered.load(std::memory_order_relaxed) &&
                       record.depth.load(std::memory_order_relaxed) == 0 &&
                       "RCU domain must outlive all participants and read guards");
            });
            retired_.Drain();
        }

        [[nodiscard]] Participant Register() { return Participant(*this); }

        template<typename T>
            requires std::is_nothrow_destructible_v<T>
        void Retire(T* pointer) {
            retired_.Retire(pointer);
        }
        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter) {
            retired_.Retire(pointer, std::forward<Deleter>(deleter));
        }
        template<typename Pointer, std::size_t Extent>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_destructible_v<std::remove_pointer_t<std::remove_cv_t<Pointer>>>
        void Retire(std::span<Pointer, Extent> pointers) {
            retired_.Retire(pointers);
        }
        template<typename Pointer, std::size_t Extent, typename Deleter>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_copy_constructible_v<Deleter> && std::is_nothrow_move_constructible_v<Deleter> &&
                     std::is_nothrow_invocable_v<Deleter&, std::remove_cv_t<Pointer>>
        void Retire(std::span<Pointer, Extent> pointers, Deleter deleter) {
            retired_.Retire(pointers, std::move(deleter));
        }

        /**
         * @brief Wait for all read-side regions that began before this grace period.
         * @pre The calling thread must not hold a read guard from this domain.
         */
        void Synchronize() {
            std::scoped_lock synchronize(synchronizeMutex_);
            const unsigned oldPhase = phase_.fetch_xor(1, std::memory_order_seq_cst);
            records_.Visit([&](const Record& record) {
                // The SC load closes publication -> validation -> flip -> scan. An acquire announcement also orders
                // an old-reader exit when this record has already entered the new phase.
                if (record.depth.load(std::memory_order_seq_cst) == 0 ||
                    record.announcedPhase.load(std::memory_order_acquire) != oldPhase) {
                    return;
                }
                while (record.depth.load(std::memory_order_acquire) != 0 &&
                       record.announcedPhase.load(std::memory_order_acquire) == oldPhase) {
                    std::this_thread::yield();
                }
            });
        }

        /**
         * @brief Complete one grace period, then reclaim exactly the callbacks pending before it.
         * @pre The calling thread must not hold a read guard from this domain.
         */
        [[nodiscard]] std::size_t Reclaim() {
            auto reclaimable = retired_.DetachAll();
            if (reclaimable.Empty()) {
                return 0;
            }
            Sora::ScopeExit restoreReclaimable{[&]() noexcept { retired_.Requeue(std::move(reclaimable)); }};
            Synchronize();
            restoreReclaimable.Release();
            const std::size_t count = reclaimable.Size();
            retired_.Destroy(std::move(reclaimable));
            return count;
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.Size(); }

    private:
        std::atomic<unsigned> phase_{0};
        Detail::StableRecordRegistry<Record> records_;
        Detail::RetirementQueue retired_;
        std::mutex synchronizeMutex_;
    };

} // namespace Sora::Concurrency
