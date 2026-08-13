/**
 * @file Rcu.h
 * @brief Phase-based read-copy-update grace periods and deferred reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/Detail/Reclamation.h>
#include <Sora/Core/Concurrency/PointerStamp.h>
#include <Sora/Core/Concurrency/Protection.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /** @brief RCU domain that waits only for read-side regions preceding a grace-period flip. */
    class RcuDomain {
        struct alignas(Platform::kCacheLineSize) Record {
            std::array<std::atomic<std::size_t>, 2> readers{};
            std::atomic<bool> registered{false};

            [[nodiscard]] bool TryRegister() noexcept {
                if (readers[0].load(std::memory_order_relaxed) != 0 ||
                    readers[1].load(std::memory_order_relaxed) != 0) {
                    return false;
                }
                bool expected = false;
                if (!registered.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
                    return false;
                }
                if (readers[0].load(std::memory_order_acquire) == 0 &&
                    readers[1].load(std::memory_order_acquire) == 0) {
                    return true;
                }
                registered.store(false, std::memory_order_release);
                return false;
            }
        };
        static_assert(sizeof(Record) % Platform::kCacheLineSize == 0);

    public:
        class Participant;

        /** @brief One nestable read-side region on an affine RCU participant. */
        class ReadGuard {
        public:
            ReadGuard() noexcept = default;
            ReadGuard(const ReadGuard&) = delete;
            ReadGuard& operator=(const ReadGuard&) = delete;
            ReadGuard(ReadGuard&& other) noexcept
                : record_(std::exchange(other.record_, nullptr)), phase_(other.phase_) {}
            ReadGuard& operator=(ReadGuard&& other) noexcept {
                if (this != &other) {
                    Reset();
                    record_ = std::exchange(other.record_, nullptr);
                    phase_ = other.phase_;
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
                record_->readers[phase_].fetch_sub(1, std::memory_order_release);
                record_ = nullptr;
            }
            [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

        private:
            friend class Participant;
            ReadGuard(Record* record, unsigned phase) noexcept : record_(record), phase_(phase) {}
            Record* record_ = nullptr;
            unsigned phase_ = 0;
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
                for (;;) {
                    const unsigned observed = phase_->load(std::memory_order_acquire);
                    record_->readers[observed].fetch_add(1, std::memory_order_seq_cst);
                    if (phase_->load(std::memory_order_seq_cst) == observed) {
                        return ReadGuard(record_, observed);
                    }
                    record_->readers[observed].fetch_sub(1, std::memory_order_release);
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
            [[nodiscard]] ProtectedStampedValue<Stamped, ReadGuard>
            Protect(const AtomicStampedValue<Stamped>& source) noexcept {
                ReadGuard guard = Read();
                Stamped snapshot = guard.Load(source);
                return ProtectedStampedValue<Stamped, ReadGuard>(snapshot, std::move(guard));
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
                       record.readers[0].load(std::memory_order_relaxed) == 0 &&
                       record.readers[1].load(std::memory_order_relaxed) == 0 &&
                       "RCU domain must outlive all participants and read guards");
            });
            retired_.Drain();
        }

        [[nodiscard]] Participant Register() { return Participant(*this); }

        template<typename T>
        void Retire(T* pointer) { retired_.Retire(pointer); }
        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter) { retired_.Retire(pointer, std::forward<Deleter>(deleter)); }

        /** @brief Wait for all read-side regions that began before this grace period. */
        void Synchronize() {
            std::scoped_lock synchronize(synchronizeMutex_);
            const unsigned oldPhase = phase_.fetch_xor(1, std::memory_order_seq_cst);
            records_.Visit([&](const Record& record) {
                while (record.readers[oldPhase].load(std::memory_order_acquire) != 0) {
                    std::this_thread::yield();
                }
            });
        }

        /** @brief Complete one grace period, then reclaim exactly the callbacks pending before it. */
        [[nodiscard]] std::size_t Reclaim() {
            auto reclaimable = retired_.DetachAll();
            try {
                Synchronize();
            } catch (...) {
                retired_.Requeue(std::move(reclaimable));
                throw;
            }
            std::size_t count = 0;
            count = reclaimable.size();
            Detail::RetiredList::Destroy(std::move(reclaimable));
            return count;
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.Size(); }

    private:
        std::atomic<unsigned> phase_{0};
        Detail::StableRecordRegistry<Record> records_;
        Detail::RetiredList retired_;
        std::mutex synchronizeMutex_;
    };

} // namespace Sora::Concurrency
