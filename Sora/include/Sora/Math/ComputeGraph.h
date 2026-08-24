/**
 * @file ComputeGraph.h
 * @brief Typed Math elementwise graph construction with automatic program registration.
 * @ingroup Math
 */
#pragma once

#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/Runtime/PortablePackageRegistry.h>
#include <Sora/Math/Elementwise.h>
#include <Sora/Math/Point.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Math {

    /** @brief Exact failure from value encoding, invocation binding, registration, or graph mutation. */
    using ElementwiseGraphError = std::variant<Compute::ScalarArgumentError, Compute::KernelInvocationError,
                                               Compute::PortablePackageRegistryError, Compute::GraphError>;

    namespace ComputeGraphDetail {

        template<size_t Size>
        [[nodiscard]] consteval auto UnsignedBitsType() {
            if constexpr (Size == 1) {
                return std::type_identity<uint8_t>{};
            } else if constexpr (Size == 2) {
                return std::type_identity<uint16_t>{};
            } else if constexpr (Size == 4) {
                return std::type_identity<uint32_t>{};
            } else if constexpr (Size == 8) {
                return std::type_identity<uint64_t>{};
            } else {
                static_assert(Size == 0, "portable scalar has no canonical integer representation");
            }
        }

        template<Compute::PortableScalar T>
        constexpr void EncodeScalar(T value, std::span<std::byte, sizeof(T)> output) noexcept {
            using Bits = typename decltype(UnsignedBitsType<sizeof(T)>())::type;
            Bits bits{};
            if constexpr (std::same_as<T, bool>) {
                static_assert(sizeof(bool) == 1);
                bits = value ? Bits{1} : Bits{0};
            } else {
                bits = std::bit_cast<Bits>(value);
            }
            for (size_t index = 0; index < sizeof(T); ++index) {
                output[index] = static_cast<std::byte>(bits & Bits{0xFF});
                bits >>= 8U;
            }
        }

        template<Compute::PortableScalar T, size_t N>
            requires(N > 0 && N <= std::numeric_limits<uint8_t>::max())
        [[nodiscard]] std::expected<Compute::ScalarArgument, Compute::ScalarArgumentError>
        MakeScalarArgument(std::span<const T, N> values) noexcept {
            if constexpr (std::endian::native == std::endian::little) {
                return Compute::ScalarArgument::Create(
                    {.scalar = Compute::ScalarKindOf<T>(), .lanes = static_cast<uint8_t>(N)}, std::as_bytes(values));
            }
            std::array<std::byte, sizeof(T) * N> bytes{};
            for (size_t index = 0; index < N; ++index) {
                const std::span<std::byte, sizeof(T)> encoded{bytes.data() + index * sizeof(T), sizeof(T)};
                EncodeScalar(values[index], encoded);
            }
            return Compute::ScalarArgument::Create(
                {.scalar = Compute::ScalarKindOf<T>(), .lanes = static_cast<uint8_t>(N)}, bytes);
        }

        [[nodiscard]] inline std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>
        ToBinding(Compute::BufferView value) noexcept {
            return Compute::KernelArgumentBinding{std::in_place_type<Compute::BufferView>, std::move(value)};
        }

        [[nodiscard]] inline std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>
        ToBinding(Compute::ScalarArgument value) noexcept {
            return Compute::KernelArgumentBinding{std::in_place_type<Compute::ScalarArgument>, std::move(value)};
        }

        template<Compute::PortableScalar T>
        [[nodiscard]] std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>
        ToBinding(T value) noexcept {
            const std::array values{value};
            auto scalar = MakeScalarArgument(std::span<const T, 1>{values});
            if (!scalar) {
                return std::unexpected(scalar.error());
            }
            return Compute::KernelArgumentBinding{std::in_place_type<Compute::ScalarArgument>, std::move(*scalar)};
        }

        template<Compute::PortableScalar T, size_t N>
        [[nodiscard]] std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>
        ToBinding(const Vector<T, N>& value) noexcept {
            auto scalar = MakeScalarArgument(std::span<const T, N>{value.Data(), N});
            if (!scalar) {
                return std::unexpected(scalar.error());
            }
            return Compute::KernelArgumentBinding{std::in_place_type<Compute::ScalarArgument>, std::move(*scalar)};
        }

        template<Compute::PortableScalar T, size_t N>
        [[nodiscard]] std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>
        ToBinding(const Point<T, N>& value) noexcept {
            auto scalar = MakeScalarArgument(std::span<const T, N>{value.Data(), N});
            if (!scalar) {
                return std::unexpected(scalar.error());
            }
            return Compute::KernelArgumentBinding{std::in_place_type<Compute::ScalarArgument>, std::move(*scalar)};
        }

        template<typename T>
        concept ElementwiseBinding = requires(T&& value) {
            {
                ToBinding(std::forward<T>(value))
            } -> std::same_as<std::expected<Compute::KernelArgumentBinding, Compute::ScalarArgumentError>>;
        };

        template<typename Error>
        [[nodiscard]] std::unexpected<ElementwiseGraphError> GraphFailure(Error error) noexcept {
            return std::unexpected(ElementwiseGraphError{std::in_place_type<Error>, std::move(error)});
        }

    } // namespace ComputeGraphDetail

    template<typename Context>
    class ElementwiseGraphBuilder;

    /**
     * @brief Move-only logical graph bound to the context that owns all referenced program registrations.
     * @tparam Context Compute context used during construction and submission.
     */
    template<typename Context>
    class ElementwiseGraph final {
    public:
        static_assert(std::is_nothrow_copy_constructible_v<Context>,
                      "ElementwiseGraph requires a nothrow-shareable compute context");

        ElementwiseGraph(const ElementwiseGraph&) = delete;
        ElementwiseGraph& operator=(const ElementwiseGraph&) = delete;
        ElementwiseGraph(ElementwiseGraph&&) noexcept = default;
        ElementwiseGraph& operator=(ElementwiseGraph&&) noexcept = default;

        /** @brief Inspect the immutable logical graph without detaching it from its registered context. */
        [[nodiscard]] const Compute::ExecutionGraph& LogicalGraph() const noexcept { return graph_; }

        /** @brief Return the logical node count. */
        [[nodiscard]] size_t NodeCount() const noexcept { return graph_.NodeCount(); }

        /**
         * @brief Submit this graph through its construction context and consume the graph value.
         * @tparam Scheduler Scheduler accepted by @c Context::Submit.
         * @param[in] scheduler Structured-execution scheduler on which preparation begins.
         * @return The context's lazy recovering submission sender.
         */
        template<typename Scheduler>
            requires requires(Context& context, Scheduler scheduler, Compute::ExecutionGraph graph) {
                context.Submit(std::move(scheduler), std::move(graph));
            }
        [[nodiscard]] auto Submit(Scheduler scheduler) && {
            return context_.Submit(std::move(scheduler), std::move(graph_));
        }

        /** @brief Submit while forwarding preparation observations to @p observer. */
        template<typename Scheduler, typename Observer>
            requires requires(Context& context, Scheduler scheduler, Compute::ExecutionGraph graph, Observer observer) {
                context.SubmitObserved(std::move(scheduler), std::move(graph), std::move(observer));
            }
        [[nodiscard]] auto SubmitObserved(Scheduler scheduler, Observer observer) && {
            return context_.SubmitObserved(std::move(scheduler), std::move(graph_), std::move(observer));
        }

        /** @brief Consume this graph into a context-bound reusable logical preparation. */
        [[nodiscard]] auto Prepare() &&
            requires requires(Context& context, Compute::ExecutionGraph graph) {
                context.Prepare(std::move(graph));
            }
        {
            return context_.Prepare(std::move(graph_));
        }

    private:
        friend class ElementwiseGraphBuilder<Context>;

        ElementwiseGraph(const Context& context, Compute::ExecutionGraph graph) noexcept
            : context_(context), graph_(std::move(graph)) {}

        Context context_;
        Compute::ExecutionGraph graph_;
    };

    /**
     * @brief Build a typed logical graph while registering every referenced Math program in its execution context.
     * @tparam Context Compute context exposing the canonical @c RegisterProgram operation.
     * @pre The bound context outlives this builder. The finished graph retains its own shared context handle.
     * @details Registration is context-scoped and idempotent. A failed binding may leave no graph mutation; a graph
     * mutation failure may leave only a reusable program-cache entry. Backend selection remains deferred to submit.
     */
    template<typename Context>
    class ElementwiseGraphBuilder final {
    public:
        /** @brief Bind this graph builder to the context that will submit the finished graph. */
        explicit ElementwiseGraphBuilder(Context& context) noexcept : context_(&context) {}

        ElementwiseGraphBuilder(const ElementwiseGraphBuilder&) = delete;
        ElementwiseGraphBuilder& operator=(const ElementwiseGraphBuilder&) = delete;
        ElementwiseGraphBuilder(ElementwiseGraphBuilder&&) noexcept = default;
        ElementwiseGraphBuilder& operator=(ElementwiseGraphBuilder&&) noexcept = default;

        /**
         * @brief Add one elementwise transform, deriving dispatch shape from @p output.
         * @tparam Definition Compiled elementwise definition carrying the exact program signature.
         * @tparam Inputs Scalar, short-vector, or logical-buffer bindings in expression-parameter order.
         * @param[in] output Exact-layout logical output view; its extents become the dispatch shape.
         * @param[in] definition Direct/portable definition generated by @ref CompileElementwise.
         * @param[in] inputs Runtime input bindings in expression-parameter order.
         * @return The inserted graph node or the exact failed construction stage.
         * @details Scalar and short-vector values are encoded canonically; buffer arguments retain logical ownership.
         * The output is appended in the exact signature position generated by @ref CompileElementwise.
         */
        template<CompiledElementwiseDefinition Definition, ComputeGraphDetail::ElementwiseBinding... Inputs>
            requires requires(Context& context, const Definition& definition) {
                {
                    context.RegisterProgram(definition.Program())
                } -> std::same_as<std::expected<Compute::PortablePackageLease, Compute::PortablePackageRegistryError>>;
            }
        [[nodiscard]] std::expected<Compute::GraphNodeId, ElementwiseGraphError>
        Transform(Compute::BufferView output, const Definition& definition, Inputs&&... inputs) noexcept {
            try {
                std::vector<Compute::KernelArgumentBinding> bindings;
                bindings.reserve(sizeof...(Inputs) + 1);
                std::optional<Compute::ScalarArgumentError> scalarError;
                auto append = [&]<typename Input>(Input&& input) {
                    if (scalarError) {
                        return;
                    }
                    auto binding = ComputeGraphDetail::ToBinding(std::forward<Input>(input));
                    if (!binding) {
                        scalarError = binding.error();
                        return;
                    }
                    bindings.emplace_back(std::move(*binding));
                };
                (append(std::forward<Inputs>(inputs)), ...);
                if (scalarError) {
                    return ComputeGraphDetail::GraphFailure(*scalarError);
                }

                const Compute::BufferViewDesc outputDesc = output.Describe();
                bindings.emplace_back(std::in_place_type<Compute::BufferView>, std::move(output));
                const std::span<const uint64_t> extents{outputDesc.extents.data(), outputDesc.rank};
                auto invocation = Compute::BindKernelProgram(definition.Program(), extents, std::move(bindings));
                if (!invocation) {
                    return ComputeGraphDetail::GraphFailure(invocation.error());
                }
                auto registered = context_->RegisterProgram(definition.Program());
                if (!registered) {
                    return ComputeGraphDetail::GraphFailure(registered.error());
                }
                auto node = graph_.AddDispatch(std::move(*invocation));
                if (!node) {
                    return ComputeGraphDetail::GraphFailure(node.error());
                }
                return *node;
            } catch (const std::bad_alloc&) {
                return ComputeGraphDetail::GraphFailure(Compute::GraphError{Compute::GraphErrorCode::OutOfMemory});
            }
        }

        /** @brief Return the current logical node count. */
        [[nodiscard]] size_t NodeCount() const noexcept { return graph_.NodeCount(); }

        /** @brief Derive hazards and bind the finished owning graph to its program-registration context. */
        [[nodiscard]] std::expected<ElementwiseGraph<Context>, Compute::GraphError> Finish() && noexcept {
            auto graph = std::move(graph_).Finish();
            if (!graph) {
                return std::unexpected(graph.error());
            }
            return ElementwiseGraph<Context>{*context_, std::move(*graph)};
        }

    private:
        Context* context_ = nullptr;
        Compute::GraphBuilder graph_{};
    };

    template<typename Context>
    ElementwiseGraphBuilder(Context&) -> ElementwiseGraphBuilder<Context>;

} // namespace Sora::Math
