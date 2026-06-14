/**
 * @file SeqLockTest.cpp
 * @brief Comprehensive tests for SeqLock: single-thread correctness, value/struct
 *        round-trips, read-modify-write, diagnostics, cross-thread torn-read
 *        detection under contention, and C++26 reflection layout audits.
 */
#include "Mashiro/Core/SeqLock.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace Mashiro;

// =============================================================================
// Helper payload types (namespace scope so they are reflectable)
// =============================================================================

namespace {

	/// @brief Two words with the invariant `b == ~a`; any torn read breaks it.
	struct Paired {
		std::uint64_t a;
		std::uint64_t b;
	};

	/// @brief 64-byte payload whose 16 words must all be equal; spans the counter line.
	struct Wide {
		std::uint32_t v[16];
	};

	/// @brief 256-byte payload guaranteed to span multiple cache lines.
	struct Huge256 {
		std::uint8_t bytes[256];
	};

	/// @brief Mixed POD with padding to exercise byte-exact snapshotting.
	struct Pod {
		int a;
		float b;
		char c[3];
		double d;
	};

	static_assert(std::is_trivially_copyable_v<Paired>);
	static_assert(std::is_trivially_copyable_v<Wide>);
	static_assert(std::is_trivially_copyable_v<Huge256>);
	static_assert(std::is_trivially_copyable_v<Pod>);

} // anonymous namespace

// =============================================================================
// [SeqLock] — Single-thread correctness
// =============================================================================

TEST_CASE("SeqLock: default-constructed value is value-initialised", AUTO_TAG) {
	SeqLock<std::uint64_t> lock;
	REQUIRE(lock.Read() == 0);
	REQUIRE_FALSE(lock.WriteInProgress());
}

TEST_CASE("SeqLock: initial-value constructor publishes the value", AUTO_TAG) {
	SeqLock<int> lock(42);
	REQUIRE(lock.Read() == 42);
}

TEST_CASE("SeqLock: write then read returns the written value", AUTO_TAG) {
	SeqLock<int> lock;
	lock.Write(7);
	REQUIRE(lock.Read() == 7);
	lock.Write(-123);
	REQUIRE(lock.Read() == -123);
}

TEST_CASE("SeqLock: Write accepts both lvalues and rvalues", AUTO_TAG) {
	SeqLock<int> lock;
	int lvalue = 99;
	lock.Write(lvalue);        // lvalue (was rejected by the old T&& signature)
	REQUIRE(lock.Read() == 99);
	lock.Write(int{17});       // rvalue
	REQUIRE(lock.Read() == 17);
}

TEST_CASE("SeqLock: sequence stays even and advances by 2 per write", AUTO_TAG) {
	SeqLock<int> lock;
	const auto s0 = lock.Sequence();
	REQUIRE((s0 % 2) == 0);
	lock.Write(1);
	const auto s1 = lock.Sequence();
	REQUIRE((s1 % 2) == 0);
	REQUIRE(s1 == static_cast<SeqLock<int>::SequenceType>(s0 + 2));
	REQUIRE_FALSE(lock.WriteInProgress());
}

// =============================================================================
// [SeqLock] — Struct payloads (byte-exact round-trip)
// =============================================================================

TEST_CASE("SeqLock: struct payload round-trips field-by-field", AUTO_TAG) {
	SeqLock<Pod> lock;
	Pod w{-7, 3.5f, {'x', 'y', 'z'}, 2.718281828};
	lock.Write(w);
	Pod r = lock.Read();
	REQUIRE(r.a == w.a);
	REQUIRE(r.b == w.b);
	REQUIRE(r.c[0] == w.c[0]);
	REQUIRE(r.c[1] == w.c[1]);
	REQUIRE(r.c[2] == w.c[2]);
	REQUIRE(r.d == w.d);
}

TEST_CASE("SeqLock: wide payload round-trips exactly", AUTO_TAG) {
	SeqLock<Wide> lock;
	Wide w{};
	for (std::uint32_t i = 0; i < 16; ++i) {
		w.v[i] = i * 0x1000193u + 1u;
	}
	lock.Write(w);
	Wide r = lock.Read();
	REQUIRE(std::memcmp(&w, &r, sizeof(Wide)) == 0);
}

// =============================================================================
// [SeqLock] — TryRead
// =============================================================================

TEST_CASE("SeqLock: TryRead succeeds on a quiescent lock", AUTO_TAG) {
	SeqLock<int> lock(55);
	int out = 0;
	REQUIRE(lock.TryRead(out));
	REQUIRE(out == 55);
}

TEST_CASE("SeqLock: TryRead leaves out untouched on success only", AUTO_TAG) {
	SeqLock<std::uint64_t> lock(0xABCDEF12u);
	std::uint64_t out = 0;
	REQUIRE(lock.TryRead(out));
	REQUIRE(out == 0xABCDEF12u);
}

// =============================================================================
// [SeqLock] — Update (read-modify-write, single writer)
// =============================================================================

TEST_CASE("SeqLock: Update applies the functor to the current value", AUTO_TAG) {
	SeqLock<int> lock(10);
	lock.Update([](int& v) { v += 5; });
	REQUIRE(lock.Read() == 15);
	lock.Update([](int& v) { v *= 2; });
	REQUIRE(lock.Read() == 30);
}

TEST_CASE("SeqLock: Update is strongly exception-safe", AUTO_TAG) {
	SeqLock<int> lock(7);
	struct Boom {};
	REQUIRE_THROWS_AS(lock.Update([](int& v) {
						  v = 99;
						  throw Boom{};
					  }),
					  Boom);
	// No change published, and the lock is not left in the "writing" (odd) state.
	REQUIRE(lock.Read() == 7);
	REQUIRE_FALSE(lock.WriteInProgress());
}

// =============================================================================
// [SeqLock] — Cross-thread correctness (the defining seqlock contract)
// =============================================================================

TEST_CASE("SeqLock: readers never observe a torn value under contention", AUTO_TAG) {
	// Invariant b == ~a holds for every value the writer publishes. A torn read
	// (a from a new write, b from the previous one) would violate it.
	SeqLock<Paired> lock(Paired{0, ~std::uint64_t{0}});

	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> violations{0};
	std::atomic<std::uint64_t> reads{0};

	constexpr int kReaders = 4;
	std::vector<std::thread> readers;
	readers.reserve(kReaders);
	for (int t = 0; t < kReaders; ++t) {
		readers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				Paired p = lock.Read();
				if (p.b != ~p.a) {
					violations.fetch_add(1, std::memory_order_relaxed);
				}
				reads.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	std::thread writer([&] {
		for (std::uint64_t i = 0; i < 1'000'000; ++i) {
			lock.Write(Paired{i, ~i});
		}
		stop.store(true, std::memory_order_relaxed);
	});

	writer.join();
	for (auto& r : readers) {
		r.join();
	}

	REQUIRE(violations.load() == 0);
	REQUIRE(reads.load() > 0);
	// The last published value must be visible once writes have drained.
	Paired final = lock.Read();
	REQUIRE(final.b == ~final.a);
}

TEST_CASE("SeqLock: wide payload stays internally consistent across threads", AUTO_TAG) {
	// Every word of the published Wide must be equal; a torn read across the
	// payload's cache lines would surface differing words.
	SeqLock<Wide> lock;

	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> violations{0};
	std::atomic<std::uint64_t> reads{0};

	constexpr int kReaders = 3;
	std::vector<std::thread> readers;
	readers.reserve(kReaders);
	for (int t = 0; t < kReaders; ++t) {
		readers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				Wide w = lock.Read();
				for (std::uint32_t i = 1; i < 16; ++i) {
					if (w.v[i] != w.v[0]) {
						violations.fetch_add(1, std::memory_order_relaxed);
						break;
					}
				}
				reads.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	std::thread writer([&] {
		for (std::uint32_t n = 1; n <= 500'000; ++n) {
			Wide w;
			for (std::uint32_t i = 0; i < 16; ++i) {
				w.v[i] = n;
			}
			lock.Write(w);
		}
		stop.store(true, std::memory_order_relaxed);
	});

	writer.join();
	for (auto& r : readers) {
		r.join();
	}

	REQUIRE(violations.load() == 0);
	REQUIRE(reads.load() > 0);
}

TEST_CASE("SeqLock: last written value is visible after the writer completes", AUTO_TAG) {
	SeqLock<std::uint64_t> lock;
	std::thread writer([&] {
		for (std::uint64_t i = 1; i <= 200'000; ++i) {
			lock.Write(i);
		}
	});
	writer.join();
	REQUIRE(lock.Read() == 200'000);
}

// =============================================================================
// Compile-time contracts (C++26 reflection audit + traits)
// =============================================================================

TEST_CASE("SeqLock: sequence counter is lock-free", AUTO_TAG) {
	STATIC_REQUIRE(std::atomic<SeqLock<int>::SequenceType>::is_always_lock_free);
}

TEST_CASE("SeqLock: copy and move are deleted", AUTO_TAG) {
	STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<SeqLock<int>>);
	STATIC_REQUIRE_FALSE(std::is_move_constructible_v<SeqLock<int>>);
	STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<SeqLock<int>>);
	STATIC_REQUIRE_FALSE(std::is_move_assignable_v<SeqLock<int>>);
}

TEST_CASE("SeqLock: hot-path operations are noexcept", AUTO_TAG) {
	STATIC_REQUIRE(noexcept(std::declval<SeqLock<int>&>().Write(0)));
	STATIC_REQUIRE(noexcept(std::declval<const SeqLock<int>&>().Read()));
	STATIC_REQUIRE(noexcept(std::declval<const SeqLock<int>&>().TryRead(std::declval<int&>())));
	STATIC_REQUIRE(noexcept(std::declval<const SeqLock<int>&>().Sequence()));
	STATIC_REQUIRE(noexcept(std::declval<const SeqLock<int>&>().WriteInProgress()));
}

TEST_CASE("SeqLock: reflection layout audit proves a cache-friendly layout", AUTO_TAG) {
	STATIC_REQUIRE(Concurrency::AuditFalseSharing<SeqLock<std::uint64_t>>());
	STATIC_REQUIRE(Concurrency::AuditFalseSharing<SeqLock<double>>());
	STATIC_REQUIRE(Concurrency::AuditFalseSharing<SeqLock<Wide>>());
	STATIC_REQUIRE(Concurrency::AuditFalseSharing<SeqLock<Huge256>>());
	// Whole-line occupancy rules out external false sharing between array neighbours.
	STATIC_REQUIRE(Concurrency::OccupiesWholeLines<SeqLock<std::uint64_t>>());
	STATIC_REQUIRE(Concurrency::OccupiesWholeLines<SeqLock<Huge256>>());
}

TEST_CASE("SeqLock: Layout() exposes compile-time layout facts", AUTO_TAG) {
	using Concurrency::ProducerOwned;

	constexpr auto small = SeqLock<std::uint64_t>::Layout();
	STATIC_REQUIRE(small.valid);
	STATIC_REQUIRE(small.classifiedAll);  // every member carries the single-writer domain
	STATIC_REQUIRE(!small.hasConflict);   // one writer, so co-location is conflict-free
	STATIC_REQUIRE(small.memberCount == 2);
	STATIC_REQUIRE(small.occupiesWholeLines);
	// The writer's state begins a cache line, and a small payload shares it: one line per read.
	STATIC_REQUIRE(Concurrency::DomainStartsLine<SeqLock<std::uint64_t>, ProducerOwned>());
	STATIC_REQUIRE(Concurrency::DomainLineSpan<SeqLock<std::uint64_t>, ProducerOwned>() == 1);

	// A 256-byte payload necessarily spans more than one cache line
	// (robust for 64- and 128-byte interference sizes).
	constexpr auto huge = SeqLock<Huge256>::Layout();
	STATIC_REQUIRE(huge.valid);
	STATIC_REQUIRE(Concurrency::DomainLineSpan<SeqLock<Huge256>, ProducerOwned>() >= 2);
}
