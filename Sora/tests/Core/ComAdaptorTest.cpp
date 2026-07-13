#include "Sora/Kernel/Core/ComAdaptor.h"
#include "Sora/Kernel/Core/Query.h"
#include "Sora/Kernel/Core/Registry.h"
#include "Sora/Kernel/Core/EventPortAdaptor.h"

#include <catch2/catch_test_macros.hpp>

namespace Sora::Kernel::Test {

    struct Payload {
        int value = 0;

        [[nodiscard]] int Double() const noexcept { return value * 2; }
    };

    struct MetadataPayload {};

    class [[= Sora::Kernel::$::Interface]] ITaggedPayload : public Sora::Kernel::BaseUnknown {
    public:
        virtual int Tag() const noexcept = 0;
    };

} // namespace Sora::Kernel::Test

namespace Sora::Kernel {

    template<>
    struct ComAdaptorDecl<Sora::Kernel::Test::Payload> {
        static constexpr TypeOfClass role = TypeOfClass::Implementation;
        using Extends = Sora::Traits::TypeList<>;
        using Implements = Sora::Traits::TypeList<>;
    };

    template<>
    struct ComAdaptorDecl<Sora::Kernel::Test::MetadataPayload> {
        static constexpr TypeOfClass role = TypeOfClass::DataExtension;
        using Extends = Sora::Traits::TypeList<Sora::Kernel::ComAdaptor<Sora::Kernel::Test::Payload>>;
        using Implements = Sora::Traits::TypeList<Sora::Kernel::Test::ITaggedPayload>;
    };

} // namespace Sora::Kernel

namespace {

    using Payload = Sora::Kernel::Test::Payload;
    using PayloadObject = Sora::Kernel::ComAdaptor<Payload>;
    using MetadataObject = Sora::Kernel::ComAdaptor<Sora::Kernel::Test::MetadataPayload>;

    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<PayloadObject>{}]]
    PayloadExtension : public Sora::Kernel::BaseUnknown {
    public:
        S_OBJECT

        int extra = 11;
    };

} // namespace

TEST_CASE("ComAdaptor wraps ordinary objects as extensible COM nuclei", "[Sora.Core.ComAdaptor]") {
    static_assert(Sora::Kernel::Concept::ImplementationClass<PayloadObject>);
    static_assert(Sora::Kernel::Traits::RoleOf<Sora::Kernel::ComAdaptor<int>> ==
                  Sora::Kernel::TypeOfClass::NothingType);
    static_assert(Sora::Kernel::Meta::ImplementedInterfaceTypesOf<MetadataObject>().size() == 1);
    static_assert(Sora::Kernel::Meta::ImplementedInterfaceTypesOf<MetadataObject>()[0] ==
                  ^^Sora::Kernel::Test::ITaggedPayload);
    static_assert(Sora::Kernel::Meta::ExtendeeTypesOf<MetadataObject>().size() == 1);
    static_assert(Sora::Kernel::Meta::ExtendeeTypesOf<MetadataObject>()[0] == std::meta::dealias(^^PayloadObject));
    static_assert(Sora::Kernel::Concept::DataExtensionClass<PayloadExtension>);

    Sora::Kernel::RegisterKernelClass<PayloadObject>();
    Sora::Kernel::RegisterKernelClass<PayloadExtension>();

    auto object = Sora::Kernel::MakeComAdaptor<Payload>(Payload{7});
    REQUIRE(object);
    CHECK(object->Object().value == 7);
    CHECK((*object)->Double() == 14);
    CHECK((**object).Double() == 14);

    auto* extension = Sora::Kernel::QueryInterface<PayloadExtension>(*object);
    REQUIRE(extension != nullptr);
    CHECK(extension->extra == 11);

    extension->extra = 23;
    CHECK(Sora::Kernel::QueryInterface<PayloadExtension>(*object)->extra == 23);
}

TEST_CASE("EventPort for BaseUnknown is backed by a DataExtension", "[Sora.Core.ComAdaptor]") {
    static_assert(Sora::Kernel::Concept::DataExtensionClass<Sora::Kernel::ComAdaptor<Sora::EventPort>>);

    auto object = Sora::Kernel::MakeComAdaptor<int>(42);

    Sora::EventPort& port = Sora::Kernel::EventPortOf(*object);
    auto* extension = Sora::Kernel::QueryInterface<Sora::Kernel::ComAdaptor<Sora::EventPort>>(*object);

    REQUIRE(extension != nullptr);
    CHECK(&extension->Object() == &port);
    CHECK(&Sora::Kernel::EventPortOf(*object) == &port);
}
