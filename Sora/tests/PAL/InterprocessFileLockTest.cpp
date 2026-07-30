/**
 * @file InterprocessFileLockTest.cpp
 * @brief Verify exclusive cross-handle file locking and close-based release.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/InterprocessFileLock.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <system_error>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<Sora::PAL::InterprocessFileLock>);
static_assert(std::is_nothrow_move_constructible_v<Sora::PAL::InterprocessFileLock>);

namespace {

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() / std::format("sora-interprocess-lock-test-{}-{}", stamp,
                                                                     sequence.fetch_add(1, std::memory_order_relaxed));
            REQUIRE(std::filesystem::create_directories(path_));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

} // namespace

TEST_CASE("InterprocessFileLock excludes the same path until ownership closes", "[Sora][PAL][FileLock]") {
    TemporaryDirectory temporary;
    const auto path = temporary.Path() / "exclusive.lock";

    auto first = Sora::PAL::InterprocessFileLock::TryAcquire(path);
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());

    auto contended = Sora::PAL::InterprocessFileLock::TryAcquire(path);
    REQUIRE(contended.has_value());
    REQUIRE_FALSE(contended->has_value());

    first->reset();
    auto released = Sora::PAL::InterprocessFileLock::TryAcquire(path);
    REQUIRE(released.has_value());
    REQUIRE(released->has_value());
}

TEST_CASE("InterprocessFileLock keeps independent paths independent", "[Sora][PAL][FileLock]") {
    TemporaryDirectory temporary;
    auto first = Sora::PAL::InterprocessFileLock::TryAcquire(temporary.Path() / "first.lock");
    auto second = Sora::PAL::InterprocessFileLock::TryAcquire(temporary.Path() / "second.lock");
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
}
