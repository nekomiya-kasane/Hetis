/**
 * @file CoreKernelTest.cpp
 * @brief Kernel tests for the Yuki Core BaseUnknown object model.
 * @ingroup Core
 *
 * @details Fixture graph: BaseUnknown -> Entity -> Spatial -> Drawable -> Mesh -> Rigged;
 * Light branches from Spatial; extensions and TIE objects attach to the same closure.
 */
#include <Yuki/Core/Dictionary.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <type_traits>

namespace {

    struct NonCoreType {};

    template<class T>
    concept FormsComPtr = requires { typename Yuki::ComPtr<T>; };

    struct[[= Yuki::Anno::Interface]] IObject : Yuki::BaseUnknown {
        virtual ~IObject() = default;
        [[nodiscard]] virtual int ObjectId() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] ITransform : IObject {
        virtual ~ITransform() = default;
        [[nodiscard]] virtual int X() const noexcept = 0;
        [[nodiscard]] virtual int Y() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IRenderable : IObject {
        virtual ~IRenderable() = default;
        [[nodiscard]] virtual int RenderLayer() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IBounds : Yuki::BaseUnknown {
        virtual ~IBounds() = default;
        [[nodiscard]] virtual int BoundsVersion() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IMaterial : Yuki::BaseUnknown {
        virtual ~IMaterial() = default;
        [[nodiscard]] virtual int MaterialCode() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IPhysics : Yuki::BaseUnknown {
        virtual ~IPhysics() = default;
        [[nodiscard]] virtual int MassTimes100() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IDiagnostics : Yuki::BaseUnknown {
        virtual ~IDiagnostics() = default;
        [[nodiscard]] virtual const char* DiagnosticName() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IAudit : Yuki::BaseUnknown {
        virtual ~IAudit() = default;
        [[nodiscard]] virtual int AuditCode() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IAnimation : Yuki::BaseUnknown {
        virtual ~IAnimation() = default;
        [[nodiscard]] virtual int ClipCount() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Interface]] IUnused : Yuki::BaseUnknown {
        virtual ~IUnused() = default;
        [[nodiscard]] virtual int Unused() const noexcept = 0;
    };

    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^IObject, ^^IDiagnostics}]] Entity
        : Yuki::BaseUnknown {
        Y_OBJECT

        struct ObjectFacet final : IObject {
            Entity* owner{};
            explicit ObjectFacet(Entity& entity) noexcept : owner{&entity} {}
            [[nodiscard]] int ObjectId() const noexcept override { return owner->id; }
        };

        struct DiagnosticsFacet final : IDiagnostics {
            Entity* owner{};
            explicit DiagnosticsFacet(Entity& entity) noexcept : owner{&entity} {}
            [[nodiscard]] const char* DiagnosticName() const noexcept override { return "Entity"; }
        };

        int id{};
        ObjectFacet object{*this};
        DiagnosticsFacet diagnostics{*this};

        explicit Entity(int value = 0) noexcept : id{value}, object{*this}, diagnostics{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IObject>) {
                return &object;
            } else if constexpr (std::same_as<Interface, IDiagnostics>) {
                return &diagnostics;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^ITransform}]] Spatial : Entity {
        Y_OBJECT

        struct TransformFacet final : ITransform {
            Spatial* owner{};
            explicit TransformFacet(Spatial& spatial) noexcept : owner{&spatial} {}
            [[nodiscard]] int ObjectId() const noexcept override { return owner->id; }
            [[nodiscard]] int X() const noexcept override { return owner->x; }
            [[nodiscard]] int Y() const noexcept override { return owner->y; }
        };

        int x{};
        int y{};
        TransformFacet transform{*this};

        Spatial(int value = 0, int xValue = 0, int yValue = 0) noexcept
            : Entity{value}, x{xValue}, y{yValue}, transform{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, ITransform>) {
                return &transform;
            } else {
                return Entity::template Facet<Interface>();
            }
        }
    };
    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^IRenderable}]] Drawable : Spatial {
        Y_OBJECT

        struct RenderFacet final : IRenderable {
            Drawable* owner{};
            explicit RenderFacet(Drawable& drawable) noexcept : owner{&drawable} {}
            [[nodiscard]] int ObjectId() const noexcept override { return owner->id; }
            [[nodiscard]] int RenderLayer() const noexcept override { return owner->layer; }
        };

        int layer{};
        RenderFacet render{*this};

        Drawable(int value = 0, int xValue = 0, int yValue = 0, int layerValue = 0) noexcept
            : Spatial{value, xValue, yValue}, layer{layerValue}, render{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IRenderable>) {
                return &render;
            } else {
                return Spatial::template Facet<Interface>();
            }
        }
    };

    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^IBounds, ^^IMaterial}]] Mesh : Drawable {
        Y_OBJECT

        struct BoundsFacet final : IBounds {
            Mesh* owner{};
            explicit BoundsFacet(Mesh& mesh) noexcept : owner{&mesh} {}
            [[nodiscard]] int BoundsVersion() const noexcept override { return owner->boundsVersion; }
        };

        struct MaterialFacet final : IMaterial {
            Mesh* owner{};
            explicit MaterialFacet(Mesh& mesh) noexcept : owner{&mesh} {}
            [[nodiscard]] int MaterialCode() const noexcept override { return owner->materialCode; }
        };

        int boundsVersion{};
        int materialCode{};
        BoundsFacet bounds{*this};
        MaterialFacet material{*this};

        Mesh(int value = 0, int xValue = 0, int yValue = 0, int layerValue = 0, int materialValue = 0) noexcept
            : Drawable{value, xValue, yValue, layerValue}, boundsVersion{value + 100},
              materialCode{materialValue}, bounds{*this}, material{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IBounds>) {
                return &bounds;
            } else if constexpr (std::same_as<Interface, IMaterial>) {
                return &material;
            } else {
                return Drawable::template Facet<Interface>();
            }
        }
    };

    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^IAnimation}]] Rigged : Mesh {
        Y_OBJECT

        struct AnimationFacet final : IAnimation {
            Rigged* owner{};
            explicit AnimationFacet(Rigged& rigged) noexcept : owner{&rigged} {}
            [[nodiscard]] int ClipCount() const noexcept override { return owner->clips; }
        };

        int clips{};
        AnimationFacet animation{*this};

        Rigged(int value = 0, int xValue = 0, int yValue = 0, int layerValue = 0, int materialValue = 0,
               int clipValue = 0) noexcept
            : Mesh{value, xValue, yValue, layerValue, materialValue}, clips{clipValue}, animation{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IAnimation>) {
                return &animation;
            } else {
                return Mesh::template Facet<Interface>();
            }
        }
    };

    struct[[= Yuki::Anno::Implementation]][[= Yuki::Anno::Implements{^^IRenderable}]] Light : Spatial {
        Y_OBJECT

        struct LightRenderFacet final : IRenderable {
            Light* owner{};
            explicit LightRenderFacet(Light& light) noexcept : owner{&light} {}
            [[nodiscard]] int ObjectId() const noexcept override { return owner->id; }
            [[nodiscard]] int RenderLayer() const noexcept override { return owner->power + 1000; }
        };

        int power{};
        LightRenderFacet render{*this};

        explicit Light(int value = 0, int powerValue = 0) noexcept
            : Spatial{value, 0, 0}, power{powerValue}, render{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IRenderable>) {
                return &render;
            } else {
                return Spatial::template Facet<Interface>();
            }
        }
    };

    struct PlainSpatialChild : Spatial {};

    struct[[= Yuki::Anno::Extension]] EmptyExtension : Yuki::BaseUnknown {
        Y_OBJECT
    };

    struct[[= Yuki::Anno::Extension]] StatefulExtensionBase : Yuki::BaseUnknown {
        Y_OBJECT
        int inheritedState{};
    };

    struct[[= Yuki::Anno::Extension]] StatefulExtensionLeaf : StatefulExtensionBase {
        Y_OBJECT
    };
    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Spatial, ^^Mesh}]]
          [[= Yuki::Anno::Implements{^^IPhysics}]] PhysicsExtension : Yuki::BaseUnknown {
        Y_OBJECT

        struct PhysicsFacet final : IPhysics {
            PhysicsExtension* owner{};
            explicit PhysicsFacet(PhysicsExtension& extension) noexcept : owner{&extension} {}
            [[nodiscard]] int MassTimes100() const noexcept override { return owner->mass; }
        };

        int mass{};
        PhysicsFacet physics{*this};

        explicit PhysicsExtension(int value = 0) noexcept : mass{value}, physics{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IPhysics>) {
                return &physics;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Mesh}]] RigidBody : PhysicsExtension {
        Y_OBJECT
        explicit RigidBody(int value = 0) noexcept : PhysicsExtension{value} {}
    };

    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Drawable}]]
          [[= Yuki::Anno::Dispatch{Yuki::DispatchKind::InlineFacet, 0}]][[= Yuki::Anno::Implements{^^IBounds}]]
        BoundsOverride : Yuki::BaseUnknown {
        Y_OBJECT

        struct BoundsFacet final : IBounds {
            BoundsOverride* owner{};
            explicit BoundsFacet(BoundsOverride& extension) noexcept : owner{&extension} {}
            [[nodiscard]] int BoundsVersion() const noexcept override { return owner->version; }
        };

        int version{};
        BoundsFacet bounds{*this};

        explicit BoundsOverride(int value = 0) noexcept : version{value}, bounds{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IBounds>) {
                return &bounds;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Mesh}]]
          [[= Yuki::Anno::Dispatch{Yuki::DispatchKind::InlineFacet, 0}]][[= Yuki::Anno::Implements{^^IMaterial}]]
        MaterialOverride : Yuki::BaseUnknown {
        Y_OBJECT

        struct MaterialFacet final : IMaterial {
            MaterialOverride* owner{};
            explicit MaterialFacet(MaterialOverride& extension) noexcept : owner{&extension} {}
            [[nodiscard]] int MaterialCode() const noexcept override { return owner->code; }
        };

        int code{};
        MaterialFacet material{*this};

        explicit MaterialOverride(int value = 0) noexcept : code{value}, material{*this} {}

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IMaterial>) {
                return &material;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Light}]]
          [[= Yuki::Anno::Implements{^^IUnused}]] InvalidLightExtension : Yuki::BaseUnknown {
        Y_OBJECT

        struct UnusedFacet final : IUnused {
            InvalidLightExtension* owner{};
            explicit UnusedFacet(InvalidLightExtension& extension) noexcept : owner{&extension} {}
            [[nodiscard]] int Unused() const noexcept override { return owner ? 1 : 0; }
        };

        std::atomic<int>* destroyed{};
        UnusedFacet unused{*this};

        explicit InvalidLightExtension(std::atomic<int>& counter) noexcept : destroyed{&counter}, unused{*this} {}
        ~InvalidLightExtension() noexcept override { destroyed->fetch_add(1, std::memory_order_relaxed); }

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IUnused>) {
                return &unused;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::Extension]][[= Yuki::Anno::Extends{^^Spatial}]] CountingPhysics : PhysicsExtension {
        Y_OBJECT

        std::atomic<int>* destroyed{};

        explicit CountingPhysics(std::atomic<int>& counter) noexcept : PhysicsExtension{12}, destroyed{&counter} {}
        ~CountingPhysics() noexcept override { destroyed->fetch_add(1, std::memory_order_relaxed); }
    };

    struct[[= Yuki::Anno::TIE]][[= Yuki::Anno::Dispatch{Yuki::DispatchKind::BoundFacet, 0}]]
          [[= Yuki::Anno::Implements{^^IDiagnostics}]] DiagnosticsTie : Yuki::BaseUnknown {
        Y_OBJECT

        struct DiagnosticsFacet final : IDiagnostics {
            DiagnosticsTie* owner{};
            explicit DiagnosticsFacet(DiagnosticsTie& tie) noexcept : owner{&tie} {}
            [[nodiscard]] const char* DiagnosticName() const noexcept override {
                return owner && owner->BoundTarget() ? "DiagnosticsTie" : "DetachedDiagnosticsTie";
            }
        };

        DiagnosticsFacet diagnostics{*this};

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IDiagnostics>) {
                return &diagnostics;
            } else {
                return nullptr;
            }
        }
    };

    struct[[= Yuki::Anno::TIEchain]][[= Yuki::Anno::Dispatch{Yuki::DispatchKind::BoundFacet, 0}]]
          [[= Yuki::Anno::Implements{^^IAudit}]] AuditTie : Yuki::BaseUnknown {
        Y_OBJECT

        struct AuditFacet final : IAudit {
            AuditTie* owner{};
            explicit AuditFacet(AuditTie& tie) noexcept : owner{&tie} {}
            [[nodiscard]] int AuditCode() const noexcept override { return owner && owner->BoundTarget() ? 700 : -1; }
        };

        std::atomic<int>* destroyed{};
        AuditFacet audit{*this};

        explicit AuditTie(std::atomic<int>* counter = nullptr) noexcept : destroyed{counter}, audit{*this} {}
        ~AuditTie() noexcept override {
            if (destroyed) {
                destroyed->fetch_add(1, std::memory_order_relaxed);
            }
        }

        template<class Interface>
        [[nodiscard]] Interface* Facet() noexcept {
            if constexpr (std::same_as<Interface, IAudit>) {
                return &audit;
            } else {
                return nullptr;
            }
        }
    };

    inline constexpr Yuki::Iid kExplicitIid{0x1234'5678'9abc'8defull, 0x9234'5678'9abc'def0ull};
    struct[[= Yuki::Anno::IidOverride{kExplicitIid}]] ExplicitIidType {};

    struct[[= Yuki::Anno::Implementation]] SectionComponent : Yuki::BaseUnknown {
        Y_OBJECT
    };

    inline constexpr Yuki::CoreSectionMetaClassRecord kSectionMetaRecord{Yuki::TypeOfClass::Implementation,
                                                                         Yuki::IidOf<SectionComponent>()};

    inline constexpr Yuki::CoreSectionProvideRecord kSectionProvideRecord{
        Yuki::IidOf<SectionComponent>(), Yuki::IidOf<IObject>(), Yuki::DispatchKind::InlineFacet,
        Yuki::RecordStatus::Declared, 2};

} // namespace
TEST_CASE("Core type constraints reject non-BaseUnknown ComPtr elements", "[Yuki][Core]") {
    STATIC_REQUIRE(Yuki::BaseUnknownClass<Yuki::BaseUnknown>);
    STATIC_REQUIRE(Yuki::BaseUnknownClass<IObject>);
    STATIC_REQUIRE(Yuki::BaseUnknownClass<Rigged>);
    STATIC_REQUIRE_FALSE(Yuki::BaseUnknownClass<NonCoreType>);
    STATIC_REQUIRE_FALSE(FormsComPtr<NonCoreType>);
}

TEST_CASE("IID synthesis is reflection-stable and supports explicit overrides", "[Yuki][Core]") {
    constexpr Yuki::Iid meshIid = Yuki::IidOf<Mesh>();
    constexpr Yuki::Iid meshIidAgain = Yuki::IidOfMeta(^^Mesh);

    REQUIRE(meshIid == meshIidAgain);
    REQUIRE_FALSE(Yuki::IsNil(meshIid));
    REQUIRE(Yuki::IidOf<ExplicitIidType>() == kExplicitIid);
}

TEST_CASE("Class roles are local facts while extension state is layout-derived", "[Yuki][Core]") {
    STATIC_REQUIRE(Yuki::ClassTypeOf<Entity> == Yuki::TypeOfClass::Implementation);
    STATIC_REQUIRE(Yuki::ClassTypeOf<PlainSpatialChild> == Yuki::TypeOfClass::NothingType);
    STATIC_REQUIRE(Yuki::ClassTypeOf<EmptyExtension> == Yuki::TypeOfClass::CodeExtension);
    STATIC_REQUIRE(Yuki::ClassTypeOf<StatefulExtensionLeaf> == Yuki::TypeOfClass::DataExtension);
    STATIC_REQUIRE(Yuki::CodeExtensionClass<EmptyExtension>);
    STATIC_REQUIRE(Yuki::DataExtensionClass<StatefulExtensionLeaf>);
    STATIC_REQUIRE(Yuki::InterfaceClass<ITransform>);
    STATIC_REQUIRE(Yuki::ImplementationClass<Rigged>);
}

TEST_CASE("BaseUnknown remains compact and uses only single non-virtual inheritance", "[Yuki][Core]") {
    STATIC_REQUIRE(sizeof(Yuki::BaseUnknown) == 2 * sizeof(void*));
    STATIC_REQUIRE(alignof(Yuki::BaseUnknown) >= 16);
    STATIC_REQUIRE(std::has_virtual_destructor_v<Yuki::BaseUnknown>);
    STATIC_REQUIRE_FALSE(std::is_destructible_v<Yuki::BaseUnknown>);
    STATIC_REQUIRE(Yuki::DirectCppBaseCountOf<Rigged> == 1);
    STATIC_REQUIRE(Yuki::DirectCppBaseCountOf<RigidBody> == 1);
    STATIC_REQUIRE(Yuki::DirectCppBaseCountOf<AuditTie> == 1);
}

TEST_CASE("Static metaclasses keep implementation, extension, and TIE chains", "[Yuki][Core]") {
    const auto& entity = Yuki::StaticMetaClass<Entity>();
    const auto& spatial = Yuki::StaticMetaClass<Spatial>();
    const auto& drawable = Yuki::StaticMetaClass<Drawable>();
    const auto& mesh = Yuki::StaticMetaClass<Mesh>();
    const auto& rigged = Yuki::StaticMetaClass<Rigged>();
    const auto& physics = Yuki::StaticMetaClass<PhysicsExtension>();
    const auto& rigid = Yuki::StaticMetaClass<RigidBody>();
    const auto& audit = Yuki::StaticMetaClass<AuditTie>();

    REQUIRE_FALSE(entity.HasDirectBase());
    REQUIRE(spatial.DirectBase() == &entity);
    REQUIRE(drawable.DirectBase() == &spatial);
    REQUIRE(mesh.DirectBase() == &drawable);
    REQUIRE(rigged.DirectBase() == &mesh);
    REQUIRE(rigid.DirectBase() == &physics);
    REQUIRE(audit.GetTypeOfClass() == Yuki::TypeOfClass::TIEchain);
    REQUIRE(entity.Provides().size() == 2);
    REQUIRE(mesh.Provides().size() == 2);
    REQUIRE(rigged.Provides().size() == 1);
    REQUIRE(physics.Extendees().size() == 2);
    REQUIRE(rigid.Extendees().size() == 1);
    REQUIRE(rigid.Extendees()[0] == Yuki::IidOf<Mesh>());
}

TEST_CASE("QueryInterface resolves concrete classes and inherited implementation providers", "[Yuki][Core]") {
    auto node = Yuki::MakeOwned<Rigged>(7, 11, 13, 3, 404, 5);

    auto object = Yuki::QueryInterface<IObject>(node);
    auto transform = Yuki::QueryInterface<ITransform>(node);
    auto renderable = Yuki::QueryInterface<IRenderable>(node);
    auto bounds = Yuki::QueryInterface<IBounds>(node);
    auto material = Yuki::QueryInterface<IMaterial>(node);
    auto animation = Yuki::QueryInterface<IAnimation>(node);
    auto diagnostics = Yuki::QueryInterface<IDiagnostics>(node);
    auto mesh = Yuki::QueryInterface<Mesh>(node);
    auto spatial = Yuki::QueryInterface<Spatial>(node);

    REQUIRE(object->ObjectId() == 7);
    REQUIRE(transform->X() == 11);
    REQUIRE(transform->Y() == 13);
    REQUIRE(renderable->RenderLayer() == 3);
    REQUIRE(bounds->BoundsVersion() == 107);
    REQUIRE(material->MaterialCode() == 404);
    REQUIRE(animation->ClipCount() == 5);
    REQUIRE(std::string_view{diagnostics->DiagnosticName()} == "Entity");
    REQUIRE(mesh.Get() == static_cast<Mesh*>(node.Get()));
    REQUIRE(spatial.Get() == static_cast<Spatial*>(node.Get()));
    REQUIRE(Yuki::ProviderClass(node.Get(), Yuki::IidOf<ITransform>()) == &Yuki::StaticMetaClass<Spatial>());
    REQUIRE(Yuki::ProviderClass(node.Get(), Yuki::IidOf<IAnimation>()) == &Yuki::StaticMetaClass<Rigged>());
    REQUIRE(Yuki::Provides(node.Get(), Yuki::IidOf<Mesh>()));
}

TEST_CASE("Inline facets are anchored to the nucleus without becoming bound facets", "[Yuki][Core]") {
    auto node = Yuki::MakeOwned<Rigged>(9, 2, 4, 1, 12, 6);

    auto transform = Yuki::QueryInterface<ITransform>(node);
    auto material = Yuki::QueryInterface<IMaterial>(node);

    REQUIRE(transform.Anchor() == node.Get());
    REQUIRE(material.Anchor() == node.Get());
    REQUIRE(transform.Get()->InlineFacetAnchor() == node.Get());
    REQUIRE(material.Get()->InlineFacetAnchor() == node.Get());
    REQUIRE(transform.Get()->BoundTarget() == nullptr);
    REQUIRE(material.Get()->BoundTarget() == nullptr);
    REQUIRE(Yuki::BoundFacets(node.Get()).Empty());
}

TEST_CASE("Attached extensions are Extends-checked and override lower-priority providers", "[Yuki][Core]") {
    auto node = Yuki::MakeOwned<Rigged>(10, 1, 2, 8, 100, 2);

    Yuki::AttachExtension(node.Get(), Yuki::MakeOwned<BoundsOverride>(900));
    Yuki::AttachExtension(node.Get(), Yuki::MakeOwned<MaterialOverride>(777));
    Yuki::AttachExtension(node.Get(), Yuki::MakeOwned<RigidBody>(250));

    auto bounds = Yuki::QueryInterface<IBounds>(node);
    auto material = Yuki::QueryInterface<IMaterial>(node);
    auto physics = Yuki::QueryInterface<IPhysics>(node);
    auto rigid = Yuki::QueryInterface<RigidBody>(node);

    REQUIRE(bounds->BoundsVersion() == 900);
    REQUIRE(material->MaterialCode() == 777);
    REQUIRE(physics->MassTimes100() == 250);
    REQUIRE(rigid);
    REQUIRE(Yuki::Extensions(node.Get()).Size() == 3);
    REQUIRE(Yuki::ProviderClass(node.Get(), Yuki::IidOf<IBounds>()) == &Yuki::StaticMetaClass<BoundsOverride>());
}

TEST_CASE("Invalid extension targets are rejected without consuming caller ownership", "[Yuki][Core]") {
    std::atomic<int> destroyed{0};
    auto node = Yuki::MakeOwned<Rigged>(1, 0, 0, 0, 0, 0);
    auto invalid = Yuki::MakeOwned<InvalidLightExtension>(destroyed);

    Yuki::AttachExtension(node.Get(), std::move(invalid));

    REQUIRE(invalid);
    REQUIRE(Yuki::Extensions(node.Get()).Empty());
    REQUIRE_FALSE(Yuki::QueryInterface<IUnused>(node));
    invalid = nullptr;
    REQUIRE(destroyed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Bound facets are closure-owned providers and keep separate TIE semantics", "[Yuki][Core]") {
    auto node = Yuki::MakeOwned<Rigged>(4, 0, 0, 0, 0, 0);
    auto diagnosticsTie = Yuki::MakeOwned<DiagnosticsTie>();
    auto auditTie = Yuki::MakeOwned<AuditTie>();

    Yuki::AttachBoundFacet(node.Get(), std::move(diagnosticsTie));
    Yuki::AttachBoundFacet(node.Get(), std::move(auditTie));

    auto diagnostics = Yuki::QueryInterface<IDiagnostics>(node);
    auto audit = Yuki::QueryInterface<IAudit>(node);
    auto tie = Yuki::QueryInterface<DiagnosticsTie>(node);

    REQUIRE(std::string_view{diagnostics->DiagnosticName()} == "DiagnosticsTie");
    REQUIRE(audit->AuditCode() == 700);
    REQUIRE(tie->BoundTarget() == node.Get());
    REQUIRE(diagnostics.Get()->InlineFacetAnchor() == node.Get());
    REQUIRE(Yuki::BoundFacets(node.Get()).Size() == 2);
}

TEST_CASE("ComPtr adopts through static and runtime compatibility", "[Yuki][Core]") {
    auto node = Yuki::MakeOwned<Rigged>(12, 3, 5, 9, 888, 4);
    Yuki::ComPtr<Yuki::BaseUnknown> base = node;

    REQUIRE(node->StrongCountForDebug() == 2);

    Yuki::BaseUnknown* rawForInterface = node.Detach();
    auto transform = Yuki::ComPtr<ITransform>::Adopt(rawForInterface);

    REQUIRE(transform);
    REQUIRE(transform.Anchor() == rawForInterface);
    REQUIRE(transform->X() == 3);

    Yuki::BaseUnknown* rawForConcrete = transform.Detach();
    auto mesh = Yuki::ComPtr<Mesh>::Adopt(rawForConcrete);

    REQUIRE(mesh);
    REQUIRE(mesh.Anchor() == rawForInterface);
    REQUIRE(mesh->Facet<IMaterial>()->MaterialCode() == 888);
}

TEST_CASE("ComPtr Adopt consumes incompatible runtime pointers", "[Yuki][Core]") {
    std::atomic<int> destroyed{0};
    auto extension = Yuki::MakeOwned<CountingPhysics>(destroyed);
    Yuki::BaseUnknown* raw = extension.Detach();

    auto missing = Yuki::ComPtr<IUnused>::Adopt(raw);

    REQUIRE_FALSE(missing);
    REQUIRE(destroyed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Closure-owned nodes release storage with the nucleus", "[Yuki][Core]") {
    std::atomic<int> extensionDestroyed{0};
    std::atomic<int> tieDestroyed{0};
    Yuki::WeakRef weak;

    {
        auto node = Yuki::MakeOwned<Rigged>(30, 0, 0, 0, 0, 0);
        weak = node->GetComponentWeakRef();
        Yuki::AttachExtension(node.Get(), Yuki::MakeOwned<CountingPhysics>(extensionDestroyed));
        Yuki::AttachBoundFacet(node.Get(), Yuki::MakeOwned<AuditTie>(&tieDestroyed));

        REQUIRE(weak.Get() == node.Get());
        REQUIRE(extensionDestroyed.load(std::memory_order_relaxed) == 0);
        REQUIRE(tieDestroyed.load(std::memory_order_relaxed) == 0);
    }

    REQUIRE(weak.Expired());
    REQUIRE(extensionDestroyed.load(std::memory_order_relaxed) == 1);
    REQUIRE(tieDestroyed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Dictionary stores metaclasses, inherited bases, and section records", "[Yuki][Core]") {
    Yuki::Dictionary dictionary;
    const auto& entity = Yuki::StaticMetaClass<Entity>();
    const auto& spatial = Yuki::StaticMetaClass<Spatial>();
    const auto& drawable = Yuki::StaticMetaClass<Drawable>();
    const auto& mesh = Yuki::StaticMetaClass<Mesh>();
    const auto& rigged = Yuki::StaticMetaClass<Rigged>();

    REQUIRE(dictionary.RegisterMetaClass(entity).has_value());
    REQUIRE(dictionary.RegisterMetaClass(spatial).has_value());
    REQUIRE(dictionary.RegisterMetaClass(drawable).has_value());
    REQUIRE(dictionary.RegisterMetaClass(mesh).has_value());
    REQUIRE(dictionary.RegisterMetaClass(rigged).has_value());
    REQUIRE(dictionary.IsAKindOf(rigged.IidValue(), entity.IidValue()));

    Yuki::ProviderEntry entry{Yuki::IidOf<IObject>(), Yuki::DispatchKind::InlineFacet, nullptr, &entity, 2};
    REQUIRE(dictionary.RegisterProvide(entity.IidValue(), Yuki::IidOf<IObject>(), entry).has_value());
    REQUIRE(dictionary.FindProvideInClassChain(rigged.IidValue(), Yuki::IidOf<IObject>()).has_value());

    REQUIRE(dictionary.RegisterSectionRecord(kSectionMetaRecord).has_value());
    REQUIRE(dictionary.RegisterSectionRecord(kSectionProvideRecord).has_value());
    REQUIRE(dictionary.FindMetaClass(Yuki::IidOf<SectionComponent>()) != nullptr);
    REQUIRE(dictionary.FindProvide(Yuki::IidOf<SectionComponent>(), Yuki::IidOf<IObject>()).has_value());
}

TEST_CASE("Dictionary decodes static module images without loading DLLs", "[Yuki][Core]") {
    constexpr std::uint32_t kNameOffset = 1;
    constexpr Yuki::CoreSectionMetaClassRecord namedRecord{
        Yuki::TypeOfClass::Implementation, Yuki::IidOf<SectionComponent>(), {}, kNameOffset};

    Yuki::ModuleImage image;
    image.path = L"T:/not-loaded/YukiSectionComponent.dll";
    image.sectionBytes.assign({std::byte{0}, std::byte{'S'}, std::byte{'e'}, std::byte{'c'}, std::byte{'t'},
                               std::byte{'i'}, std::byte{'o'}, std::byte{'n'}, std::byte{'C'}, std::byte{'o'},
                               std::byte{'m'}, std::byte{'p'}, std::byte{'o'}, std::byte{'n'}, std::byte{'e'},
                               std::byte{'n'}, std::byte{'t'}, std::byte{0}});
    image.metaClasses.push_back(namedRecord);
    image.provides.push_back(kSectionProvideRecord);

    Yuki::Dictionary dictionary;

    REQUIRE(dictionary.RegisterModuleImage(image).has_value());
    const Yuki::MetaClass* meta = dictionary.FindMetaClass(Yuki::IidOf<SectionComponent>());
    REQUIRE(meta != nullptr);
    REQUIRE(meta->ClassName() == "SectionComponent");
    REQUIRE(dictionary.Modules().Size() == 1);
}

TEST_CASE("Dictionary reports missing DLLs during static module scan", "[Yuki][Core]") {
    Yuki::Dictionary dictionary;
    auto image = dictionary.ScanModuleFile(std::filesystem::path{L"T:/definitely-missing-yuki-core.dll"});

    REQUIRE_FALSE(image.has_value());
    REQUIRE(image.error() == Yuki::CoreErrc::PathNotFound);
}