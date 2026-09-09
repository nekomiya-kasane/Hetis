#include <Sora/Core/PAL/ComAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <type_traits>

#if defined(PLATFORM_WINDOWS)
#    define SORA_TEST_COM_CALL __stdcall
#else
#    define SORA_TEST_COM_CALL
#endif

namespace {

    namespace PAL = Sora::PAL;

    struct LifetimeState {
        PAL::ULong references = 1;
        bool destroyed = false;
    };

    class TestUnknown final : public PAL::IUnknown {
    public:
        explicit TestUnknown(LifetimeState& state) noexcept : state_(state) {}

        PAL::HResult SORA_TEST_COM_CALL QueryInterface(PAL::RefInterfaceId iid, void** object) override {
            if (object == nullptr) {
                return PAL::EInvalidArg;
            }
            *object = nullptr;
            if (!PAL::EqualGuid(iid, PAL::InterfaceIdOf<PAL::IUnknown>())) {
                return PAL::ENoInterface;
            }
            *object = this;
            AddRef();
            return PAL::SOk;
        }

        PAL::ULong SORA_TEST_COM_CALL AddRef() override { return ++state_.references; }

        PAL::ULong SORA_TEST_COM_CALL Release() override {
            const PAL::ULong references = --state_.references;
            if (references == 0) {
                state_.destroyed = true;
                delete this;
            }
            return references;
        }

    private:
        LifetimeState& state_;
    };

    static_assert(sizeof(PAL::Guid) == 16);
    static_assert(!std::is_move_constructible_v<PAL::Apartment>);
    static_assert(std::derived_from<PAL::IStream, PAL::IUnknown>);
    static_assert(std::derived_from<PAL::IFillLockBytes, PAL::IUnknown>);
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
    static_assert(std::derived_from<PAL::IDispatch, PAL::IUnknown>);
    static_assert(PAL::InterfaceIdOf<PAL::IFillLockBytes>().data1 == 0x99caf010u);
    static_assert(PAL::InterfaceIdOf<PAL::IProgressNotify>().data1 == 0xa9d758a0u);
    static_assert(PAL::InterfaceIdOf<PAL::ILayoutStorage>().data1 == 0x0e6d4d90u);
    static_assert(PAL::InterfaceIdOf<PAL::IDirectWriterLock>().data1 == 0x0e6d4d92u);
    static_assert(PAL::InterfaceIdOf<PAL::IOleDocument>().data1 == 0xb722bcc5u);
    static_assert(PAL::InterfaceIdOf<PAL::IOleDocumentSite>().data1 == 0xb722bcc7u);
    static_assert(PAL::InterfaceIdOf<PAL::IOleDocumentView>().data1 == 0xb722bcc6u);
    static_assert(PAL::InterfaceIdOf<PAL::IEnumOleDocumentViews>().data1 == 0xb722bcc8u);
    static_assert(PAL::InterfaceIdOf<PAL::IContinueCallback>().data1 == 0xb722bccau);
    static_assert(PAL::InterfaceIdOf<PAL::IPrint>().data1 == 0xb722bcc9u);
    static_assert(PAL::InterfaceIdOf<PAL::IOleCommandTarget>().data1 == 0xb722bccbu);
    static_assert(PAL::InterfaceIdOf<PAL::IZoomEvents>().data1 == 0x41b68150u);
    static_assert(sizeof(PAL::Rect) == 16);
    static_assert(sizeof(PAL::OleCommand) == 8);
    static_assert(sizeof(PAL::PageRange) == 8);

} // namespace

TEST_CASE("ComPtr preserves adopted, copied, queried, and detached references", "[Sora.PAL.COM]") {
    LifetimeState state;
    PAL::ComPtr<PAL::IUnknown> owner = PAL::AdoptCom<PAL::IUnknown>(new TestUnknown(state));
    REQUIRE(state.references == 1);

    {
        PAL::ComPtr<PAL::IUnknown> copy = owner;
        REQUIRE(state.references == 2);

        auto queried = owner.QueryInterface<PAL::IUnknown>();
        REQUIRE(queried.has_value());
        REQUIRE(state.references == 3);
    }
    REQUIRE(state.references == 1);

    PAL::IUnknown* detached = owner.Detach();
    REQUIRE_FALSE(owner);
    REQUIRE(state.references == 1);
    owner.Attach(detached);
    owner.Attach(detached);
    REQUIRE(state.references == 1);
    owner.Reset();
    REQUIRE(state.destroyed);
}

TEST_CASE("Empty ComPtr follows COM output-pointer conventions", "[Sora.PAL.COM]") {
    PAL::ComPtr<PAL::IUnknown> empty;
    PAL::IUnknown* output = reinterpret_cast<PAL::IUnknown*>(uintptr_t{1});
    REQUIRE(empty.CopyTo(&output) == PAL::SOk);
    REQUIRE(output == nullptr);
    REQUIRE(empty.CopyTo<PAL::IUnknown>(nullptr) == PAL::EPointer);
    auto queried = empty.QueryInterface<PAL::IUnknown>();
    REQUIRE_FALSE(queried.has_value());
    REQUIRE(queried.error() == PAL::EPointer);
}

TEST_CASE("COM runtime facades use dynamically resolved SystemAPI entries", "[Sora.PAL.COM]") {
#if defined(PLATFORM_WINDOWS)
    PAL::Apartment apartment;
    REQUIRE(apartment.Status() != PAL::ENotImpl);

    PAL::Guid guid{};
    REQUIRE(PAL::CreateGuid(guid) == PAL::SOk);

    PAL::TaskMemory<std::byte> memory = PAL::TaskMemory<std::byte>::Allocate(32);
    REQUIRE(memory.Get() != nullptr);

    PAL::BStr text{L"Sora"};
    REQUIRE(text);
    REQUIRE(text.CStr()[0] == L'S');

    PAL::SafeArray array = PAL::SafeArray::CreateVector(17, 0, 4);
    REQUIRE(array.Get() != nullptr);

    PAL::Variant value;
    auto clone = value.Clone();
    REQUIRE(clone.has_value());

    PAL::PropertyVariant propertyValue;
    auto propertyClone = propertyValue.Clone();
    REQUIRE(propertyClone.has_value());
#else
    PAL::Apartment apartment;
    REQUIRE(apartment.Status() == PAL::ENotImpl);
    REQUIRE_FALSE(apartment.IsInitialized());
#endif
}

#undef SORA_TEST_COM_CALL
