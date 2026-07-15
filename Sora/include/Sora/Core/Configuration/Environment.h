/**
 * @file Environment.h
 * @brief Reflection-compiled hierarchical configuration projection over the native process environment.
 * @ingroup Core
 *
 * @details Dotted logical paths are encoded as upper-snake native names with double underscores between levels. A
 * compiled schema performs all field discovery, naming, collision checks, and codec selection during translation.
 * Runtime bulk reads merge two sorted contiguous sequences and then decode directly into reflected members.
 */
#pragma once

#include <Sora/Core/Configuration/Path.h>
#include <Sora/Core/FixedString.h>
#include <Sora/Core/PAL/Environment.h>
#include <Sora/Core/ToString.h>
#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <inplace_vector>
#include <limits>
#include <meta>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Configuration {

    namespace $ {

        /** @brief Opt a reflected object into hierarchical process-environment projection. */
        struct Object {
            FixedString<64> prefix{}; /**< Native root scope placed before the first double underscore. */
        };

        /** @brief Override one member's default logical path relative to the root object. */
        struct Path {
            FixedString<128> value{}; /**< Absolute dotted logical path inside the configured root. */
        };

        /** @brief Bind one field to an exact native name, bypassing scope and dotted-path encoding. */
        struct NativeName {
            FixedString<128> value{}; /**< Exact native process-environment name. */
        };

        /** @brief Add a fallback logical path used only while reading. */
        struct Alias {
            FixedString<128> value{}; /**< Absolute dotted fallback path inside the configured root. */
        };

        /** @brief Require a field to be present through its primary name or one alias. */
        struct Required {
            constexpr bool operator==(const Required&) const = default;
        };

        /** @brief Exclude a reflected member from process-environment projection. */
        struct Ignore {
            constexpr bool operator==(const Ignore&) const = default;
        };

        /** @brief Recurse into an object without adding the containing member as a logical path segment. */
        struct Flatten {
            constexpr bool operator==(const Flatten&) const = default;
        };

    } // namespace $

    /** @brief Runtime hierarchical scope for direct dotted-path access. */
    class Scope {
    public:
        /** @brief Validate @p prefix and construct a root environment scope. */
        [[nodiscard]] static Result<Scope> Create(std::string_view prefix) {
            if (!prefix.empty() && !Sora::Configuration::IsPathSegment(prefix)) {
                return std::unexpected(ErrorCode::InvalidEnvironmentName);
            }
            return Scope{std::string{prefix}, {}};
        }

        /** @brief Return a child scope rooted at dotted @p path. */
        [[nodiscard]] Result<Scope> Child(std::string_view path) const {
            auto joined = Sora::Configuration::JoinPath(base_, path);
            if (!joined) {
                return std::unexpected(joined.error());
            }
            return Scope{prefix_, std::move(*joined)};
        }

        /** @brief Encode dotted @p path as its exact native environment-variable name. */
        [[nodiscard]] Result<std::string> NativeNameOf(std::string_view path) const {
            auto joined = Sora::Configuration::JoinPath(base_, path);
            if (!joined) {
                return std::unexpected(joined.error());
            }
            return Sora::Configuration::EncodeEnvironmentName(prefix_, *joined);
        }

        /** @brief Read one dotted path, distinguishing absence from an existing empty value. */
        [[nodiscard]] Result<std::optional<std::string>> Get(std::string_view path) const {
            auto name = NativeNameOf(path);
            if (!name) {
                return std::unexpected(name.error());
            }
            return PAL::ReadEnvironmentVariable(*name);
        }

        /** @brief Set one dotted path to UTF-8 @p value. */
        [[nodiscard]] Result<void> Set(std::string_view path, std::string_view value) const {
            auto name = NativeNameOf(path);
            if (!name) {
                return std::unexpected(name.error());
            }
            return PAL::WriteEnvironmentVariable(*name, value);
        }

        /** @brief Remove one dotted path. */
        [[nodiscard]] Result<void> Unset(std::string_view path) const {
            auto name = NativeNameOf(path);
            if (!name) {
                return std::unexpected(name.error());
            }
            return PAL::RemoveEnvironmentVariable(*name);
        }

        /** @brief Return the contiguous descendant range of dotted @p path in @p snapshot. */
        [[nodiscard]] Result<PAL::EnvironmentIndexRange> Subtree(const PAL::EnvironmentSnapshot& snapshot,
                                                                 std::string_view path) const {
            auto name = NativeNameOf(path);
            if (!name) {
                return std::unexpected(name.error());
            }
            *name += "__";
            return snapshot.PrefixRange(*name);
        }

    private:
        Scope(std::string prefix, std::string base) : prefix_{std::move(prefix)}, base_{std::move(base)} {}

        std::string prefix_;
        std::string base_;
    };

    /** @brief Stable static pointer/length string stored in a compiled environment schema. */
    struct NameEntry {
        const char* data = nullptr;
        uint32_t size = 0;

        /** @brief Project the static pointer and length as a standard string view. */
        [[nodiscard]] constexpr std::string_view Text() const noexcept {
            return size == 0 ? std::string_view{} : std::string_view{data, size};
        }
    };

    enum class IssueKind : uint8_t {
        SourceFailure,   /**< Capturing or reading the native environment failed. */
        MissingRequired, /**< A required field has no primary or alias value. */
        InvalidValue,    /**< A present value cannot be decoded as the field type. */
    };

    /** @brief One structured typed environment read or write issue. */
    struct Issue {
        IssueKind kind = IssueKind::InvalidValue;  /**< Failure category. */
        NameEntry logicalPath{};                   /**< Static logical field path. */
        NameEntry nativeName{};                    /**< Static primary or matched native name. */
        std::optional<ErrorCode> decodeError = {}; /**< Decode failure when applicable. */
        std::optional<ErrorCode> sourceError = {}; /**< Native source failure when applicable. */
    };

    template<size_t Capacity>
    struct Report {
        std::inplace_vector<Issue, Capacity> issues; /**< Issues in deterministic field order. */

        /** @brief Return whether no issues were reported. */
        [[nodiscard]] bool Empty() const noexcept { return issues.empty(); }
    };

    namespace Detail {

        template<typename T>
        struct OptionalTraits {
            static constexpr bool kIsOptional = false;
        };

        template<typename T>
        struct OptionalTraits<std::optional<T>> {
            static constexpr bool kIsOptional = true;
            using ValueType = T;
        };

        template<typename T>
        inline constexpr bool kIsOptional = OptionalTraits<std::remove_cvref_t<T>>::kIsOptional;

        template<typename T>
        using OptionalValue = typename OptionalTraits<std::remove_cvref_t<T>>::ValueType;

        template<typename T, bool = kIsOptional<T>>
        struct EnvironmentLeaf : std::bool_constant<!std::same_as<std::remove_cvref_t<T>, std::string_view> &&
                                                    Concept::StringDeserializable<T> && Concept::StringFormattable<T>> {
        };

        template<typename T>
        struct EnvironmentLeaf<T, true> : std::bool_constant<Concept::StringDeserializable<OptionalValue<T>> &&
                                                             Concept::StringFormattable<OptionalValue<T>>> {};

        template<typename T>
        inline constexpr bool kEnvironmentLeaf = EnvironmentLeaf<T>::value;

        using DecodeFieldFn = VoidResult (*)(void*, size_t, std::string_view);
        using EncodeFieldFn = std::optional<std::string> (*)(const void*, size_t);

        struct FieldDesc {
            NameEntry logicalPath{};
            NameEntry nativeName{};
            size_t offset = 0;
            DecodeFieldFn decode = nullptr;
            EncodeFieldFn encode = nullptr;
            bool required = false;
        };

        struct LookupDesc {
            NameEntry nativeName{};
            uint32_t fieldIndex = 0;
            uint32_t priority = 0;
        };

        struct Schema {
            std::span<const FieldDesc> fields;
            std::span<const LookupDesc> lookups;
        };

        template<typename Annotation>
        [[nodiscard]] consteval std::optional<Annotation> SingleAnnotation(std::meta::info entity) {
            const auto annotations = std::meta::annotations_of(entity, ^^Annotation);
            if (annotations.size() > 1) {
                throw "Sora environment declaration carries a duplicate singleton annotation.";
            }
            return annotations.empty() ? std::nullopt
                                       : std::optional<Annotation>{std::meta::extract<Annotation>(annotations.front())};
        }

        [[nodiscard]] consteval NameEntry StaticName(std::string_view text) {
            const char* stable = std::define_static_string(text);
            return NameEntry{.data = stable, .size = static_cast<uint32_t>(text.size())};
        }

        [[nodiscard]] consteval bool IsNativeName(std::string_view name) {
            if (name.empty()) {
                return false;
            }
            for (char c : name) {
                if (c == '=' || c == '\0' || static_cast<unsigned char>(c) >= 0x80u) {
                    return false;
                }
            }
            return true;
        }

        template<typename Field, typename Object>
        [[nodiscard]] Field& FieldAt(Object* object, size_t offset) noexcept {
            using Byte = std::conditional_t<std::is_const_v<Object>, const std::byte, std::byte>;
            auto* address = reinterpret_cast<Byte*>(object) + offset;
            return *std::launder(reinterpret_cast<Field*>(address));
        }

        template<typename Field>
        [[nodiscard]] VoidResult DecodeValue(Field& field, std::string_view text) {
            if constexpr (kIsOptional<Field>) {
                auto decoded = FromString(std::in_place_type<OptionalValue<Field>>, text);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                field = std::move(*decoded);
            } else {
                auto decoded = FromString(std::in_place_type<Field>, text);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                field = std::move(*decoded);
            }
            return {};
        }

        template<typename Field>
        [[nodiscard]] std::optional<std::string> EncodeValue(const Field& field) {
            if constexpr (kIsOptional<Field>) {
                if (!field) {
                    return std::optional<std::string>{};
                }
                return std::optional<std::string>{ToString(*field)};
            } else {
                return std::optional<std::string>{ToString(field)};
            }
        }

        template<typename Field>
        [[nodiscard]] VoidResult DecodeField(void* object, size_t offset, std::string_view text) {
            return DecodeValue(FieldAt<Field>(object, offset), text);
        }

        template<typename Field>
        [[nodiscard]] std::optional<std::string> EncodeField(const void* object, size_t offset) {
            return EncodeValue(FieldAt<const Field>(object, offset));
        }

        template<typename Current>
        consteval void Collect(std::vector<FieldDesc>& fields, std::vector<LookupDesc>& lookups,
                               std::string_view prefix, std::string_view parentPath, size_t parentOffset) {
            template for (constexpr std::meta::info member : Traits::PublicDataMembers<Current>) {
                if constexpr (Sora::$::Has<Sora::Configuration::$::Ignore>(member)) {
                    continue;
                }

                using RawField = typename [:std::meta::type_of(member):];
                using Field = std::remove_cvref_t<RawField>;
                if (std::meta::is_bit_field(member)) {
                    throw "Sora environment projection does not support bit-field members.";
                }
                const size_t fieldOffset = parentOffset + static_cast<size_t>(std::meta::offset_of(member).bytes);
                std::string logicalPath;
                if (const auto path = SingleAnnotation<Sora::Configuration::$::Path>(member); path.has_value()) {
                    logicalPath = std::string{path->value.view()};
                    if (!Sora::Configuration::ValidatePath(logicalPath)) {
                        throw "Sora environment Path annotation is invalid.";
                    }
                } else {
                    auto joined = Sora::Configuration::JoinPath(parentPath, std::meta::identifier_of(member));
                    if (!joined) {
                        throw "Sora environment reflected member identifier is not a valid configuration path.";
                    }
                    logicalPath = std::move(*joined);
                }

                if constexpr (kEnvironmentLeaf<Field>) {
                    if constexpr (std::same_as<Field, std::string_view>) {
                        throw "Sora environment fields must own decoded text; std::string_view is not allowed.";
                    }
                    std::string nativeName;
                    if (const auto native = SingleAnnotation<Sora::Configuration::$::NativeName>(member);
                        native.has_value()) {
                        nativeName = std::string{native->value.view()};
                        if (!IsNativeName(nativeName)) {
                            throw "Sora environment NativeName annotation is invalid.";
                        }
                    } else {
                        auto encoded = Sora::Configuration::EncodeEnvironmentName(prefix, logicalPath);
                        if (!encoded) {
                            throw "Sora environment field name cannot be encoded.";
                        }
                        nativeName = std::move(*encoded);
                    }

                    const auto fieldIndex = static_cast<uint32_t>(fields.size());
                    fields.push_back(FieldDesc{.logicalPath = StaticName(logicalPath),
                                               .nativeName = StaticName(nativeName),
                                               .offset = fieldOffset,
                                               .decode = &DecodeField<Field>,
                                               .encode = &EncodeField<Field>,
                                               .required = Sora::$::Has<Sora::Configuration::$::Required>(member)});
                    lookups.push_back(
                        LookupDesc{.nativeName = StaticName(nativeName), .fieldIndex = fieldIndex, .priority = 0});

                    uint32_t priority = 1;
                    for (const std::meta::info annotation : std::meta::annotations_of(member, ^^$::Alias)) {
                        const $::Alias alias = std::meta::extract<$::Alias>(annotation);
                        if (!Sora::Configuration::ValidatePath(alias.value.view())) {
                            throw "Sora environment Alias annotation is invalid.";
                        }
                        auto encoded = Sora::Configuration::EncodeEnvironmentName(prefix, alias.value.view());
                        if (!encoded) {
                            throw "Sora environment Alias cannot be encoded.";
                        }
                        lookups.push_back(LookupDesc{
                            .nativeName = StaticName(*encoded), .fieldIndex = fieldIndex, .priority = priority++});
                    }
                } else if constexpr (std::is_class_v<Field> && Traits::PublicDataMembersCount<Field> > 0) {
                    if constexpr (Sora::$::HasAny<$::Required, $::NativeName, $::Alias>(member)) {
                        throw "Sora environment leaf-only annotation was applied to a nested object.";
                    }
                    const std::string_view nestedPath = Sora::$::Has<$::Flatten>(member) ? parentPath : logicalPath;
                    Collect<Field>(fields, lookups, prefix, nestedPath, fieldOffset);
                } else {
                    throw "Sora environment member is neither string-convertible nor a reflected nested object.";
                }
            }
        }

        template<typename Root>
        [[nodiscard]] consteval Schema BuildSchema() {
            static_assert(std::default_initializable<Root>,
                          "Sora environment objects must be default-initializable for transactional reads.");
            const auto object = SingleAnnotation<$::Object>(^^Root);
            if (!object) {
                throw "Sora environment root type must carry exactly one Object annotation.";
            }
            if (!object->prefix.empty() && !Sora::Configuration::IsPathSegment(object->prefix.view())) {
                throw "Sora environment Object prefix is invalid.";
            }

            std::vector<FieldDesc> fields;
            std::vector<LookupDesc> lookups;
            Collect<Root>(fields, lookups, object->prefix.view(), {}, 0);
            std::ranges::sort(lookups, [](const LookupDesc& lhs, const LookupDesc& rhs) {
                return PAL::CompareEnvironmentNames(lhs.nativeName.Text(), rhs.nativeName.Text()) < 0;
            });
            for (size_t index = 1; index < lookups.size(); ++index) {
                if (PAL::CompareEnvironmentNames(lookups[index - 1].nativeName.Text(),
                                                 lookups[index].nativeName.Text()) == 0) {
                    throw "Sora environment schema contains colliding primary names or aliases.";
                }
            }

            const auto& staticFields = std::define_static_array(fields);
            const auto& staticLookups = std::define_static_array(lookups);
            return Schema{.fields = staticFields, .lookups = staticLookups};
        }

        template<typename Root>
        inline constexpr Schema kSchema = BuildSchema<Root>();

    } // namespace Detail

    template<size_t Capacity>
    class Patch {
    public:
        struct Entry {
            NameEntry nativeName{};
            std::optional<std::string> value;
        };

        /** @brief Return the number of set/unset operations in this patch. */
        [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }

        /** @brief Apply this patch transactionally through @ref PAL::ApplyEnvironmentMutations. */
        [[nodiscard]] Result<void> Apply() const {
            std::inplace_vector<PAL::EnvironmentMutation, Capacity> mutations;
            for (const Entry& entry : entries_) {
                mutations.push_back(PAL::EnvironmentMutation{
                    .name = entry.nativeName.Text(),
                    .value = entry.value ? std::optional<std::string_view>{*entry.value} : std::nullopt,
                });
            }
            return PAL::ApplyEnvironmentMutations(mutations);
        }

    private:
        template<typename>
        friend class CompiledEnvironment;

        std::inplace_vector<Entry, Capacity> entries_;
    };

    template<typename Root>
    class CompiledEnvironment {
    public:
        static constexpr size_t kFieldCount = Detail::kSchema<Root>.fields.size();
        using ReadReport = Report<kFieldCount + 1>;
        using EnvironmentPatch = Patch<kFieldCount>;

        /** @brief Return the exact static field descriptors in declaration order. */
        [[nodiscard]] constexpr std::span<const Detail::FieldDesc> Fields() const noexcept {
            return Detail::kSchema<Root>.fields;
        }

        /** @brief Capture the process environment and decode it over a value-initialized root object. */
        [[nodiscard]] std::expected<Root, ReadReport> Read() const { return Read(Root{}); }

        /** @brief Capture the process environment and overlay present values on @p baseline transactionally. */
        [[nodiscard]] std::expected<Root, ReadReport> Read(Root baseline) const {
            auto snapshot = PAL::CaptureEnvironment();
            if (!snapshot) {
                ReadReport report;
                report.issues.push_back(
                    Issue{.kind = IssueKind::SourceFailure, .sourceError = std::move(snapshot.error())});
                return std::unexpected(std::move(report));
            }
            return Read(*snapshot, std::move(baseline));
        }

        /** @brief Decode @p snapshot over a value-initialized root object. */
        [[nodiscard]] std::expected<Root, ReadReport> Read(const PAL::EnvironmentSnapshot& snapshot) const {
            return Read(snapshot, Root{});
        }

        /** @brief Decode @p snapshot over @p baseline using one sorted merge and direct field thunks. */
        [[nodiscard]] std::expected<Root, ReadReport> Read(const PAL::EnvironmentSnapshot& snapshot,
                                                           Root baseline) const {
            struct Match {
                std::string_view value{};
                NameEntry nativeName{};
                uint32_t priority = std::numeric_limits<uint32_t>::max();
                bool present = false;
            };
            std::array<Match, kFieldCount> matches{};

            size_t lookupIndex = 0;
            size_t snapshotIndex = 0;
            const auto lookups = Detail::kSchema<Root>.lookups;
            while (lookupIndex < lookups.size() && snapshotIndex < snapshot.Size()) {
                const Detail::LookupDesc& lookup = lookups[lookupIndex];
                const PAL::EnvironmentEntryView entry = snapshot[snapshotIndex];
                const auto order = PAL::CompareEnvironmentNames(lookup.nativeName.Text(), entry.name);
                if (order < 0) {
                    ++lookupIndex;
                } else if (order > 0) {
                    ++snapshotIndex;
                } else {
                    Match& match = matches[lookup.fieldIndex];
                    if (!match.present || lookup.priority < match.priority) {
                        match = Match{.value = entry.value,
                                      .nativeName = lookup.nativeName,
                                      .priority = lookup.priority,
                                      .present = true};
                    }
                    ++lookupIndex;
                    ++snapshotIndex;
                }
            }

            ReadReport report;
            for (size_t index = 0; index < kFieldCount; ++index) {
                const Detail::FieldDesc& field = Detail::kSchema<Root>.fields[index];
                if (!matches[index].present) {
                    if (field.required) {
                        report.issues.push_back(Issue{.kind = IssueKind::MissingRequired,
                                                      .logicalPath = field.logicalPath,
                                                      .nativeName = field.nativeName});
                    }
                    continue;
                }
                if (auto decoded = field.decode(&baseline, field.offset, matches[index].value); !decoded) {
                    report.issues.push_back(Issue{.kind = IssueKind::InvalidValue,
                                                  .logicalPath = field.logicalPath,
                                                  .nativeName = matches[index].nativeName,
                                                  .decodeError = decoded.error()});
                }
            }
            if (!report.Empty()) {
                return std::unexpected(std::move(report));
            }
            return baseline;
        }

        /** @brief Encode every field into a side-effect-free set/unset patch. */
        [[nodiscard]] EnvironmentPatch Encode(const Root& object) const {
            EnvironmentPatch patch;
            for (const Detail::FieldDesc& field : Detail::kSchema<Root>.fields) {
                patch.entries_.push_back(typename EnvironmentPatch::Entry{
                    .nativeName = field.nativeName, .value = field.encode(&object, field.offset)});
            }
            return patch;
        }
    };

    /** @brief Compile and validate the hierarchical environment schema for @p Root. */
    template<typename Root>
    [[nodiscard]] consteval CompiledEnvironment<Root> Compile() {
        static_cast<void>(Detail::kSchema<Root>);
        return {};
    }

} // namespace Sora::Configuration
