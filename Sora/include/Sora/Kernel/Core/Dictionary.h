// #pragma once

// #include "Sora/Kernel/Core/MetaClass.h"

// #include <cstdint>
// #include <expected>
// #include <filesystem>
// #include <vector>

// namespace Sora::Kernel {

// #if defined(_WIN32)
// #    pragma section(".sora$m", read)
// #    define DICTIONARY_SECTION_ENTRY(Name, Record) __declspec(allocate(".sora$m")) inline constexpr auto Name = Record
// #    define DICTIONARY_SECTION_RECORD(Name, Type, ...)                                                                 \
//         __declspec(allocate(".sora$m")) inline constexpr Type Name {                                                   \
//             __VA_ARGS__                                                                                                \
//         }
// #else
// #    define DICTIONARY_SECTION_ENTRY(Name, Record)                                                                     \
//         [[maybe_unused]] inline constexpr auto Name __attribute__((used, section("sora_core"))) = Record
// #    define DICTIONARY_SECTION_RECORD(Name, Type, ...)                                                                 \
//         [[maybe_unused]] inline constexpr Type Name __attribute__((used, section("sora_core"))) {                      \
//             __VA_ARGS__                                                                                                \
//         }
// #endif

//     /** @brief Recoverable errors reported by Dictionary cold-path operations. */
//     enum class DictErrorCode : uint8_t {
//         Ok = 0,
//         DuplicateMetaClass,
//         DuplicateProvideEntry,
//         InvalidManifest,
//         InvalidSectionRecord,
//         UnsupportedSectionRecord,
//         IoError,
//         PathNotFound,
//         NotPortableExecutable,
//         SectionNotFound,
//         LoadFailed,
//     };

//     /** @brief Runtime availability state of a dictionary record. */
//     enum class RecordStatus : uint8_t {
//         Declared = 0,
//         Loaded = 1,
//         Disabled = 2,
//         Unauthorized = 3,
//         Unreachable = 4,
//     };

//     /** @brief Runtime condition predicate after a module has been realized. */
//     using ConditionFunc = bool (*)() noexcept;

//     /** @brief Magic value stored at the head of every Yuki Core section record. */
//     inline constexpr std::uint32_t kDictSectionMagic = 0x594b4352u;
//     /** @brief Current relocation-free section ABI version understood by this runtime. */
//     inline constexpr std::uint16_t kDictSectionAbiVersion = 1;

//     /** @brief Relocation-free record kind stored in the Core DLL section. */
//     enum class DictSectionRecordKind : std::uint16_t {
//         MetaClass = 1,
//         Provide = 2,
//         Factory = 3,
//         Library = 4,
//     };

//     /** @brief Relocation-free record header common to all section entries. */
//     struct DictSectionRecordHeader {
//         std::uint32_t magic{kDictSectionMagic};
//         std::uint16_t abiVersion{kDictSectionAbiVersion};
//         DictSectionRecordKind kind{DictSectionRecordKind::MetaClass};
//         std::uint32_t recordSize{};
//         std::uint32_t flags{};

//         constexpr DictSectionRecordHeader() noexcept = default;

//         /** @brief Construct a typed record header. */
//         constexpr DictSectionRecordHeader(DictSectionRecordKind recordKind, std::uint32_t size,
//                                           std::uint32_t recordFlags = 0) noexcept
//             : kind{recordKind}, recordSize{size}, flags{recordFlags} {}
//     };

//     /** @brief Static image of one module's Core section, copied from disk before the DLL is loaded. */
//     struct ModuleImage {
//         std::filesystem::path path{};
//         std::vector<std::byte> sectionBytes{};
//         std::vector<DictSectionMetaClassRecord> metaClasses{};
//         std::vector<DictSectionProvideRecord> provides{};
//         std::vector<DictSectionFactoryRecord> factories{};
//         std::vector<DictSectionLibraryRecord> libraries{};
//     };

//     /** @brief Runtime module record retained by the dictionary after a section image is folded. */
//     struct ModuleRecord {
//         std::filesystem::path path{};
//         RecordStatus status{RecordStatus::Declared};
//         size_t metaClassCount{};
//         size_t provideCount{};
//         size_t factoryCount{};
//         size_t libraryCount{};
//         void* nativeHandle{};
//     };

//     /** @brief Runtime dictionary for metaclasses, providers, module images, and lazy realization state. */
//     class Dictionary {
//     public:
//         using DictResult = std::expected<void, DictErrorCode>;

//         /** @brief Register an externally owned metaclass. */
//         [[nodiscard]] DictResult RegisterMetaClass(const MetaClass& meta);

//         /** @brief Register or merge a component-interface provider fact. */
//         [[nodiscard]] DictResult RegisterProvide(Iid component, Iid interfaceIid, ProviderEntry entry,
//                                                  RecordStatus status = RecordStatus::Loaded, std::string source = {});

//         /** @brief Register a library descriptor. */
//         [[nodiscard]] DictResult RegisterLibrary(LibrarySpec library);

//         /** @brief Register a factory descriptor. */
//         [[nodiscard]] DictResult RegisterFactory(FactorySpec factory);

//         /** @brief Fold a caller-owned manifest into the dictionary. */
//         [[nodiscard]] DictResult RegisterManifest(const Manifest& manifest);

//         /** @brief Fold one relocation-free section record without a section string table. */
//         [[nodiscard]] DictResult RegisterSectionRecord(const DictSectionRecordHeader& record);

//         /** @brief Fold one typed metaclass section record without relying on header-subobject punning. */
//         [[nodiscard]] DictResult RegisterSectionRecord(const DictSectionMetaClassRecord& record);

//         /** @brief Fold one typed provider section record without relying on header-subobject punning. */
//         [[nodiscard]] DictResult RegisterSectionRecord(const DictSectionProvideRecord& record);

//         /** @brief Fold one typed factory section record without relying on header-subobject punning. */
//         [[nodiscard]] DictResult RegisterSectionRecord(const DictSectionFactoryRecord& record);

//         /** @brief Fold one typed library section record without relying on header-subobject punning. */
//         [[nodiscard]] DictResult RegisterSectionRecord(const DictSectionLibraryRecord& record);

//         /** @brief Fold a copied module section image; the owning DLL is not loaded. */
//         [[nodiscard]] DictResult RegisterModuleImage(const ModuleImage& image);

//         /** @brief Read one DLL file and copy its Core section records without loading the DLL. */
//         [[nodiscard]] std::expected<ModuleImage, CoreErrorCode> ScanModuleFile(const std::filesystem::path& path) const;

//         /** @brief Scan explicit directories for DLLs with a Core section and fold the records found. */
//         [[nodiscard]] std::expected<size_t, CoreErrorCode> ScanPath(std::span<const std::filesystem::path> paths);

//         /** @brief Scan directories from the process PATH environment variable. */
//         [[nodiscard]] std::expected<size_t, CoreErrorCode> ScanPath();

//         /** @brief Load a previously scanned module by absolute path without rescanning unrelated PATH entries. */
//         [[nodiscard]] DictResult RealizeModule(const std::filesystem::path& path);

//         /** @brief Find a metaclass by IID. */
//         [[nodiscard]] const MetaClass* FindMetaClass(Iid iid) const;

//         /** @brief Return whether @p derived is the same as or transitively derived from @p base. */
//         [[nodiscard]] bool IsAKindOf(Iid derived, Iid base) const;

//         /** @brief Find an exact component-interface provider. */
//         [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvide(Iid component, Iid interfaceIid) const;

//         /** @brief Find a provider on @p component or one of its object-model bases. */
//         [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvideInClassChain(Iid component,
//                                                                                      Iid interfaceIid) const;

//         /** @brief Return a lifetime-safe view of registered metaclasses. */
//         [[nodiscard]] PinnedView<const MetaClass*> MetaClasses() const;

//         /** @brief Return a lifetime-safe view of registered provider records. */
//         [[nodiscard]] PinnedView<DictionaryProvideRecord> ProvideRecords() const;

//         /** @brief Return a lifetime-safe view of registered libraries. */
//         [[nodiscard]] PinnedView<LibrarySpec> Libraries() const;

//         /** @brief Return a lifetime-safe view of registered factories. */
//         [[nodiscard]] PinnedView<FactorySpec> Factories() const;

//         /** @brief Return a lifetime-safe view of scanned module records. */
//         [[nodiscard]] PinnedView<ModuleRecord> Modules() const;

//     private:
//         [[nodiscard]] DictResult RegisterMetaClassUnlocked(const MetaClass& meta);
//         [[nodiscard]] DictResult RegisterOwnedMetaClassUnlocked(TypeOfClass type, Iid iid, Iid directBaseIid,
//                                                                 std::string name);
//         [[nodiscard]] DictResult RegisterProvideUnlocked(Iid component, Iid interfaceIid, ProviderEntry entry,
//                                                          RecordStatus status, std::string source);
//         [[nodiscard]] DictResult RegisterSectionRecordUnlocked(std::span<const std::byte> recordBytes,
//                                                                std::span<const std::byte> section,
//                                                                const std::filesystem::path& path);
//         [[nodiscard]] const MetaClass* FindMetaClassUnlocked(Iid iid) const;
//         [[nodiscard]] std::optional<DictionaryProvideRecord> FindProvideUnlocked(Iid component, Iid interfaceIid) const;
//         void ResolveDirectBaseLinksUnlocked() noexcept;

//         mutable std::mutex mutex_{};
//         std::unordered_map<Iid, const MetaClass*, IidHash> metaByIid_{};
//         std::unordered_map<ProvideKey, DictionaryProvideRecord, ProvideKeyHash> provides_{};
//         std::vector<LibrarySpec> libraries_{};
//         std::vector<FactorySpec> factories_{};
//         std::vector<ModuleRecord> modules_{};
//         std::deque<std::string> ownedStrings_{};
//         std::vector<std::unique_ptr<MetaClass>> ownedMeta_{};
//     };

//     /** @brief Return the process-wide dictionary instance. */
//     [[nodiscard]] Dictionary& GlobalDictionary();

// } // namespace Sora::Kernel