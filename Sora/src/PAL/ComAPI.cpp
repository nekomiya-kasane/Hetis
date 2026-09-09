#include <Sora/Core/PAL/ComAPI.h>

#include <concepts>
#include <cstddef>
#include <cstring>
#include <type_traits>

#if defined(PLATFORM_WINDOWS)
#    define SORA_PAL_EXPECTED_COM_CALL __stdcall
#else
#    define SORA_PAL_EXPECTED_COM_CALL
#endif

namespace {

    namespace PAL = Sora::PAL;

    static_assert(std::is_same_v<decltype(&PAL::CreateInstanceRaw), PAL::HResult (*)(PAL::RefClassId, void*, PAL::DWord, PAL::RefInterfaceId, void**) noexcept>);
    static_assert(std::is_same_v<decltype(&PAL::CreateStreamOnGlobal), std::expected<PAL::ComPtr<PAL::IStream>, PAL::HResult> (*)(void*, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&PAL::CreateGuid), PAL::HResult (*)(PAL::Guid&) noexcept>);
    static_assert(std::is_same_v<decltype(&PAL::AllocateTaskMemory), void* (*)(size_t) noexcept>);
    static_assert(std::is_same_v<decltype(&PAL::FreeTaskMemory), void (*)(void*) noexcept>);

#if defined(PLATFORM_WINDOWS)
    static_assert(std::is_same_v<PAL::COMSystemAPI::InitializeExFunction, PAL::HResult(__stdcall*)(void*, PAL::DWord)>);
    static_assert(std::is_same_v<PAL::COMSystemAPI::CreateInstanceFunction, PAL::HResult(__stdcall*)(PAL::RefClassId, void*, PAL::DWord, PAL::RefInterfaceId, void**)>);
    static_assert(std::is_same_v<PAL::COMSystemAPI::CreateStreamOnGlobalFunction, PAL::HResult(__stdcall*)(PAL::WindowsSystem::Handle, PAL::WindowsSystem::Bool, void**)>);
    static_assert(std::is_same_v<PAL::COMSystemAPI::CreateGuidFunction, PAL::HResult(__stdcall*)(PAL::WindowsSystem::Guid*)>);
    static_assert(std::is_same_v<PAL::COMSystemAPI::TaskMemAllocFunction, void*(__stdcall*)(size_t)>);
    static_assert(std::is_same_v<PAL::COMSystemAPI::TaskMemFreeFunction, void(__stdcall*)(void*)>);
    static_assert(std::is_same_v<PAL::OleAutomationSystemAPI::AllocStringFunction, PAL::WindowsSystem::AutomationString(__stdcall*)(const wchar_t*)>);
    static_assert(std::is_same_v<PAL::OleAutomationSystemAPI::SafeArrayCreateVectorFunction, void*(__stdcall*)(unsigned short, long, unsigned long)>);
    static_assert(std::is_same_v<PAL::OleAutomationSystemAPI::VariantInitFunction, void(__stdcall*)(void*)>);
    static_assert(std::is_same_v<PAL::OleAutomationSystemAPI::VariantCopyFunction, PAL::HResult(__stdcall*)(void*, const void*)>);
#endif

    static_assert(std::is_standard_layout_v<PAL::Guid>);
    static_assert(sizeof(PAL::Guid)                 == 16);
    static_assert(alignof(PAL::Guid)                == alignof(uint32_t));
    static_assert(sizeof(PAL::LargeInteger)         == sizeof(int64_t));
    static_assert(sizeof(PAL::UnsignedLargeInteger) == sizeof(uint64_t));
    static_assert(sizeof(PAL::PointL)               == sizeof(int32_t) * 2);
    static_assert(sizeof(PAL::Variant)              == PAL::AutomationValueBytes);
    static_assert(alignof(PAL::Variant)             == PAL::AutomationValueAlignment);
    static_assert(sizeof(PAL::IUnknown)             == sizeof(void*));
    static_assert(alignof(PAL::IUnknown)            == alignof(void*));
    static_assert(std::is_polymorphic_v<PAL::IUnknown>);
    static_assert(std::derived_from<PAL::ISequentialStream, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IStream, PAL::ISequentialStream>);
    static_assert(std::derived_from<PAL::IClassFactory, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IStorage, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::ILockBytes, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IRootStorage, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IFillLockBytes, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IProgressNotify, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::ILayoutStorage, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IDirectWriterLock, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IOleDocument, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IOleDocumentSite, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IOleDocumentView, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IEnumOleDocumentViews, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IContinueCallback, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IPrint, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IOleCommandTarget, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IZoomEvents, PAL::IUnknown>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocument::CreateView), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocument::*)(PAL::IOleInPlaceSite*, PAL::IStream*, PAL::DWord, PAL::IOleDocumentView**)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocument::GetDocMiscStatus), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocument::*)(PAL::DWord*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocument::EnumViews), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocument::*)(PAL::IEnumOleDocumentViews**, PAL::IOleDocumentView**)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentSite::ActivateMe), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentSite::*)(PAL::IOleDocumentView*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::SetInPlaceSite), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IOleInPlaceSite*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::GetInPlaceSite), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IOleInPlaceSite**)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::GetDocument), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IUnknown**)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::SetRect), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::Rect*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::GetRect), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::Rect*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::SetRectComplex), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::Rect*, PAL::Rect*, PAL::Rect*, PAL::Rect*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::Show), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(int)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::UIActivate), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(int)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::Open), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)()>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::CloseView), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::DWord)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::SaveViewState), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IStream*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::ApplyViewState), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IStream*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleDocumentView::Clone), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleDocumentView::*)(PAL::IOleInPlaceSite*, PAL::IOleDocumentView**)>);
    static_assert(std::is_same_v<decltype(&PAL::IEnumOleDocumentViews::Next), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IEnumOleDocumentViews::*)(PAL::ULong, PAL::IOleDocumentView**, PAL::ULong*)>);
    static_assert(std::is_same_v<decltype(&PAL::IEnumOleDocumentViews::Skip), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IEnumOleDocumentViews::*)(PAL::ULong)>);
    static_assert(std::is_same_v<decltype(&PAL::IEnumOleDocumentViews::Reset), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IEnumOleDocumentViews::*)()>);
    static_assert(std::is_same_v<decltype(&PAL::IEnumOleDocumentViews::Clone), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IEnumOleDocumentViews::*)(PAL::IEnumOleDocumentViews**)>);
    static_assert(std::is_same_v<decltype(&PAL::IContinueCallback::FContinue), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IContinueCallback::*)()>);
    static_assert(std::is_same_v<decltype(&PAL::IContinueCallback::FContinuePrinting), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IContinueCallback::*)(int32_t, int32_t, wchar_t*)>);
    static_assert(std::is_same_v<decltype(&PAL::IPrint::SetInitialPageNum), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IPrint::*)(int32_t)>);
    static_assert(std::is_same_v<decltype(&PAL::IPrint::GetPageInfo), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IPrint::*)(int32_t*, int32_t*)>);
    static_assert(std::is_same_v<decltype(&PAL::IPrint::Print), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IPrint::*)(PAL::DWord, PAL::TargetDevice**, PAL::PageSet**, PAL::StorageMedium*, PAL::IContinueCallback*, int32_t, int32_t*, int32_t*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleCommandTarget::QueryStatus), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleCommandTarget::*)(const PAL::Guid*, PAL::ULong, PAL::OleCommand*, PAL::OleCommandText*)>);
    static_assert(std::is_same_v<decltype(&PAL::IOleCommandTarget::Exec), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IOleCommandTarget::*)(const PAL::Guid*, PAL::DWord, PAL::DWord, PAL::Variant*, PAL::Variant*)>);
    static_assert(std::is_same_v<decltype(&PAL::IZoomEvents::OnZoomPercentChanged), PAL::HResult (SORA_PAL_EXPECTED_COM_CALL PAL::IZoomEvents::*)(PAL::ULong)>);
    static_assert(std::derived_from<PAL::IPersist, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IPersistStream, PAL::IPersist>);
    static_assert(std::derived_from<PAL::IPersistFile, PAL::IPersist>);
    static_assert(std::derived_from<PAL::IPersistStorage, PAL::IPersist>);
    static_assert(std::derived_from<PAL::IPropertyStorage, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IPropertySetStorage, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IEnumStatPropStg, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IEnumStatPropSetStg, PAL::IUnknown>);
    static_assert(sizeof(PAL::StatStg)          == 80);
    static_assert(alignof(PAL::StatStg)         == 8);
    static_assert(sizeof(PAL::StorageLayout)    == 32);
    static_assert(alignof(PAL::StorageLayout)   == 8);
    static_assert(sizeof(PAL::Rect)             == sizeof(int32_t) * 4);
    static_assert(sizeof(PAL::OleCommand)       == sizeof(uint32_t) * 2);
    static_assert(sizeof(PAL::OleCommandText)   == 16);
    static_assert(sizeof(PAL::PageRange)        == sizeof(int32_t) * 2);
    static_assert(sizeof(PAL::PageSet)          == 24);
    static_assert(sizeof(PAL::PropertySpec)     == 16);
    static_assert(sizeof(PAL::PropertyVariant)  == PAL::AutomationValueBytes);
    static_assert(alignof(PAL::PropertyVariant) == PAL::AutomationValueAlignment);
    static_assert(std::is_standard_layout_v<PAL::PropertyVariant>);
    static_assert(!std::is_copy_constructible_v<PAL::PropertyVariant>);
    static_assert(std::is_nothrow_move_constructible_v<PAL::PropertyVariant>);
    static_assert(sizeof(PAL::StatPropStg)      == 16);
    static_assert(sizeof(PAL::StatPropSetStg)   == 64);
    static_assert(std::derived_from<PAL::IDataObject, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IDispatch, PAL::IUnknown>);
    static_assert(std::is_nothrow_destructible_v<PAL::ComPtr<PAL::IUnknown>>);
    static_assert(!std::is_copy_constructible_v<PAL::Apartment>);
    static_assert(!std::is_move_constructible_v<PAL::Apartment>);
    static_assert(!std::is_copy_constructible_v<PAL::TaskMemory<std::byte>>);
    static_assert(!std::is_copy_constructible_v<PAL::Variant>);

} // namespace

namespace Sora::PAL {

    Apartment::Apartment(ApartmentModel model) noexcept {
#if defined(PLATFORM_WINDOWS)
        const COMSystemAPI& api = LoadCOMSystemAPI();
        if (api.initializeEx == nullptr) {
            status_ = ENotImpl;
            return;
        }
        status_ = api.initializeEx(nullptr, static_cast<DWord>(model));
        initialized_ = Succeeded(status_);
#else
        static_cast<void>(model);
#endif
    }

    Apartment::~Apartment() noexcept {
        if (!initialized_) {
            return;
        }
#if defined(PLATFORM_WINDOWS)
        if (const COMSystemAPI& api = LoadCOMSystemAPI(); api.uninitialize != nullptr) {
            api.uninitialize();
        }
#endif
    }

    HResult CreateInstanceRaw(RefClassId classId, void* outer, DWord context, RefInterfaceId iid,
                              void** object) noexcept {
        if (object == nullptr) {
            return EPointer;
        }
        *object = nullptr;
#if defined(PLATFORM_WINDOWS)
        const COMSystemAPI& api = LoadCOMSystemAPI();
        if (api.createInstance == nullptr) {
            return ENotImpl;
        }
        return api.createInstance(classId, outer, context, iid, object);
#else
        static_cast<void>(classId);
        static_cast<void>(outer);
        static_cast<void>(context);
        static_cast<void>(iid);
        return ENotImpl;
#endif
    }

    std::expected<ComPtr<IStream>, HResult> CreateStreamOnGlobal(void* global, bool deleteOnRelease) noexcept {
        ComPtr<IStream> stream;
#if defined(PLATFORM_WINDOWS)
        const COMSystemAPI& api = LoadCOMSystemAPI();
        if (api.createStreamOnGlobal == nullptr) {
            return std::unexpected(ENotImpl);
        }
        const HResult status = api.createStreamOnGlobal(global, deleteOnRelease ? 1 : 0,
                                                        reinterpret_cast<void**>(stream.Put()));
        if (Failed(status)) {
            return std::unexpected(status);
        }
        return stream;
#else
        static_cast<void>(global);
        static_cast<void>(deleteOnRelease);
        return std::unexpected(ENotImpl);
#endif
    }

    HResult CreateGuid(Guid& guid) noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const COMSystemAPI& api = LoadCOMSystemAPI(); api.createGuid != nullptr) {
            return api.createGuid(&guid);
        }
#else
        static_cast<void>(guid);
#endif
        return ENotImpl;
    }

    void* AllocateTaskMemory(size_t bytes) noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const COMSystemAPI& api = LoadCOMSystemAPI(); api.taskMemAlloc != nullptr) {
            return api.taskMemAlloc(bytes);
        }
#else
        static_cast<void>(bytes);
#endif
        return nullptr;
    }

    void FreeTaskMemory(void* memory) noexcept {
#if defined(PLATFORM_WINDOWS)
        if (memory != nullptr) {
            if (const COMSystemAPI& api = LoadCOMSystemAPI(); api.taskMemFree != nullptr) {
                api.taskMemFree(memory);
            }
        }
#else
        static_cast<void>(memory);
#endif
    }

    BStr::BStr(const wchar_t* text) noexcept {
#if defined(PLATFORM_WINDOWS)
        if (text != nullptr) {
            if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.allocString != nullptr) {
                value_ = api.allocString(text);
            }
        }
#else
        static_cast<void>(text);
#endif
    }

    BStr::BStr(BStr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    BStr& BStr::operator=(BStr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    BStr::~BStr() noexcept { Reset(); }

    wchar_t* BStr::Detach() noexcept { return std::exchange(value_, nullptr); }

    void BStr::Reset() noexcept {
#if defined(PLATFORM_WINDOWS)
        if (value_ != nullptr) {
            if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.freeString != nullptr) {
                api.freeString(value_);
            }
        }
#endif
        value_ = nullptr;
    }

    SafeArray::SafeArray(SafeArray&& other) noexcept : array_(std::exchange(other.array_, nullptr)) {}

    SafeArray SafeArray::CreateVector(uint16_t variantType, int32_t lowerBound, uint32_t count) noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.safeArrayCreateVector != nullptr) {
            return SafeArray{api.safeArrayCreateVector(variantType, lowerBound, count)};
        }
#else
        static_cast<void>(variantType);
        static_cast<void>(lowerBound);
        static_cast<void>(count);
#endif
        return {};
    }

    SafeArray& SafeArray::operator=(SafeArray&& other) noexcept {
        if (this != &other) {
            Reset();
            array_ = std::exchange(other.array_, nullptr);
        }
        return *this;
    }

    SafeArray::~SafeArray() noexcept { Reset(); }

    void* SafeArray::Detach() noexcept { return std::exchange(array_, nullptr); }

    void SafeArray::Reset() noexcept {
#if defined(PLATFORM_WINDOWS)
        if (array_ != nullptr) {
            if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.safeArrayDestroy != nullptr) {
                api.safeArrayDestroy(array_);
            }
        }
#endif
        array_ = nullptr;
    }

    PropertyVariant::PropertyVariant(PropertyVariant&& other) noexcept {
        std::memcpy(storage_.data(), other.storage_.data(), storage_.size());
        std::memset(other.storage_.data(), 0, other.storage_.size());
    }

    PropertyVariant& PropertyVariant::operator=(PropertyVariant&& other) noexcept {
        if (this != &other) {
            Clear();
            std::memcpy(storage_.data(), other.storage_.data(), storage_.size());
            std::memset(other.storage_.data(), 0, other.storage_.size());
        }
        return *this;
    }

    PropertyVariant::~PropertyVariant() noexcept { Clear(); }

    std::expected<PropertyVariant, HResult> PropertyVariant::Clone() const noexcept {
        PropertyVariant result;
#if defined(PLATFORM_WINDOWS)
        if (const StructuredStorageSystemAPI& api = LoadStructuredStorageSystemAPI();
            api.copyPropertyVariant != nullptr) {
            const HResult status = api.copyPropertyVariant(result.Data(), Data());
            if (Failed(status)) {
                return std::unexpected(status);
            }
            return result;
        }
#endif
        return std::unexpected(ENotImpl);
    }

    void PropertyVariant::Clear() noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const StructuredStorageSystemAPI& api = LoadStructuredStorageSystemAPI();
            api.clearPropertyVariant != nullptr) {
            static_cast<void>(api.clearPropertyVariant(storage_.data()));
            return;
        }
#endif
        std::memset(storage_.data(), 0, storage_.size());
    }

    Variant::Variant() noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.variantInit != nullptr) {
            api.variantInit(storage_.data());
        }
#endif
    }

    std::expected<Variant, HResult> Variant::Clone() const noexcept {
        Variant result;
#if defined(PLATFORM_WINDOWS)
        if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.variantCopy != nullptr) {
            const HResult status = api.variantCopy(result.storage_.data(), storage_.data());
            if (Failed(status)) {
                return std::unexpected(status);
            }
            return result;
        }
#endif
        return std::unexpected(ENotImpl);
    }

    Variant::Variant(Variant&& other) noexcept : Variant() {
        std::memcpy(storage_.data(), other.storage_.data(), storage_.size());
        std::memset(other.storage_.data(), 0, other.storage_.size());
    }

    Variant& Variant::operator=(Variant&& other) noexcept {
        if (this != &other) {
            Clear();
            std::memcpy(storage_.data(), other.storage_.data(), storage_.size());
            std::memset(other.storage_.data(), 0, other.storage_.size());
        }
        return *this;
    }

    Variant::~Variant() noexcept { Clear(); }

    void Variant::Clear() noexcept {
#if defined(PLATFORM_WINDOWS)
        if (const OleAutomationSystemAPI& api = LoadOleAutomationSystemAPI(); api.variantClear != nullptr) {
            api.variantClear(storage_.data());
            return;
        }
#endif
        std::memset(storage_.data(), 0, storage_.size());
    }

} // namespace Sora::PAL

#undef SORA_PAL_EXPECTED_COM_CALL
