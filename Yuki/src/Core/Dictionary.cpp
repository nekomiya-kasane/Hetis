/**
 * @file Dictionary.cpp
 * @brief Runtime dictionary and relocation-free Core section scanning implementation.
 * @ingroup Core
 */
#include <Yuki/Core/Dictionary.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#endif

namespace Yuki {

    namespace {

        [[nodiscard]] std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
            std::error_code ec;
            std::filesystem::path absolute = std::filesystem::absolute(path, ec);
            return ec ? path : absolute.lexically_normal();
        }

        [[nodiscard]] bool HasDllExtension(const std::filesystem::path& path) {
            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".dll";
        }

        [[nodiscard]] std::expected<std::string, CoreErrc> SectionString(std::span<const std::byte> section,
                                                                         uint32_t offset) {
            if (offset == 0) {
                return std::string{};
            }
            if (offset >= section.size()) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }

            const auto* first = reinterpret_cast<const char*>(section.data() + offset);
            const size_t capacity = section.size() - offset;
            const void* nul = std::memchr(first, '\0', capacity);
            if (!nul) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }
            const auto* last = static_cast<const char*>(nul);
            return std::string{first, static_cast<size_t>(last - first)};
        }

        template<class T>
        [[nodiscard]] std::expected<T, CoreErrc> CopyRecord(std::span<const std::byte> bytes,
                                                            const CoreSectionRecordHeader& header) {
            if (header.recordSize < sizeof(T) || header.recordSize > bytes.size()) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }
            T value;
            std::memcpy(&value, bytes.data(), sizeof(T));
            return value;
        }

        /** @brief Return the byte view of one trivially copyable section record object. */
        template<class T>
        [[nodiscard]] std::span<const std::byte> RecordBytesOf(const T& record) noexcept {
            return std::as_bytes(std::span{&record, 1});
        }

        [[nodiscard]] uint16_t ReadU16(std::span<const std::byte> bytes, size_t offset) noexcept {
            if (offset + 2 > bytes.size()) {
                return 0;
            }
            const auto b0 = static_cast<uint16_t>(std::to_integer<unsigned char>(bytes[offset]));
            const auto b1 = static_cast<uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1]));
            return static_cast<uint16_t>(b0 | (b1 << 8));
        }

        [[nodiscard]] uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset) noexcept {
            if (offset + 4 > bytes.size()) {
                return 0;
            }
            uint32_t result = 0;
            for (size_t i = 0; i < 4; ++i) {
                result |= static_cast<uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8);
            }
            return result;
        }

        [[nodiscard]] std::string ReadSectionName(std::span<const std::byte> bytes, size_t offset) {
            std::string name;
            for (size_t i = 0; i < 8 && offset + i < bytes.size(); ++i) {
                char ch = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + i]));
                if (ch == '\0') {
                    break;
                }
                name.push_back(ch);
            }
            return name;
        }

        [[nodiscard]] bool IsYukiSectionName(std::string_view name) noexcept {
            return name == ".yuki" || name.starts_with(".yuki$") || name.starts_with("yuki_core");
        }

        [[nodiscard]] std::expected<void, CoreErrc> ParseCoreSection(std::span<const std::byte> section,
                                                                     ModuleImage& image) {
            bool found = false;
            for (size_t offset = 0; offset + sizeof(CoreSectionRecordHeader) <= section.size();) {
                CoreSectionRecordHeader header{};
                std::memcpy(&header, section.data() + offset, sizeof(CoreSectionRecordHeader));
                if (header.magic != kCoreSectionMagic) {
                    ++offset;
                    continue;
                }
                if (header.abiVersion != kCoreSectionAbiVersion ||
                    header.recordSize < sizeof(CoreSectionRecordHeader) ||
                    offset + header.recordSize > section.size()) {
                    return std::unexpected(CoreErrc::InvalidSectionRecord);
                }

                std::span<const std::byte> recordBytes{section.data() + offset, header.recordSize};
                switch (header.kind) {
                case CoreSectionRecordKind::MetaClass: {
                    auto record = CopyRecord<CoreSectionMetaClassRecord>(recordBytes, header);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    image.metaClasses.push_back(*record);
                    break;
                }
                case CoreSectionRecordKind::Provide: {
                    auto record = CopyRecord<CoreSectionProvideRecord>(recordBytes, header);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    image.provides.push_back(*record);
                    break;
                }
                case CoreSectionRecordKind::Factory: {
                    auto record = CopyRecord<CoreSectionFactoryRecord>(recordBytes, header);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    image.factories.push_back(*record);
                    break;
                }
                case CoreSectionRecordKind::Library: {
                    auto record = CopyRecord<CoreSectionLibraryRecord>(recordBytes, header);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    image.libraries.push_back(*record);
                    break;
                }
                default:
                    return std::unexpected(CoreErrc::UnsupportedSectionRecord);
                }

                found = true;
                offset += header.recordSize;
            }
            return found ? std::expected<void, CoreErrc>{} : std::unexpected(CoreErrc::SectionNotFound);
        }

    } // namespace

    std::expected<void, CoreErrc> Dictionary::RegisterMetaClassUnlocked(const MetaClass& meta) {
        auto [_, inserted] = metaByIid_.emplace(meta.IidValue(), &meta);
        if (!inserted) {
            return std::unexpected(CoreErrc::DuplicateMetaClass);
        }
        ResolveDirectBaseLinksUnlocked();
        return {};
    }

    std::expected<void, CoreErrc> Dictionary::RegisterOwnedMetaClassUnlocked(TypeOfClass type, Iid iid,
                                                                             Iid directBaseIid, std::string name) {
        if (metaByIid_.contains(iid)) {
            return std::unexpected(CoreErrc::DuplicateMetaClass);
        }
        if (name.empty()) {
            name = "<yuki-section-metaclass>";
        }
        ownedStrings_.push_back(std::move(name));
        const MetaClass* directBase = IsNil(directBaseIid) ? nullptr : FindMetaClassUnlocked(directBaseIid);
        auto meta = std::make_unique<MetaClass>(type, iid, std::string_view{ownedStrings_.back()},
                                                std::span<const ProviderEntry>{}, directBaseIid, directBase);
        const MetaClass* raw = meta.get();
        ownedMeta_.push_back(std::move(meta));
        metaByIid_.emplace(iid, raw);
        ResolveDirectBaseLinksUnlocked();
        return {};
    }

    void Dictionary::ResolveDirectBaseLinksUnlocked() noexcept {
        for (const std::unique_ptr<MetaClass>& meta : ownedMeta_) {
            if (!meta || !meta->HasDirectBase() || meta->DirectBase()) {
                continue;
            }
            meta->BindDirectBase(FindMetaClassUnlocked(meta->DirectBaseIid()));
        }
    }

    std::expected<void, CoreErrc> Dictionary::RegisterProvideUnlocked(Iid component, Iid interfaceIid,
                                                                      ProviderEntry entry, RecordStatus status,
                                                                      std::string source) {
        entry.interfaceIid = interfaceIid;
        ProvideKey key{component, interfaceIid};
        DictionaryProvideRecord record{component, interfaceIid, entry, status, std::move(source)};
        auto [it, inserted] = provides_.emplace(key, record);
        if (inserted) {
            return {};
        }

        const uint32_t oldPriority = it->second.entry.priority;
        const uint32_t newPriority = record.entry.priority;
        if (newPriority < oldPriority) {
            it->second = std::move(record);
            return {};
        }
        return std::unexpected(CoreErrc::DuplicateProvideEntry);
    }

    std::expected<void, CoreErrc> Dictionary::RegisterMetaClass(const MetaClass& meta) {
        std::scoped_lock lock(mutex_);
        return RegisterMetaClassUnlocked(meta);
    }

    std::expected<void, CoreErrc> Dictionary::RegisterProvide(Iid component, Iid interfaceIid, ProviderEntry entry,
                                                              RecordStatus status, std::string source) {
        std::scoped_lock lock(mutex_);
        return RegisterProvideUnlocked(component, interfaceIid, entry, status, std::move(source));
    }

    std::expected<void, CoreErrc> Dictionary::RegisterLibrary(LibrarySpec library) {
        std::scoped_lock lock(mutex_);
        libraries_.push_back(std::move(library));
        return {};
    }

    std::expected<void, CoreErrc> Dictionary::RegisterFactory(FactorySpec factory) {
        std::scoped_lock lock(mutex_);
        factories_.push_back(std::move(factory));
        return {};
    }

    std::expected<void, CoreErrc> Dictionary::RegisterManifest(const Manifest& manifest) {
        for (const ManifestProvideRecord& provide : manifest.provides) {
            ProviderEntry entry{provide.interfaceIid, provide.kind, nullptr, nullptr, provide.priority};
            if (auto result =
                    RegisterProvide(provide.component, provide.interfaceIid, entry, provide.status, provide.source);
                !result) {
                return result;
            }
        }
        for (const LibrarySpec& library : manifest.libraries) {
            if (auto result = RegisterLibrary(library); !result) {
                return result;
            }
        }
        for (const FactorySpec& factory : manifest.factories) {
            if (auto result = RegisterFactory(factory); !result) {
                return result;
            }
        }
        return {};
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecordUnlocked(std::span<const std::byte> recordBytes,
                                                                            std::span<const std::byte> section,
                                                                            const std::filesystem::path& path) {
        if (recordBytes.size() < sizeof(CoreSectionRecordHeader)) {
            return std::unexpected(CoreErrc::InvalidSectionRecord);
        }

        CoreSectionRecordHeader header{};
        std::memcpy(&header, recordBytes.data(), sizeof(header));
        if (header.magic != kCoreSectionMagic || header.abiVersion != kCoreSectionAbiVersion ||
            header.recordSize < sizeof(CoreSectionRecordHeader) || header.recordSize > recordBytes.size()) {
            return std::unexpected(CoreErrc::InvalidSectionRecord);
        }

        switch (header.kind) {
        case CoreSectionRecordKind::MetaClass: {
            auto record = CopyRecord<CoreSectionMetaClassRecord>(recordBytes, header);
            if (!record) {
                return std::unexpected(record.error());
            }
            auto name = SectionString(section, record->nameOffset);
            if (!name) {
                return std::unexpected(name.error());
            }
            return RegisterOwnedMetaClassUnlocked(record->type, record->iid, record->directBaseIid,
                                                  std::move(*name));
        }
        case CoreSectionRecordKind::Provide: {
            auto record = CopyRecord<CoreSectionProvideRecord>(recordBytes, header);
            if (!record) {
                return std::unexpected(record.error());
            }
            auto source = SectionString(section, record->sourceOffset);
            if (!source) {
                return std::unexpected(source.error());
            }
            if (source->empty() && !path.empty()) {
                *source = path.string();
            }
            ProviderEntry entry{record->interfaceIid, record->kind, nullptr, nullptr, record->priority};
            return RegisterProvideUnlocked(record->component, record->interfaceIid, entry, record->status,
                                           std::move(*source));
        }
        case CoreSectionRecordKind::Factory: {
            auto record = CopyRecord<CoreSectionFactoryRecord>(recordBytes, header);
            if (!record) {
                return std::unexpected(record.error());
            }
            auto framework = SectionString(section, record->frameworkOffset);
            auto exportName = SectionString(section, record->createExportOffset);
            if (!framework || !exportName) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }
            factories_.push_back(FactorySpec{record->object, record->factory, std::move(*framework), nullptr,
                                             record->createRva, std::move(*exportName), record->status});
            return {};
        }
        case CoreSectionRecordKind::Library: {
            auto record = CopyRecord<CoreSectionLibraryRecord>(recordBytes, header);
            if (!record) {
                return std::unexpected(record.error());
            }
            auto name = SectionString(section, record->nameOffset);
            auto framework = SectionString(section, record->frameworkOffset);
            if (!name || !framework) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }
            libraries_.push_back(LibrarySpec{std::move(*name), std::move(*framework), record->status});
            return {};
        }
        }

        return std::unexpected(CoreErrc::UnsupportedSectionRecord);
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecord(const CoreSectionRecordHeader& record) {
        std::scoped_lock lock(mutex_);
        return RegisterSectionRecordUnlocked(RecordBytesOf(record), {}, {});
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecord(const CoreSectionMetaClassRecord& record) {
        std::scoped_lock lock(mutex_);
        auto recordBytes = RecordBytesOf(record);
        return RegisterSectionRecordUnlocked(recordBytes, recordBytes, {});
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecord(const CoreSectionProvideRecord& record) {
        std::scoped_lock lock(mutex_);
        auto recordBytes = RecordBytesOf(record);
        return RegisterSectionRecordUnlocked(recordBytes, recordBytes, {});
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecord(const CoreSectionFactoryRecord& record) {
        std::scoped_lock lock(mutex_);
        auto recordBytes = RecordBytesOf(record);
        return RegisterSectionRecordUnlocked(recordBytes, recordBytes, {});
    }

    std::expected<void, CoreErrc> Dictionary::RegisterSectionRecord(const CoreSectionLibraryRecord& record) {
        std::scoped_lock lock(mutex_);
        auto recordBytes = RecordBytesOf(record);
        return RegisterSectionRecordUnlocked(recordBytes, recordBytes, {});
    }

    std::expected<void, CoreErrc> Dictionary::RegisterModuleImage(const ModuleImage& image) {
        const std::filesystem::path modulePath = AbsolutePath(image.path);
        std::scoped_lock lock(mutex_);
        auto registerRecord = [&](const auto& record) {
            return RegisterSectionRecordUnlocked(RecordBytesOf(record), image.sectionBytes, modulePath);
        };
        for (const CoreSectionMetaClassRecord& record : image.metaClasses) {
            if (auto result = registerRecord(record); !result) {
                return result;
            }
        }
        for (const CoreSectionProvideRecord& record : image.provides) {
            if (auto result = registerRecord(record); !result) {
                return result;
            }
        }
        for (const CoreSectionFactoryRecord& record : image.factories) {
            if (auto result = registerRecord(record); !result) {
                return result;
            }
        }
        for (const CoreSectionLibraryRecord& record : image.libraries) {
            if (auto result = registerRecord(record); !result) {
                return result;
            }
        }
        modules_.push_back(ModuleRecord{modulePath, RecordStatus::Declared, image.metaClasses.size(),
                                        image.provides.size(), image.factories.size(), image.libraries.size(),
                                        nullptr});
        return {};
    }

    std::expected<ModuleImage, CoreErrc> Dictionary::ScanModuleFile(const std::filesystem::path& path) const {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return std::unexpected(CoreErrc::PathNotFound);
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return std::unexpected(CoreErrc::IoError);
        }
        const std::streamoff size = file.tellg();
        if (size <= 0) {
            return std::unexpected(CoreErrc::NotPortableExecutable);
        }
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            return std::unexpected(CoreErrc::IoError);
        }

        std::span<const std::byte> image{bytes};
        if (ReadU16(image, 0) != 0x5a4d) {
            return std::unexpected(CoreErrc::NotPortableExecutable);
        }
        const uint32_t ntOffset = ReadU32(image, 0x3c);
        if (ntOffset == 0 || ntOffset + 24 > image.size() || ReadU32(image, ntOffset) != 0x00004550) {
            return std::unexpected(CoreErrc::NotPortableExecutable);
        }

        const size_t fileHeader = ntOffset + 4;
        const uint16_t sectionCount = ReadU16(image, fileHeader + 2);
        const uint16_t optionalHeaderSize = ReadU16(image, fileHeader + 16);
        const size_t sectionTable = fileHeader + 20 + optionalHeaderSize;
        if (sectionCount == 0 || sectionTable + static_cast<size_t>(sectionCount) * 40 > image.size()) {
            return std::unexpected(CoreErrc::NotPortableExecutable);
        }

        for (uint16_t index = 0; index < sectionCount; ++index) {
            const size_t sectionHeader = sectionTable + static_cast<size_t>(index) * 40;
            if (!IsYukiSectionName(ReadSectionName(image, sectionHeader))) {
                continue;
            }

            const uint32_t rawSize = ReadU32(image, sectionHeader + 16);
            const uint32_t rawPointer = ReadU32(image, sectionHeader + 20);
            if (rawSize == 0 || rawPointer == 0 || static_cast<size_t>(rawPointer) + rawSize > image.size()) {
                return std::unexpected(CoreErrc::InvalidSectionRecord);
            }

            ModuleImage result;
            result.path = AbsolutePath(path);
            result.sectionBytes.assign(bytes.begin() + rawPointer, bytes.begin() + rawPointer + rawSize);
            if (auto parsed = ParseCoreSection(result.sectionBytes, result); !parsed) {
                return std::unexpected(parsed.error());
            }
            return result;
        }

        return std::unexpected(CoreErrc::SectionNotFound);
    }

    std::expected<size_t, CoreErrc> Dictionary::ScanPath(std::span<const std::filesystem::path> paths) {
        size_t loaded = 0;
        for (const std::filesystem::path& directory : paths) {
            std::error_code ec;
            if (!std::filesystem::is_directory(directory, ec) || ec) {
                continue;
            }
            for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
                if (ec || !it->is_regular_file(ec) || !HasDllExtension(it->path())) {
                    continue;
                }
                auto image = ScanModuleFile(it->path());
                if (!image) {
                    continue;
                }
                if (auto registered = RegisterModuleImage(*image); registered) {
                    ++loaded;
                }
            }
        }
        return loaded;
    }

    std::expected<size_t, CoreErrc> Dictionary::ScanPath() {
        const char* rawPath = std::getenv("PATH");
        if (!rawPath) {
            return size_t{};
        }

        std::vector<std::filesystem::path> paths;
        std::string_view text{rawPath};
#if defined(_WIN32)
        constexpr char kSeparator = ';';
#else
        constexpr char kSeparator = ':';
#endif
        while (!text.empty()) {
            const size_t next = text.find(kSeparator);
            std::string_view part = next == std::string_view::npos ? text : text.substr(0, next);
            if (!part.empty()) {
                paths.emplace_back(part);
            }
            if (next == std::string_view::npos) {
                break;
            }
            text.remove_prefix(next + 1);
        }
        return ScanPath(paths);
    }

    std::expected<void, CoreErrc> Dictionary::RealizeModule(const std::filesystem::path& path) {
        const std::filesystem::path absolute = AbsolutePath(path);
        {
            std::scoped_lock lock(mutex_);
            auto it =
                std::ranges::find_if(modules_, [&](const ModuleRecord& module) { return module.path == absolute; });
            if (it == modules_.end()) {
                return std::unexpected(CoreErrc::PathNotFound);
            }
            if (it->status == RecordStatus::Loaded) {
                return {};
            }
        }

#if defined(_WIN32)
        HMODULE handle = LoadLibraryW(absolute.c_str());
        if (!handle) {
            return std::unexpected(CoreErrc::LoadFailed);
        }
        std::scoped_lock lock(mutex_);
        for (ModuleRecord& module : modules_) {
            if (module.path == absolute) {
                module.status = RecordStatus::Loaded;
                module.nativeHandle = handle;
                break;
            }
        }
        return {};
#else
        return std::unexpected(CoreErrc::LoadFailed);
#endif
    }

    const MetaClass* Dictionary::FindMetaClassUnlocked(Iid iid) const {
        auto it = metaByIid_.find(iid);
        return it == metaByIid_.end() ? nullptr : it->second;
    }

    std::optional<DictionaryProvideRecord> Dictionary::FindProvideUnlocked(Iid component, Iid interfaceIid) const {
        auto it = provides_.find(ProvideKey{component, interfaceIid});
        if (it == provides_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    const MetaClass* Dictionary::FindMetaClass(Iid iid) const {
        std::scoped_lock lock(mutex_);
        return FindMetaClassUnlocked(iid);
    }

    bool Dictionary::IsAKindOf(Iid derived, Iid base) const {
        if (IsNil(derived) || IsNil(base)) {
            return false;
        }
        std::scoped_lock lock(mutex_);
        Iid cursor = derived;
        for (size_t depth = 0; depth <= metaByIid_.size(); ++depth) {
            if (cursor == base) {
                return true;
            }
            const MetaClass* meta = FindMetaClassUnlocked(cursor);
            if (!meta || !meta->HasDirectBase()) {
                return false;
            }
            cursor = meta->DirectBase() ? meta->DirectBase()->IidValue() : meta->DirectBaseIid();
        }
        return false;
    }

    std::optional<DictionaryProvideRecord> Dictionary::FindProvide(Iid component, Iid interfaceIid) const {
        std::scoped_lock lock(mutex_);
        return FindProvideUnlocked(component, interfaceIid);
    }

    std::optional<DictionaryProvideRecord> Dictionary::FindProvideInClassChain(Iid component, Iid interfaceIid) const {
        if (IsNil(component) || IsNil(interfaceIid)) {
            return std::nullopt;
        }
        std::scoped_lock lock(mutex_);
        Iid cursor = component;
        for (size_t depth = 0; depth <= metaByIid_.size(); ++depth) {
            if (std::optional<DictionaryProvideRecord> record = FindProvideUnlocked(cursor, interfaceIid)) {
                return record;
            }
            const MetaClass* meta = FindMetaClassUnlocked(cursor);
            if (!meta || !meta->HasDirectBase()) {
                return std::nullopt;
            }
            cursor = meta->DirectBase() ? meta->DirectBase()->IidValue() : meta->DirectBaseIid();
        }
        return std::nullopt;
    }

    PinnedView<const MetaClass*> Dictionary::MetaClasses() const {
        std::scoped_lock lock(mutex_);
        std::vector<const MetaClass*> values;
        values.reserve(metaByIid_.size());
        for (const auto& meta : metaByIid_ | std::views::values) {
            values.push_back(meta);
        }
        return PinnedView<const MetaClass*>{std::move(values)};
    }

    PinnedView<DictionaryProvideRecord> Dictionary::ProvideRecords() const {
        std::scoped_lock lock(mutex_);
        std::vector<DictionaryProvideRecord> values;
        values.reserve(provides_.size());
        for (const auto& record : provides_ | std::views::values) {
            values.push_back(record);
        }
        return PinnedView<DictionaryProvideRecord>{std::move(values)};
    }

    PinnedView<LibrarySpec> Dictionary::Libraries() const {
        std::scoped_lock lock(mutex_);
        return PinnedView<LibrarySpec>{libraries_};
    }

    PinnedView<FactorySpec> Dictionary::Factories() const {
        std::scoped_lock lock(mutex_);
        return PinnedView<FactorySpec>{factories_};
    }

    PinnedView<ModuleRecord> Dictionary::Modules() const {
        std::scoped_lock lock(mutex_);
        return PinnedView<ModuleRecord>{modules_};
    }

    Dictionary& GlobalDictionary() {
        static Dictionary dictionary;
        return dictionary;
    }

} // namespace Yuki
