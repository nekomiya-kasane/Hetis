/**
 * @file NativeCrashStreamTest.cpp
 * @brief Verify native crash-stream ownership, complete writes, formatting, and truncation.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/NativeCrashStream.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>

namespace PAL = Sora::PAL;

namespace {

    class TemporaryCrashFile {
    public:
        TemporaryCrashFile() {
            std::random_device random;
            const std::uint64_t nonce = static_cast<std::uint64_t>(random()) << 32u ^ random();
            path_ = std::filesystem::temp_directory_path() /
                    std::format("sora-native-crash-stream-test-{:016x}.txt", nonce);
        }

        ~TemporaryCrashFile() {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

        [[nodiscard]] std::string Read() const {
            std::ifstream input{path_, std::ios::binary};
            return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        }

    private:
        std::filesystem::path path_;
    };

} // namespace

TEST_CASE("Native crash stream writes complete unbuffered records", "[Sora.PAL.NativeCrashStream]") {
    TemporaryCrashFile file;
    PAL::OwnedNativeCrashStream owner = PAL::OwnedNativeCrashStream::OpenTruncated(file.Path());
    REQUIRE(owner);

    const PAL::NativeCrashStream stream = owner.View();
    REQUIRE(stream);
    REQUIRE(stream.Write("record:"));
    REQUIRE(stream.WriteUnsigned(18446744073709551615ULL));
    REQUIRE(stream.Write(" "));
    REQUIRE(stream.WriteHex(0xDEADBEEF));
    REQUIRE(stream.Write("\n"));
    stream.Flush();

    CHECK(file.Read() == "record:18446744073709551615 0xDEADBEEF\n");
}

TEST_CASE("Native crash stream owner moves and truncates existing files", "[Sora.PAL.NativeCrashStream]") {
    TemporaryCrashFile file;
    {
        PAL::OwnedNativeCrashStream first = PAL::OwnedNativeCrashStream::OpenTruncated(file.Path());
        REQUIRE(first);
        REQUIRE(first.View().Write("obsolete payload"));
    }

    PAL::OwnedNativeCrashStream source = PAL::OwnedNativeCrashStream::OpenTruncated(file.Path());
    REQUIRE(source);
    PAL::OwnedNativeCrashStream destination = std::move(source);
    CHECK_FALSE(source);
    REQUIRE(destination);
    REQUIRE(destination.View().Write("replacement"));
    destination.View().Flush();

    CHECK(file.Read() == "replacement");
}

TEST_CASE("Default native crash streams reject output", "[Sora.PAL.NativeCrashStream]") {
    const PAL::NativeCrashStream stream{};
    CHECK_FALSE(stream);
    CHECK_FALSE(stream.Write("ignored"));
    CHECK_FALSE(stream.WriteHex(1));
    CHECK_FALSE(stream.WriteUnsigned(1));
    stream.Flush();
}
