
/**
 * @file Module.cpp
 * @brief Platform implementation of module image inspection, dynamic loading, and symbol lookup.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/PAL/File.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <Sora/Core/Path.h>
#include <Sora/Core/Wire.h>
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <array>
#include <expected>
#include <functional>
#include <limits>
#include <new>
#include <utility>

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

        using NativeLoadResult = Result<void*>;

        /** @brief Add @p candidate if it has not already appeared. */
        template<typename Candidate>
        void AddCandidate(std::vector<Candidate>& candidates, Candidate candidate) {
            if (!candidate.empty() && !std::ranges::contains(candidates, candidate)) {
                candidates.push_back(std::move(candidate));
            }
        }

        /** @brief Shared-library suffixes accepted while generating portable module candidates. */
        inline constexpr std::array kSharedLibrarySuffixes{std::string_view{".dll"}, std::string_view{".so"},
                                                           std::string_view{".dylib"}};

        /** @brief Find a terminal or versioned shared-library suffix without allocating. */
        [[nodiscard]] constexpr size_t FindSharedLibrarySuffix(std::string_view name) noexcept {
            for (std::string_view suffix : kSharedLibrarySuffixes) {
                size_t position = name.find(suffix);
                while (position != std::string_view::npos) {
                    const size_t end = position + suffix.size();
                    if (end == name.size() || (end < name.size() && name[end] == '.')) {
                        return position;
                    }
                    position = name.find(suffix, position + 1);
                }
            }
            return std::string_view::npos;
        }

        /** @brief Generate decorated filename candidates for one spelling without applying search roots. */
        [[nodiscard]] std::vector<std::string> DecorateName(std::string_view name, ModuleLoadOptions options) {
            // 1. Preserve the caller-provided spelling as the highest-priority candidate.
            std::vector<std::string> decorated;
            AddCandidate(decorated, std::string{name});
            if (options.candidatePolicy == ModuleCandidatePolicy::ExactOnly) {
                return decorated;
            }

            // 2. Split path and filename so decoration never alters the directory component.
            const bool pathLike = Sora::HasPathSeparator(name);
            if (options.nameKind == ModuleNameKind::ExactPath && pathLike &&
                FindSharedLibrarySuffix(name) != std::string_view::npos) {
                return decorated;
            }

            auto [directory, filename] = Sora::SplitDirectory(name);
            std::string stem{filename};
            const size_t suffixPosition = FindSharedLibrarySuffix(filename);
            const bool filenameHasSuffix = suffixPosition != std::string_view::npos;
            if (options.nameKind != ModuleNameKind::Stem && filenameHasSuffix) {
                stem.resize(suffixPosition);
            }
            std::array prefixes{std::string_view{}, std::string_view{"lib"}};

            // 3. Combine portable prefixes and suffixes while retaining first-seen priority.
            for (std::string_view prefix : prefixes) {
                if (!prefix.empty() && stem.starts_with(prefix)) {
                    continue;
                }
                const auto appendCandidate = [&](std::string_view suffix) {
                    std::string candidate;
                    candidate.reserve(directory.size() + prefix.size() + stem.size() + suffix.size());
                    candidate += directory;
                    candidate += prefix;
                    candidate += stem;
                    candidate += suffix;
                    AddCandidate(decorated, std::move(candidate));
                };
                for (std::string_view suffix : std::array{std::string_view{}, Sora::Platform::kSharedLibrarySuffix}) {
                    if (suffix.empty() && filenameHasSuffix) {
                        continue;
                    }
                    appendCandidate(suffix);
                }
                for (std::string_view suffix : kSharedLibrarySuffixes) {
                    appendCandidate(suffix);
                }
            }
            return decorated;
        }

        /** @brief Read an entire file as bytes. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path) {
            auto file = File::Open(path);
            if (!file) {
                return std::unexpected{file.error()};
            }
            const auto size = file->Size();
            if (!size) {
                return std::unexpected{size.error()};
            }
            if (*size > std::numeric_limits<size_t>::max()) {
                return std::unexpected{ErrorCode::OutOfRange};
            }
            try {
                std::vector<std::byte> bytes(static_cast<size_t>(*size));
                if (auto read = file->ReadAllAt(bytes, 0); !read) {
                    return std::unexpected{read.error()};
                }
                return bytes;
            } catch (const std::bad_alloc&) {
                return std::unexpected{ErrorCode::OutOfMemory};
            }
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

        struct FixedTextField {
            uint64_t offset;
            size_t size;
        };

        /** @brief Decode a fixed-width, NUL-padded section name. */
        [[nodiscard]] std::string FixedName(std::span<const std::byte> bytes, FixedTextField field) {
            std::string name;
            name.reserve(field.size);
            for (size_t i = 0; i < field.size; ++i) {
                char c = static_cast<char>(std::to_integer<uint8_t>(bytes[static_cast<size_t>(field.offset) + i]));
                if (c == '\0') {
                    break;
                }
                name.push_back(c);
            }
            return name;
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
                sections.push_back(SectionView{
                    .name = FixedName(bytes, {.offset = base, .size = 8}),
                    .bytes = Wire::Subspan(storage, rawOffset, rawSize).value_or(std::span<const std::byte>{}),
                    .virtualAddress = virtualAddress,
                    .fileOffset = rawOffset,
                    .flags = PeFlags(characteristics)});
                if (rawSize == 0 && virtualSize != 0) {
                    sections.back().flags = sections.back().flags | SectionFlag::Uninitialized;
                }
            }
            return {};
        }

        struct ElfSectionMetadata {
            uint64_t flags;
            uint32_t type;
        };

        /** @brief Normalize ELF section flags and type. */
        [[nodiscard]] SectionFlag ElfFlags(ElfSectionMetadata metadata) noexcept {
            SectionFlag result = SectionFlag::None;
            if ((metadata.flags & 0x2u) != 0) {
                result = result | SectionFlag::Alloc;
            }
            if ((metadata.flags & 0x1u) != 0) {
                result = result | SectionFlag::Write;
            }
            if ((metadata.flags & 0x4u) != 0) {
                result = result | SectionFlag::Execute | SectionFlag::Code;
            }
            if (metadata.type == 8u) {
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
            // 1. Validate the ELF class and byte order before reading class-dependent fields.
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
            // 2. Validate the complete section table and its string-table index.
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

            // 3. Materialize normalized section views using the validated name table.
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
                sections.push_back(SectionView{
                    .name = ElfString(bytes, strOffset, strSize, nameOffset),
                    .bytes = type == 8u ? std::span<const std::byte>{}
                                        : Wire::Subspan(storage, offset, size).value_or(std::span<const std::byte>{}),
                    .virtualAddress = address,
                    .fileOffset = offset,
                    .flags = ElfFlags({.flags = flags, .type = type})});
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
            // 1. Walk the bounded load-command stream.
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
                    // 2. Validate each segment's complete section array before accessing it.
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
                    // 3. Convert validated native section records into portable views.
                    for (uint32_t section = 0; section < nsects; ++section) {
                        const uint64_t base = sectionBase + static_cast<uint64_t>(section) * sectionSize;
                        const uint64_t address =
                            is64 ? ReadLe<uint64_t>(bytes, base + 32) : ReadLe<uint32_t>(bytes, base + 32);
                        const uint64_t size =
                            is64 ? ReadLe<uint64_t>(bytes, base + 40) : ReadLe<uint32_t>(bytes, base + 36);
                        const uint32_t offset = ReadLe<uint32_t>(bytes, base + (is64 ? 48u : 40u));
                        const uint32_t attributes = ReadLe<uint32_t>(bytes, base + (is64 ? 68u : 56u));
                        sections.push_back(SectionView{
                            .name = FixedName(bytes, {.offset = base, .size = 16}),
                            .bytes = Wire::Subspan(storage, offset, size).value_or(std::span<const std::byte>{}),
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

#if defined(PLATFORM_WINDOWS)
        /** @brief Observe the current process image without acquiring unload ownership. */
        [[nodiscard]] void* CurrentProcessNativeHandle() noexcept {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            return EnsureSystemAPIs(api.getModuleHandleWide) ? api.getModuleHandleWide(nullptr) : nullptr;
        }

        /** @brief Load @p candidate through the normalized Win32 loader table. */
        [[nodiscard]] NativeLoadResult TryLoadNative(const std::filesystem::path& candidate, ModuleLoadOptions) {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            if (!EnsureSystemAPIs(api.loadLibraryWide)) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            if (void* handle = api.loadLibraryWide(candidate.c_str())) {
                return handle;
            }
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

        /** @brief Close a Win32 module through the normalized loader table. */
        void CloseNative(void* handle) noexcept {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            if (handle && EnsureSystemAPIs(api.freeLibrary)) {
                static_cast<void>(api.freeLibrary(handle));
            }
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        /** @brief Acquire a process-lifetime handle spanning the current POSIX global symbol scope. */
        [[nodiscard]] void* CurrentProcessNativeHandle() noexcept {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            return api.open != nullptr ? api.open(nullptr, PosixSystem::kDynamicLazy | PosixSystem::kDynamicLocal)
                                       : nullptr;
        }

        /** @brief Load @p candidate through the normalized POSIX loader table. */
        [[nodiscard]] NativeLoadResult TryLoadNative(const std::filesystem::path& candidate,
                                                     ModuleLoadOptions options) {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            if (api.open == nullptr) {
                return std::unexpected{ErrorCode::ModuleLoadFailed};
            }
            int flags = options.bindMode == ModuleBindMode::Now ? PosixSystem::kDynamicNow : PosixSystem::kDynamicLazy;
            flags |= options.visibility == ModuleVisibility::Global ? PosixSystem::kDynamicGlobal
                                                                    : PosixSystem::kDynamicLocal;
            if (void* handle = api.open(candidate.c_str(), flags)) {
                return handle;
            }
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

        /** @brief Close a POSIX module through the normalized loader table. */
        void CloseNative(void* handle) noexcept {
            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            if (handle != nullptr && api.close != nullptr) {
                static_cast<void>(api.close(handle));
            }
        }
#else
        /** @brief Report that dynamic module loading is unavailable on this platform. */
        [[nodiscard]] NativeLoadResult TryLoadNative(const std::filesystem::path&, ModuleLoadOptions) {
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

        /** @brief Report that the current process has no dynamic module view on this platform. */
        [[nodiscard]] void* CurrentProcessNativeHandle() noexcept {
            return nullptr;
        }

        /** @brief Ignore a native handle on an unsupported platform. */
        void CloseNative(void*) noexcept {}
#endif

        /** @brief Close a newly loaded native handle unless ownership is transferred to a Module. */
        class NativeModuleGuard {
        public:
            explicit NativeModuleGuard(void* handle) noexcept : handle_{handle} {}

            NativeModuleGuard(const NativeModuleGuard&) = delete;
            NativeModuleGuard& operator=(const NativeModuleGuard&) = delete;

            ~NativeModuleGuard() { CloseNative(handle_); }

            [[nodiscard]] void* Get() const noexcept { return handle_; }

            void Release() noexcept { handle_ = nullptr; }

        private:
            void* handle_ = nullptr;
        };

    } // namespace

    Result<Ref<const SectionView>> ModuleImage::FindSection(std::string_view name) const noexcept {
        auto it = std::ranges::find_if(sections_, [name](const SectionView& section) { return section.name == name; });
        if (it != sections_.end()) {
            return std::ref(*it);
        }
        return std::unexpected{ErrorCode::SectionNotFound};
    }

    namespace Detail {

        void* FindNativeSymbol(void* handle, std::string_view symbol) noexcept {
            if (handle == nullptr || symbol.empty()) {
                return nullptr;
            }
#if !defined(PLATFORM_WINDOWS) && !defined(PLATFORM_LINUX) && !defined(PLATFORM_MACOS)
            static_cast<void>(handle);
            return nullptr;
#endif

            constexpr size_t kStackNameCapacity = 256;
            std::array<char, kStackNameCapacity> stackName{};
            std::string ownedName;
            const char* nativeName = nullptr;
            if (symbol.size() < stackName.size()) {
                std::ranges::copy(symbol, stackName.begin());
                nativeName = stackName.data();
            } else {
                try {
                    ownedName.assign(symbol);
                    nativeName = ownedName.c_str();
                } catch (...) {
                    return nullptr;
                }
            }

            const ModuleSystemAPI& api = LoadModuleSystemAPI();
            return EnsureSystemAPIs(api.findSymbol) ? api.findSymbol(handle, nativeName) : nullptr;
        }

    } // namespace Detail
    Module::Module(void* handle, std::filesystem::path path, bool unloadOnDestroy) noexcept
        : handle_(handle), path_(std::move(path)), unloadOnDestroy_(unloadOnDestroy) {}

    Module::~Module() {
        if (handle_ != nullptr && unloadOnDestroy_) {
            CloseNative(handle_);
        }
    }

    const ModulePtr& CurrentProcessModule() noexcept {
        static const ModulePtr module = [] {
            void* handle = CurrentProcessNativeHandle();
            if (handle == nullptr) {
                return ModulePtr{};
            }
            try {
                return ModulePtr{new Module{handle, {}, false}};
            } catch (...) {
                return ModulePtr{};
            }
        }();
        return module;
    }

    ModuleLoader& ModuleLoader::Default() noexcept {
        static ModuleLoader loader;
        return loader;
    }

    ModuleLoader::ModuleLoader() = default;

    std::vector<std::filesystem::path> ModuleLoader::GenerateCandidates(std::span<const std::string_view> names,
                                                                        ModuleLoadOptions options) const {
        // 1. Snapshot configured roots under the lock, then append per-call roots.
        std::vector<std::filesystem::path> roots;
        {
            std::scoped_lock lock{mutex_};
            roots = searchPaths_;
        }
        roots.insert_range(roots.end(), options.searchPaths);

        std::vector<std::filesystem::path> candidates;
        candidates.reserve(names.size() * std::max<size_t>(roots.size() + 1u, 1u) * 8u);

        // 2. Decorate each name and apply roots only to non-exact spellings.
        for (std::string_view name : names) {
            std::vector<std::string> decorated = DecorateName(name, options);
            const bool exact = options.nameKind == ModuleNameKind::ExactPath || Sora::HasPathSeparator(name);
            for (const std::string& candidate : decorated) {
                const std::u8string utf8Candidate{candidate.begin(), candidate.end()};
                const std::filesystem::path nativeCandidate{utf8Candidate};
                if (!exact) {
                    for (const std::filesystem::path& root : roots) {
                        AddCandidate(candidates, root / nativeCandidate);
                    }
                }
                AddCandidate(candidates, nativeCandidate);
            }
        }

        return candidates;
    }

    Result<ModulePtr> ModuleLoader::Load(std::span<const std::string_view> names, ModuleLoadOptions options) {
        auto attempted = GenerateCandidates(names, options);

        for (const std::filesystem::path& candidate : attempted) {
            CacheKey key;

            // 1. See if the module is already cached and return it if so.
            if (options.cachePolicy == ModuleCachePolicy::Shared) {
                std::error_code error;
                std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
                if (error) {
                    canonical = candidate;
                }
                key = CacheKey{
                    .path = std::move(canonical),
                    .bindMode = options.bindMode,
                    .visibility = options.visibility,
                    .unloadOnDestroy = options.unloadOnDestroy,
                };
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
                NativeModuleGuard nativeHandle{*loaded};
                Module* moduleObject = new Module{nativeHandle.Get(), candidate, options.unloadOnDestroy};
                nativeHandle.Release();
                ModulePtr module{moduleObject};
                if (options.cachePolicy == ModuleCachePolicy::Shared) {
                    std::scoped_lock lock{mutex_};
                    if (auto it = cache_.find(key); it != cache_.end()) {
                        if (ModulePtr cached = it->second.lock()) {
                            return cached;
                        }
                    }
                    cache_[key] = module;
                }
                return module;
            }
        }
        return std::unexpected{ErrorCode::ModuleLoadFailed};
    }

    Result<ModuleImage> ModuleLoader::OpenImage(const std::filesystem::path& path) const {
        // 1. Read the complete image so every section view can reference stable storage.
        auto bytes = ReadFileBytes(path);
        if (!bytes) {
            return std::unexpected{ErrorCode::ModuleLoadFailed};
        }

        // 2. Transfer storage into the image before constructing any non-owning views.
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

    Result<ModulePtr> LoadModule(std::span<const std::string_view> names, ModuleLoadOptions options) {
        return ModuleLoader::Default().Load(names, options);
    }

    Result<ModulePtr> LoadModule(std::initializer_list<std::string_view> names, ModuleLoadOptions options) {
        return LoadModule(std::span<const std::string_view>{names.begin(), names.size()}, options);
    }

} // namespace Sora::PAL
