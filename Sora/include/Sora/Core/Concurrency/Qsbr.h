/**
 * @file Qsbr.h
 * @brief Quiescent-state-based reclamation with explicit participant acknowledgements.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/Detail/Reclamation.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /** @brief QSBR domain for affine participants that periodically report quiescent states. */
    class QsbrDomain {
        struct alignas(Platform::kCacheLineSize) Record {
            std::atomic<std::uint64_t> acknowledged{0};
            std::atomic<bool> registered{false};

            [[nodiscard]] bool TryRegister() noexcept {
                bool expected = false;
                return registered.compare_exchange_strong(expected, true, std::memory_order_seq_cst,
                                                          std::memory_order_relaxed);
            }
        };
        static_assert(sizeof(Record) % Platform::kCacheLineSize == 0);

    public:
        /** @brief Affine registration whose calls and protected reference use must not overlap between threads. */
        class Participant {
        public:
            explicit Participant(QsbrDomain& domain)
                : generation_(std::addressof(domain.generation_)), record_(domain.records_.Register()) {
                Quiescent();
            }
            Participant(const Participant&) = delete;
            Participant& operator=(const Participant&) = delete;
            Participant(Participant&& other) noexcept
                : generation_(std::exchange(other.generation_, nullptr)),
                  record_(std::exchange(other.record_, nullptr)) {}
            Participant& operator=(Participant&& other) noexcept {
                if (this != &other) {
                    Release();
                    generation_ = std::exchange(other.generation_, nullptr);
                    record_ = std::exchange(other.record_, nullptr);
                }
                return *this;
            }
            ~Participant() { Release(); }

            /** @brief Report that no reference acquired before this call remains in use afterward. */
            void Quiescent() noexcept {
                assert(record_ != nullptr);
                record_->acknowledged.store(generation_->load(std::memory_order_acquire), std::memory_order_release);
            }

        private:
            void Release() noexcept {
                if (record_ == nullptr) {
                    return;
                }
                record_->registered.store(false, std::memory_order_seq_cst);
                record_ = nullptr;
                generation_ = nullptr;
            }

            std::atomic<std::uint64_t>* generation_ = nullptr;
            Record* record_ = nullptr;
        };

        QsbrDomain() = default;
        QsbrDomain(const QsbrDomain&) = delete;
        QsbrDomain& operator=(const QsbrDomain&) = delete;
        ~QsbrDomain() {
            records_.Visit([](const Record& record) {
                assert(!record.registered.load(std::memory_order_relaxed) &&
                       "QSBR domain must outlive all participants");
            });
            retired_.Drain();
        }

        [[nodiscard]] Participant Register() { return Participant(*this); }

        template<typename T>
        void Retire(T* pointer) {
            if (pointer != nullptr) {
                retired_.Retire(pointer, CaptureGeneration());
            }
        }
        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter) {
            if (pointer != nullptr) {
                retired_.Retire(pointer, std::forward<Deleter>(deleter), CaptureGeneration());
            }
        }
        template<typename Pointer, std::size_t Extent>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*>
        void Retire(std::span<Pointer, Extent> pointers) {
            if (!pointers.empty()) {
                retired_.Retire(pointers, CaptureGeneration());
            }
        }
        template<typename Pointer, std::size_t Extent, typename Deleter>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_copy_constructible_v<Deleter> && std::is_nothrow_move_constructible_v<Deleter> &&
                     std::is_nothrow_invocable_v<Deleter&, std::remove_cv_t<Pointer>>
        void Retire(std::span<Pointer, Extent> pointers, Deleter deleter) {
            if (!pointers.empty()) {
                retired_.Retire(pointers, std::move(deleter), CaptureGeneration());
            }
        }

        [[nodiscard]] std::uint64_t Advance() noexcept {
            return generation_.fetch_add(1, std::memory_order_seq_cst) + 1;
        }

        void Synchronize() {
            const std::uint64_t target = Advance();
            records_.Visit([&](const Record& record) {
                while (record.registered.load(std::memory_order_seq_cst) &&
                       !Detail::GenerationReached(record.acknowledged.load(std::memory_order_acquire), target)) {
                    std::this_thread::yield();
                }
            });
            (void)Collect();
        }

        [[nodiscard]] std::size_t Collect() {
            auto candidates = retired_.DetachAll();
            const std::uint64_t current = generation_.load(std::memory_order_acquire);
            std::uint64_t minimum = current;
            std::uint64_t greatestAge = 0;
            bool generationRangeValid = true;
            records_.Visit([&](const Record& record) {
                if (record.registered.load(std::memory_order_seq_cst)) {
                    const std::uint64_t acknowledged = record.acknowledged.load(std::memory_order_acquire);
                    const std::uint64_t age = current - acknowledged;
                    if (age >= Detail::kGenerationHalfRange) {
                        generationRangeValid = false;
                        return;
                    }
                    if (age > greatestAge) {
                        greatestAge = age;
                        minimum = acknowledged;
                    }
                }
            });
            return Detail::ReclaimDetached(
                retired_, std::move(candidates), [&](const Detail::RetiredObject& object) noexcept {
                    return generationRangeValid && Detail::GenerationReached(minimum, object.generation + 1);
                });
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.Size(); }

    private:
        /** @brief Read the immediate predecessor in generation modification order before transferring ownership. */
        [[nodiscard]] std::uint64_t CaptureGeneration() noexcept {
            return generation_.fetch_add(0, std::memory_order_acquire);
        }

        std::atomic<std::uint64_t> generation_{1};
        Detail::StableRecordRegistry<Record> records_;
        Detail::RetirementQueue retired_;
    };

} // namespace Sora::Concurrency
