/**
 * @file ComAPI.h
 * @brief Platform-neutral COM ABI declarations and ownership helpers.
 * @ingroup PAL
 * @details This header deliberately contains no Windows SDK or ATL include. It separates core COM ABI and lifetime
 * facilities from the OLE data-transfer and Automation interface groups, while all native entry points remain owned
 * by the corresponding @ref COMSystemAPI, @ref OleSystemAPI, and @ref OleAutomationSystemAPI tables. ATL is modeled
 * only as zero-overhead ownership and object-root facilities; windowing, registry, UI, and server-registration
 * frameworks are outside this PAL boundary.
 */
#pragma once

#include <Sora/Core/PAL/SystemAPI.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <expected>
#include <limits>
#include <utility>

namespace Sora::PAL {

    /** @name ABI vocabulary @{ ------------------------------------------------------------------------------- */

#if defined(PLATFORM_WINDOWS)
    using HResult = WindowsSystem::HResult;
    using ULong = WindowsSystem::ULong;
    using DWord = WindowsSystem::DWord;
    using Guid = WindowsSystem::Guid;
    using RefGuid = WindowsSystem::RefGuid;
    using RefInterfaceId = WindowsSystem::RefInterfaceId;
    using RefClassId = WindowsSystem::RefClassId;
    using IUnknown = WindowsSystem::Unknown;
    using ISequentialStream = WindowsSystem::SequentialStream;
#    define SORA_PAL_COM_CALL __stdcall
#else
    using HResult = int32_t;
    using ULong = uint32_t;
    using DWord = uint32_t;

    struct Guid {
        uint32_t data1 = 0;
        uint16_t data2 = 0;
        uint16_t data3 = 0;
        uint8_t data4[8]{};
    };
    using RefGuid = const Guid&;
    using RefInterfaceId = const Guid&;
    using RefClassId = const Guid&;

    struct IUnknown {
        virtual HResult QueryInterface(RefInterfaceId iid, void** object) = 0;
        virtual ULong AddRef() = 0;
        virtual ULong Release() = 0;
    };

    struct ISequentialStream : IUnknown {
        virtual HResult Read(void* destination, ULong bytes, ULong* bytesRead) = 0;
        virtual HResult Write(const void* source, ULong bytes, ULong* bytesWritten) = 0;
    };
#    define SORA_PAL_COM_CALL
#endif

    /** @brief HRESULT success test using COM's signed status convention. */
    [[nodiscard]] constexpr bool Succeeded(HResult result) noexcept { return result >= 0; }

    /** @brief HRESULT failure test using COM's signed status convention. */
    [[nodiscard]] constexpr bool Failed(HResult result) noexcept { return result < 0; }

    /** @brief Compare two COM identifiers without relying on platform helpers. */
    [[nodiscard]] constexpr bool EqualGuid(RefGuid left, RefGuid right) noexcept {
        if (left.data1 != right.data1 || left.data2 != right.data2 || left.data3 != right.data3) {
            return false;
        }
        for (size_t index = 0; index < 8; ++index) {
            if (left.data4[index] != right.data4[index]) {
                return false;
            }
        }
        return true;
    }

    inline constexpr HResult SOk = static_cast<HResult>(0x00000000u);
    inline constexpr HResult ENoInterface = static_cast<HResult>(0x80004002u);
    inline constexpr HResult EPointer = static_cast<HResult>(0x80004003u);
    inline constexpr HResult EInvalidArg = static_cast<HResult>(0x80070057u);
    inline constexpr HResult ENotImpl = static_cast<HResult>(0x80004001u);
    inline constexpr HResult EOutOfMemory = static_cast<HResult>(0x8007000Eu);

    /** @brief COM apartment model passed to CoInitializeEx. */
    enum class ApartmentModel : DWord {
        SingleThreaded = 0x2u,
        MultiThreaded = 0x0u,
    };

    /** @brief ABI-compatible 64-bit integer used by IStream methods. */
    struct LargeInteger {
        int64_t quadPart = 0;
    };

    /** @brief ABI-compatible unsigned 64-bit integer used by IStream methods. */
    struct UnsignedLargeInteger {
        uint64_t quadPart = 0;
    };

    /** @brief ABI-compatible OLE point passed by value to drag/drop callbacks. */
    struct PointL {
        int32_t x = 0;
        int32_t y = 0;
    };

    /** @brief Opaque storage statistics record accepted by IStream::Stat. */
    struct StatStg;

    /** @brief ABI-compatible stream interface extending ISequentialStream. */
    struct IStream : ISequentialStream {
        virtual HResult SORA_PAL_COM_CALL Seek(LargeInteger move, DWord origin, UnsignedLargeInteger* newPosition) = 0;
        virtual HResult SORA_PAL_COM_CALL SetSize(UnsignedLargeInteger newSize) = 0;
        virtual HResult SORA_PAL_COM_CALL CopyTo(IStream* target, UnsignedLargeInteger bytes,
                                                 UnsignedLargeInteger* read, UnsignedLargeInteger* written) = 0;
        virtual HResult SORA_PAL_COM_CALL Commit(DWord flags) = 0;
        virtual HResult SORA_PAL_COM_CALL Revert() = 0;
        virtual HResult SORA_PAL_COM_CALL LockRegion(UnsignedLargeInteger offset, UnsignedLargeInteger bytes,
                                                     DWord type) = 0;
        virtual HResult SORA_PAL_COM_CALL UnlockRegion(UnsignedLargeInteger offset, UnsignedLargeInteger bytes,
                                                       DWord type) = 0;
        virtual HResult SORA_PAL_COM_CALL Stat(StatStg* statistics, DWord flags) = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IStream** stream) = 0;
    };

    struct IStorage;

    /** @brief COM class-factory interface used by in-process and out-of-process activation. */
    struct IClassFactory : IUnknown {
        virtual HResult SORA_PAL_COM_CALL CreateInstance(IUnknown* outer, RefInterfaceId iid, void** object) = 0;
        virtual HResult SORA_PAL_COM_CALL LockServer(int lock) = 0;
    };

    /** @brief COM enumerator for unknown interfaces. */
    struct IEnumUnknown : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, IUnknown** values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumUnknown** enumerator) = 0;
    };

    struct StatStg;

    /** @brief COM enumerator for strings. */
    struct IEnumString : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, wchar_t** values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumString** enumerator) = 0;
    };

    /** @brief OLE data-format enumerator. */
    struct IEnumFormatEtc : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, void* values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumFormatEtc** enumerator) = 0;
    };

    /** @brief Structured-storage statistics enumerator. */
    struct IEnumStatStg : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, StatStg* values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumStatStg** enumerator) = 0;
    };

    /** @name Structured Storage ABI @{ ----------------------------------------------------------------------- */

    /** @brief ABI-compatible Windows FILETIME record used by compound-file metadata. */
    struct FileTime {
        uint32_t lowDateTime = 0;
        uint32_t highDateTime = 0;
    };

    /** @brief ABI-compatible structured-storage statistics record. */
    struct StatStg {
        wchar_t* name = nullptr;
        DWord type = 0;
        UnsignedLargeInteger size{};
        FileTime creationTime{};
        FileTime accessTime{};
        FileTime modificationTime{};
        DWord mode = 0;
        DWord locksSupported = 0;
        Guid classId{};
        DWord stateBits = 0;
        DWord reserved = 0;
    };

    /** @brief Byte-addressable backing store used by structured storage. */
    struct ILockBytes : IUnknown {
        virtual HResult SORA_PAL_COM_CALL ReadAt(UnsignedLargeInteger offset, void* buffer, ULong bytes,
                                                 ULong* bytesRead) = 0;
        virtual HResult SORA_PAL_COM_CALL WriteAt(UnsignedLargeInteger offset, const void* buffer, ULong bytes,
                                                  ULong* bytesWritten) = 0;
        virtual HResult SORA_PAL_COM_CALL Flush() = 0;
        virtual HResult SORA_PAL_COM_CALL SetSize(UnsignedLargeInteger size) = 0;
        virtual HResult SORA_PAL_COM_CALL LockRegion(UnsignedLargeInteger offset, UnsignedLargeInteger bytes,
                                                     DWord lockType) = 0;
        virtual HResult SORA_PAL_COM_CALL UnlockRegion(UnsignedLargeInteger offset, UnsignedLargeInteger bytes,
                                                       DWord lockType) = 0;
        virtual HResult SORA_PAL_COM_CALL Stat(StatStg* statistics, DWord flags) = 0;
    };

    /** @brief Root-storage operation for switching a compound file to a new backing file. */
    struct IRootStorage : IUnknown {
        virtual HResult SORA_PAL_COM_CALL SwitchToFile(const wchar_t* fileName) = 0;
    };

    /** @} ----------------------------------------------------------------------------------------------------- */

    /** @name Persistence Protocol ABI @{ --------------------------------------------------------------------- */

    /** @brief Base persistence identity protocol. */
    struct IPersist : IUnknown {
        virtual HResult SORA_PAL_COM_CALL GetClassID(Guid* classId) = 0;
    };

    /** @brief Persistence protocol backed by an IStream. */
    struct IPersistStream : IPersist {
        virtual HResult SORA_PAL_COM_CALL IsDirty() = 0;
        virtual HResult SORA_PAL_COM_CALL Load(IStream* stream) = 0;
        virtual HResult SORA_PAL_COM_CALL Save(IStream* stream, int clearDirty) = 0;
        virtual HResult SORA_PAL_COM_CALL GetSizeMax(UnsignedLargeInteger* size) = 0;
    };

    /** @brief Persistence protocol backed by a file path. */
    struct IPersistFile : IPersist {
        virtual HResult SORA_PAL_COM_CALL IsDirty() = 0;
        virtual HResult SORA_PAL_COM_CALL Load(const wchar_t* fileName, DWord mode) = 0;
        virtual HResult SORA_PAL_COM_CALL Save(const wchar_t* fileName, int remember) = 0;
        virtual HResult SORA_PAL_COM_CALL SaveCompleted(const wchar_t* fileName) = 0;
        virtual HResult SORA_PAL_COM_CALL GetCurFile(wchar_t** fileName) = 0;
    };

    /** @brief Persistence protocol backed by structured storage. */
    struct IPersistStorage : IPersist {
        virtual HResult SORA_PAL_COM_CALL IsDirty() = 0;
        virtual HResult SORA_PAL_COM_CALL InitNew(IStorage* storage) = 0;
        virtual HResult SORA_PAL_COM_CALL Load(IStorage* storage) = 0;
        virtual HResult SORA_PAL_COM_CALL Save(IStorage* storage, int sameAsLoad) = 0;
        virtual HResult SORA_PAL_COM_CALL SaveCompleted(IStorage* storage) = 0;
        virtual HResult SORA_PAL_COM_CALL HandsOffStorage() = 0;
    };

    /** @} ----------------------------------------------------------------------------------------------------- */

    /** @name Property Set Storage ABI @{ ---------------------------------------------------------------------- */

    /** @brief Property identifier selector, either a numeric ID or a caller-owned name. */
    struct PropertySpec {
        DWord kind = 0;
        union {
            DWord propertyId;
            wchar_t* name;
        } value{};
    };

    /** @brief Opaque ABI-compatible PROPVARIANT storage. */
    struct alignas(8) PropertyVariant {
        std::byte storage[24]{};
    };

    /** @brief Property enumeration statistics record. */
    struct StatPropStg {
        wchar_t* name = nullptr;
        DWord propertyId = 0;
        uint16_t variantType = 0;
    };

    /** @brief Property-set enumeration statistics record. */
    struct StatPropSetStg {
        Guid formatId{};
        Guid classId{};
        DWord flags = 0;
        FileTime modificationTime{};
        FileTime creationTime{};
        FileTime accessTime{};
        DWord operatingSystemVersion = 0;
    };

    /** @brief Enumerator for properties within one property set. */
    struct IEnumStatPropStg : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, StatPropStg* values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumStatPropStg** enumerator) = 0;
    };

    /** @brief Enumerator for property sets within one storage. */
    struct IEnumStatPropSetStg : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Next(ULong count, StatPropSetStg* values, ULong* fetched) = 0;
        virtual HResult SORA_PAL_COM_CALL Skip(ULong count) = 0;
        virtual HResult SORA_PAL_COM_CALL Reset() = 0;
        virtual HResult SORA_PAL_COM_CALL Clone(IEnumStatPropSetStg** enumerator) = 0;
    };

    /** @brief Property values and names stored in one property set. */
    struct IPropertyStorage : IUnknown {
        virtual HResult SORA_PAL_COM_CALL ReadMultiple(ULong count, const PropertySpec* specifications,
                                                        PropertyVariant* values) = 0;
        virtual HResult SORA_PAL_COM_CALL WriteMultiple(ULong count, const PropertySpec* specifications,
                                                         const PropertyVariant* values, DWord firstProperty) = 0;
        virtual HResult SORA_PAL_COM_CALL DeleteMultiple(ULong count, const PropertySpec* specifications) = 0;
        virtual HResult SORA_PAL_COM_CALL ReadPropertyNames(ULong count, const DWord* propertyIds,
                                                            wchar_t** names) = 0;
        virtual HResult SORA_PAL_COM_CALL WritePropertyNames(ULong count, const DWord* propertyIds,
                                                             const wchar_t* const* names) = 0;
        virtual HResult SORA_PAL_COM_CALL DeletePropertyNames(ULong count, const DWord* propertyIds) = 0;
        virtual HResult SORA_PAL_COM_CALL Commit(DWord flags) = 0;
        virtual HResult SORA_PAL_COM_CALL Revert() = 0;
        virtual HResult SORA_PAL_COM_CALL Enum(IEnumStatPropStg** enumerator) = 0;
        virtual HResult SORA_PAL_COM_CALL SetTimes(const FileTime* creation, const FileTime* access,
                                                   const FileTime* modification) = 0;
        virtual HResult SORA_PAL_COM_CALL SetClass(RefClassId classId) = 0;
        virtual HResult SORA_PAL_COM_CALL Stat(StatPropSetStg* statistics) = 0;
    };

    /** @brief Collection of property sets attached to one storage object. */
    struct IPropertySetStorage : IUnknown {
        virtual HResult SORA_PAL_COM_CALL Create(RefGuid formatId, const Guid* classId, DWord flags, DWord mode,
                                                  IPropertyStorage** propertyStorage) = 0;
        virtual HResult SORA_PAL_COM_CALL Open(RefGuid formatId, DWord mode, IPropertyStorage** propertyStorage) = 0;
        virtual HResult SORA_PAL_COM_CALL Delete(RefGuid formatId) = 0;
        virtual HResult SORA_PAL_COM_CALL Enum(IEnumStatPropSetStg** enumerator) = 0;
    };

    /** @} ----------------------------------------------------------------------------------------------------- */

    /** @brief Opaque OLE storage statistics and Automation records. */
    struct DispatchParams;
    struct ExceptionInfo;
    struct TypeAttr;
    struct FuncDesc;
    struct VarDesc;
    struct TypeComp;
    struct ITypeInfo;
    struct ITypeLib;
    struct IEnumStatData;

    /** @brief Structured-storage persistence interface. */
    struct IStorage : IUnknown {
        virtual HResult SORA_PAL_COM_CALL CreateStream(const wchar_t* name, DWord mode, DWord reserved1,
                                                       DWord reserved2, IStream** stream) = 0;
        virtual HResult SORA_PAL_COM_CALL OpenStream(const wchar_t* name, void* reserved1, DWord mode,
                                                     DWord reserved2, IStream** stream) = 0;
        virtual HResult SORA_PAL_COM_CALL CreateStorage(const wchar_t* name, DWord mode, DWord reserved1,
                                                        DWord reserved2, IStorage** storage) = 0;
        virtual HResult SORA_PAL_COM_CALL OpenStorage(const wchar_t* name, IStorage* priority, DWord mode,
                                                      void* exclude, DWord reserved, IStorage** storage) = 0;
        virtual HResult SORA_PAL_COM_CALL CopyTo(DWord count, const Guid* excluded, void* reserved,
                                                 IStorage* destination) = 0;
        virtual HResult SORA_PAL_COM_CALL MoveElementTo(const wchar_t* name, IStorage* destination,
                                                        const wchar_t* newName, DWord flags) = 0;
        virtual HResult SORA_PAL_COM_CALL Commit(DWord flags) = 0;
        virtual HResult SORA_PAL_COM_CALL Revert() = 0;
        virtual HResult SORA_PAL_COM_CALL EnumElements(DWord reserved1, void* reserved2, DWord reserved3,
                                                       IEnumStatStg** enumerator) = 0;
        virtual HResult SORA_PAL_COM_CALL DestroyElement(const wchar_t* name) = 0;
        virtual HResult SORA_PAL_COM_CALL RenameElement(const wchar_t* oldName, const wchar_t* newName) = 0;
        virtual HResult SORA_PAL_COM_CALL SetElementTimes(const wchar_t* name, const void* creation,
                                                          const void* access, const void* modified) = 0;
        virtual HResult SORA_PAL_COM_CALL SetClass(RefClassId classId) = 0;
        virtual HResult SORA_PAL_COM_CALL SetStateBits(DWord stateBits, DWord mask) = 0;
        virtual HResult SORA_PAL_COM_CALL Stat(StatStg* statistics, DWord flags) = 0;
    };

    /** @brief OLE data-transfer interface using caller-owned FORMATETC/STGMEDIUM records. */
    struct IDataObject : IUnknown {
        virtual HResult SORA_PAL_COM_CALL GetData(const void* format, void* medium) = 0;
        virtual HResult SORA_PAL_COM_CALL GetDataHere(const void* format, void* medium) = 0;
        virtual HResult SORA_PAL_COM_CALL QueryGetData(const void* format) = 0;
        virtual HResult SORA_PAL_COM_CALL GetCanonicalFormatEtc(const void* format, void* result) = 0;
        virtual HResult SORA_PAL_COM_CALL SetData(const void* format, void* medium, int release) = 0;
        virtual HResult SORA_PAL_COM_CALL EnumFormatEtc(DWord direction, IEnumFormatEtc** enumerator) = 0;
        virtual HResult SORA_PAL_COM_CALL DAdvise(const void* format, DWord flags, IUnknown* sink,
                                                  DWord* connection) = 0;
        virtual HResult SORA_PAL_COM_CALL DUnadvise(DWord connection) = 0;
        virtual HResult SORA_PAL_COM_CALL EnumDAdvise(IEnumStatData** enumerator) = 0;
    };

    /** @brief OLE drag-source callback interface. */
    struct IDropSource : IUnknown {
        virtual HResult SORA_PAL_COM_CALL QueryContinueDrag(int escapePressed, DWord keyState) = 0;
        virtual HResult SORA_PAL_COM_CALL GiveFeedback(DWord effect) = 0;
    };

    /** @brief OLE drag-target callback interface. */
    struct IDropTarget : IUnknown {
        virtual HResult SORA_PAL_COM_CALL DragEnter(IDataObject* data, DWord keyState, PointL point,
                                                    DWord* effect) = 0;
        virtual HResult SORA_PAL_COM_CALL DragOver(DWord keyState, PointL point, DWord* effect) = 0;
        virtual HResult SORA_PAL_COM_CALL DragLeave() = 0;
        virtual HResult SORA_PAL_COM_CALL Drop(IDataObject* data, DWord keyState, PointL point,
                                               DWord* effect) = 0;
    };

    /** @brief Automation dispatch interface. */
    struct IDispatch : IUnknown {
        virtual HResult SORA_PAL_COM_CALL GetTypeInfoCount(uint32_t* count) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeInfo(uint32_t index, uint32_t locale, ITypeInfo** typeInfo) = 0;
        virtual HResult SORA_PAL_COM_CALL GetIDsOfNames(RefGuid riid, wchar_t** names, uint32_t count,
                                                        uint32_t locale, int32_t* dispids) = 0;
        virtual HResult SORA_PAL_COM_CALL Invoke(int32_t dispid, RefGuid riid, uint32_t locale, uint16_t flags,
                                                 DispatchParams* parameters, void* result, ExceptionInfo* exception,
                                                 uint32_t* argumentError) = 0;
    };

    /** @brief Type-information interface used by Automation clients. */
    struct ITypeInfo : IUnknown {
        virtual HResult SORA_PAL_COM_CALL GetTypeAttr(TypeAttr** attributes) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeComp(TypeComp** typeComp) = 0;
        virtual HResult SORA_PAL_COM_CALL GetFuncDesc(uint32_t index, FuncDesc** description) = 0;
        virtual HResult SORA_PAL_COM_CALL GetVarDesc(uint32_t index, VarDesc** description) = 0;
        virtual HResult SORA_PAL_COM_CALL GetNames(int32_t memberId, wchar_t** names, uint32_t maximum,
                                                   uint32_t* count) = 0;
        virtual HResult SORA_PAL_COM_CALL GetRefTypeOfImplType(uint32_t index, uint32_t* reference) = 0;
        virtual HResult SORA_PAL_COM_CALL GetImplTypeFlags(uint32_t index, int32_t* flags) = 0;
        virtual HResult SORA_PAL_COM_CALL GetIDsOfNames(wchar_t** names, uint32_t count, int32_t* memberIds) = 0;
        virtual HResult SORA_PAL_COM_CALL Invoke(void* instance, int32_t memberId, uint16_t flags,
                                                 DispatchParams* parameters, void* result, ExceptionInfo* exception,
                                                 uint32_t* argumentError) = 0;
        virtual HResult SORA_PAL_COM_CALL GetDocumentation(int32_t memberId, wchar_t** name, wchar_t** documentation,
                                                           uint32_t* helpContext, wchar_t** helpFile) = 0;
        virtual HResult SORA_PAL_COM_CALL GetDllEntry(int32_t memberId, uint32_t invocationKind, wchar_t** dllName,
                                                      wchar_t** name, uint16_t* ordinal) = 0;
        virtual HResult SORA_PAL_COM_CALL GetRefTypeInfo(uint32_t reference, ITypeInfo** typeInfo) = 0;
        virtual HResult SORA_PAL_COM_CALL AddressOfMember(int32_t memberId, uint32_t invocationKind,
                                                          void** address) = 0;
        virtual HResult SORA_PAL_COM_CALL CreateInstance(IUnknown* outer, RefInterfaceId iid, void** instance) = 0;
        virtual HResult SORA_PAL_COM_CALL GetMops(int32_t memberId, wchar_t** mops) = 0;
        virtual HResult SORA_PAL_COM_CALL GetContainingTypeLib(ITypeLib** typeLib, uint32_t* index) = 0;
        virtual void SORA_PAL_COM_CALL ReleaseTypeAttr(TypeAttr* attributes) = 0;
        virtual void SORA_PAL_COM_CALL ReleaseFuncDesc(FuncDesc* description) = 0;
        virtual void SORA_PAL_COM_CALL ReleaseVarDesc(VarDesc* description) = 0;
    };

    /** @brief Type-library interface used by Automation discovery. */
    struct ITypeLib : IUnknown {
        virtual uint32_t SORA_PAL_COM_CALL GetTypeInfoCount() = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeInfo(uint32_t index, ITypeInfo** typeInfo) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeInfoType(uint32_t index, uint32_t* kind) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeInfoOfGuid(RefGuid guid, ITypeInfo** typeInfo) = 0;
        virtual HResult SORA_PAL_COM_CALL GetLibAttr(void** attributes) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeComp(TypeComp** typeComp) = 0;
        virtual HResult SORA_PAL_COM_CALL GetDocumentation(int32_t memberId, wchar_t** name, wchar_t** documentation,
                                                           uint32_t* helpContext, wchar_t** helpFile) = 0;
        virtual HResult SORA_PAL_COM_CALL IsName(wchar_t* name, uint32_t hash, int* result) = 0;
        virtual HResult SORA_PAL_COM_CALL FindName(wchar_t* name, uint32_t hash, ITypeInfo** typeInfo,
                                                   int32_t* memberIds, uint16_t* count) = 0;
        virtual void SORA_PAL_COM_CALL ReleaseTLibAttr(void* attributes) = 0;
    };

    /** @brief Record metadata interface used by VARIANT record values. */
    struct IRecordInfo : IUnknown {
        virtual HResult SORA_PAL_COM_CALL RecordInit(void* record) = 0;
        virtual HResult SORA_PAL_COM_CALL RecordClear(void* record) = 0;
        virtual HResult SORA_PAL_COM_CALL RecordCopy(const void* source, void* destination) = 0;
        virtual HResult SORA_PAL_COM_CALL GetGuid(Guid* guid) = 0;
        virtual HResult SORA_PAL_COM_CALL GetName(wchar_t** name) = 0;
        virtual HResult SORA_PAL_COM_CALL GetSize(ULong* size) = 0;
        virtual HResult SORA_PAL_COM_CALL GetTypeInfo(ITypeInfo** typeInfo) = 0;
        virtual HResult SORA_PAL_COM_CALL GetField(void* record, const wchar_t* name, void* value) = 0;
        virtual HResult SORA_PAL_COM_CALL GetFieldNoCopy(void* record, const wchar_t* name, void* value,
                                                         void** owner) = 0;
        virtual HResult SORA_PAL_COM_CALL PutField(ULong flags, void* record, const wchar_t* name, void* value) = 0;
        virtual HResult SORA_PAL_COM_CALL PutFieldNoCopy(ULong flags, void* record, const wchar_t* name,
                                                         void* value) = 0;
        virtual HResult SORA_PAL_COM_CALL GetFieldNames(ULong* count, wchar_t** names) = 0;
        virtual int SORA_PAL_COM_CALL IsMatchingType(IRecordInfo* other) = 0;
        virtual void* SORA_PAL_COM_CALL RecordCreate() = 0;
        virtual HResult SORA_PAL_COM_CALL RecordCreateCopy(void* source, void** destination) = 0;
        virtual HResult SORA_PAL_COM_CALL RecordDestroy(void* record) = 0;
    };

    /** @brief Construct a COM GUID value without importing guiddef.h. */
    [[nodiscard]] consteval Guid MakeGuid(uint32_t data1, uint16_t data2, uint16_t data3,
                                          std::array<uint8_t, 8> data4) noexcept {
        return {data1, data2, data3,
                {data4[0], data4[1], data4[2], data4[3], data4[4], data4[5], data4[6], data4[7]}};
    }

    /** @brief Return the standard IID for a supported COM ABI interface. */
    template<typename T>
    [[nodiscard]] consteval Guid InterfaceIdOf() {
        constexpr std::array<uint8_t, 8> standardTail{0xc0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u};
        if constexpr (std::same_as<T, IUnknown>) {
            return MakeGuid(0x00000000u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IClassFactory>) {
            return MakeGuid(0x00000001u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IStorage>) {
            return MakeGuid(0x0000000bu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IStream>) {
            return MakeGuid(0x0000000cu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumStatStg>) {
            return MakeGuid(0x0000000du, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, ILockBytes>) {
            return MakeGuid(0x0000000au, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IRootStorage>) {
            return MakeGuid(0x00000012u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPersistStream>) {
            return MakeGuid(0x00000109u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPersistStorage>) {
            return MakeGuid(0x0000010au, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPersistFile>) {
            return MakeGuid(0x0000010bu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPersist>) {
            return MakeGuid(0x0000010cu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPropertyStorage>) {
            return MakeGuid(0x00000138u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumStatPropStg>) {
            return MakeGuid(0x00000139u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IPropertySetStorage>) {
            return MakeGuid(0x0000013au, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumStatPropSetStg>) {
            return MakeGuid(0x0000013bu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IRecordInfo>) {
            return MakeGuid(0x0000002fu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumUnknown>) {
            return MakeGuid(0x00000100u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumString>) {
            return MakeGuid(0x00000101u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IEnumFormatEtc>) {
            return MakeGuid(0x00000103u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IDataObject>) {
            return MakeGuid(0x0000010eu, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IDropSource>) {
            return MakeGuid(0x00000121u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IDropTarget>) {
            return MakeGuid(0x00000122u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, IDispatch>) {
            return MakeGuid(0x00020400u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, ITypeInfo>) {
            return MakeGuid(0x00020401u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, ITypeLib>) {
            return MakeGuid(0x00020402u, 0x0000u, 0x0000u, standardTail);
        } else if constexpr (std::same_as<T, ISequentialStream>) {
            return MakeGuid(0x0c733a30u, 0x2a1cu, 0x11ceu,
                            {0xadu, 0xe5u, 0x00u, 0xaau, 0x00u, 0x44u, 0x77u, 0x3du});
        } else {
            static_assert(std::same_as<T, void>, "No standard COM IID mapping is available for this interface");
        }
    }

    /** @} ----------------------------------------------------------------------------------------------- */

    /** @name Reference-counted interface owner @{ -------------------------------------------------------- */

    template<typename T>
    class ComPtr {
    public:
        constexpr ComPtr() noexcept = default;
        constexpr ComPtr(std::nullptr_t) noexcept {}

        explicit ComPtr(T* pointer) noexcept : pointer_(pointer) { AddReference(); }

        ComPtr(const ComPtr& other) noexcept : pointer_(other.pointer_) { AddReference(); }

        ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        template<typename U>
            requires std::convertible_to<U*, T*>
        ComPtr(const ComPtr<U>& other) noexcept : pointer_(other.Get()) {
            AddReference();
        }

        template<typename U>
            requires std::convertible_to<U*, T*>
        ComPtr(ComPtr<U>&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        ~ComPtr() noexcept { Reset(); }

        ComPtr& operator=(std::nullptr_t) noexcept {
            Reset();
            return *this;
        }

        ComPtr& operator=(const ComPtr& other) noexcept {
            if (this != &other) {
                ComPtr copy(other);
                Swap(copy);
            }
            return *this;
        }

        ComPtr& operator=(ComPtr&& other) noexcept {
            if (this != &other) {
                Reset();
                pointer_ = std::exchange(other.pointer_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] T* Get() const noexcept { return pointer_; }
        [[nodiscard]] T* operator->() const noexcept { return pointer_; }
        [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

        void Reset() noexcept {
            if (T* pointer = std::exchange(pointer_, nullptr); pointer != nullptr) {
                pointer->Release();
            }
        }

        void Attach(T* pointer) noexcept {
            if (pointer_ == pointer) {
                return;
            }
            Reset();
            pointer_ = pointer;
        }

        [[nodiscard]] T* Detach() noexcept { return std::exchange(pointer_, nullptr); }

        [[nodiscard]] T** Put() noexcept {
            Reset();
            return &pointer_;
        }

        [[nodiscard]] T** ReleaseAndGetAddressOf() noexcept { return Put(); }

        template<typename U>
        [[nodiscard]] HResult QueryInterface(RefInterfaceId iid, ComPtr<U>& result) const noexcept {
            result.Reset();
            if (pointer_ == nullptr) {
                return EPointer;
            }
            return pointer_->QueryInterface(iid, reinterpret_cast<void**>(result.Put()));
        }

        template<typename U>
        [[nodiscard]] std::expected<ComPtr<U>, HResult> QueryInterface(RefInterfaceId iid) const noexcept {
            ComPtr<U> result;
            const HResult status = QueryInterface(iid, result);
            if (Failed(status)) {
                return std::unexpected(status);
            }
            return result;
        }

        template<typename U>
        [[nodiscard]] std::expected<ComPtr<U>, HResult> QueryInterface() const noexcept {
            return QueryInterface<U>(InterfaceIdOf<U>());
        }

        template<typename U>
        [[nodiscard]] HResult CopyTo(U** output) const noexcept {
            if (output == nullptr) {
                return EPointer;
            }
            *output = nullptr;
            if (pointer_ == nullptr) {
                return SOk;
            }
            *output = static_cast<U*>(pointer_);
            (*output)->AddRef();
            return SOk;
        }

        void Swap(ComPtr& other) noexcept { std::swap(pointer_, other.pointer_); }

    private:
        struct AdoptTag {};
        explicit ComPtr(T* pointer, AdoptTag) noexcept : pointer_(pointer) {}

        void AddReference() noexcept {
            if (pointer_ != nullptr) {
                pointer_->AddRef();
            }
        }

        template<typename>
        friend class ComPtr;

        T* pointer_ = nullptr;
    };

    /** @brief Adopt an already-owned COM interface reference without incrementing its count. */
    template<typename T>
    [[nodiscard]] ComPtr<T> AdoptCom(T* pointer) noexcept {
        ComPtr<T> result;
        result.Attach(pointer);
        return result;
    }

    /** @} ----------------------------------------------------------------------------------------------- */

    /** @name Runtime and allocation facades @{ ----------------------------------------------------------- */

    /** @brief Initialize COM for the calling thread and uninitialize it on destruction. */
    class Apartment {
    public:
        explicit Apartment(ApartmentModel model = ApartmentModel::MultiThreaded) noexcept;
        Apartment(const Apartment&) = delete;
        Apartment& operator=(const Apartment&) = delete;
        Apartment(Apartment&&) = delete;
        Apartment& operator=(Apartment&&) = delete;
        ~Apartment() noexcept;

        [[nodiscard]] HResult Status() const noexcept { return status_; }
        [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

    private:
        HResult status_ = ENotImpl;
        bool initialized_ = false;
    };

    /** @brief Invoke CoCreateInstance through COMSystemAPI and return an owning interface reference. */
    [[nodiscard]] HResult CreateInstanceRaw(RefClassId classId, void* outer, DWord context, RefInterfaceId iid,
                                             void** object) noexcept;

    template<typename T>
    [[nodiscard]] std::expected<ComPtr<T>, HResult> CreateInstance(RefClassId classId, DWord context,
                                                                    RefInterfaceId iid) noexcept {
        ComPtr<T> result;
        const HResult status =
            CreateInstanceRaw(classId, nullptr, context, iid, reinterpret_cast<void**>(result.Put()));
        if (Failed(status)) {
            return std::unexpected(status);
        }
        return result;
    }

    template<typename T>
    [[nodiscard]] std::expected<ComPtr<T>, HResult> CreateInstance(RefClassId classId, DWord context) noexcept {
        return CreateInstance<T>(classId, context, InterfaceIdOf<T>());
    }

    /** @brief Create an IStream over an HGLOBAL-compatible handle through COMSystemAPI. */
    [[nodiscard]] std::expected<ComPtr<IStream>, HResult> CreateStreamOnGlobal(void* global,
                                                                                 bool deleteOnRelease) noexcept;

    /** @brief Create a new GUID through COMSystemAPI. */
    [[nodiscard]] HResult CreateGuid(Guid& guid) noexcept;

    /** @brief Allocate and release COM task memory through CoTaskMemAlloc/CoTaskMemFree. */
    [[nodiscard]] void* AllocateTaskMemory(size_t bytes) noexcept;
    void FreeTaskMemory(void* memory) noexcept;

    template<typename T>
        requires std::is_object_v<T> && (alignof(T) <= alignof(std::max_align_t))
    class TaskMemory {
    public:
        TaskMemory() noexcept = default;
        explicit TaskMemory(T* memory) noexcept : memory_(memory) {}
        TaskMemory(const TaskMemory&) = delete;
        TaskMemory& operator=(const TaskMemory&) = delete;
        TaskMemory(TaskMemory&& other) noexcept : memory_(std::exchange(other.memory_, nullptr)) {}
        TaskMemory& operator=(TaskMemory&& other) noexcept {
            Reset();
            memory_ = std::exchange(other.memory_, nullptr);
            return *this;
        }
        ~TaskMemory() noexcept { Reset(); }

        [[nodiscard]] static TaskMemory Allocate(size_t count = 1) noexcept {
            if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
                return {};
            }
            return TaskMemory{static_cast<T*>(AllocateTaskMemory(sizeof(T) * count))};
        }
        [[nodiscard]] T* Get() const noexcept { return memory_; }
        [[nodiscard]] T* Detach() noexcept { return std::exchange(memory_, nullptr); }
        void Reset() noexcept {
            FreeTaskMemory(memory_);
            memory_ = nullptr;
        }

    private:
        T* memory_ = nullptr;
    };

    /** @brief Owning BSTR-compatible automation string. */
    class BStr {
    public:
        BStr() noexcept = default;
        explicit BStr(const wchar_t* text) noexcept;
        BStr(const BStr&) = delete;
        BStr& operator=(const BStr&) = delete;
        BStr(BStr&& other) noexcept;
        BStr& operator=(BStr&& other) noexcept;
        ~BStr() noexcept;

        [[nodiscard]] wchar_t* Get() const noexcept { return value_; }
        [[nodiscard]] const wchar_t* CStr() const noexcept { return value_ != nullptr ? value_ : L""; }
        [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
        [[nodiscard]] wchar_t* Detach() noexcept;
        void Reset() noexcept;

    private:
        wchar_t* value_ = nullptr;
    };

    /** @brief Owning SAFEARRAY-compatible automation array. */
    class SafeArray {
    public:
        SafeArray() noexcept = default;
        explicit SafeArray(void* array) noexcept : array_(array) {}
        SafeArray(const SafeArray&) = delete;
        SafeArray& operator=(const SafeArray&) = delete;
        SafeArray(SafeArray&& other) noexcept;
        SafeArray& operator=(SafeArray&& other) noexcept;
        ~SafeArray() noexcept;

        [[nodiscard]] void* Get() const noexcept { return array_; }
        [[nodiscard]] static SafeArray CreateVector(uint16_t variantType, int32_t lowerBound,
                                                     uint32_t count) noexcept;
        [[nodiscard]] void* Detach() noexcept;
        void Reset() noexcept;

    private:
        void* array_ = nullptr;
    };

    /** @brief Owning VARIANT-compatible value storage initialized through OleAutomationSystemAPI. */
    class Variant {
    public:
        Variant() noexcept;
        Variant(const Variant&) = delete;
        Variant& operator=(const Variant&) = delete;
        Variant(Variant&& other) noexcept;
        Variant& operator=(Variant&& other) noexcept;
        ~Variant() noexcept;

        [[nodiscard]] void* Data() noexcept { return storage_.data(); }
        [[nodiscard]] const void* Data() const noexcept { return storage_.data(); }
        [[nodiscard]] std::expected<Variant, HResult> Clone() const noexcept;
        void Clear() noexcept;

    private:
        // VARIANT is 16 bytes on Win32 and 24 bytes on Win64; this keeps the public object ABI-independent.
        alignas(8) std::array<std::byte, 24> storage_{};
    };

    /** @} ----------------------------------------------------------------------------------------------- */

    /** @name ATL-style object root @{ -------------------------------------------------------------------- */

    /** @brief Minimal CComObjectRootEx-style reference-counted implementation for Sora-owned COM objects. */
    class ObjectRoot : public IUnknown {
    public:
        HResult SORA_PAL_COM_CALL QueryInterface(RefInterfaceId iid, void** object) override = 0;
        ULong SORA_PAL_COM_CALL AddRef() override { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }
        ULong SORA_PAL_COM_CALL Release() override {
            const ULong references = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (references == 0) {
                delete this;
            }
            return references;
        }

    protected:
        virtual ~ObjectRoot() = default;

    private:
        std::atomic<ULong> references_{1};
    };

    /** @} ----------------------------------------------------------------------------------------------- */

} // namespace Sora::PAL

#undef SORA_PAL_COM_CALL
