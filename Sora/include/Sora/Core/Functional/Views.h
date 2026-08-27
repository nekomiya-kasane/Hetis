/**
 * @file Views.h
 * @brief Lazy range adaptors and Sora range views.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Functional/Callable.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concept {

        /** @brief Input range eligible for Sora lazy view adaptors. */
        template<typename T>
        concept LazyRange = std::ranges::input_range<T>;

    } // namespace Concept

    namespace View {

        namespace Detail {

            template<typename Reference, typename Value, bool = std::is_lvalue_reference_v<Reference>>
            class CacheSlot;

            template<typename Reference, typename Value>
            class CacheSlot<Reference, Value, true> {
              public:
                constexpr void Reset() noexcept { value_ = nullptr; }

                template<typename Iterator>
                [[nodiscard]] constexpr Reference Read(Iterator& iterator) noexcept(noexcept(*iterator)) {
                    if (value_ == nullptr) {
                        value_ = std::addressof(*iterator);
                    }
                    return static_cast<Reference>(*value_);
                }

              private:
                std::remove_reference_t<Reference>* value_ = nullptr;
            };

            template<typename Reference, typename Value>
            class CacheSlot<Reference, Value, false> {
              public:
                constexpr void Reset() noexcept { value_.reset(); }

                template<typename Iterator>
                [[nodiscard]] constexpr const Value& Read(Iterator& iterator) {
                    if (!value_) {
                        value_.emplace(*iterator);
                    }
                    return *value_;
                }

              private:
                std::optional<Value> value_;
            };

        } // namespace Detail

        /**
         * @brief Input view caching the latest dereference result until its iterator advances.
         * @details Lvalue references are cached as pointers; proxy references are materialized once. The cache belongs
         * to the view, is cleared by @c begin and increment, and is deliberately not propagated by view copies.
         * @code{.cpp}
         * auto input = std::array{1, 2};
         * Sora::View::CacheLatestView view{std::views::all(input)};
         * auto iterator = view.begin();
         * auto first = *iterator;
         * auto same = *iterator; // first == same; one source read
         * @endcode
         */
        template<std::ranges::view V>
            requires std::ranges::input_range<V>
        class CacheLatestView : public std::ranges::view_interface<CacheLatestView<V>> {
            using Reference = std::ranges::range_reference_t<V>;

          public:
            CacheLatestView()
                requires std::default_initializable<V>
            = default;

            constexpr explicit CacheLatestView(V base) : base_(std::move(base)) {}

            constexpr CacheLatestView(const CacheLatestView& other)
                requires std::copy_constructible<V>
                : base_(other.base_) {}

            constexpr CacheLatestView& operator=(const CacheLatestView& other)
                requires std::copyable<V>
            {
                base_ = other.base_;
                cache_.Reset();
                return *this;
            }

            constexpr CacheLatestView(CacheLatestView&& other) noexcept(std::is_nothrow_move_constructible_v<V>)
                : base_(std::move(other.base_)) {}

            constexpr CacheLatestView& operator=(CacheLatestView&& other) noexcept(std::is_nothrow_move_assignable_v<V>)
                requires std::movable<V>
            {
                base_ = std::move(other.base_);
                cache_.Reset();
                return *this;
            }

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                cache_.Reset();
                return Iterator{*this, std::ranges::begin(base_)};
            }

            [[nodiscard]] constexpr auto end() { return std::ranges::end(base_); }

            [[nodiscard]] constexpr V base() const&
                requires std::copy_constructible<V>
            {
                return base_;
            }

            [[nodiscard]] constexpr V base() && { return std::move(base_); }

            class Iterator {
              public:
                using iterator_concept = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = std::ranges::range_value_t<V>;

                Iterator() = default;

                [[nodiscard]] constexpr decltype(auto) operator*() const {
                    return parent_->cache_.Read(current_);
                }

                constexpr Iterator& operator++() {
                    ++current_;
                    parent_->cache_.Reset();
                    return *this;
                }

                constexpr void operator++(int) { ++*this; }

                friend constexpr bool operator==(const Iterator& iterator, const std::ranges::sentinel_t<V>& sentinel) {
                    return iterator.current_ == sentinel;
                }

              private:
                friend CacheLatestView;

                constexpr Iterator(CacheLatestView& parent, std::ranges::iterator_t<V> current)
                    : parent_(std::addressof(parent)), current_(std::move(current)) {}

                CacheLatestView* parent_ = nullptr;
                std::ranges::iterator_t<V> current_{};
            };

          private:
            V base_{};
            SORA_NO_UNIQUE_ADDRESS Detail::CacheSlot<Reference, std::ranges::range_value_t<V>> cache_{};
        };

        template<typename R>
        CacheLatestView(R&&) -> CacheLatestView<std::views::all_t<R>>;

        /**
         * @brief Input view emitting every @c stride-th source element.
         * @code{.cpp}
         * auto input = std::array{0, 1, 2, 3, 4};
         * auto view = input | Sora::View::Stride(2);
         * auto output = std::ranges::to<std::vector>(view); // [0, 2, 4]
         * @endcode
         */
        template<std::ranges::view V>
            requires std::ranges::input_range<V>
        class StrideView : public std::ranges::view_interface<StrideView<V>> {
          public:
            StrideView()
                requires std::default_initializable<V>
            = default;

            constexpr StrideView(V base, size_t stride)
                : base_(std::move(base)), stride_(stride) {}

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                return Iterator{std::ranges::begin(base_), std::ranges::end(base_), stride_};
            }

            [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

            [[nodiscard]] constexpr auto size()
                requires std::ranges::sized_range<V>
            {
                using Size = std::ranges::range_size_t<V>;
                const auto count = std::ranges::size(base_);
                if (stride_ == 0 || count == 0) {
                    return Size{0};
                }
                if (std::cmp_greater(stride_, count)) {
                    return Size{1};
                }
                const auto stride = static_cast<Size>(stride_);
                return static_cast<Size>(count / stride + (count % stride != 0));
            }

            class Iterator {
              public:
                using iterator_concept = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = std::ranges::range_value_t<V>;

                Iterator() = default;

                [[nodiscard]] constexpr decltype(auto) operator*() const { return *current_; }

                constexpr Iterator& operator++() {
                    if (!valid_) {
                        return *this;
                    }
                    for (size_t offset = 0; offset < stride_ && current_ != end_; ++offset) {
                        ++current_;
                    }
                    return *this;
                }

                constexpr void operator++(int) { ++*this; }

                friend constexpr bool operator==(const Iterator& iterator, std::default_sentinel_t) {
                    return !iterator.valid_ || iterator.current_ == iterator.end_;
                }

              private:
                friend StrideView;

                constexpr Iterator(std::ranges::iterator_t<V> current, std::ranges::sentinel_t<V> end,
                                   size_t stride)
                    : current_(std::move(current)), end_(std::move(end)), stride_(stride) {}

                std::ranges::iterator_t<V> current_{};
                std::ranges::sentinel_t<V> end_{};
                size_t stride_ = 1;
                bool valid_ = stride_ > 0;
            };

          private:
            V base_{};
            size_t stride_ = 1;
        };

        /**
         * @brief Forward view partitioning a range into fixed-width, non-overlapping subranges.
         * @code{.cpp}
         * auto input = std::array{1, 2, 3, 4, 5};
         * auto view = input | Sora::View::Chunk(2);
         * auto output = std::ranges::to<std::vector>(view); // [[1, 2], [3, 4], [5]]
         * @endcode
         */
        template<std::ranges::view V>
            requires std::ranges::forward_range<V>
        class ChunkView : public std::ranges::view_interface<ChunkView<V>> {
          public:
            ChunkView()
                requires std::default_initializable<V>
            = default;

            constexpr ChunkView(V base, size_t width)
                : base_(std::move(base)), width_(width) {}

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                return Iterator{std::ranges::begin(base_), std::ranges::end(base_), width_};
            }

            [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

            [[nodiscard]] constexpr auto size()
                requires std::ranges::sized_range<V>
            {
                using Size = std::ranges::range_size_t<V>;
                const auto count = std::ranges::size(base_);
                if (width_ == 0 || count == 0) {
                    return Size{0};
                }
                if (std::cmp_greater(width_, count)) {
                    return Size{1};
                }
                const auto width = static_cast<Size>(width_);
                return static_cast<Size>(count / width + (count % width != 0));
            }

            class Iterator {
              public:
                using iterator_concept = std::forward_iterator_tag;
                using iterator_category = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = std::ranges::subrange<std::ranges::iterator_t<V>>;

                Iterator() = default;

                [[nodiscard]] constexpr value_type operator*() const { return {current_, next_}; }

                constexpr Iterator& operator++() {
                    if (!valid_) {
                        return *this;
                    }
                    current_ = next_;
                    FindNext();
                    return *this;
                }

                constexpr Iterator operator++(int) {
                    auto previous = *this;
                    ++*this;
                    return previous;
                }

                friend constexpr bool operator==(const Iterator& left, const Iterator& right) {
                    return (!left.valid_ && !right.valid_) ||
                           (left.valid_ == right.valid_ && left.current_ == right.current_);
                }

                friend constexpr bool operator==(const Iterator& iterator, std::default_sentinel_t) {
                    return !iterator.valid_ || iterator.current_ == iterator.end_;
                }

              private:
                friend ChunkView;

                constexpr Iterator(std::ranges::iterator_t<V> current, std::ranges::sentinel_t<V> end,
                                   size_t width)
                    : current_(std::move(current)), next_(current_), end_(std::move(end)), width_(width) {
                    valid_ = width_ > 0;
                    FindNext();
                }

                constexpr void FindNext() {
                    next_ = current_;
                    for (size_t offset = 0; offset < width_ && next_ != end_; ++offset) {
                        ++next_;
                    }
                }

                std::ranges::iterator_t<V> current_{};
                std::ranges::iterator_t<V> next_{};
                std::ranges::sentinel_t<V> end_{};
                size_t width_ = 1;
                bool valid_ = false;
            };

          private:
            V base_{};
            size_t width_ = 1;
        };

        /**
         * @brief Forward view emitting overlapping fixed-width subranges.
         * @code{.cpp}
         * auto input = std::array{1, 2, 3, 4};
         * auto view = input | Sora::View::Slide(3);
         * auto output = std::ranges::to<std::vector>(view); // [[1, 2, 3], [2, 3, 4]]
         * @endcode
         */
        template<std::ranges::view V>
            requires std::ranges::forward_range<V>
        class SlideView : public std::ranges::view_interface<SlideView<V>> {
          public:
            SlideView()
                requires std::default_initializable<V>
            = default;

            constexpr SlideView(V base, size_t width)
                : base_(std::move(base)), width_(width) {}

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                return Iterator{std::ranges::begin(base_), std::ranges::end(base_), width_};
            }

            [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

            [[nodiscard]] constexpr auto size()
                requires std::ranges::sized_range<V>
            {
                using Size = std::ranges::range_size_t<V>;
                const auto count = std::ranges::size(base_);
                if (width_ == 0 || std::cmp_greater(width_, count)) {
                    return Size{0};
                }
                return static_cast<Size>(count - static_cast<Size>(width_) + 1);
            }

            class Iterator {
              public:
                using iterator_concept = std::forward_iterator_tag;
                using iterator_category = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = std::ranges::subrange<std::ranges::iterator_t<V>>;

                Iterator() = default;

                [[nodiscard]] constexpr value_type operator*() const { return {current_, windowEnd_}; }

                constexpr Iterator& operator++() {
                    if (windowEnd_ == end_) {
                        valid_ = false;
                    } else {
                        ++current_;
                        ++windowEnd_;
                    }
                    return *this;
                }

                constexpr Iterator operator++(int) {
                    auto previous = *this;
                    ++*this;
                    return previous;
                }

                friend constexpr bool operator==(const Iterator& left, const Iterator& right) {
                    return (!left.valid_ && !right.valid_) ||
                           (left.valid_ == right.valid_ && left.current_ == right.current_);
                }

                friend constexpr bool operator==(const Iterator& iterator, std::default_sentinel_t) noexcept {
                    return !iterator.valid_;
                }

              private:
                friend SlideView;

                constexpr Iterator(std::ranges::iterator_t<V> current, std::ranges::sentinel_t<V> end,
                                   size_t width)
                    : current_(std::move(current)), windowEnd_(current_), end_(std::move(end)), valid_(width > 0) {
                    for (size_t offset = 0; offset < width && valid_; ++offset) {
                        if (windowEnd_ == end_) {
                            valid_ = false;
                        } else {
                            ++windowEnd_;
                        }
                    }
                }

                std::ranges::iterator_t<V> current_{};
                std::ranges::iterator_t<V> windowEnd_{};
                std::ranges::sentinel_t<V> end_{};
                bool valid_ = false;
            };

          private:
            V base_{};
            size_t width_ = 1;
        };

        /**
         * @brief Lazy seeded prefix accumulation, emitting one result per source element.
         * @code{.cpp}
         * auto input = std::array{1, 2, 3};
         * auto view = input | Sora::View::Scan(std::plus{}, 0);
         * auto output = std::ranges::to<std::vector>(view); // [1, 3, 6]
         * @endcode
         */
        template<std::ranges::view V, typename F, typename Acc>
            requires std::ranges::input_range<V> && std::is_object_v<F> && std::is_object_v<Acc> &&
                     std::copy_constructible<Acc> &&
                     std::invocable<F&, Acc&&, std::ranges::range_reference_t<V>> &&
                     std::constructible_from<
                         Acc, std::invoke_result_t<F&, Acc&&, std::ranges::range_reference_t<V>>>
        class ScanView : public std::ranges::view_interface<ScanView<V, F, Acc>> {
          public:
            ScanView()
                requires std::default_initializable<V> && std::default_initializable<F> &&
                         std::default_initializable<Acc>
            = default;

            constexpr ScanView(V base, F function, Acc initial)
                : base_(std::move(base)), function_(std::move(function)), initial_(std::move(initial)) {}

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                return Iterator{*this, std::ranges::begin(base_), std::ranges::end(base_)};
            }

            [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

            class Iterator {
              public:
                using iterator_concept = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = Acc;

                Iterator() = default;

                [[nodiscard]] constexpr const Acc& operator*() const noexcept { return *accumulator_; }

                constexpr Iterator& operator++() {
                    ++current_;
                    if (current_ != end_) {
                        Acc next(std::invoke(parent_->function_, std::move(*accumulator_), *current_));
                        accumulator_.emplace(std::move(next));
                    } else {
                        accumulator_.reset();
                    }
                    return *this;
                }

                constexpr void operator++(int) { ++*this; }

                friend constexpr bool operator==(const Iterator& iterator, std::default_sentinel_t) noexcept {
                    return !iterator.accumulator_.has_value();
                }

              private:
                friend ScanView;

                constexpr Iterator(ScanView& parent, std::ranges::iterator_t<V> current, std::ranges::sentinel_t<V> end)
                    : parent_(std::addressof(parent)), current_(std::move(current)), end_(std::move(end)) {
                    if (current_ != end_) {
                        Acc initial(parent_->initial_);
                        accumulator_.emplace(std::invoke(parent_->function_, std::move(initial), *current_));
                    }
                }

                ScanView* parent_ = nullptr;
                std::ranges::iterator_t<V> current_{};
                std::ranges::sentinel_t<V> end_{};
                std::optional<Acc> accumulator_;
            };

          private:
            V base_{};
            SORA_NO_UNIQUE_ADDRESS F function_{};
            SORA_NO_UNIQUE_ADDRESS Acc initial_{};
        };

        /**
         * @brief Lazy unseeded prefix accumulation whose first result is the first source value.
         * @code{.cpp}
         * auto input = std::array{1, 2, 3};
         * auto view = input | Sora::View::PartialSum();
         * auto output = std::ranges::to<std::vector>(view); // [1, 3, 6]
         * @endcode
         */
        template<std::ranges::view V, typename F>
            requires std::ranges::input_range<V> && std::is_object_v<F> &&
                     std::constructible_from<std::ranges::range_value_t<V>, std::ranges::range_reference_t<V>> &&
                     std::invocable<F&, std::ranges::range_value_t<V>&&, std::ranges::range_reference_t<V>> &&
                     std::constructible_from<
                         std::ranges::range_value_t<V>,
                         std::invoke_result_t<
                             F&, std::ranges::range_value_t<V>&&, std::ranges::range_reference_t<V>>>
        class PartialSumView : public std::ranges::view_interface<PartialSumView<V, F>> {
            using Value = std::ranges::range_value_t<V>;

          public:
            PartialSumView()
                requires std::default_initializable<V> && std::default_initializable<F>
            = default;

            constexpr PartialSumView(V base, F function) : base_(std::move(base)), function_(std::move(function)) {}

            class Iterator;

            [[nodiscard]] constexpr Iterator begin() {
                return Iterator{*this, std::ranges::begin(base_), std::ranges::end(base_)};
            }

            [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

            class Iterator {
              public:
                using iterator_concept = std::input_iterator_tag;
                using difference_type = std::ranges::range_difference_t<V>;
                using value_type = Value;

                Iterator() = default;

                [[nodiscard]] constexpr const Value& operator*() const noexcept { return *accumulator_; }

                constexpr Iterator& operator++() {
                    ++current_;
                    if (current_ != end_) {
                        Value next(std::invoke(parent_->function_, std::move(*accumulator_), *current_));
                        accumulator_.emplace(std::move(next));
                    } else {
                        accumulator_.reset();
                    }
                    return *this;
                }

                constexpr void operator++(int) { ++*this; }

                friend constexpr bool operator==(const Iterator& iterator, std::default_sentinel_t) noexcept {
                    return !iterator.accumulator_.has_value();
                }

              private:
                friend PartialSumView;

                constexpr Iterator(
                    PartialSumView& parent, std::ranges::iterator_t<V> current, std::ranges::sentinel_t<V> end)
                    : parent_(std::addressof(parent)), current_(std::move(current)), end_(std::move(end)) {
                    if (current_ != end_) {
                        accumulator_.emplace(*current_);
                    }
                }

                PartialSumView* parent_ = nullptr;
                std::ranges::iterator_t<V> current_{};
                std::ranges::sentinel_t<V> end_{};
                std::optional<Value> accumulator_;
            };

          private:
            V base_{};
            SORA_NO_UNIQUE_ADDRESS F function_{};
        };

        namespace Detail {

            template<typename F>
            struct TransformFunction {
                SORA_NO_UNIQUE_ADDRESS F function;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& range) {
                    return std::views::transform(
                        std::forward<R>(range), std::forward<Self>(self).function);
                }
            };

            template<typename P>
            struct FilterFunction {
                SORA_NO_UNIQUE_ADDRESS P predicate;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& range) {
                    return std::views::filter(
                        std::forward<R>(range), std::forward<Self>(self).predicate);
                }
            };

            template<typename F, typename Initial>
            struct ScanFunction {
                SORA_NO_UNIQUE_ADDRESS F function;
                SORA_NO_UNIQUE_ADDRESS Initial initial;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& range) {
                    return ScanView{std::views::all(std::forward<R>(range)),
                                    std::forward<Self>(self).function, std::forward<Self>(self).initial};
                }
            };

            template<typename F>
            struct PartialSumFunction {
                SORA_NO_UNIQUE_ADDRESS F function;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& range) {
                    return PartialSumView{
                        std::views::all(std::forward<R>(range)), std::forward<Self>(self).function};
                }
            };

            template<typename Tuple>
            struct ZipWithFunction {
                SORA_NO_UNIQUE_ADDRESS Tuple others;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& first) {
                    auto owned = std::forward<Self>(self).others;
                    return std::apply(
                        [&]<typename... Others>(Others&&... others) {
                            return std::views::zip(
                                std::forward<R>(first), std::forward<Others>(others)...);
                        },
                        std::move(owned));
                }
            };

            template<typename P>
            struct ChunkByFunction {
                SORA_NO_UNIQUE_ADDRESS P predicate;

                template<typename Self, std::ranges::viewable_range R>
                constexpr auto operator()(this Self&& self, R&& range) {
                    return std::views::chunk_by(
                        std::forward<R>(range), std::forward<Self>(self).predicate);
                }
            };

            struct StrideFunction {
                size_t stride;

                template<std::ranges::viewable_range R>
                constexpr auto operator()(R&& range) const {
                    using V = std::views::all_t<R>;
                    return StrideView<V>{std::views::all(std::forward<R>(range)), stride};
                }
            };

            struct ChunkFunction {
                size_t width;

                template<std::ranges::viewable_range R>
                    requires std::ranges::forward_range<R>
                constexpr auto operator()(R&& range) const {
                    using V = std::views::all_t<R>;
                    return ChunkView<V>{std::views::all(std::forward<R>(range)), width};
                }
            };

            struct SlideFunction {
                size_t width;

                template<std::ranges::viewable_range R>
                    requires std::ranges::forward_range<R>
                constexpr auto operator()(R&& range) const {
                    using V = std::views::all_t<R>;
                    return SlideView<V>{std::views::all(std::forward<R>(range)), width};
                }
            };

        } // namespace Detail

        /**
         * @brief Lazily transform every element of a viewable range.
         * @details The returned view stores the decayed callable and follows the standard transform-view ownership
         * rules: lvalue ranges are referenced and rvalue ranges are owned. The callable is constrained by the
         * underlying @c std::views::transform implementation and is invoked only when an element is read.
         * @tparam F Element transformation callable.
         * @param[in] function Callable receiving one range element.
         * @return A lazy transform view preserving the source range category supported by the standard view.
         * @code{.cpp}
         * auto values = std::array{1, 2, 3};
         * auto doubled = values | Sora::View::Transform([](int value) { return value * 2; });
         * auto output = std::ranges::to<std::vector>(doubled); // [2, 4, 6]
         * @endcode
         */
        template<typename F>
        [[nodiscard]] constexpr auto Transform(F&& function) {
            return Fn(Detail::TransformFunction<std::decay_t<F>>{std::forward<F>(function)});
        }

        /**
         * @brief Lazily keep elements satisfying @p predicate.
         * @details Predicate evaluation is deferred until iteration and may occur more than once for an element when
         * the underlying iterator searches for the next match. Lvalue ranges remain borrowed; rvalue ranges are owned.
         * @tparam P Boolean-testable predicate.
         * @param[in] predicate Predicate receiving one range element.
         * @return A lazy filter view.
         * @code{.cpp}
         * auto values = std::array{-1, 2, -3, 4};
         * auto positives = values | Sora::View::Filter([](int value) { return value > 0; });
         * auto output = std::ranges::to<std::vector>(positives); // [2, 4]
         * @endcode
         */
        template<typename P>
        [[nodiscard]] constexpr auto Filter(P&& predicate) {
            return Fn(Detail::FilterFunction<std::decay_t<P>>{std::forward<P>(predicate)});
        }

        /**
         * @brief Filter by comparing a projection of each element with @p expected.
         * @details The projection and comparator are stored by value in the predicate closure. Evaluation is lazy and
         * follows the same borrowed-lvalue and owned-rvalue range rules as @ref Filter.
         * @tparam Expected Value compared against the projected element.
         * @tparam Projection Projection callable; defaults to @c std::identity.
         * @tparam Comparator Equality-like comparator; defaults to @c std::ranges::equal_to.
         * @param[in] expected Comparison value.
         * @param[in] projection Element projection.
         * @param[in] comparator Comparison operation.
         * @return A lazy filtered view.
         * @code{.cpp}
         * auto records = std::array{41, 42};
         * auto selected = records | Sora::View::Where(42);
         * auto output = std::ranges::to<std::vector>(selected); // [42]
         * @endcode
         */
        template<typename Expected, typename Projection = std::identity, typename Comparator = std::ranges::equal_to>
        [[nodiscard]] constexpr auto Where(
            Expected&& expected, Projection&& projection = {}, Comparator&& comparator = {}) {
            return Filter([expected = std::decay_t<Expected>(std::forward<Expected>(expected)),
                           projection = std::decay_t<Projection>(std::forward<Projection>(projection)),
                           comparator = std::decay_t<Comparator>(std::forward<Comparator>(comparator))](auto&& value)
                          mutable {
                return std::invoke(comparator, std::invoke(projection, std::forward<decltype(value)>(value)), expected);
            });
        }

        /**
         * @brief Lazily flatten a range whose elements are themselves ranges.
         * @details Inner ranges are traversed on demand. The returned view borrows lvalue inner ranges and owns
         * viewable rvalue ranges according to @c std::views::join. No elements are copied merely by constructing it.
         * @return A lazy joined view.
         * @code{.cpp}
         * auto batches = std::array{std::vector{1, 2}, std::vector{3}};
         * auto flattened = batches | Sora::View::Join();
         * auto output = std::ranges::to<std::vector>(flattened); // [1, 2, 3]
         * @endcode
         */
        [[nodiscard]] constexpr auto Join() {
            return Fn([](auto&& range) { return std::views::join(std::forward<decltype(range)>(range)); });
        }

        /**
         * @brief Transform each element into a range and lazily flatten the result.
         * @details This is equivalent to @ref Transform followed by @ref Join. The mapping callable and source
         * ranges retain the standard view ownership rules, and inner ranges are consumed only as requested.
         * @tparam F Callable returning a range compatible with @c std::views::join for each source element.
         * @param[in] function Range-producing mapping callable.
         * @return A lazy flattened mapping view.
         * @code{.cpp}
         * auto words = std::array{std::string{"ab"}, std::string{"c"}};
         * auto chars = words | Sora::View::FlatMap([](const auto& word) { return word; });
         * auto output = std::ranges::to<std::vector>(chars); // ['a', 'b', 'c']
         * @endcode
         */
        template<typename F>
        [[nodiscard]] constexpr auto FlatMap(F&& function) {
            return Transform(std::forward<F>(function)) | Join();
        }

        /**
         * @brief Cache the latest dereference of the piped input range.
         * @details Repeated dereference of the same iterator reuses one cached result until increment. Lvalue
         * references are cached as non-owning pointers; proxy and value references are materialized in view-owned
         * storage. The cache belongs to this view and is shared by its iterators; copying the view creates an
         * independent cache.
         * @return A lazy input view with per-position dereference caching.
         * @code{.cpp}
         * auto values = std::array{1, 2};
         * auto stable = values | Sora::View::Transform(expensive) | Sora::View::CacheLatest();
         * auto iterator = stable.begin();
         * auto first = *iterator;
         * auto same = *iterator; // first == same; expensive runs once
         * @endcode
         */
        [[nodiscard]] constexpr auto CacheLatest() {
            return Fn([]<std::ranges::viewable_range R>(R&& range) {
                return CacheLatestView{std::views::all(std::forward<R>(range))};
            });
        }

        /**
         * @brief Lazily compute a seeded prefix accumulation.
         * @details For input @c [x0, x1, ...], the output is @c [f(init,x0), f(previous,x1), ...]. Construction
         * performs no source traversal; each increment computes at most the next prefix. The accumulator is owned by
         * the view, copied into each iterator, and must be constructible from the callable result. A forward-range
         * source can therefore be traversed repeatedly without consuming the stored seed.
         * @tparam F Binary accumulation callable.
         * @tparam Initial Initial accumulator type.
         * @param[in] function Callable receiving accumulator and current element.
         * @param[in] initial Copyable seed value copied or moved into the view.
         * @return A lazy input view with one result per source element; an empty source yields an empty view.
         * @code{.cpp}
         * auto values = std::array{1, 2, 3};
         * auto totals = values | Sora::View::Scan(std::plus{}, 0);
         * auto output = std::ranges::to<std::vector>(totals); // [1, 3, 6]
         * @endcode
         */
        template<typename F, typename Initial>
        [[nodiscard]] constexpr auto Scan(F&& function, Initial&& initial) {
            return Fn(Detail::ScanFunction<std::decay_t<F>, std::decay_t<Initial>>{
                std::forward<F>(function), std::forward<Initial>(initial)});
        }

        /**
         * @brief Lazily compute an unseeded prefix accumulation.
         * @details The first output is the first source value; subsequent outputs apply @p function to the previous
         * result and the next source value. Thus @ref PartialSum differs from @ref Scan by not combining a seed.
         * Construction is lazy and preserves the input length.
         * @tparam F Binary accumulation callable, defaulting to @c std::plus<>.
         * @param[in] function Accumulation callable.
         * @return A lazy input view; an empty source yields an empty view.
         * @code{.cpp}
         * auto values = std::array{1, 2, 3};
         * auto running = values | Sora::View::PartialSum();
         * auto output = std::ranges::to<std::vector>(running); // [1, 3, 6]
         * @endcode
         */
        template<typename F = std::plus<>>
        [[nodiscard]] constexpr auto PartialSum(F&& function = {}) {
            return Fn(Detail::PartialSumFunction<std::decay_t<F>>{std::forward<F>(function)});
        }

        /**
         * @brief Zip viewable ranges and stop when the shortest input ends.
         * @details The result is the standard lazy zip view. Each lvalue range is referenced, while a viewable rvalue
         * range is owned by the resulting view. Elements are exposed as tuple-like references.
         * @tparam Ranges Viewable input ranges.
         * @param[in] ranges Ranges to traverse in lockstep.
         * @return A lazy tuple-like zip view.
         * @code{.cpp}
         * auto left = std::array{1, 2};
         * auto right = std::array{10, 20};
         * auto pairs = Sora::View::Zip(left, right);
         * auto output = std::ranges::to<std::vector>(pairs); // [(1, 10), (2, 20)]
         * @endcode
         */
        template<std::ranges::viewable_range... Ranges>
        [[nodiscard]] constexpr auto Zip(Ranges&&... ranges) {
            return std::views::zip(std::forward<Ranges>(ranges)...);
        }

        /**
         * @brief Zip the piped range with additional viewable ranges.
         * @details Use this adapter as @c source | ZipWith(other). Lvalue arguments remain references; viewable rvalue
         * arguments are moved into the adapter and then into the resulting zip view, so the returned view owns them.
         * @tparam Ranges Additional viewable ranges.
         * @param[in] ranges Ranges traversed alongside the piped range.
         * @return A lazy tuple-like zip view ending at the shortest range.
         * @code{.cpp}
         * auto values = std::array{'a', 'b'};
         * auto indices = std::array{0, 1};
         * auto pairs = values | Sora::View::ZipWith(indices);
         * auto output = std::ranges::to<std::vector>(pairs); // [('a', 0), ('b', 1)]
         * @endcode
         */
        template<std::ranges::viewable_range... Ranges>
        [[nodiscard]] constexpr auto ZipWith(Ranges&&... ranges) {
            using Others = std::tuple<std::views::all_t<Ranges>...>;
            return Fn(Detail::ZipWithFunction<Others>{
                Others{std::views::all(std::forward<Ranges>(ranges))...}});
        }

        /**
         * @brief Zip ranges and lazily invoke @p function on each element tuple.
         * @details The callable receives the elements as separate arguments. The zip view stops at the shortest
         * range, and all range ownership follows @c std::views::zip.
         * @tparam F Callable accepting one element from each range.
         * @tparam Ranges Viewable ranges.
         * @param[in] function Element-wise transformation callable.
         * @param[in] ranges Ranges traversed in lockstep.
         * @return A lazy transformed zip view.
         * @code{.cpp}
         * auto left = std::array{1, 2};
         * auto right = std::array{10, 20};
         * auto sums = Sora::View::ZipTransform(std::plus{}, left, right);
         * auto output = std::ranges::to<std::vector>(sums); // [11, 22]
         * @endcode
         */
        template<typename F, std::ranges::viewable_range... Ranges>
        [[nodiscard]] constexpr auto ZipTransform(F&& function, Ranges&&... ranges) {
            return std::views::zip(std::forward<Ranges>(ranges)...) |
                   std::views::transform([function = std::decay_t<F>(std::forward<F>(function))](auto&& values) mutable
                                         -> decltype(auto) {
                       return std::apply(function, std::forward<decltype(values)>(values));
                   });
        }

        /**
         * @brief Zip the piped range with other ranges and lazily invoke @p function.
         * @details This adapter is the piped counterpart of @ref ZipTransform. The first range is supplied by the
         * pipe, additional lvalue ranges are referenced, and viewable rvalue ranges are owned by the result.
         * @tparam F Callable accepting one element from each range.
         * @tparam Ranges Additional viewable ranges.
         * @param[in] function Element-wise transformation callable.
         * @param[in] ranges Additional ranges traversed in lockstep.
         * @return A lazy transformed zip view ending at the shortest range.
         * @code{.cpp}
         * auto left = std::array{1, 2};
         * auto right = std::array{10, 20};
         * auto sums = left | Sora::View::ZipWithTransform(std::plus{}, right);
         * auto output = std::ranges::to<std::vector>(sums); // [11, 22]
         * @endcode
         */
        template<typename F, std::ranges::viewable_range... Ranges>
        [[nodiscard]] constexpr auto ZipWithTransform(F&& function, Ranges&&... ranges) {
            return ZipWith(std::forward<Ranges>(ranges)...) |
                   Transform([function = std::decay_t<F>(std::forward<F>(function))](auto&& values) mutable
                                 -> decltype(auto) {
                       return std::apply(function, std::forward<decltype(values)>(values));
                   });
        }

        /**
         * @brief Zip a range with monotonically increasing indices.
         * @details The index starts at @p start and advances once per source element. The source follows standard
         * view ownership rules, while the index sequence is generated lazily.
         * @tparam I Integral index type.
         * @param[in] start First index value.
         * @return A lazy tuple-like view of @c (index, value) pairs.
         * @code{.cpp}
         * auto values = std::array{'x', 'y'};
         * auto numbered = values | Sora::View::Enumerate(1);
         * auto output = std::ranges::to<std::vector>(numbered); // [(1, 'x'), (2, 'y')]
         * @endcode
         */
        template<std::integral I = size_t>
        [[nodiscard]] constexpr auto Enumerate(I start = {}) {
            return Fn([start](auto&& range) {
                return std::views::zip(std::views::iota(start), std::forward<decltype(range)>(range));
            });
        }

        /**
         * @brief Group adjacent elements while @p predicate holds.
         * @details The predicate compares neighboring elements and is evaluated lazily as groups are traversed. The
         * result follows @c std::views::chunk_by and therefore requires a forward range.
         * @tparam P Predicate comparing adjacent elements.
         * @param[in] predicate Adjacency predicate.
         * @return A lazy view of contiguous subranges.
         * @code{.cpp}
         * auto values = std::array{1, 1, 2, 2, 3};
         * auto runs = values | Sora::View::ChunkBy(std::equal_to{});
         * auto output = std::ranges::to<std::vector>(runs); // [[1, 1], [2, 2], [3]]
         * @endcode
         */
        template<typename P>
        [[nodiscard]] constexpr auto ChunkBy(P&& predicate) {
            return Fn(Detail::ChunkByFunction<std::decay_t<P>>{std::forward<P>(predicate)});
        }

        /**
         * @brief Emit every @p stride-th source element.
         * @details The first source element is emitted, then each increment skips @p stride - 1 elements. A zero
         * stride is defined as an empty view. The source may be any input range and is borrowed or owned according to
         * @c std::views::all.
         * @param[in] stride Number of source positions between emitted elements.
         * @return A lazy input view, empty when @p stride is zero or the source is empty.
         * @code{.cpp}
         * auto values = std::array{0, 1, 2, 3, 4, 5};
         * auto samples = values | Sora::View::Stride(3);
         * auto output = std::ranges::to<std::vector>(samples); // [0, 3]
         * @endcode
         */
        [[nodiscard]] constexpr auto Stride(size_t stride) { return Fn(Detail::StrideFunction{stride}); }

        /**
         * @brief Partition a forward range into non-overlapping chunks.
         * @details Each output is a subrange with at most @p width elements. The source must model
         * @c std::ranges::forward_range so each chunk can be represented as a stable subrange. A zero width is an
         * empty view; a nonzero width larger than the source yields one final partial chunk.
         * @param[in] width Maximum elements per chunk.
         * @return A lazy view of subranges.
         * @code{.cpp}
         * auto values = std::array{1, 2, 3, 4, 5};
         * auto groups = values | Sora::View::Chunk(4);
         * auto output = std::ranges::to<std::vector>(groups); // [[1, 2, 3, 4], [5]]
         * @endcode
         */
        [[nodiscard]] constexpr auto Chunk(size_t width) { return Fn(Detail::ChunkFunction{width}); }

        /**
         * @brief Emit overlapping windows from a forward range.
         * @details Each output contains exactly @p width elements. If the remaining source is shorter than @p width,
         * no trailing window is emitted. The source must model @c std::ranges::forward_range. A zero width is an
         * empty view.
         * @param[in] width Number of elements in each window.
         * @return A lazy view of overlapping subranges.
         * @code{.cpp}
         * auto values = std::array{1, 2, 3, 4};
         * auto windows = values | Sora::View::Slide(3);
         * auto output = std::ranges::to<std::vector>(windows); // [[1, 2, 3], [2, 3, 4]]
         * @endcode
         */
        [[nodiscard]] constexpr auto Slide(size_t width) { return Fn(Detail::SlideFunction{width}); }

        /**
         * @brief Treat dereferenced lvalue elements as rvalues.
         * @details This adapter delegates to @c std::views::as_rvalue. It does not transfer ownership of the source;
         * it only changes the value category observed by downstream consumers, which is useful for moving elements out
         * of the source range.
         * @return A lazy rvalue view of the piped range.
         * @code{.cpp}
         * auto values = std::array{std::string{"a"}, std::string{"b"}};
         * auto moved = values | Sora::View::AsRvalue();
         * auto output = std::ranges::to<std::vector>(moved); // moves "a" and "b"
         * @endcode
         */
        [[nodiscard]] constexpr auto AsRvalue() {
            return Fn([](auto&& range) { return std::views::as_rvalue(std::forward<decltype(range)>(range)); });
        }

    } // namespace View

} // namespace Sora
