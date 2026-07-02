/**
 * @file Module.h
 * @brief Portable module images, dynamic-library loading, and symbol lookup services.
 * @ingroup PAL
 */
#pragma once

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

    /** @brief Diagnostics produced when no candidate module can be loaded. */
    struct ModuleLoadError {
        std::vector<std::string> attempted;   /**< Candidate spellings attempted in order. */
        std::vector<std::string> diagnostics; /**< Platform loader diagnostics, one entry per failed candidate. */
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

    /** @brief Return the bitwise union of two normalized section flag sets. */
    [[nodiscard]] constexpr SectionFlag operator|(SectionFlag lhs, SectionFlag rhs) noexcept {
        return static_cast<SectionFlag>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    /** @brief Return true when @p flags contains every bit in @p bit. */
    [[nodiscard]] constexpr bool HasFlag(SectionFlag flags, SectionFlag bit) noexcept {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) == static_cast<uint32_t>(bit);
    }

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

    /** @brief Owning runtime handle for a dynamically loaded module. */
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
         * @tparam Fn Function type, not a pointer type.
         * @param[in] name Exported symbol name.
         * @return Function pointer, or @c nullptr when the symbol is absent.
         */
        template<typename Fn>
            requires std::is_function_v<Fn>
        [[nodiscard]] Fn* TryFindFunction(std::string_view name) const noexcept {
            return TryFindSymbol<Fn>(name);
        }

    private:
        friend class ModuleLoader;
        friend struct Detail::ModuleImageBuilder;

        /** @brief Adopt @p handle as a native module handle produced from @p path. */
        Module(void* handle, std::filesystem::path path, bool unloadOnDestroy) noexcept;

        void* handle_ = nullptr;
        std::filesystem::path path_;
        bool unloadOnDestroy_ = false;
    };

    /** @brief Shared handle type returned by the process module loader. */
    using ModulePtr = std::shared_ptr<const Module>;

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
         * @param[in] names Candidate module spellings in priority order.
         * @param[in] options Candidate-generation, cache, search, and native-loader options.
         * @return Shared loaded module, or detailed diagnostics for all attempted candidates.
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
        [[nodiscard]] std::vector<std::string> GenerateCandidates(std::span<const std::string_view> names,
                                                                  ModuleLoadOptions options) const;

        mutable std::mutex mutex_;
        std::vector<std::filesystem::path> searchPaths_;
        std::unordered_map<std::string, std::weak_ptr<const Module>> cache_;
    };

    /**
     * @brief Load the first available dynamic module from @p names through @ref ModuleLoader::Default.
     * @param[in] names Candidate module spellings in priority order.
     * @param[in] options Candidate-generation and native-loader options.
     * @return Shared loaded module, or detailed diagnostics for all attempted candidates.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::span<const std::string_view> names, ModuleLoadOptions options = {});

    /**
     * @brief Convenience overload for brace-init candidate lists.
     * @param[in] names Candidate module spellings in priority order.
     * @param[in] options Candidate-generation and native-loader options.
     * @return Shared loaded module, or detailed diagnostics for all attempted candidates.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::initializer_list<std::string_view> names,
                                               ModuleLoadOptions options = {});

    /**
     * @brief Compatibility overload mapping a boolean native-candidate switch to @ref ModuleLoadOptions.
     * @param[in] names Candidate module spellings in priority order.
     * @param[in] includeNativeCandidates Whether to try @c lib prefixes and shared-library suffix variants.
     * @return Shared loaded module, or detailed diagnostics for all attempted candidates.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::span<const std::string_view> names, bool includeNativeCandidates);

    /**
     * @brief Compatibility overload for brace-init candidate lists and a boolean native-candidate switch.
     * @param[in] names Candidate module spellings in priority order.
     * @param[in] includeNativeCandidates Whether to try @c lib prefixes and shared-library suffix variants.
     * @return Shared loaded module, or detailed diagnostics for all attempted candidates.
     */
    [[nodiscard]] Result<ModulePtr> LoadModule(std::initializer_list<std::string_view> names,
                                               bool includeNativeCandidates);

} // namespace Sora::PAL
