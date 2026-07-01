/**
 * @file Dictionary.h
 * @brief Runtime dictionary, relocation-free Core section ABI, PATH scan, and lazy module records.
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Query.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Yuki {

    /** @brief Recoverable errors reported by Dictionary cold-path operations. */
    enum class CoreErrc {
        Ok = 0,
        DuplicateMetaClass,
        DuplicateProvideEntry,
        InvalidManifest,
        InvalidSectionRecord,
        UnsupportedSectionRecord,
        IoError,
        PathNotFound,
        NotPortableExecutable,
        SectionNotFound,
        LoadFailed,
    };

    /** @brief Return a stable diagnostic string for @p error. */
    [[nodiscard]] constexpr std::string_view MessageOf(CoreErrc error) noexcept {
        switch (error) {
        case CoreErrc::Ok:
            return "ok";
        case CoreErrc::DuplicateMetaClass:
            return "duplicate metaclass";
        case CoreErrc::DuplicateProvideEntry:
            return "duplicate provide entry";
        case CoreErrc::InvalidManifest:
            return "invalid manifest";
        case CoreErrc::InvalidSectionRecord:
            return "invalid core section record";
        case CoreErrc::UnsupportedSectionRecord:
            return "unsupported core section record";
        case CoreErrc::IoError:
            return "I/O error";
        case CoreErrc::PathNotFound:
            return "path not found";
        case CoreErrc::NotPortableExecutable:
            return "not a PE/COFF image";
        case CoreErrc::SectionNotFound:
            return "Yuki core section not found";
        case CoreErrc::LoadFailed:
            return "module load failed";
        }
        return "unknown core error";
    }

    /** @brief Runtime availability state of a dictionary record. */
    enum class RecordStatus : std::uint8_t {
        Declared = 0,
        Loaded = 1,
        Disabled = 2,
        Unauthorized = 3,
        Unreachable = 4,
    };

    /** @brief Runtime factory function after a module has been realized. */
    using DefaultFactory = BaseUnknown* (*)();

    /** @brief Runtime bound-facet factory function after a module has been realized. */
    using FacetFactory = BaseUnknown* (*)(BaseUnknown * nucleus);

    /** @brief Runtime condition predicate after a module has been realized. */
    using ConditionFunc = bool (*)() noexcept;

    /** @brief Cold-path library descriptor folded from a module image. */
    struct LibrarySpec {
        std::string name{};
        std::string framework{};
        RecordStatus status{RecordStatus::Declared};
    };

    /** @brief Cold-path factory descriptor; unresolved tokens remain values until module realization. */
    struct FactorySpec {
        Iid object{};
        Iid factory{};
        std::string framework{};
        DefaultFactory create{};
        std::uint32_t createRva{};
        std::string createExport{};
        RecordStatus status{RecordStatus::Declared};
    };

    /** @brief Manifest-level provider fact before it is folded into a runtime ProviderEntry. */
    struct ManifestProvideRecord {
        Iid component{};
        Iid interfaceIid{};
        DispatchKind kind{DispatchKind::Direct};
        RecordStatus status{RecordStatus::Declared};
        std::uint32_t priority{2};
        std::string source{};
    };

    /** @brief Caller-owned manifest used by tests and non-DLL embedding paths. */
    struct Manifest {
        std::vector<ManifestProvideRecord> provides{};
        std::vector<LibrarySpec> libraries{};
        std::vector<FactorySpec> factories{};
    };

    /** @brief Default object factory for complete default-initializable BaseUnknown-anchored types. */
    template<class T>
        requires YObjectClass<T> && std::default_initializable<T>
    [[nodiscard]] BaseUnknown* NewDefaultObject() {
        return MakeOwned<T>().Detach();
    }

    /** @brief Hash functor for IID keys, delegating byte mixing to Mashiro's hashing CPO. */
    struct IidHash {
        /** @brief Return the platform-sized FNV-1a hash of @p iid's UUID representation. */
        [[nodiscard]] std::size_t operator()(Iid iid) const noexcept {
            return static_cast<std::size_t>(Mashiro::Hashing::Hash(iid.ToUuid(), Mashiro::Hashing::Fnv1a64{}));
        }
    };

    /** @brief component-interface key for provider lookup. */
    struct ProvideKey {
        Iid component{};
        Iid interfaceIid{};

        constexpr bool operator==(const ProvideKey&) const noexcept = default;
    };

    /** @brief Hash functor for component-interface keys. */
    struct ProvideKeyHash {
        [[nodiscard]] std::size_t operator()(const ProvideKey& key) const noexcept {
            IidHash hash;
            return Mashiro::Hashing::Combine(hash(key.component), hash(key.interfaceIid));
        }
    };

    /** @brief Runtime dictionary provider record after section/manifest fold. */
    struct DictionaryProvideRecord {
        Iid component{};
        Iid interfaceIid{};
        ProviderEntry entry{};
        RecordStatus status{RecordStatus::Declared};
        std::string source{};
    };

    /** @brief Magic value stored at the head of every Yuki Core section record. */
    inline constexpr std::uint32_t kCoreSectionMagic = 0x594b4352u;
    /** @brief Current relocation-free section ABI version understood by this runtime. */
    inline constexpr std::uint16_t kCoreSectionAbiVersion = 1;

    /** @brief Relocation-free record kind stored in the Core DLL section. */
    enum class CoreSectionRecordKind : std::uint16_t {
        MetaClass = 1,
        Provide = 2,
        Factory = 3,
        Library = 4,
    };

    /** @brief Relocation-free record header common to all section entries. */
    struct CoreSectionRecordHeader {
        std::uint32_t magic{kCoreSectionMagic};
        std::uint16_t abiVersion{kCoreSectionAbiVersion};
        CoreSectionRecordKind kind{CoreSectionRecordKind::MetaClass};
        std::uint32_t recordSize{};
        std::uint32_t flags{};

        constexpr CoreSectionRecordHeader() noexcept = default;

        /** @brief Construct a typed record header. */
        constexpr CoreSectionRecordHeader(CoreSectionRecordKind recordKind, std::uint32_t size,
                                          std::uint32_t recordFlags = 0) noexcept
            : kind{recordKind}, recordSize{size}, flags{recordFlags} {}
    };

    /** @brief Relocation-free metaclass shadow record; strings are offsets into the same section image. */
    struct CoreSectionMetaClassRecord {
        CoreSectionRecordHeader header{CoreSectionRecordKind::MetaClass, sizeof(CoreSectionMetaClassRecord)};
        TypeOfClass type{TypeOfClass::NothingType};
        Iid iid{};
        Iid directBaseIid{};
        std::uint32_t nameOffset{};
        std::uint32_t reserved{};

        constexpr CoreSectionMetaClassRecord() noexcept = default;

        /** @brief Construct a metaclass section record. */
        constexpr CoreSectionMetaClassRecord(TypeOfClass classType, Iid classIid, Iid baseIid = {},
                                             std::uint32_t name = 0) noexcept
            : type{classType}, iid{classIid}, directBaseIid{baseIid}, nameOffset{name} {}
    };

    /** @brief Relocation-free provider record; factories are represented by RVA or export-name offsets. */
    struct CoreSectionProvideRecord {
        CoreSectionRecordHeader header{CoreSectionRecordKind::Provide, sizeof(CoreSectionProvideRecord)};
        Iid component{};
        Iid interfaceIid{};
        DispatchKind kind{DispatchKind::Direct};
        RecordStatus status{RecordStatus::Declared};
        std::uint16_t reserved16{};
        std::uint32_t priority{2};
        std::uint32_t sourceOffset{};
        std::uint32_t factoryRva{};
        std::uint32_t factoryExportOffset{};

        constexpr CoreSectionProvideRecord() noexcept = default;

        /** @brief Construct a provider section record. */
        constexpr CoreSectionProvideRecord(Iid componentIid, Iid interfaceIid,
                                           DispatchKind dispatch = DispatchKind::Direct,
                                           RecordStatus recordStatus = RecordStatus::Declared,
                                           std::uint32_t recordPriority = 2, std::uint32_t source = 0) noexcept
            : component{componentIid},
              interfaceIid{interfaceIid},
              kind{dispatch},
              status{recordStatus},
              priority{recordPriority},
              sourceOffset{source} {}
    };

    /** @brief Relocation-free factory record; runtime function pointers are resolved only after module load. */
    struct CoreSectionFactoryRecord {
        CoreSectionRecordHeader header{CoreSectionRecordKind::Factory, sizeof(CoreSectionFactoryRecord)};
        Iid object{};
        Iid factory{};
        RecordStatus status{RecordStatus::Declared};
        std::uint8_t reserved8{};
        std::uint16_t reserved16{};
        std::uint32_t frameworkOffset{};
        std::uint32_t createRva{};
        std::uint32_t createExportOffset{};

        constexpr CoreSectionFactoryRecord() noexcept = default;

        /** @brief Construct a factory section record. */
        constexpr CoreSectionFactoryRecord(Iid objectIid, Iid factoryIid, std::uint32_t framework = 0,
                                           std::uint32_t rva = 0, std::uint32_t exportName = 0) noexcept
            : object{objectIid},
              factory{factoryIid},
              frameworkOffset{framework},
              createRva{rva},
              createExportOffset{exportName} {}
    };

    /** @brief Relocation-free library record. */
    struct CoreSectionLibraryRecord {
        CoreSectionRecordHeader header{CoreSectionRecordKind::Library, sizeof(CoreSectionLibraryRecord)};
        std::uint32_t nameOffset{};
        std::uint32_t frameworkOffset{};
        RecordStatus status{RecordStatus::Declared};
        std::uint8_t reserved8{};
        std::uint16_t reserved16{};

        constexpr CoreSectionLibraryRecord() noexcept = default;

        /** @brief Construct a library section record. */
        constexpr CoreSectionLibraryRecord(std::uint32_t name, std::uint32_t framework = 0,
                                           RecordStatus recordStatus = RecordStatus::Declared) noexcept
            : nameOffset{name}, frameworkOffset{framework}, status{recordStatus} {}
    };

    static_assert(std::is_trivially_copyable_v<CoreSectionMetaClassRecord>);
    static_assert(std::is_trivially_copyable_v<CoreSectionProvideRecord>);
    static_assert(std::is_trivially_copyable_v<CoreSectionFactoryRecord>);
    static_assert(std::is_trivially_copyable_v<CoreSectionLibraryRecord>);

#if defined(_WIN32)
#    pragma section(".yuki$m", read)
#    define YUKI_CORE_SECTION_ENTRY(Name, Record) __declspec(allocate(".yuki$m")) inline constexpr auto Name = Record
#    define YUKI_CORE_SECTION_RECORD(Name, Type, ...)                                                                  \
        __declspec(allocate(".yuki$m")) inline constexpr Type Name {                                                   \
            __VA_ARGS__                                                                                                \
        }
#else
#    define YUKI_CORE_SECTION_ENTRY(Name, Record)                                                                      \
        [[maybe_unused]] inline constexpr auto Name __attribute__((used, section("yuki_core"))) = Record
#    define YUKI_CORE_SECTION_RECORD(Name, Type, ...)                                                                  \
        [[maybe_unused]] inline constexpr Type Name __attribute__((used, section("yuki_core"))) {                      \
            __VA_ARGS__                                                                                                \
        }
#endif

    /** @brief Static image of one module's Core section, copied from disk before the DLL is loaded. */
    struct ModuleImage {
        std::filesystem::path path{};
        std::vector<std::byte> sectionBytes{};
        std::vector<CoreSectionMetaClassRecord> metaClasses{};
        std::vector<CoreSectionProvideRecord> provides{};
        std::vector<CoreSectionFactoryRecord> factories{};
        std::vector<CoreSectionLibraryRecord> libraries{};
    };

    /** @brief Runtime module record retained by the dictionary after a section image is folded. */
    struct ModuleRecord {
        std::filesystem::path path{};
        RecordStatus status{RecordStatus::Declared};
        std::size_t metaClassCount{};
        std::size_t provideCount{};
        std::size_t factoryCount{};
        std::size_t libraryCount{};
        void* nativeHandle{};
    };

    /** @brief Runtime dictionary for metaclasses, providers, module images, and lazy realization state. */
    class Dictionary {
    public:
        /** @brief Register an externally owned metaclass. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterMetaClass(const MetaClass& meta);

        /** @brief Register or merge a component-interface provider fact. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterProvide(Iid component, Iid interfaceIid,
                                                                    ProviderEntry entry,
                                                                    RecordStatus status = RecordStatus::Loaded,
                                                                    std::string source = {});

        /** @brief Register a library descriptor. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterLibrary(LibrarySpec library);

        /** @brief Register a factory descriptor. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterFactory(FactorySpec factory);

        /** @brief Fold a caller-owned manifest into the dictionary. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterManifest(const Manifest& manifest);

        /** @brief Fold one relocation-free section record without a section string table. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterSectionRecord(const CoreSectionRecordHeader& record);

        /** @brief Fold one typed metaclass section record without relying on header-subobject punning. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterSectionRecord(const CoreSectionMetaClassRecord& record);

        /** @brief Fold one typed provider section record without relying on header-subobject punning. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterSectionRecord(const CoreSectionProvideRecord& record);

        /** @brief Fold one typed factory section record without relying on header-subobject punning. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterSectionRecord(const CoreSectionFactoryRecord& record);

        /** @brief Fold one typed library section record without relying on header-subobject punning. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterSectionRecord(const CoreSectionLibraryRecord& record);

        /** @brief Fold a copied module section image; the owning DLL is not loaded. */
        [[nodiscard]] std::expected<void, CoreErrc> RegisterModuleImage(const ModuleImage& image);

        /** @brief Read one DLL file and copy its Core section records without loading the DLL. */
        [[nodiscard]] std::expected<ModuleImage, CoreErrc> ScanModuleFile(const std::filesystem::path& path) const;

        /** @brief Scan explicit directories for DLLs with a Core section and fold the records found. */
        [[nodiscard]] std::expected<std::size_t, CoreErrc> ScanPath(std::span<const std::filesystem::path> paths);

        /** @brief Scan directories from the process PATH environment variable. */
        [[nodiscard]] std::expected<std::size_t, CoreErrc> ScanPath();

        /** @brief Load a previously scanned module by absolute path without rescanning unrelated PATH entries. */
        [[nodiscard]] std::expected<void, CoreErrc> RealizeModule(const std::filesystem::path& path);

        /** @brief Find a metaclass by IID. */
        [[nodiscard]] const MetaClass* FindMetaClass(Iid iid) const;

        /** @brief Return whether @p derived is the same as or transitively derived from @p base. */
        [[nodiscard]] bool IsAKindOf(Iid derived, Iid base) const;

        /** @brief Find an exact component-interface provider. */
        [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvide(Iid component, Iid interfaceIid) const;

        /** @brief Find a provider on @p component or one of its object-model bases. */
        [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvideInClassChain(Iid component,
                                                                                     Iid interfaceIid) const;

        /** @brief Return a lifetime-safe view of registered metaclasses. */
        [[nodiscard]] PinnedView<const MetaClass*> MetaClasses() const;

        /** @brief Return a lifetime-safe view of registered provider records. */
        [[nodiscard]] PinnedView<DictionaryProvideRecord> ProvideRecords() const;

        /** @brief Return a lifetime-safe view of registered libraries. */
        [[nodiscard]] PinnedView<LibrarySpec> Libraries() const;

        /** @brief Return a lifetime-safe view of registered factories. */
        [[nodiscard]] PinnedView<FactorySpec> Factories() const;

        /** @brief Return a lifetime-safe view of scanned module records. */
        [[nodiscard]] PinnedView<ModuleRecord> Modules() const;

    private:
        [[nodiscard]] std::expected<void, CoreErrc> RegisterMetaClassUnlocked(const MetaClass& meta);
        [[nodiscard]] std::expected<void, CoreErrc> RegisterOwnedMetaClassUnlocked(TypeOfClass type, Iid iid,
                                                                                   Iid directBaseIid, std::string name);
        [[nodiscard]] std::expected<void, CoreErrc> RegisterProvideUnlocked(Iid component, Iid interfaceIid,
                                                                            ProviderEntry entry, RecordStatus status,
                                                                            std::string source);
        [[nodiscard]] std::expected<void, CoreErrc>
        RegisterSectionRecordUnlocked(std::span<const std::byte> recordBytes,
                                      std::span<const std::byte> section,
                                      const std::filesystem::path& path);
        [[nodiscard]] const MetaClass* FindMetaClassUnlocked(Iid iid) const;
        [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvideUnlocked(Iid component, Iid interfaceIid) const;
        void ResolveDirectBaseLinksUnlocked() noexcept;

        mutable std::mutex mutex_{};
        std::unordered_map<Iid, const MetaClass*, IidHash> metaByIid_{};
        std::unordered_map<ProvideKey, DictionaryProvideRecord, ProvideKeyHash> provides_{};
        std::vector<LibrarySpec> libraries_{};
        std::vector<FactorySpec> factories_{};
        std::vector<ModuleRecord> modules_{};
        std::deque<std::string> ownedStrings_{};
        std::vector<std::unique_ptr<MetaClass>> ownedMeta_{};
    };

    /** @brief Return the process-wide dictionary instance. */
    [[nodiscard]] Dictionary& GlobalDictionary();

} // namespace Yuki
