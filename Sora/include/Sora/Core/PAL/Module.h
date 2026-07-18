/**
 * @file Module.h
 * @brief Portable module images, dynamic-library loading, and symbol lookup services.
 * @ingroup PAL
 *
 * @details Narrow module spellings supplied through @ref LoadModule or @ref ModuleLoader::Load are UTF-8. Search roots
 * use @c std::filesystem::path and therefore retain the platform-native path representation. A loaded @ref Module owns
 * its native handle by default and provides typed symbol lookup without exposing platform loader declarations.
 */
#pragma once

#include "Sora/Common.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Sora/Core/Flags.h"
#include "Sora/ErrorCode.h"

namespace Sora::PAL {

    /** @brief Interpretation of a module name supplied to @ref LoadModule or @ref ModuleLoader::Load. */
    enum class ModuleNameKind : uint8_t {
        Auto = 0,  /**< Infer exact-path versus filename behavior from the spelling. */
        ExactPath, /**< Treat every input as an exact platform path and do not rewrite its directory portion. */
        FileName,  /**< Treat every input as a filename or library name to be searched by the platform loader. */
        Stem,      /**< Treat every input as an undecorated logical stem such as @c c++abi or @c dbghelp. */
    };

    /** @brief Candidate generation policy for dynamic-library names. */
    enum class ModuleCandidatePolicy : uint8_t {
        ExactOnly = 0,   /**< Try only the names provided by the caller. */
        NativeDecorated, /**< Also try native and cross-toolchain @c lib prefixes and shared-library suffixes. */
    };

    /** @brief Binding mode requested from loaders that distinguish lazy and eager relocation. */
    enum class ModuleBindMode : uint8_t {
        Lazy = 0, /**< Resolve relocations lazily when the platform loader supports it. */
        Now,      /**< Resolve relocations before loading returns when the platform loader supports it. */
    };

    /** @brief Symbol visibility requested from loaders that support local/global module scopes. */
    enum class ModuleVisibility : uint8_t {
        Local = 0, /**< Keep loaded symbols local to the module when the platform loader supports it. */
        Global, /**< Make loaded symbols available for resolving later modules when the platform loader supports it. */
    };

    /** @brief Cache and lifetime policy for a loaded runtime module. */
    enum class ModuleCachePolicy : uint8_t {
        Shared = 0, /**< Reuse a process-local shared module object for the same resolved module path. */
        Private,    /**< Always create a fresh module object and leave it outside the shared cache. */
    };

    /** @brief Options controlling module candidate generation and platform loader flags. */
    struct ModuleLoadOptions {
        ModuleNameKind nameKind = ModuleNameKind::Auto; /**< How input names should be interpreted. */
        ModuleCandidatePolicy candidatePolicy = ModuleCandidatePolicy::NativeDecorated; /**< Candidate generation. */
        ModuleBindMode bindMode = ModuleBindMode::Lazy;            /**< Lazy/eager relocation policy. */
        ModuleVisibility visibility = ModuleVisibility::Local;     /**< Loader symbol visibility policy. */
        ModuleCachePolicy cachePolicy = ModuleCachePolicy::Shared; /**< Whether loads participate in loader cache. */
        bool unloadOnDestroy = true; /**< Whether the last owning @ref Module should close the native handle. */
        /** @brief Per-call search roots tried before loader defaults. */
        std::span<const std::filesystem::path> searchPaths = {};
    };

    /** @brief Section flags normalized from PE, ELF, and Mach-O image metadata. */
    enum class SectionFlag : uint8_t {
        None = 0,                /**< No normalized section property is known. */
        Read = 1u << 0u,         /**< Section can be read when mapped by the native loader. */
        Write = 1u << 1u,        /**< Section can be written when mapped by the native loader. */
        Execute = 1u << 2u,      /**< Section contains executable code. */
        Alloc = 1u << 3u,        /**< Section occupies memory in the loaded image. */
        Code = 1u << 4u,         /**< Section primarily contains code. */
        Initialized = 1u << 5u,  /**< Section contains initialized data. */
        Uninitialized = 1u << 6u /**< Section describes zero-initialized data rather than stored file bytes. */
    };

    /** @brief Immutable view of one section in a module image file. */
    struct SectionView {
        std::string name;                      /**< Image-format section name, decoded as narrow text where possible. */
        std::span<const std::byte> bytes;      /**< Stored section bytes; empty for pure zero-fill sections. */
        uint64_t virtualAddress = 0;           /**< Section virtual address relative to the loaded image base. */
        uint64_t fileOffset = 0;               /**< Offset of the stored bytes in the image file. */
        SectionFlag flags = SectionFlag::None; /**< Normalized section properties. */
    };

    namespace Detail {

        /** @brief Internal builder access for module-image parsers. */
        struct ModuleImageBuilder;

        /** @brief Find @p symbol in a native dynamic-library handle. */
        [[nodiscard]] void* FindNativeSymbol(void* handle, std::string_view symbol) noexcept;

    } // namespace Detail

    /** @brief Read-only view of a PE, ELF, or Mach-O image on disk without loading it into the process. */
    class ModuleImage {
    public:
        /** @brief Return the source path used to open this image. */
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

        /** @brief Return every section discovered in image order. */
        [[nodiscard]] std::span<const SectionView> Sections() const noexcept { return sections_; }

        /**
         * @brief Find the first section whose name is exactly @p name.
         * @param[in] name Image-format section name to search for.
         * @return Matching section view, or @c nullptr when no such section exists.
         */
        [[nodiscard]] Result<Ref<const SectionView>> FindSection(std::string_view name) const noexcept;

        /**
         * @brief Interpret a section as a contiguous table of trivially copyable entries.
         * @tparam Entry Entry type stored by value in the section.
         * @param[in] sectionName Section containing an array of @p Entry values.
         * @return Entry span, or a diagnostic when the section is missing, misaligned, or has a partial tail.
         */
        template<typename Entry>
            requires std::is_trivially_copyable_v<Entry>
        [[nodiscard]] std::expected<std::span<const Entry>, ErrorCode>
        Entries(std::string_view sectionName) const noexcept {
            Result<Ref<const SectionView>> section = FindSection(sectionName);
            if (!section) {
                return std::unexpected(section.error());
            }
            if constexpr (alignof(Entry) > 1) {
                auto address = reinterpret_cast<uintptr_t>(section->get().bytes.data());
                if (address % alignof(Entry) != 0) {
                    return std::unexpected{ErrorCode::ModuleLoadFailed};
                }
            }
            if (section->get().bytes.size_bytes() % sizeof(Entry) != 0) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            auto* data = reinterpret_cast<const Entry*>(section->get().bytes.data());
            return std::span<const Entry>{data, section->get().bytes.size_bytes() / sizeof(Entry)};
        }

    private:
        friend class ModuleLoader;
        friend struct Detail::ModuleImageBuilder;

        std::filesystem::path path_;
        std::vector<std::byte> bytes_;
        std::vector<SectionView> sections_;
    };

    class Module;

    /** @brief Shared handle for a loaded module or the current process module. */
    using ModulePtr = std::shared_ptr<const Module>;

    /** @brief Runtime view of an owned or process-lifetime native module handle. */
    class Module {
    public:
        Module(const Module&) = delete;
        Module& operator=(const Module&) = delete;

        /** @brief Close the native handle when this object owns one. */
        ~Module();

        /** @brief Return true if this object owns or observes a native module handle. */
        [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

        /** @brief Return the opaque native module handle. */
        [[nodiscard]] void* NativeHandle() const noexcept { return handle_; }

        /** @brief Return the candidate or resolved path that produced this module. */
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

        /** @brief Return whether the native handle will be closed by the destructor. */
        [[nodiscard]] bool UnloadOnDestroy() const noexcept { return unloadOnDestroy_; }

        /**
         * @brief Look up @p name in this module.
         * @param[in] name Exported symbol name.
         * @return Opaque symbol address, or @c nullptr when the symbol is absent or this module is empty.
         */
        template<typename T>
        [[nodiscard]] T* TryFindSymbol(std::string_view name) const noexcept {
            return reinterpret_cast<T*>(Detail::FindNativeSymbol(handle_, name));
        }

        /**
         * @brief Look up @p name and cast it to a function pointer.
         * @tparam Fn Exact function type, including its platform calling convention.
         * @param[in] name Exported symbol name.
         * @return Function pointer, or @c nullptr when the symbol is absent.
         */
        template<typename Fn>
            requires std::is_function_v<Fn>
        [[nodiscard]] Fn* TryFindFunction(std::string_view name) const noexcept {
            return TryFindSymbol<Fn>(name);
        }

        /**
         * @brief Look up @p name and cast it to a function pointer.
         * @tparam Fn Exact function-pointer type, including its platform calling convention.
         * @param[in] name Exported symbol name.
         * @return Function pointer, or @c nullptr when the symbol is absent.
         */
        template<typename Fn>
            requires std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>
        [[nodiscard]] Fn TryFindFunction(std::string_view name) const noexcept {
            return TryFindSymbol<std::remove_pointer_t<Fn>>(name);
        }

    private:
        friend class ModuleLoader;
        friend struct Detail::ModuleImageBuilder;
        friend const ModulePtr& CurrentProcessModule() noexcept;

        /** @brief Create a module view over @p handle with explicit unload ownership. */
        Module(void* handle, std::filesystem::path path, bool unloadOnDestroy) noexcept;

        void* handle_ = nullptr;
        std::filesystem::path path_;
        bool unloadOnDestroy_ = false;
    };

    /**
     * @brief Return the process-lifetime module view used to resolve symbols from the current process scope.
     * @details The view observes the main executable on Windows and the @c dlopen(nullptr, ...) global lookup scope on
     * POSIX. Its native handle is never unloaded, and its path is empty because no module path produced the view.
     * @return Immutable shared view, or an empty pointer when the platform has no process-module facility.
     */
    [[nodiscard]] const ModulePtr& CurrentProcessModule() noexcept;

    /** @brief Process-level service for module search paths, image inspection, loading, and module cache ownership. */
    class ModuleLoader {
    public:
        /** @brief Return the default process-wide module loader. */
        [[nodiscard]] static ModuleLoader& Default() noexcept;

        ModuleLoader();
        ModuleLoader(const ModuleLoader&) = delete;
        ModuleLoader& operator=(const ModuleLoader&) = delete;

        /**
         * @brief Load the first available dynamic module from @p names.
         * @param[in] names UTF-8 candidate module spellings in priority order.
         * @param[in] options Candidate-generation, cache, search, and native-loader options.
         * @return Shared loaded module, or @ref ErrorCode::ModuleLoadFailed when no candidate can be loaded.
         */
        [[nodiscard]] Result<ModulePtr> Load(std::span<const std::string_view> names, ModuleLoadOptions options = {});

        /**
         * @brief Open @p path as a read-only module image without loading or initializing it.
         * @param[in] path File path to a PE, ELF, or Mach-O image.
         * @return Parsed module image, or a diagnostic describing why it could not be interpreted.
         */
        [[nodiscard]] Result<ModuleImage> OpenImage(const std::filesystem::path& path) const;

        /** @brief Add @p path to this loader's search roots if it is not already present. */
        void AddSearchPath(std::filesystem::path path);

        /** @brief Remove @p path from this loader's search roots. */
        [[nodiscard]] bool RemoveSearchPath(const std::filesystem::path& path);

        /** @brief Remove all custom search roots from this loader. */
        void ClearSearchPaths();

        /** @brief Return a snapshot of this loader's custom search roots. */
        [[nodiscard]] std::vector<std::filesystem::path> SearchPaths() const;

        /** @brief Drop expired entries from the shared module cache. */
        void PruneCache();

    private:
        struct CacheKey {
            std::filesystem::path path;
            ModuleBindMode bindMode = ModuleBindMode::Lazy;
            ModuleVisibility visibility = ModuleVisibility::Local;
            bool unloadOnDestroy = true;

            [[nodiscard]] friend bool operator==(const CacheKey&, const CacheKey&) noexcept = default;
        };

        struct CacheKeyHash {
            [[nodiscard]] size_t operator()(const CacheKey& key) const noexcept {
                size_t hash = std::filesystem::hash_value(key.path);
                const auto combine = [&hash](size_t value) {
                    constexpr size_t kGoldenRatio = static_cast<size_t>(0x9E3779B97F4A7C15ull);
                    hash ^= value + kGoldenRatio + (hash << 6u) + (hash >> 2u);
                };
                combine(static_cast<size_t>(key.bindMode));
                combine(static_cast<size_t>(key.visibility));
                combine(static_cast<size_t>(key.unloadOnDestroy));
                return hash;
            }
        };

        [[nodiscard]] std::vector<std::filesystem::path> GenerateCandidates(std::span<const std::string_view> names,
                                                                            ModuleLoadOptions options) const;

        mutable std::mutex mutex_;
        std::vector<std::filesystem::path> searchPaths_;
        std::unordered_map<CacheKey, std::weak_ptr<const Module>, CacheKeyHash> cache_;
    };

    /**
     * @brief Load the first available dynamic module from @p names through @ref ModuleLoader::Default.
     * @param[in] names UTF-8 candidate module spellings in priority order.
     * @param[in] options Candidate-generation and native-loader options.
     * @return Shared loaded module, or @ref ErrorCode::ModuleLoadFailed when no candidate can be loaded.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::span<const std::string_view> names, ModuleLoadOptions options = {});

    /**
     * @brief Convenience overload for brace-init candidate lists.
     * @param[in] names UTF-8 candidate module spellings in priority order.
     * @param[in] options Candidate-generation and native-loader options.
     * @return Shared loaded module, or @ref ErrorCode::ModuleLoadFailed when no candidate can be loaded.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::initializer_list<std::string_view> names,
                                               ModuleLoadOptions options = {});

} // namespace Sora::PAL
