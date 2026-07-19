/**
 * @file AnnotationTraits.h
 * @brief C++26 reflection helpers for annotation queries and annotation extraction.
 * @ingroup Core
 *
 * @details Provides compile-time utilities for detecting, counting, and extracting P3394-style annotations from
 * reflected declarations and types. The utilities are grouped under @c Sora::$ because they are intended as compact
 * reflection vocabulary for annotation-heavy code, while the public concepts live under @c Sora::Concept.
 */
#pragma once

#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Core/Traits/ScopeTraits.h"

#include <cstddef>
#include <initializer_list>
#include <meta>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace Sora {

    namespace Meta {

        /**
         * @brief Return a static array of annotations for a given reflected entity.
         * @param[in] ent Reflected entity whose annotations are requested.
         * @return Static array of reflected annotations on @p ent.
         */
        consteval auto AnnotationsOf(std::meta::info ent) {
            return std::define_static_array(std::meta::annotations_of(ent));
        }

    } // namespace Meta

    namespace $ {

        /** @brief Marker annotation used to opt a regular class into @ref Concept::Annotation. */
        struct AsAnnotation {};

        /**
         * @brief Return whether type @p T has at least one annotation of type @p A.
         * @tparam T Annotated type to inspect.
         * @tparam A Annotation type to search for.
         */
        template<typename T, typename A>
        consteval bool Has() {
            return std::meta::annotations_of(^^T, ^^A).size() > 0;
        }

        /**
         * @brief Return whether reflected entity @p ent has at least one annotation of type @p A.
         * @tparam A Annotation type to search for.
         * @param[in] ent Reflected entity to inspect.
         */
        template<typename A>
        consteval bool Has(std::meta::info ent) {
            return std::meta::annotations_of(ent, ^^A).size() > 0;
        }

        /**
         * @brief Return whether reflected entity @p ent has at least one annotation reflected by @p annotation.
         * @param[in] ent Reflected entity to inspect.
         * @param[in] annotation Reflection of the annotation type to search for.
         */
        consteval bool Has(std::meta::info ent, std::meta::info annotation) {
            return std::meta::annotations_of(ent, annotation).size() > 0;
        }

        /** @brief Return whether @p T has any annotation in @p As. */
        template<typename T, typename... As>
        consteval bool HasAny() {
            return (Has<T, As>() || ...);
        }

        /** @brief Return whether reflected entity @p ent has any annotation in @p As. */
        template<typename... As>
        consteval bool HasAny(std::meta::info ent) {
            return (($::Has<As>(ent)) || ...);
        }

        /** @brief Return whether reflected entity @p ent has any annotation in @p annotations. */
        consteval bool HasAny(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            for (auto a : annotations) {
                if ($::Has(ent, a)) {
                    return true;
                }
            }
            return false;
        }

        /** @brief Return whether @p T has every annotation in @p As. */
        template<typename T, typename... As>
        consteval bool HasAll() {
            return (Has<T, As>() && ...);
        }

        /** @brief Return whether reflected entity @p ent has every annotation in @p As. */
        template<typename... As>
        consteval bool HasAll(std::meta::info ent) {
            return (($::Has<As>(ent)) && ...);
        }

        /** @brief Return whether reflected entity @p ent has every annotation in @p annotations. */
        consteval bool HasAll(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            for (auto a : annotations) {
                if (!$::Has(ent, a)) {
                    return false;
                }
            }
            return true;
        }

        /** @brief Return whether @p T has none of the annotations in @p As. */
        template<typename T, typename... As>
        consteval bool HasNone() {
            return !(Has<T, As>() || ...);
        }

        /** @brief Return whether reflected entity @p ent has none of the annotations in @p As. */
        template<typename... As>
        consteval bool HasNone(std::meta::info ent) {
            return !(($::Has<As>(ent)) || ...);
        }

        /** @brief Return whether reflected entity @p ent has none of the annotations in @p annotations. */
        consteval bool HasNone(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            for (auto a : annotations) {
                if ($::Has(ent, a)) {
                    return false;
                }
            }
            return true;
        }

        /** @brief Count annotations of types @p As on @p T. */
        template<typename T, typename... As>
        consteval size_t Count() {
            return (... + std::meta::annotations_of(^^T, ^^As).size());
        }

        /** @brief Count annotations of types @p As on reflected entity @p ent. */
        template<typename... As>
        consteval size_t Count(std::meta::info ent) {
            return (... + std::meta::annotations_of(ent, ^^As).size());
        }

        /** @brief Count annotations listed in @p annotations on reflected entity @p ent. */
        consteval size_t Count(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            size_t count = 0;
            for (auto a : annotations) {
                count += std::meta::annotations_of(ent, a).size();
            }
            return count;
        }

        /** @brief Count annotations of reflected annotation type @p annotation on reflected entity @p ent. */
        consteval size_t Count(std::meta::info ent, std::meta::info annotation) {
            return std::meta::annotations_of(ent, annotation).size();
        }

        /** @brief Variant type used when extraction accepts multiple annotation classes. */
        template<typename... As>
        using MultiAnnoVariant = typename std::conditional_t<sizeof...(As) == 1, As...[0], std::variant<As...>>;

        /**
         * @brief Return all matching annotation reflections for @p ent.
         * @param[in] ent Reflected entity to inspect.
         * @param[in] annotations Reflected annotation types to collect.
         * @return Static-storage array containing matching annotation reflections in query order.
         */
        consteval auto GetAll(std::meta::info ent, std::initializer_list<std::meta::info> annotations = {}) {
            std::vector<std::meta::info> results;
            if (annotations.size() == 0) {
                results = std::meta::annotations_of(ent);
            } else {
                for (auto a : annotations) {
                    auto annots = std::meta::annotations_of(ent, a);
                    results.insert_range(results.end(), annots);
                }
            }
            return std::define_static_array(results);
        }

        /** @brief Return extracted annotation values of types @p As from reflected entity @p ent. */
        template<typename... As>
        consteval auto GetAll(std::meta::info ent) -> std::vector<MultiAnnoVariant<As...>> {
            std::vector<MultiAnnoVariant<As...>> results;
            auto collect = [&]<typename A> {
                auto annots = std::meta::annotations_of(ent, ^^A);
                for (auto annot : annots) {
                    results.emplace_back(std::meta::extract<A>(annot));
                }
            };
            (collect.template operator()<As>(), ...);
            return results;
        }

        /** @brief Return extracted annotation values of types @p As from type @p T. */
        template<typename T, typename... As>
        consteval auto GetAll() {
            return GetAll<As...>(^^T);
        }

        /** @brief Return the first matching annotation reflection for @p ent. */
        consteval auto GetFirst(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            std::vector<std::meta::info> results;
            for (auto a : annotations) {
                auto annots = std::meta::annotations_of(ent, a);
                results.insert_range(results.end(), annots);
            }
            return results.size() ? std::optional<std::meta::info>{results[0]} : std::nullopt;
        }

        /** @brief Return the first extracted annotation value of types @p As from reflected entity @p ent. */
        template<typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetFirst(std::meta::info ent) {
            std::optional<MultiAnnoVariant<As...>> result;
            auto collect = [&]<typename A> {
                if (!result.has_value()) {
                    auto annots = std::meta::annotations_of(ent, ^^A);
                    if (!annots.empty()) {
                        result = std::meta::extract<A>(annots.front());
                    }
                }
            };
            (collect.template operator()<As>(), ...);
            return result;
        }

        /** @brief Return the first extracted annotation value of types @p As from type @p T. */
        template<typename T, typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetFirst() {
            return GetFirst<As...>(^^T);
        }

        /** @brief Return the last matching annotation reflection for @p ent. */
        consteval auto GetLast(std::meta::info ent, std::initializer_list<std::meta::info> annotations) {
            std::vector<std::meta::info> results;
            for (auto a : annotations) {
                auto annots = std::meta::annotations_of(ent, a);
                results.insert_range(results.end(), annots);
            }
            return results.size() ? std::optional<std::meta::info>{results[results.size() - 1]} : std::nullopt;
        }

        /** @brief Return the last extracted annotation value of types @p As from reflected entity @p ent. */
        template<typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetLast(std::meta::info ent) {
            std::optional<MultiAnnoVariant<As...>> result;
            auto collect = [&]<typename A> {
                auto annots = std::meta::annotations_of(ent, ^^A);
                if (!annots.empty()) {
                    result = std::meta::extract<A>(annots.back());
                }
            };
            (collect.template operator()<As>(), ...);
            return result;
        }

        /** @brief Return the last extracted annotation value of types @p As from type @p T. */
        template<typename T, typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetLast() {
            return GetLast<As...>(^^T);
        }

        /**
         * @brief Return exactly one annotation value of reflected annotation type @p Annotation.
         * @throws std::logic_error when no matching annotation exists or more than one exists.
         */
        template<std::meta::info Annotation>
        consteval auto GetSingle(std::meta::info ent) -> [:Annotation:] {
            auto annots = std::meta::annotations_of(ent, Annotation);

            if (annots.size() > 1) {
                throw std::logic_error("Multiple annotations of type found");
            }
            if (annots.empty()) {
                throw std::logic_error("No annotations of type found");
            }

            return std::meta::extract<Meta::InfoType<Annotation>>(annots.front());
        }

        /**
         * @brief Return exactly one extracted annotation value of types @p As from reflected entity @p ent.
         * @throws std::logic_error when no matching annotation exists or more than one exists.
         */
        template<typename... As>
        consteval MultiAnnoVariant<As...> GetSingle(std::meta::info ent) {
            std::optional<MultiAnnoVariant<As...>> result;
            size_t count = 0;
            auto collect = [&]<typename A> {
                auto annots = std::meta::annotations_of(ent, ^^A);
                count += annots.size();
                if (!annots.empty() && !result.has_value()) {
                    result = std::meta::extract<A>(annots.front());
                }
            };
            (collect.template operator()<As>(), ...);
            if (count > 1) {
                throw std::logic_error("Multiple annotations of type found");
            }
            if (!result.has_value()) {
                throw std::logic_error("No annotations of type found");
            }
            return *result;
        }

        /** @brief Return exactly one extracted annotation value of types @p As from type @p T. */
        template<typename T, typename... As>
        consteval MultiAnnoVariant<As...> GetSingle() {
            return GetSingle<As...>(^^T);
        }

        /**
         * @brief Return zero or one annotation value for reflected annotation types @p As.
         * @throws std::logic_error when more than one matching annotation exists.
         */
        template<std::meta::info... As>
        consteval auto GetSingleOptional(std::meta::info ent)
            -> std::optional<MultiAnnoVariant<Meta::InfoType<As>...>> {
            std::optional<MultiAnnoVariant<Meta::InfoType<As>...>> result;
            size_t count = 0;
            auto collect = [&]<std::meta::info A> {
                using AnnotationType = Meta::InfoType<A>;
                auto annots = std::meta::annotations_of(ent, A);
                count += annots.size();
                if (!annots.empty() && !result.has_value()) {
                    result = std::meta::extract<AnnotationType>(annots.front());
                }
            };
            (collect.template operator()<As>(), ...);
            if (count > 1) {
                throw std::logic_error("Multiple annotations of type found");
            }
            return result;
        }

        /**
         * @brief Return zero or one extracted annotation value of types @p As from reflected entity @p ent.
         * @throws std::logic_error when more than one matching annotation exists.
         */
        template<typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetSingleOptional(std::meta::info ent) {
            std::optional<MultiAnnoVariant<As...>> result;
            size_t count = 0;
            auto collect = [&]<typename A> {
                auto annots = std::meta::annotations_of(ent, ^^A);
                count += annots.size();
                if (!annots.empty() && !result.has_value()) {
                    result = std::meta::extract<A>(annots.front());
                }
            };
            (collect.template operator()<As>(), ...);
            if (count > 1) {
                throw std::logic_error("Multiple annotations of type found");
            }
            return result;
        }

        /** @brief Return zero or one extracted annotation value of types @p As from type @p T. */
        template<typename T, typename... As>
        consteval std::optional<MultiAnnoVariant<As...>> GetSingleOptional() {
            return GetSingleOptional<As...>(^^T);
        }

    } // namespace $

    namespace Concept {

        /** @brief Type that is recognized as an annotation by scope, marker annotation, or reflection metadata. */
        template<typename T>
        concept Annotation =
            std::is_class_v<T> && (Meta::IsInScope(^^T, ^^Sora::$) || Sora::$::Has<Sora::$::AsAnnotation>(^^T) ||
                                   std::meta::is_annotation(^^T));

        /** @brief Class type with at least one annotation. */
        template<typename T>
        concept Annotated = std::is_class_v<T> && std::meta::annotations_of(^^T).size() > 0;

        /** @brief Class type annotated with reflected annotation type @p A. */
        template<typename T, std::meta::info A>
        concept AnnotatedWith = std::is_class_v<T> && $::Count(^^T, A) > 0;

        /** @brief Class type annotated with at least one reflected annotation type in @p As. */
        template<typename T, std::meta::info... As>
        concept AnnotatedWithAny = std::is_class_v<T> && (($::Count(^^T, As) > 0) || ...);

        /** @brief Class type annotated with every reflected annotation type in @p As. */
        template<typename T, std::meta::info... As>
        concept AnnotatedWithAll = std::is_class_v<T> && (($::Count(^^T, As) > 0) && ...);

    } // namespace Concept

} // namespace Sora

namespace $ {

    /** @brief Global shorthand namespace for @c Sora::$ annotation helpers. */
    namespace Sora = Sora::$;

} // namespace $
