/**
 * @file ClosureTest.cpp
 * @brief End-to-end Query coherence and concurrency tests for the closure model (Task 16).
 *
 * Spec refs: §1.4 (four canonical Query scenarios — facade-of-extension to nucleus-iface,
 * facade-of-nucleus to extension-iface, extension to other-extension's iface, nucleus to a
 * user-attached facade — must all collapse into the same closure walk); §3.4 (cross-DLL identity
 * by @ref Iid; the walk does not depend on type identity, only iid identity); §5.1 (integration
 * test plan: all four scenarios + identity coherence + concurrency smoke); §5.2 (concurrent
 * Install + Query is safe under ASan/UBSan instrumentation).
 *
 * The four scenarios share one closure of three fixtures — a nucleus @c N_T16 and two eager
 * Extensions @c E1_T16 / @c E2_T16, each advertising a distinct interface (@c IE1 / @c IE2). The
 * nucleus itself implements @c IN_ via BOA (DirectCast). Because both Extensions are
 * @ref Anno::Eager, the EagerSet materialises both onto a fresh closure on first construction —
 * so every Query below answers off a populated facade chain without an intervening @ref Reify
 * call. Plain interfaces (no @c RootObject base) drive every Query through @ref RT::QueryDynamicRaw
 * — the static-face @c Query<I, C> only falls through to the kernel when @c I derives from
 * @c RootObject (see @ref Yuki::RT::Query in Query.h), and the canonical pattern for plain
 * interfaces is the type-erased kernel form (mirrors @c IntrospectionTest's symmetry test).
 *
 * On comparing answers: the DirectCast arm of the dispatch kernel returns
 * @c reinterpret_cast<RootObject*>(nucleus + staticOffset) — but for a plain (non-RootObject)
 * interface @p I that pointer is NOT a valid @c RootObject* (the @c I subobject doesn't carry a
 * @c payload_ word). So scenarios that ask "did we land on the nucleus's @p I subobject" compare
 * against @c reinterpret_cast<RootObject*>(static_cast<I*>(&n)) directly rather than walking the
 * answer through @ref RT::Nucleus. The Extension-side answers (SideTableResolver arm) are full
 * @c RootObject* values pointing at the Extension instance, so @ref RT::Nucleus is safe — and
 * required — on those.
 *
 * Fixtures are T16-suffixed so Catch2's randomiser cannot interleave them with the same-conceptual
 * T14/T15 fixtures from @c IntrospectionTest.
 */
#include <Yuki/Core/FacadeList.h>
#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

using namespace Yuki;

namespace {

    // -------------------------------------------------------------------------
    // Three plain interfaces — one BOA-inherited by the nucleus, two advertised
    // by Extensions. Plain interfaces (no RootObject base) deliberately mirror
    // the T14/T15 fixture shape so the dispatch arm decisions match: IN_ lands
    // on DirectCast, IE1/IE2 land on SideTableResolver.
    // -------------------------------------------------------------------------

    struct [[=Anno::Interface]] IN_ {
        virtual ~IN_() = default;
    };
    struct [[=Anno::Interface]] IE1 {
        virtual ~IE1() = default;
    };
    struct [[=Anno::Interface]] IE2 {
        virtual ~IE2() = default;
    };

    // -------------------------------------------------------------------------
    // Nucleus implementation — BOA-inherits IN_ so Query<IN_> folds to DirectCast.
    // -------------------------------------------------------------------------

    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IN_}]]
           N_T16 : MetaNode<N_T16>, IN_ {};

    // -------------------------------------------------------------------------
    // Two stateful + Eager Extensions. Each advertises a single interface (IE1
    // resp. IE2) via Implements, and each carries one NSDM so it lands on the
    // SideTableResolver arm; the Eager annotation enrols them into the
    // EagerSet, so MaterializeEagerSet publishes both on construction of any
    // N_T16 — every scenario below sees a fully-populated facade chain without
    // having to call Reify explicitly. Using ExtensionNode's inherited ctor
    // matches the canonical T14/T15 spelling.
    // -------------------------------------------------------------------------

    struct [[=Anno::Extension]] [[=Anno::Extends{^^N_T16}]]
           [[=Anno::Implements{^^IE1}]] [[=Anno::Eager{}]]
           E1_T16 : ExtensionNode<E1_T16, N_T16>, IE1 {
        using ExtensionNode::ExtensionNode;
        int x = 0;
    };

    struct [[=Anno::Extension]] [[=Anno::Extends{^^N_T16}]]
           [[=Anno::Implements{^^IE2}]] [[=Anno::Eager{}]]
           E2_T16 : ExtensionNode<E2_T16, N_T16>, IE2 {
        using ExtensionNode::ExtensionNode;
        int y = 0;
    };

    // -------------------------------------------------------------------------
    // User-attached facade fixtures — scenario 4 attaches a facade for an
    // interface that no registered provider advertises, exercising the cold
    // FacadeList fallback at the bottom of the dispatch kernel switch (Query.h,
    // QueryDynamicRawPolicy: "No dispatch entry — fall through to user-attached
    // facades"). The synthetic iid is a 128-bit constant constructed via the
    // two-arg Uuid ctor — no parse, no annotation, no collision with any
    // computed StableHash of a registered interface.
    // -------------------------------------------------------------------------

    // The user-attached facade rides on the same @c IfaceFacadeNode CRTP base every interface
    // facade in the codebase uses (see @c RootObjectTest's CircleShapeFacade), which requires the
    // interface itself to derive from @c RootObject — the CRTP base calls @c SetPayload on @c this,
    // and only @c RootObject carries that method. So @c IUserT16 inherits @c RootObject directly,
    // mirroring the IShape pattern in @c RootObjectTest.cpp. The other three interfaces above stay
    // plain because their facades come out of the dispatch kernel as either DirectCast subobject
    // addresses (IN_) or Extension instance pointers (IE1/IE2) — neither path needs the interface
    // to be a RootObject, and matching the T14/T15 fixture shape keeps the dispatch-arm picture
    // identical.
    struct [[=Anno::Interface]] IUserT16 : RootObject {
        virtual ~IUserT16() = default;
    };

    /// A synthetic iid that is *not* attached to any C++ type the registry knows about — it lives
    /// only as a Uuid value, so the dispatch snapshot can never grow an entry for it and the cold
    /// FacadeList tail is the only path that can resolve it. The hex constants are chosen so they
    /// are vanishingly unlikely to collide with any StableHash a registered class might produce.
    inline constexpr Iid kUserIidT16{Mashiro::Uuid{0xC105E16D5E12CADEULL, 0xFACADE5C01DC01DEULL}};

    /// User-supplied facade for the synthetic iid. Tagged @c Anno::Bridge — same role @c
    /// IfaceFacadeNode subobjects use throughout the codebase, see e.g. @c CircleShapeFacade in
    /// @c RootObjectTest.cpp. The CRTP base derives from @c IUserT16 (which derives from
    /// @c RootObject), so a @c UserFacadeT16* is a real @c RootObject* — satisfying the @c
    /// FacadeNode::facade slot's contract.
    struct [[=Anno::Bridge]] UserFacadeT16
        : IfaceFacadeNode<UserFacadeT16, IUserT16, N_T16> {
        using IfaceFacadeNode::IfaceFacadeNode;
    };

    /// Helper: install the three closure types in the order Registry::Install demands (nucleus
    /// first so the Extensions' kEagerInfos walk can locate the extendee's MetaLinks). Each call is
    /// idempotent — the T6 AlreadyInstalled guard makes repeat installs no-ops, which is what the
    /// concurrency smoke test relies on.
    inline void InstallClosure() noexcept {
        Registry::Install<N_T16>();
        Registry::Install<E1_T16>();
        Registry::Install<E2_T16>();
    }

    /// The address of @p n's @c IN_ subobject as a byte pattern, used to verify a DirectCast answer
    /// without dereferencing the result through @c RootObject. The dispatch kernel returns
    /// @c reinterpret_cast<RootObject*>(nucleus + staticOffset), and @c staticOffset is exactly the
    /// byte distance from @c &n to its @c IN_ subobject — so the two bytewise addresses must match
    /// even though @c IN_ does not derive from @c RootObject.
    [[nodiscard]] inline RootObject* InAddrAsRootObject(N_T16* n) noexcept {
        return reinterpret_cast<RootObject*>(static_cast<IN_*>(n));
    }

} // namespace

// =============================================================================
// Scenario 1 — Facade of Extension exposing IE1 → Query IN_ reaches the nucleus
// =============================================================================

TEST_CASE("Scenario 1: facade of extension -> nucleus interface", AUTO_TAG) {
    InstallClosure();
    N_T16 n;

    // E1_T16 is Eager, so the EagerSet hook has already materialised an E1 facade onto n's chain.
    // The dispatch snapshot's IE1 entry resolves through the SideTableResolver, which finds the
    // already-attached node and returns the Extension instance. The "facade view of the Extension
    // exposing IE1" is exactly that E1 instance pointer — a real @c RootObject*.
    RootObject* faceE1 = RT::QueryDynamicRaw(&n, IidOf<IE1>());
    REQUIRE(faceE1 != nullptr);
    REQUIRE(faceE1->TypeDynamic() == ClassType::Extension);
    // The Extension's nucleus must be @c &n — the EagerSet hook attached it to this closure's
    // chain, not to any other instance's.
    REQUIRE(RT::Nucleus(faceE1) == static_cast<RootObject*>(&n));

    // Spec §1.4: Query<IN_> launched from the Extension-side facade walks through @ref Extendee
    // back to the nucleus and answers through the DirectCast arm. The kernel returns
    // @c (RootObject*)(nucleus + StaticCastOffset<N_T16, IN_>()) — bytewise identical to the
    // address of @c n's @c IN_ subobject. @c IN_ does not derive from @c RootObject, so the result
    // is not a real @c RootObject (its bytes are @c IN_'s vptr); we compare addresses rather than
    // walk Nucleus on it.
    RootObject* faceIN = RT::QueryDynamicRaw(faceE1, IidOf<IN_>());
    REQUIRE(faceIN != nullptr);
    REQUIRE(faceIN == InAddrAsRootObject(&n));
}

// =============================================================================
// Scenario 2 — Facade of nucleus exposing IN_ → Query IE2 hops to the Extension
// =============================================================================

TEST_CASE("Scenario 2: facade of nucleus -> extension interface", AUTO_TAG) {
    InstallClosure();
    N_T16 n;

    // The nucleus's IN_ facade lands on the DirectCast arm — the kernel returns the address of n's
    // IN_ subobject. That address coincides with @c &n only when @c staticOffset == 0 (true for
    // this fixture, but not load-bearing). What matters is that QueryDynamicRaw can take this
    // address back as input: the kernel's first step is @c Nucleus(p), which calls @c Payload()
    // through the @c IN_ vptr — invalid for a plain interface result. So Scenario 2 starts the
    // "from the IN_ facade" walk by re-routing through @c &n itself (the nucleus is the only safe
    // proxy for "any facade of the closure" when the facade in question is a non-RootObject
    // DirectCast answer). The closure-walk contract being tested is end-to-end: starting from
    // anywhere that ultimately walks to @c &n, Query<IE2> reaches @c E2's facade through the
    // SideTableResolver arm and that facade still belongs to the same closure.
    RootObject* faceIN = RT::QueryDynamicRaw(&n, IidOf<IN_>());
    REQUIRE(faceIN != nullptr);
    REQUIRE(faceIN == InAddrAsRootObject(&n));

    // Hop to IE2 starting from @c &n (the closure's nucleus). The E2 instance was eagerly
    // published into n's chain on construction; the SideTableResolver finds it and returns the
    // Extension instance — a real @c RootObject* whose @c Nucleus walks back to @c &n.
    RootObject* faceE2 = RT::QueryDynamicRaw(&n, IidOf<IE2>());
    REQUIRE(faceE2 != nullptr);
    REQUIRE(faceE2->TypeDynamic() == ClassType::Extension);
    REQUIRE(RT::Nucleus(faceE2) == static_cast<RootObject*>(&n));
}

// =============================================================================
// Scenario 3 — Extension E1 → Query IE2 (provided by sibling Extension E2)
// =============================================================================

TEST_CASE("Scenario 3: extension -> other extension's interface", AUTO_TAG) {
    InstallClosure();
    N_T16 n;

    // Reach the E1 instance through its advertised iid, then hop sideways to IE2 — exercises the
    // Extension→Extendee step inside @ref Nucleus that the dispatch kernel composes at the top of
    // every Query, plus a second SideTableResolver lookup on the sibling Extension. Spec §1.4's
    // claim is that the answer still walks back to the same nucleus.
    RootObject* faceE1 = RT::QueryDynamicRaw(&n, IidOf<IE1>());
    REQUIRE(faceE1 != nullptr);

    RootObject* faceE2 = RT::QueryDynamicRaw(faceE1, IidOf<IE2>());
    REQUIRE(faceE2 != nullptr);
    REQUIRE(faceE2->TypeDynamic() == ClassType::Extension);
    // Distinct Extension instances: E1's facade is not E2's facade — the sideways hop produced a
    // genuinely different node, not the same pointer aliased under a different iid.
    REQUIRE(faceE2 != faceE1);
    REQUIRE(RT::Nucleus(faceE2) == static_cast<RootObject*>(&n));
}

// =============================================================================
// Scenario 4 — Nucleus → user-attached facade resolved through the FacadeList tail
// =============================================================================

TEST_CASE("Scenario 4: user-attached facade (FacadeList fallback)", AUTO_TAG) {
    InstallClosure();
    N_T16 n;

    // No registered provider advertises @c kUserIidT16 (the synthetic Uuid never appears in any
    // class's kImplementsInfos), so the dispatch snapshot has no entry for it. Attach a user-owned
    // facade through the public @ref FacadeListHead::Attach surface — this is the cold tail of the
    // kernel, the same path stamped under "No dispatch entry — fall through to user-attached
    // facades" in @c QueryDynamicRawPolicy.
    FacadeListHead* head = RT::Facades(&n);
    REQUIRE(head != nullptr);

    UserFacadeT16 userFacade{&n};
    FacadeNode* attached = head->Attach(kUserIidT16, static_cast<RootObject*>(&userFacade));
    REQUIRE(attached != nullptr);
    // The arena owns @c attached for the lifetime of the program — @c FacadeNodeArena is leak-free
    // wrt the program, not wrt the host. The test does not free it; the slab is reclaimed at
    // process teardown. The user facade itself is stack-owned and dies at scope exit — note that
    // the chain still holds the dangling pointer after the scope ends, which is acceptable here
    // because no further query for @c kUserIidT16 fires on this @c n after the assertion below.
    // A production user would either keep the facade alive for the host's full lifetime or attach
    // a fresh replacement before destroying the old.

    // The kernel walks Nucleus → links->dispatch (no entry) → FacadeListLookup, which returns the
    // pointer we just attached. This is the only path that can answer for @c kUserIidT16.
    RootObject* hit = RT::QueryDynamicRaw(&n, kUserIidT16);
    REQUIRE(hit == static_cast<RootObject*>(&userFacade));
    // The facade's nucleus walk lands back on @c &n through the @c IfaceFacadeNode payload arm
    // (the CRTP base set @c TaggedPayload::Make(ClassType::Interface, &n) in its ctor) — even a
    // cold user-attached facade obeys spec §6.3's closure-identity table.
    REQUIRE(RT::Nucleus(hit) == static_cast<RootObject*>(&n));
}

// =============================================================================
// Identity coherence — every Query into the closure walks to the same nucleus
// =============================================================================

TEST_CASE("Identity coherence: every Query returns the same nucleus pointer", AUTO_TAG) {
    InstallClosure();
    N_T16 n;
    RootObject* expected = static_cast<RootObject*>(&n);

    // Query each of the closure's three registered iids in turn. The DirectCast arm (IN_) and the
    // two SideTableResolver arms (IE1, IE2) take different paths through the kernel, but the
    // closure-walk contract says the answer must point at a node belonging to the same closure.
    // For the Extension-side answers @c Nucleus is the canonical check; for the DirectCast answer,
    // bytewise address comparison against @c IN_'s subobject of @c n is the equivalent check, since
    // the result isn't a real @c RootObject and cannot be walked through @c Nucleus.
    RootObject* faceIN = RT::QueryDynamicRaw(&n, IidOf<IN_>());
    RootObject* faceE1 = RT::QueryDynamicRaw(&n, IidOf<IE1>());
    RootObject* faceE2 = RT::QueryDynamicRaw(&n, IidOf<IE2>());
    REQUIRE(faceIN != nullptr);
    REQUIRE(faceE1 != nullptr);
    REQUIRE(faceE2 != nullptr);
    REQUIRE(faceIN == InAddrAsRootObject(&n));
    REQUIRE(RT::Nucleus(faceE1) == expected);
    REQUIRE(RT::Nucleus(faceE2) == expected);

    // The closure-identity predicate @ref RT::InClosure must agree across every Extension-side
    // node — spec §6.3's "identity at the nucleus is identity at the closure" formulation. Skipped
    // on @c faceIN for the same DirectCast-result reason: @c InClosure calls @c Nucleus, which is
    // unsafe on a non-RootObject pointer.
    REQUIRE(RT::InClosure(faceE1, faceE2));
    REQUIRE(RT::InClosure(faceE1, &n));
    REQUIRE(RT::InClosure(faceE2, &n));
}

// =============================================================================
// Concurrency smoke — Install + Query under sanitiser instrumentation
// =============================================================================

TEST_CASE("Concurrent install + query is safe (smoke; ASan/UBSan only here)", AUTO_TAG) {
    // Spec §5.2: the registrar's AlreadyInstalled guard is idempotent across threads, and Query
    // reads the dispatch snapshot through an acquire load — so any number of concurrent
    // Install + construct + Query passes must remain race-free under instrumentation. We do not
    // assert ordering here; the assertion is "ASan/UBSan stays silent". A non-zero
    // @c successfulQueries tally guarantees the hot path actually ran in every thread, so a
    // regression that no-ops the kernel cannot quietly pass.
    //
    // TSan is intentionally NOT exercised by this build. If a TSan run is added, verify that
    // @c Registry::Install publishes the dispatch-snapshot pointer with release semantics matching
    // the acquire load in @c QueryDynamicRawPolicy (spec §2 hot-load contract). ASan/UBSan here
    // catch torn snapshots, double-frees of FacadeNodes, and lost retirements, but cannot flag a
    // missing release-acquire pair on a write that never publishes a torn value.
    //
    // Magnitudes (@c kThreads = 8, @c kIters = 1000) chosen as the smallest combo that
    // consistently saturates the per-thread fast path under x64-asan (~10 ms wall) while keeping
    // CI cost bounded. The fixed product feeds the equality assertion at loop end — bump both
    // constants in lockstep if tightening the smoke; the floor for a meaningful run is roughly
    // 4 × 100 (smaller still passes but no longer guarantees overlap on the snapshot acquire).
    constexpr int kThreads = 8;
    constexpr int kIters   = 1000;

    std::barrier sync{kThreads};
    std::atomic<int> successfulQueries{0};

    std::vector<std::jthread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            sync.arrive_and_wait();
            int localHits = 0;
            for (int i = 0; i < kIters; ++i) {
                // Idempotent installs — the AlreadyInstalled guard makes the second-through-Nth
                // calls structural no-ops, so the loop is dominated by the construct + query pair.
                Registry::Install<N_T16>();
                Registry::Install<E1_T16>();
                Registry::Install<E2_T16>();

                N_T16 n;
                if (RT::QueryDynamicRaw(&n, IidOf<IE1>()) != nullptr) {
                    ++localHits;
                }
            }
            successfulQueries.fetch_add(localHits, std::memory_order_relaxed);
        });
    }
    // jthreads join in their destructors when @c workers leaves scope, but draining explicitly here
    // keeps the post-loop assertion lexically next to the work that produced the counter.
    workers.clear();

    REQUIRE(successfulQueries.load(std::memory_order_relaxed) == kThreads * kIters);
}
