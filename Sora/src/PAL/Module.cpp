
/**
 * @file Module.cpp
 * @brief Platform implementation of module image inspection, dynamic loading, and symbol lookup.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/PAL/NativeError.h>
#include <Sora/Core/PAL/Path.h>

#include <Sora/Core/Wire.h>
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <array>
#include <expected>
#include <fstream>
#include <functional>
#include <optional>
#include <utility>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace Sora::PAL {

    namespace Detail {

        /** @brief Internal access shim for parsers that build immutable module images. */
        struct ModuleImageBuilder {
            /** @brief Return mutable image path storage. */
            [[nodiscard]] static std::filesystem::path& Path(ModuleImage& image) noexcept { return image.path_; }

            /** @brief Return mutable image byte storage. */
            [[nodiscard]] static std::vector<std::byte>& Bytes(ModuleImage& image) noexcept { return image.bytes_; }

            /** @brief Return mutable image section storage. */
            [[nodiscard]] static std::vector<SectionView>& Sections(ModuleImage& image) noexcept {
                return image.sections_;
            }
        };

    } // namespace Detail

    namespace {

        /** @brief Native-loader failure retained without formatting OS messages on the success path. */
        struct NativeLoadFailure {
            NativeError error;
            std::string diagnostic;
        };

        using NativeLoadResult = std::expected<void*, NativeLoadFailure>;

        /** @brief Add @p candidate if it has not already appeared. */
        void AddCandidate(std::vector<std::string>& candidates, std::string candidate) {
            if (!candidate.empty() && !std::ranges::contains(candidates, candidate)) {
                candidates.push_back(std::move(candidate));
            }
        }

        /** @brief Generate decorated filename candidates for one spelling without applying search roots. */
        [[nodiscard]] std::vector<std::string> DecorateName(std::string_view name, ModuleLoadOptions options) {
            std::vector<std::string> decorated;
            AddCandidate(decorated, std::string{name});
            if (options.candidatePolicy == ModuleCandidatePolicy::ExactOnly) {
                return decorated;
            }

            const bool pathLike = HasPathSeparator(name);
            if (options.nameKind == ModuleNameKind::ExactPath && pathLike && HasSharedLibrarySuffix(name)) {
                return decorated;
            }

            auto [directory, filename] = SplitDirectory(name);
            std::string stem =
                options.nameKind == ModuleNameKind::Stem ? std::string{filename} : RemoveSharedLibrarySuffix(filename);
            std::array prefixes{std::string_view{}, std::string_view{"lib"}};
            std::array suffixes{std::string_view{}, Sora::Platform::kSharedLibrarySuffix, std::string_view{".dll"},
                                std::string_view{".so"}, std::string_view{".dylib"}};

            for (std::string_view prefix : prefixes) {
                if (!prefix.empty() && stem.starts_with(prefix)) {
                    continue;
                }
                for (std::string_view suffix : suffixes) {
                    if (suffix.empty() && HasSharedLibrarySuffix(filename)) {
                        continue;
                    }
                    std::string candidate;
                    candidate.reserve(directory.size() + prefix.size() + stem.size() + suffix.size());
                    candidate += directory;
                    candidate += prefix;
                    candidate += stem;
                    candidate += suffix;
                    AddCandidate(decorated, std::move(candidate));
                }
            }
            return decorated;
        }

        /** @brief Return a stable cache spelling for @p candidate and loader policy. */
        [[nodiscard]] std::string CacheKey(std::string_view candidate, ModuleLoadOptions options) {
            std::error_code ec;
            auto path = std::filesystem::weakly_canonical(std::filesystem::path{candidate}, ec);
            std::string key = ec ? std::string{candidate} : path.string();
            key += '|';
            key += static_cast<char>('0' + static_cast<int>(options.bindMode));
            key += static_cast<char>('0' + static_cast<int>(options.visibility));
            key += static_cast<char>(options.unloadOnDestroy ? '1' : '0');
            return key;
        }

        /** @brief Read an entire file as bytes. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path) {
            std::ifstream file{path, std::ios::binary | std::ios::ate};
            if (!file) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            const std::streamoff size = file.tellg();
            if (size < 0) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            file.seekg(0, std::ios::beg);
            if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            return bytes;
        }

        using Sora::Wire::HasRange;

        /** @brief Read an unsigned little-endian integer from @p bytes. */
        template<typename T>
        [[nodiscard]] T ReadLe(std::span<const std::byte> bytes, uint64_t offset) noexcept {
            return Sora::Wire::ReadLittleEndianUnchecked<T>(bytes, static_cast<size_t>(offset));
        }

        /** @brief Read an unsigned big-endian integer from @p bytes. */
        template<typename T>
        [[nodiscard]] T ReadBe(std::span<const std::byte> bytes, uint64_t offset) noexcept {
            return Sora::Wire::ReadBigEndianUnchecked<T>(bytes, static_cast<size_t>(offset));
        }

        /** @brief Read an endian-selected unsigned integer from @p bytes. */
        template<typename T>
        [[nodiscard]] T ReadEndian(std::span<const std::byte> bytes, uint64_t offset, bool littleEndian) noexcept {
            return littleEndian ? ReadLe<T>(bytes, offset) : ReadBe<T>(bytes, offset);
        }

        /** @brief Decode a fixed-width, NUL-padded section name. */
        [[nodiscard]] std::string FixedName(std::span<const std::byte> bytes, uint64_t offset, size_t size) {
            std::string name;
            name.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                char c = static_cast<char>(std::to_integer<uint8_t>(bytes[static_cast<size_t>(offset) + i]));
                if (c == '\0') {
                    break;
                }
                name.push_back(c);
            }
            return name;
        }

        /** @brief Build a byte span for a section range, clamping invalid zero-sized storage to an empty span. */
        [[nodiscard]] std::span<const std::byte> SectionBytes(const std::vector<std::byte>& bytes, uint64_t offset,
                                                              uint64_t size) noexcept {
            if (!HasRange(bytes, offset, size) || size == 0) {
                return {};
            }
            return {bytes.data() + static_cast<size_t>(offset), static_cast<size_t>(size)};
        }

        /** @brief Normalize PE section characteristics. */
        [[nodiscard]] SectionFlag PeFlags(uint32_t characteristics) noexcept {
            SectionFlag flags = SectionFlag::None;
            if ((characteristics & 0x00000020u) != 0) {
                flags = flags | SectionFlag::Code;
            }
            if ((characteristics & 0x00000040u) != 0) {
                flags = flags | SectionFlag::Initialized;
            }
            if ((characteristics & 0x00000080u) != 0) {
                flags = flags | SectionFlag::Uninitialized;
            }
            if ((characteristics & 0x20000000u) != 0) {
                flags = flags | SectionFlag::Execute;
            }
            if ((characteristics & 0x40000000u) != 0) {
                flags = flags | SectionFlag::Read;
            }
            if ((characteristics & 0x80000000u) != 0) {
                flags = flags | SectionFlag::Write;
            }
            return flags | SectionFlag::Alloc;
        }

        /** @brief Try to parse PE sections from @p image. */
        [[nodiscard]] Result<void> ParsePeSections(ModuleImage& image) {
            const auto& path = Detail::ModuleImageBuilder::Path(image);
            const auto& storage = Detail::ModuleImageBuilder::Bytes(image);
            auto& sections = Detail::ModuleImageBuilder::Sections(image);
            std::span<const std::byte> bytes = storage;
            if (!HasRange(bytes, 0, 0x40) || ReadLe<uint16_t>(bytes, 0) != 0x5A4Du) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            const uint32_t peOffset = ReadLe<uint32_t>(bytes, 0x3C);
            if (!HasRange(bytes, peOffset, 24) || ReadLe<uint32_t>(bytes, peOffset) != 0x00004550u) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const uint16_t sectionCount = ReadLe<uint16_t>(bytes, peOffset + 6);
            const uint16_t optionalHeaderSize = ReadLe<uint16_t>(bytes, peOffset + 20);
            const uint64_t sectionTable = static_cast<uint64_t>(peOffset) + 24u + optionalHeaderSize;
            if (!HasRange(bytes, sectionTable, static_cast<uint64_t>(sectionCount) * 40u)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            sections.reserve(sectionCount);
            for (uint16_t i = 0; i < sectionCount; ++i) {
                const uint64_t base = sectionTable + static_cast<uint64_t>(i) * 40u;
                const uint32_t virtualSize = ReadLe<uint32_t>(bytes, base + 8);
                const uint32_t virtualAddress = ReadLe<uint32_t>(bytes, base + 12);
                const uint32_t rawSize = ReadLe<uint32_t>(bytes, base + 16);
                const uint32_t rawOffset = ReadLe<uint32_t>(bytes, base + 20);
                const uint32_t characteristics = ReadLe<uint32_t>(bytes, base + 36);
                sections.push_back(SectionView{.name = FixedName(bytes, base, 8),
                                               .bytes = SectionBytes(storage, rawOffset, rawSize),
                                               .virtualAddress = virtualAddress,
                                               .fileOffset = rawOffset,
                                               .flags = PeFlags(characteristics)});
                if (rawSize == 0 && virtualSize != 0) {
                    sections.back().flags = sections.back().flags | SectionFlag::Uninitialized;
                }
            }
            return {};
        }

        /** @brief Normalize ELF section flags and type. */
        [[nodiscard]] SectionFlag ElfFlags(uint64_t flags, uint32_t type) noexcept {
            SectionFlag result = SectionFlag::None;
            if ((flags & 0x2u) != 0) {
                result = result | SectionFlag::Alloc;
            }
            if ((flags & 0x1u) != 0) {
                result = result | SectionFlag::Write;
            }
            if ((flags & 0x4u) != 0) {
                result = result | SectionFlag::Execute | SectionFlag::Code;
            }
            if (type == 8u) {
                result = result | SectionFlag::Uninitialized;
            } else {
                result = result | SectionFlag::Initialized | SectionFlag::Read;
            }
            return result;
        }

        /** @brief Resolve an ELF string-table entry. */
        [[nodiscard]] std::string ElfString(std::span<const std::byte> bytes, uint64_t tableOffset, uint64_t tableSize,
                                            uint32_t stringOffset) {
            if (stringOffset >= tableSize) {
                return {};
            }
            [[assume(stringOffset < tableSize)]];
            const uint64_t begin = tableOffset + stringOffset;
            const uint64_t end = tableOffset + tableSize;
            std::string name;
            for (uint64_t pos = begin; pos < end; ++pos) {
                char c = static_cast<char>(std::to_integer<uint8_t>(bytes[static_cast<size_t>(pos)]));
                if (c == '\0') {
                    break;
                }
                name.push_back(c);
            }
            return name;
        }

        /** @brief Try to parse ELF sections from @p image. */
        [[nodiscard]] Result<void> ParseElfSections(ModuleImage& image) {
            const auto& path = Detail::ModuleImageBuilder::Path(image);
            const auto& storage = Detail::ModuleImageBuilder::Bytes(image);
            auto& sections = Detail::ModuleImageBuilder::Sections(image);
            std::span<const std::byte> bytes = storage;
            if (!HasRange(bytes, 0, 16) || std::to_integer<uint8_t>(bytes[0]) != 0x7Fu ||
                static_cast<char>(std::to_integer<uint8_t>(bytes[1])) != 'E' ||
                static_cast<char>(std::to_integer<uint8_t>(bytes[2])) != 'L' ||
                static_cast<char>(std::to_integer<uint8_t>(bytes[3])) != 'F') {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const uint8_t elfClass = std::to_integer<uint8_t>(bytes[4]);
            const uint8_t endian = std::to_integer<uint8_t>(bytes[5]);
            const bool littleEndian = endian == 1;
            if ((elfClass != 1 && elfClass != 2) || (endian != 1 && endian != 2)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const bool is64 = elfClass == 2;
            const uint64_t elfHeaderSize = is64 ? 64u : 52u;
            if (!HasRange(bytes, 0, elfHeaderSize)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const uint64_t shoff =
                is64 ? ReadEndian<uint64_t>(bytes, 40, littleEndian) : ReadEndian<uint32_t>(bytes, 32, littleEndian);
            const uint16_t shentsize =
                is64 ? ReadEndian<uint16_t>(bytes, 58, littleEndian) : ReadEndian<uint16_t>(bytes, 46, littleEndian);
            const uint16_t shnum =
                is64 ? ReadEndian<uint16_t>(bytes, 60, littleEndian) : ReadEndian<uint16_t>(bytes, 48, littleEndian);
            const uint16_t shstrndx =
                is64 ? ReadEndian<uint16_t>(bytes, 62, littleEndian) : ReadEndian<uint16_t>(bytes, 50, littleEndian);
            const uint16_t minSectionHeaderSize = is64 ? 64u : 40u;
            const uint64_t sectionTableBytes = uint64_t{shentsize} * shnum;
            if (shentsize < minSectionHeaderSize || shstrndx >= shnum || !HasRange(bytes, shoff, sectionTableBytes)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            [[assume(shentsize >= minSectionHeaderSize)]];
            [[assume(shstrndx < shnum)]];

            const uint64_t strBase = shoff + static_cast<uint64_t>(shstrndx) * shentsize;
            const uint64_t strOffset = is64 ? ReadEndian<uint64_t>(bytes, strBase + 24, littleEndian)
                                            : ReadEndian<uint32_t>(bytes, strBase + 16, littleEndian);
            const uint64_t strSize = is64 ? ReadEndian<uint64_t>(bytes, strBase + 32, littleEndian)
                                          : ReadEndian<uint32_t>(bytes, strBase + 20, littleEndian);
            if (!HasRange(bytes, strOffset, strSize)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            sections.reserve(shnum);
            for (uint16_t i = 0; i < shnum; ++i) {
                const uint64_t base = shoff + static_cast<uint64_t>(i) * shentsize;
                const uint32_t nameOffset = ReadEndian<uint32_t>(bytes, base, littleEndian);
                const uint32_t type = ReadEndian<uint32_t>(bytes, base + 4, littleEndian);
                const uint64_t flags = is64 ? ReadEndian<uint64_t>(bytes, base + 8, littleEndian)
                                            : ReadEndian<uint32_t>(bytes, base + 8, littleEndian);
                const uint64_t address = is64 ? ReadEndian<uint64_t>(bytes, base + 16, littleEndian)
                                              : ReadEndian<uint32_t>(bytes, base + 12, littleEndian);
                const uint64_t offset = is64 ? ReadEndian<uint64_t>(bytes, base + 24, littleEndian)
                                             : ReadEndian<uint32_t>(bytes, base + 16, littleEndian);
                const uint64_t size = is64 ? ReadEndian<uint64_t>(bytes, base + 32, littleEndian)
                                           : ReadEndian<uint32_t>(bytes, base + 20, littleEndian);
                sections.push_back(SectionView{.name = ElfString(bytes, strOffset, strSize, nameOffset),
                                               .bytes = type == 8u ? std::span<const std::byte>{}
                                                                   : SectionBytes(storage, offset, size),
                                               .virtualAddress = address,
                                               .fileOffset = offset,
                                               .flags = ElfFlags(flags, type)});
            }
            return {};
        }

        /** @brief Normalize Mach-O section attributes. */
        [[nodiscard]] SectionFlag MachOFlags(uint32_t attributes) noexcept {
            SectionFlag flags = SectionFlag::Read | SectionFlag::Alloc | SectionFlag::Initialized;
            constexpr uint32_t pureInstructions = 0x80000000u;
            constexpr uint32_t someInstructions = 0x00000400u;
            if ((attributes & (pureInstructions | someInstructions)) != 0) {
                flags = flags | SectionFlag::Execute | SectionFlag::Code;
            }
            return flags;
        }

        /** @brief Try to parse Mach-O sections from @p image. */
        [[nodiscard]] Result<void> ParseMachOSections(ModuleImage& image) {
            const auto& path = Detail::ModuleImageBuilder::Path(image);
            const auto& storage = Detail::ModuleImageBuilder::Bytes(image);
            auto& sections = Detail::ModuleImageBuilder::Sections(image);
            std::span<const std::byte> bytes = storage;
            if (!HasRange(bytes, 0, 32)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const uint32_t magic = ReadLe<uint32_t>(bytes, 0);
            const bool is64 = magic == 0xFEEDFACFu;
            if (magic != 0xFEEDFACEu && magic != 0xFEEDFACFu) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }

            const uint32_t ncmds = ReadLe<uint32_t>(bytes, 16);
            uint64_t command = is64 ? 32u : 28u;
            for (uint32_t i = 0; i < ncmds; ++i) {
                if (!HasRange(bytes, command, 8)) {
                    return std::unexpected{ErrorCode::ModuleLoadFailed};
                }
                const uint32_t cmd = ReadLe<uint32_t>(bytes, command);
                const uint32_t cmdsize = ReadLe<uint32_t>(bytes, command + 4);
                if (cmdsize < 8 || !HasRange(bytes, command, cmdsize)) {
                    return std::unexpected{ErrorCode::ModuleLoadFailed};
                }

                const uint32_t segmentCommand = is64 ? 0x19u : 0x1u;
                if (cmd == segmentCommand) {
                    const uint32_t segmentHeaderSize = is64 ? 72u : 56u;
                    if (cmdsize < segmentHeaderSize || !HasRange(bytes, command, segmentHeaderSize)) {
                        return std::unexpected{ErrorCode::ModuleLoadFailed};
                    }
                    [[assume(cmdsize >= segmentHeaderSize)]];

                    const uint64_t sectionBase = command + segmentHeaderSize;
                    const uint32_t nsects = ReadLe<uint32_t>(bytes, command + (is64 ? 64u : 48u));
                    const uint32_t sectionSize = is64 ? 80u : 68u;
                    const uint64_t sectionBytes = static_cast<uint64_t>(nsects) * sectionSize;
                    if (!HasRange(bytes, sectionBase, sectionBytes) ||
                        sectionBytes > static_cast<uint64_t>(cmdsize - segmentHeaderSize)) {
                        return std::unexpected{ErrorCode::ModuleLoadFailed};
                    }
                    for (uint32_t section = 0; section < nsects; ++section) {
                        const uint64_t base = sectionBase + static_cast<uint64_t>(section) * sectionSize;
                        const uint64_t address =
                            is64 ? ReadLe<uint64_t>(bytes, base + 32) : ReadLe<uint32_t>(bytes, base + 32);
                        const uint64_t size =
                            is64 ? ReadLe<uint64_t>(bytes, base + 40) : ReadLe<uint32_t>(bytes, base + 36);
                        const uint32_t offset = ReadLe<uint32_t>(bytes, base + (is64 ? 48u : 40u));
                        const uint32_t attributes = ReadLe<uint32_t>(bytes, base + (is64 ? 68u : 56u));
                        sections.push_back(SectionView{.name = FixedName(bytes, base, 16),
                                                       .bytes = SectionBytes(storage, offset, size),
                                                       .virtualAddress = address,
                                                       .fileOffset = offset,
                                                       .flags = MachOFlags(attributes)});
                    }
                }
                command += cmdsize;
            }
            return {};
        }

        /** @brief Parse sections from a known module image format. */
        [[nodiscard]] Result<void> ParseSections(ModuleImage& image) {
            if (auto pe = ParsePeSections(image)) {
                return pe;
            }
            if (auto elf = ParseElfSections(image)) {
                return elf;
            }
            if (auto macho = ParseMachOSections(image)) {
                return macho;
            }
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

#ifdef PLATFORM_WINDOWS
        /** @brief Convert UTF-8 or active-code-page text to a Windows UTF-16 string. */
        [[nodiscard]] std::wstring ToWide(std::string_view text) {
            if (text.empty()) {
                return {};
            }
            UINT codePage = CP_UTF8;
            DWORD flags = MB_ERR_INVALID_CHARS;
            int size = ::MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0) {
                codePage = CP_ACP;
                flags = 0;
                size = ::MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
            }
            if (size <= 0) {
                return {};
            }
            std::wstring result(static_cast<size_t>(size), L'\0');
            ::MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        /** @brief Load @p candidate through the Win32 loader. */
        [[nodiscard]] NativeLoadResult TryLoadNative(std::string_view candidate, ModuleLoadOptions) {
            std::wstring wide = ToWide(candidate);
            if (wide.empty()) {
                return std::unexpected{NativeLoadFailure{
                    .diagnostic = "module name is empty or cannot be converted to a native Windows path"}};
            }
            if (HMODULE handle = ::LoadLibraryW(wide.c_str())) {
                return reinterpret_cast<void*>(handle);
            }
            return std::unexpected{NativeLoadFailure{.error = CaptureLastNativeError()}};
        }

        /** @brief Close a Win32 module handle. */
        void CloseNative(void* handle) noexcept {
            if (handle != nullptr) {
                ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
            }
        }
#else
        /** @brief Load @p candidate through the POSIX dynamic loader. */
        [[nodiscard]] NativeLoadResult TryLoadNative(std::string_view candidate, ModuleLoadOptions options) {
            int flags = options.bindMode == ModuleBindMode::Now ? RTLD_NOW : RTLD_LAZY;
            flags |= options.visibility == ModuleVisibility::Global ? RTLD_GLOBAL : RTLD_LOCAL;
            std::string name{candidate};
            ::dlerror();
            if (void* handle = ::dlopen(name.c_str(), flags)) {
                return handle;
            }
            const char* error = ::dlerror();
            return std::unexpected{NativeLoadFailure{
                .diagnostic = error != nullptr ? std::string{error} : "unknown dynamic-loader error"}};
        }

        /** @brief Close a POSIX module handle. */
        void CloseNative(void* handle) noexcept {
            if (handle != nullptr) {
                ::dlclose(handle);
            }
        }
#endif

    } // namespace

    Result<Ref<const SectionView>> ModuleImage::FindSection(std::string_view name) const noexcept {
        auto it = std::ranges::find_if(sections_, [name](const SectionView& section) { return section.name == name; });
        if (it != sections_.end()) {
            return std::ref(*it);
        }
        return std::unexpected{ErrorCode::SectionNotFound};
    }

    namespace Detail {

#ifdef PLATFORM_WINDOWS
        void* FindNativeSymbol(void* handle, std::string_view symbol) noexcept {
            if (handle == nullptr || symbol.empty()) {
                return nullptr;
            }
            std::string name{symbol};
            return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name.c_str()));
        }
#else
        void* FindNativeSymbol(void* handle, std::string_view symbol) noexcept {
            if (handle == nullptr || symbol.empty()) {
                return nullptr;
            }
            std::string name{symbol};
            return ::dlsym(handle, name.c_str());
        }
#endif

    } // namespace Detail
    Module::Module(void* handle, std::filesystem::path path, bool unloadOnDestroy) noexcept
        : handle_(handle), path_(std::move(path)), unloadOnDestroy_(unloadOnDestroy) {}

    Module::~Module() {
        if (handle_ != nullptr && unloadOnDestroy_) {
            CloseNative(handle_);
        }
    }

    ModuleLoader& ModuleLoader::Default() noexcept {
        static ModuleLoader loader;
        return loader;
    }

    ModuleLoader::ModuleLoader() = default;

    std::vector<std::string> ModuleLoader::GenerateCandidates(std::span<const std::string_view> names,
                                                              ModuleLoadOptions options) const {
        std::vector<std::filesystem::path> roots;
        {
            std::scoped_lock lock{mutex_};
            roots = searchPaths_;
        }
        roots.insert_range(roots.end(), options.searchPaths);

        std::vector<std::string> candidates;
        candidates.reserve(names.size() * std::max<size_t>(roots.size() + 1u, 1u) * 8u);
        for (std::string_view name : names) {
            std::vector<std::string> decorated = DecorateName(name, options);
            const bool exact = options.nameKind == ModuleNameKind::ExactPath || HasPathSeparator(name);
            for (const std::string& candidate : decorated) {
                if (!exact) {
                    for (const std::filesystem::path& root : roots) {
                        AddCandidate(candidates, (root / candidate).string());
                    }
                }
                AddCandidate(candidates, candidate);
            }
        }
        return candidates;
    }

    std::string ModuleLoadAttempt::Message() const {
        if (!diagnostic.empty()) {
            return diagnostic;
        }
        return nativeError.ToString();
    }

    std::string ModuleLoadError::Message() const {
        if (attempts.empty()) {
            return "no module candidates were generated";
        }
        std::string message = "failed to load module candidates:";
        for (const ModuleLoadAttempt& attempt : attempts) {
            message += "\n  ";
            message += attempt.candidate;
            message += ": ";
            message += attempt.Message();
        }
        return message;
    }

    ModuleLoadResult ModuleLoader::Load(std::span<const std::string_view> names, ModuleLoadOptions options) {
        auto attempted = GenerateCandidates(names, options);
        ModuleLoadError error;

        for (const std::string& candidate : attempted) {
            const std::string key = CacheKey(candidate, options);

            // 1. See if the module is already cached and return it if so.
            if (options.cachePolicy == ModuleCachePolicy::Shared) {
                std::scoped_lock lock{mutex_};
                if (auto it = cache_.find(key); it != cache_.end()) {
                    if (ModulePtr cached = it->second.lock()) {
                        return cached;
                    }
                    cache_.erase(it);
                }
            }

            // 2. Try to load the module from the native loader and cache it if successful.
            NativeLoadResult loaded = TryLoadNative(candidate, options);
            if (loaded) {
                ModulePtr module{new Module{*loaded, std::filesystem::path{candidate}, options.unloadOnDestroy}};
                if (options.cachePolicy == ModuleCachePolicy::Shared) {
                    std::scoped_lock lock{mutex_};
                    cache_[key] = module;
                }
                return module;
            }
            if (error.attempts.empty()) {
                error.attempts.reserve(attempted.size());
            }
            error.attempts.push_back(ModuleLoadAttempt{.candidate = candidate,
                                                       .nativeError = loaded.error().error,
                                                       .diagnostic = std::move(loaded.error().diagnostic)});
        }
        return std::unexpected{std::move(error)};
    }

    Result<ModuleImage> ModuleLoader::OpenImage(const std::filesystem::path& path) const {
        auto bytes = ReadFileBytes(path);
        if (!bytes) {
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

        ModuleImage image;
        Detail::ModuleImageBuilder::Path(image) = path;
        Detail::ModuleImageBuilder::Bytes(image) = std::move(*bytes);
        if (auto parsed = ParseSections(image); !parsed) {
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }
        return image;
    }

    void ModuleLoader::AddSearchPath(std::filesystem::path path) {
        std::scoped_lock lock{mutex_};
        if (!std::ranges::contains(searchPaths_, path)) {
            searchPaths_.push_back(std::move(path));
        }
    }

    bool ModuleLoader::RemoveSearchPath(const std::filesystem::path& path) {
        std::scoped_lock lock{mutex_};
        auto oldSize = searchPaths_.size();
        std::erase(searchPaths_, path);
        return searchPaths_.size() != oldSize;
    }

    void ModuleLoader::ClearSearchPaths() {
        std::scoped_lock lock{mutex_};
        searchPaths_.clear();
    }

    std::vector<std::filesystem::path> ModuleLoader::SearchPaths() const {
        std::scoped_lock lock{mutex_};
        return searchPaths_;
    }

    void ModuleLoader::PruneCache() {
        std::scoped_lock lock{mutex_};
        std::erase_if(cache_, [](const auto& entry) { return entry.second.expired(); });
    }

    ModuleLoadResult LoadModule(std::span<const std::string_view> names, ModuleLoadOptions options) {
        return ModuleLoader::Default().Load(names, options);
    }

    ModuleLoadResult LoadModule(std::initializer_list<std::string_view> names, ModuleLoadOptions options) {
        return LoadModule(std::span<const std::string_view>{names.begin(), names.size()}, options);
    }

} // namespace Sora::PAL
