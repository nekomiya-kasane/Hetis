/**
 * @file VisitOverloader.h
 * @brief Callable overload-set helper for @c std::visit and general dispatch.
 * @ingroup Core
 */
#pragma once

namespace Sora {

    /**
     * @brief Compose several callable objects into one overload set.
     *
     * @details Inherits @c operator() from each callable in @p Ts, producing a single callable object suitable for
     * @c std::visit and other overload-based dispatch sites. The deduction guide allows direct brace construction from
     * lambdas or other callable objects.
     *
     * @tparam Ts Callable object types.
     *
     * @code{.cpp}
     * std::variant<int, std::string, float> v = std::string{"hi"};
     * auto label = std::visit(Overload{
     *     [](int i) { return "int:" + std::to_string(i); },
     *     [](const std::string& s) { return "str:" + s; },
     *     [](auto&&) { return std::string{"other"}; },
     * }, v);
     * @endcode
     */
    template<typename... Ts>
    struct Overload : Ts... {
        using Ts::operator()...;
    };

    /**
     * @brief Deduction guide for @ref Overload.
     * @tparam Ts Callable object types deduced from constructor arguments.
     */
    template<typename... Ts>
    Overload(Ts...) -> Overload<Ts...>;

} // namespace Sora
