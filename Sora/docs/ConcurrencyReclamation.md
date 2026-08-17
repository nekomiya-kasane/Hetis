# Sora stamped publication and safe reclamation

ABA identity and object lifetime are related but distinct. `StampedValue.h` assigns a bounded publication identity to
one atomic word. The safe-reclamation headers determine when an unlinked object may be destroyed. Neither layer makes
an arbitrary data structure linearizable; the data structure still owns its linearization point and ownership transfer.

## Abstraction model

```mermaid
flowchart TD
    U["Concurrent data structure"] --> P["std::atomic<T*> / AtomicStampedValue"]
    P --> O["ProtectedPointer / ProtectedStampedPointer"]
    O --> H["HazardDomain::HazardPointer"]
    O --> E["EpochDomain::ReadGuard"]
    O --> R["RcuDomain::ReadGuard"]
    U --> Q["QsbrDomain::Participant"]
    H --> L["Retired batches"]
    E --> L
    Q --> L
    R --> L
    P --> V["TaggedValue / VersionedValue"]
    V --> PC["PointerLowBitsCodec"]
    V --> IC["BoundedUnsignedCodec"]
```

The layers deliberately do not inherit semantic meaning from their representation:

1. A **codec** maps a payload and stamp to disjoint fields in one canonical unsigned word. A pointer and a bounded
   unsigned value are peer codecs; a pointer is not modeled as an unsigned-integer subtype.
2. `TaggedValue` gives the application control of a tag. `VersionedValue` alone defines modulo successor semantics.
3. `AtomicStampedValue` publishes either semantic value through one always-lock-free atomic word.
4. A protection object owns the time interval in which dereference is valid.
5. A reclamation domain owns objects after `Retire` and invokes their deleters after its protocol proves safety.

The stamp is finite. After `2^N` changes an `N`-bit version wraps, so a versioned word prevents ABA only within that
reuse window. Allocation discipline or safe reclamation must make the window large enough for the data structure.
`PointerLowBitsCodec` additionally assumes the target ABI preserves a pointer converted through `uintptr_t` after its
stamp bits are cleared. This is a target-level representation contract, not a portable pointer-provenance theorem.

Epoch and QSBR generation comparisons use serial-number half-range ordering. If a participant or retired object lags
by at least half of the 64-bit range, collection conservatively retains ownership instead of guessing across wraparound;
the collector validates both participant age and each retired object's own age before reclaiming it.

## Protocol contracts

| Facility | Read-side semantic action | Reclaimer proof | Principal liveness cost |
| --- | --- | --- | --- |
| `HazardDomain` | holder publishes and validates one exact address | detached retire batch is absent from all scanned records | scan is linear in published records |
| `EpochDomain` | affine participant enters a nestable epoch region | object is two completed epoch transitions behind | a stalled reader delays advancement |
| `QsbrDomain` | affine participant reports that no older reference remains | every registered participant acknowledges a later generation | a participant that never reports blocks progress |
| `RcuDomain` | affine participant publishes one phase and a nesting depth | flip phase and wait for continuous regions begun in the old phase | an old-phase reader delays its grace period |

An affine participant may move between threads, but operations through it and its derived guards must not overlap
between threads. Hazard holders are independent move-only capabilities. Epoch and RCU read guards keep their stable
participant record alive after the registration handle is destroyed; release/acquire registration handoff and a counter
recheck prevent premature reuse.

RCU `Synchronize`/`Reclaim` must not run while the calling thread holds a guard from the same domain. QSBR
`Synchronize` must not be called by a thread owning a registered participant from that domain: because the call advances
the target generation itself, it would wait for a future report that the blocked caller cannot issue.

Hazard, epoch, and RCU protection directly accept `std::atomic<T*>`. They also accept `AtomicStampedValue` whose payload
is a pointer and return `ProtectedStampedPointer`, preserving both pointee lifetime and the exact identity required by a
later CAS. QSBR deliberately has no scoped protection wrapper: its references remain valid only until that participant's
next quiescent-state report.

## Ordering and ownership

Hazard protection and unlink use sequentially consistent operations. The collector detaches its candidate batch before
scanning records; therefore an object retired during the scan cannot be reclaimed by that scan. The SC order ensures
that either validation observes the unlink or the scan observes the protection publication, without an additional
standalone fence. Each collector thread reuses its protected-address vector capacity across calls; recursive collection
is safe because deleters run only after the outer scan and partition have completed. The sorted address set is deduplicated
in place before candidate lookup, avoiding repeated comparisons when many holders protect the same object.

Top-level epoch and RCU entry first announce the candidate generation or phase, publish active state in the SC order,
and then validate the global state. A failed validation withdraws the announcement before any protected load. For RCU,
the reader's SC depth publication and validation, the reclaimer's SC phase flip, and its first SC depth observation share
one total order: validation before the flip makes the old-phase publication visible to the scan; a flip before validation
makes the reader retry in the new phase. Later polling needs only acquire because it observes exit, not entry.

Nested epoch and RCU entry increments the affine record's depth with a release store and inherits the outer announcement.
Overlapping RCU guards that span a flip intentionally form one continuous read-side interval, even when guards are
destroyed out of lexical order; only the final depth transition to zero ends that interval. Epoch and QSBR retirement
capture the immediate predecessor in the global atomic's modification order with one read-modify-write operation per
call or batch. If an RCU record exits an old interval and enters the new phase between the scanner's depth and
announcement observations, the release/acquire announcement handoff orders the completed old interval before the
scanner skips the new one.

Epoch uses the same release/acquire announcement handoff when a record turns over from an old interval to the current
generation during a scan. Epoch and QSBR retirement use a zero-increment release RMW: unlike an ordinary load, the RMW
returns the immediate modification-order predecessor and publishes the preceding unlink to a later advancing RMW.
QSBR's quiescent generation load is SC so a participant published after a grace-period membership scan cannot acknowledge
an older generation and then begin using pre-grace-period references.

An ordinary atomic load could legally observe an older generation and shorten the required grace interval. QSBR
membership and generation advance share the SC order so a collector cannot miss a pre-advance participant;
acknowledgements remain release/acquire boundaries around quiescence.

`Retire` transfers ownership to the domain only after successful admission. The default `Retire(T*)` overload requires a
nothrow-destructible `T`; its deleter is `std::default_delete<T>`. A custom deleter must be `noexcept`, transfer without
throwing, and fit `InlineFunction<void() noexcept, 32>`. Retirement storage is reserved before a callback captures the
pointer, so an allocation failure leaves the caller's pointer untouched and does not invoke its deleter. `RetiredObject`
is a move-only RAII owner whose destructor invokes any still-owned callback after admission, providing the final no-loss
rollback. Batch deleters must additionally copy without throwing because every object owns an independent callback. Every
non-null pointer is consumed exactly once after successful admission. Deleters run outside the lock and may re-enter the
domain. Deferred batches are reattached without allocation, so a collection failure cannot lose ownership. Domain
destruction requires all
participation and protection objects to have ended, then drains retained ownership.

## Performance and evidence boundary

Participant records are stable, reusable, and cache-line isolated. The registry allocates pages of 16 contiguous
records, then scans only each page's SC-published prefix; scans themselves allocate nothing. Retired objects use
domain-owned blocks of 64 intrusive nodes: a cold block expansion allocates, while warmed retirement reuses nodes and
detach/requeue remain O(1). The queue is mutex protected and makes no lock-free claim. `Retire(span)` acquires that mutex
once and captures one epoch or generation for the entire batch. Hazard collection allocates its sorted address set. A
measured `std::inplace_vector<void*, 64>` hybrid was not retained because it regressed the tested collection path;
availability in the active libc++ does not make it the right storage policy here.

The four domains require their record-state atomics to be `is_always_lock_free`; a target ABI that would silently
implement phase, generation, depth, address, or registration state with an internal lock is rejected at translation.

`Benchmark.Sora.Core.Reclamation` measures reusable-holder read cost, nested epoch/RCU cost, pointer versus versioned
CAS, reader scaling, allocation cost, cold and warmed retirement metadata, and hazard collection across record and batch
counts. A process pinned to one logical CPU is suitable for fixed-cost and scan measurements but invalidates the
multi-reader scaling section; scaling evidence needs distinct worker affinities and is not reported below.
Results apply only to the recorded compiler, CPU, optimization level, workload, and allocator. Competitor comparisons
require identical memory ordering, ownership protocol, pinning, warm-up, and correctness checks. TSan is intentionally
not part of the Sora validation matrix. Race-safety claims instead require an explicit happens-before and lifetime
argument, targeted concurrent stress, and complementary Debug, Release, ASan, and UBSan execution; sanitizer success
alone is never treated as a data-race proof.

## Reusable storage layers

The reclamation queue is composed from three policy-free layers rather than owning a retirement-specific topology:

1. `IntrusiveForwardList<Node, &Node::link>` supplies non-owning singly-linked topology. The pointer-to-member link
   is part of the type, so one node can participate in multiple independent lists without a hook base class or runtime
   dispatch. Detached `Segment` values transfer a complete chain in O(1).
2. `PagedNodePool<Node, &Node::link>` supplies stable-address, permanently constructed node slots in fixed-size pages.
   It is deliberately single-threaded and has no generation, identity, synchronization, or reclamation meaning; a
   caller that publishes addresses must provide those semantics separately.
3. `ConcurrentBatchQueue<T>` composes the topology and pool with one mutex protecting ownership transitions. Producers
   construct values under the mutex, `TakeAll` detaches in O(1), and consumers destroy or partition detached batches
   outside the mutex. `Detail::RetirementQueue` adds the retirement protocol, while `RetiredObject` itself retains the
   final callback ownership so exceptional queue paths cannot silently discard a retired object.

`ConcurrentBatchQueue::Entry` is an intentional public batch-node type: it allows a reclaimer to partition a detached
batch without type erasure or per-node callback state. A detached batch handle carries its source-queue identity, so
whole-batch `Append`, `Requeue`, and `Release` can diagnose a wrong destination in assertion-enabled builds. Nodes still
carry no owner token or generation, which keeps the per-node layout unchanged; extracting entries and mixing them across
queues remains outside the contract.
`EmplaceBatch` provides the basic guarantee: a successfully constructed prefix remains pending after an exception, the
current unconstructed node returns to the pool, and no constructed value is destroyed during exception propagation.

The 2026-08-16 Windows x64 Release run used COCA clang-p2996, `-O3 -march=haswell`, and process affinity mask `4`. The
table reports the median of five independently warmed runs and the full run-to-run range on the current workstation:

| Boundary | Median of runs | Range |
| --- | ---: | ---: |
| raw acquire load | 2.57 ns | 2.48-2.60 ns |
| reusable hazard protection | 8.07 ns | 7.94-8.15 ns |
| epoch protected observation | 12.55 ns | 12.28-12.73 ns |
| RCU protected observation | 12.49 ns | 12.20-12.62 ns |
| nested epoch protected observation | 2.57 ns | 2.48-2.61 ns |
| nested RCU protected observation | 2.56 ns | 2.51-2.65 ns |
| plain pointer CAS | 10.95 ns | 10.35-11.67 ns |
| manually packed pointer/version CAS | 11.97 ns | 11.48-12.26 ns |
| `AtomicVersionedPointer` CAS | 12.10 ns | 11.40-12.70 ns |
| `new Node` baseline | 36.39 ns/object | 36.16-40.09 ns/object |
| `delete Node` baseline | 17.27 ns/object | 16.80-17.92 ns/object |
| cold `Retire(span)`, 16,384 objects | 34.30 ns/object | 33.33-45.83 ns/object |
| warmed single-object `Retire`, 16,384 objects | 24.17 ns/object | 23.73-24.97 ns/object |
| warmed `Retire(span)`, 16,384 objects | 10.13 ns/object | 9.99-10.43 ns/object |
| hazard collect, 8 objects / 0 records | 12.98 ns/object | 12.78-13.68 ns/object |
| hazard collect, 8 objects / 64 records | 105.35 ns/object | 92.99-124.12 ns/object |
| hazard collect, 16,384 objects / 64 records | 15.48 ns/object | 14.69-20.01 ns/object |

The versioned wrapper did not measure slower than the same-semantics manually packed CAS in this run. Disassembly shows
one `lock cmpxchg` for both representations, supporting a zero-overhead representation claim on this target; timing
noise prevents interpreting their small difference as a speedup. Epoch and RCU exits and nested depth changes compile to
ordinary stores under their affine single-writer contract, while top-level entry retains the required SC `xchg`. The
nested measurements are consequently close to the raw-load floor. Paging reduced the same-mask 8-object/64-record
hazard scan from the previously recorded 313.23 ns/object single run to a 105.35 ns/object five-run median. Because the
old baseline had only one run, this is directional evidence rather than a controlled universal speedup ratio.

The portable hazard and RCU read paths use standard SC ordering. Folly can shift a reader barrier toward reclaimers
through platform-specific asymmetric fences, but its current Windows path falls back to an ordinary fence; Linux
sub-nanosecond reused-holder results are therefore not transferable to this Windows baseline.

## Primary references

- [P2530R3 - Hazard Pointers for C++26](https://wg21.link/P2530R3)
- [P2545R4 - Read-Copy-Update for C++26](https://wg21.link/P2545R4)
- [Folly Hazard Pointers](https://github.com/facebook/folly/blob/main/folly/synchronization/Hazptr.h)
- [Folly RCU](https://github.com/facebook/folly/blob/main/folly/synchronization/Rcu.h)
