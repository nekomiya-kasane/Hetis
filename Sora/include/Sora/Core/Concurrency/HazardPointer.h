/**
 * @file HazardPointer.h
 * @brief Address-granular hazard-pointer protection and deferred reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/PointerStamp.h>
#include <Sora/Core/Concurrency/Detail/Reclamation.h>
#include <Sora/Core/Concurrency/Protection.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Concurrency {

    /** @brief Explicit hazard-pointer domain with reusable, domain-owned protection records. */
    class HazardDomain {
        struct alignas(Platform::kCacheLineSize) Record {
            std::atomic<void*> protectedAddress{nullptr};
            std::atomic<bool> owned{false};

            [[nodiscard]] bool TryRegister() noexcept {
                bool expected = false;
                return owned.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                     std::memory_order_relaxed);
            }
        };
        static_assert(sizeof(Record) % Platform::kCacheLineSize == 0);

    public:
        /** @brief Move-only ownership of one hazard record and its successive protection epochs. */
        class HazardPointer {
        public:
            HazardPointer() noexcept = default;
            HazardPointer(const HazardPointer&) = delete;
            HazardPointer& operator=(const HazardPointer&) = delete;
            HazardPointer(HazardPointer&& other) noexcept : record_(std::exchange(other.record_, nullptr)) {}
            HazardPointer& operator=(HazardPointer&& other) noexcept {
                if (this != &other) {
                    Release();
                    record_ = std::exchange(other.record_, nullptr);
                }
                return *this;
            }
            ~HazardPointer() { Release(); }

            /** @brief Protect the pointer value validated from @p source using the hazard publication protocol. */
            template<typename T>
            [[nodiscard]] T* Protect(const std::atomic<T*>& source) noexcept {
                T* pointer = source.load(std::memory_order_seq_cst);
                for (;;) {
                    ResetProtection(pointer);
                    T* validated = source.load(std::memory_order_seq_cst);
                    if (validated == pointer) {
                        return pointer;
                    }
                    pointer = validated;
                }
            }

            /** @brief Protect the pointee and exact identity of a stamped pointer publication. */
            template<typename Stamped>
                requires std::is_pointer_v<typename Stamped::ValueType>
            [[nodiscard]] Stamped Protect(const AtomicStampedValue<Stamped>& source) noexcept {
                Stamped snapshot = source.Load(std::memory_order_seq_cst);
                for (;;) {
                    ResetProtection(snapshot.Value());
                    Stamped validated = source.Load(std::memory_order_seq_cst);
                    if (validated == snapshot) {
                        return snapshot;
                    }
                    snapshot = validated;
                }
            }

            template<typename T>
            void ResetProtection(T* pointer) noexcept {
                assert(record_ != nullptr);
                record_->protectedAddress.store(pointer, std::memory_order_seq_cst);
            }
            void ResetProtection(std::nullptr_t = nullptr) noexcept {
                assert(record_ != nullptr);
                record_->protectedAddress.store(nullptr, std::memory_order_seq_cst);
            }
            [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

        private:
            friend class HazardDomain;
            explicit HazardPointer(Record* record) noexcept : record_(record) {}

            void Release() noexcept {
                if (record_ == nullptr) {
                    return;
                }
                record_->protectedAddress.store(nullptr, std::memory_order_seq_cst);
                record_->owned.store(false, std::memory_order_release);
                record_ = nullptr;
            }

            Record* record_ = nullptr;
        };

        HazardDomain() = default;
        HazardDomain(const HazardDomain&) = delete;
        HazardDomain& operator=(const HazardDomain&) = delete;
        ~HazardDomain() {
            records_.Visit([](const Record& record) {
                assert(!record.owned.load(std::memory_order_relaxed) &&
                       record.protectedAddress.load(std::memory_order_relaxed) == nullptr &&
                       "hazard domain must outlive all hazard pointers");
            });
            retired_.Drain();
        }

        [[nodiscard]] HazardPointer MakeHazardPointer() { return HazardPointer(records_.Register()); }

        template<typename T>
        [[nodiscard]] ProtectedPointer<T, HazardPointer> Protect(const std::atomic<T*>& source) {
            HazardPointer hazard = MakeHazardPointer();
            T* pointer = hazard.Protect(source);
            return ProtectedPointer<T, HazardPointer>(pointer, std::move(hazard));
        }

        template<typename Stamped>
            requires std::is_pointer_v<typename Stamped::ValueType>
        [[nodiscard]] ProtectedStampedValue<Stamped, HazardPointer> Protect(const AtomicStampedValue<Stamped>& source) {
            HazardPointer hazard = MakeHazardPointer();
            Stamped snapshot = hazard.Protect(source);
            return ProtectedStampedValue<Stamped, HazardPointer>(snapshot, std::move(hazard));
        }

        template<typename T>
        void Retire(T* pointer) { retired_.Retire(pointer); }
        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter) { retired_.Retire(pointer, std::forward<Deleter>(deleter)); }

        /** @brief Reclaim the batch retired before this call when no scanned record protects its address. */
        [[nodiscard]] std::size_t Collect() {
            auto candidates = retired_.DetachAll();
            if (candidates.empty()) {
                return 0;
            }

            // The SC fence is paired with SC protection publication. A source must be unlinked before Retire().
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::vector<void*> protectedAddresses;
            try {
                records_.Visit([&](const Record& record) {
                    if (void* address = record.protectedAddress.load(std::memory_order_seq_cst); address != nullptr) {
                        protectedAddresses.push_back(address);
                    }
                });
            } catch (...) {
                retired_.Requeue(std::move(candidates));
                throw;
            }
            std::ranges::sort(protectedAddresses);
            return Detail::ReclaimDetached(
                retired_, std::move(candidates), [&](const Detail::RetiredObject& object) noexcept {
                    return !std::ranges::binary_search(protectedAddresses, object.pointer);
                });
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.Size(); }

    private:
        Detail::StableRecordRegistry<Record> records_;
        Detail::RetiredList retired_;
    };

} // namespace Sora::Concurrency
