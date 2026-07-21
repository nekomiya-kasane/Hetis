/**
 * @file FileTest.cpp
 * @brief Verify PAL file ownership, positional and Direct I/O, mappings, atomic publication, and monitoring.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/AtomicFile.h>
#include <Sora/Core/PAL/FileMapping.h>
#include <Sora/Core/PAL/FileWatcher.h>
#include <Sora/Core/Memory/DirectBuffer.h>
#include <Sora/Core/Traits/EnumTraits.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <numeric>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace PAL = Sora::PAL;

static_assert(std::is_trivially_copyable_v<PAL::BorrowedFile>);
static_assert(Sora::Traits::IsValidEnumValue(PAL::FileMappingAccess::Read));
static_assert(!Sora::Traits::IsValidEnumValue(static_cast<PAL::FileMappingAccess>(0xFF)));
static_assert(Sora::Traits::IsValidEnumValue(PAL::FileMappingAdvice::Sequential));
static_assert(!Sora::Traits::IsValidEnumValue(static_cast<PAL::FileMappingAdvice>(0xFF)));

namespace {

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / std::format("sora-pal-file-test-{}", stamp);
            if (!std::filesystem::create_directories(path_)) {
                throw std::runtime_error{"failed to create temporary directory"};
            }
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
        auto file = PAL::File::Open(path);
        if (!file) {
            return {};
        }
        const auto size = file->Size();
        if (!size) {
            return {};
        }
        std::vector<std::byte> bytes(static_cast<size_t>(*size));
        const auto read = file->ReadAt(bytes, 0);
        return read && *read == bytes.size() ? bytes : std::vector<std::byte>{};
    }

} // namespace

TEST_CASE("File performs Unicode positional I/O and reports atomic creation conflicts", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.Path() / L"\u6570\u636e.bin";
    auto opened = PAL::File::Open(path, PAL::FileOpenOptions{
                                            .access = PAL::FileAccess::Read | PAL::FileAccess::Write,
                                            .creation = PAL::FileCreation::CreateAlways,
                                        });
    REQUIRE(opened.has_value());
    PAL::File file = std::move(*opened);
    REQUIRE(file);
    REQUIRE_FALSE(file.Borrow());

    constexpr std::array source{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    REQUIRE(file.Resize(32).has_value());
    REQUIRE(file.WriteAllAt(source, 7).has_value());
    const auto fileSize = file.Size();
    REQUIRE(fileSize.has_value());
    REQUIRE(*fileSize == 32);

    std::array<std::byte, source.size()> destination{};
    const auto readSize = file.ReadAt(destination, 7);
    REQUIRE(readSize.has_value());
    REQUIRE(*readSize == source.size());
    REQUIRE(destination == source);
    std::array<std::byte, source.size()> exactDestination{};
    REQUIRE(file.ReadAllAt(exactDestination, 7).has_value());
    REQUIRE(exactDestination == source);
    std::array<std::byte, 26> truncatedDestination{};
    const auto truncatedRead = file.ReadAllAt(truncatedDestination, 7);
    REQUIRE_FALSE(truncatedRead.has_value());
    REQUIRE(truncatedRead.error() == Sora::ErrorCode::DataTruncated);
    REQUIRE(file.Flush().has_value());

    const auto conflict = PAL::File::Open(path, PAL::FileOpenOptions{
                                                    .access = PAL::FileAccess::Write,
                                                    .creation = PAL::FileCreation::CreateNew,
                                                });
    REQUIRE_FALSE(conflict.has_value());
    REQUIRE(conflict.error() == Sora::ErrorCode::AlreadyExists);

    const auto invalidFlags = PAL::File::Open(path, PAL::FileOpenOptions{
                                                        .flags = static_cast<PAL::FileOpenFlag>(1u << 15u),
                                                    });
    REQUIRE_FALSE(invalidFlags.has_value());
    REQUIRE(invalidFlags.error() == Sora::ErrorCode::InvalidArgument);
}

TEST_CASE("BorrowedFile owns no state and writes through a shared cursor", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.Path() / "sequential.txt";
    auto opened = PAL::File::Open(path, PAL::FileOpenOptions{
                                            .access = PAL::FileAccess::Write,
                                            .creation = PAL::FileCreation::CreateAlways,
                                            .flags = PAL::FileOpenFlag::None,
                                        });
    REQUIRE(opened.has_value());
    PAL::BorrowedFile borrowed = opened->Borrow();
    REQUIRE(borrowed);
    REQUIRE(borrowed.Write("alpha"));
    REQUIRE(borrowed.Write("-beta"));
    std::array<std::byte, 1> positionalRead{};
    const auto unsupportedRead = opened->ReadAt(positionalRead, 0);
    REQUIRE_FALSE(unsupportedRead.has_value());
    REQUIRE(unsupportedRead.error() == Sora::ErrorCode::NotSupported);
    borrowed.Flush();
    REQUIRE(opened->Close().has_value());

    const std::vector<std::byte> bytes = ReadFile(path);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} == "alpha-beta");
}

TEST_CASE("FileMapping exposes exact unaligned ranges and retains an independent flush handle", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.Path() / "mapped.bin";
    std::vector<std::byte> source(8192);
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>(index % 251);
    }

    auto file = PAL::File::Open(path, PAL::FileOpenOptions{
                                          .access = PAL::FileAccess::Read | PAL::FileAccess::Write,
                                          .creation = PAL::FileCreation::CreateAlways,
                                      });
    REQUIRE(file.has_value());
    REQUIRE(file->WriteAllAt(source, 0).has_value());

    auto readMapping = PAL::FileMapping::Map(*file, {.offset = 123, .size = 4096});
    REQUIRE(readMapping.has_value());
    REQUIRE(std::ranges::equal(readMapping->Bytes(), std::span{source}.subspan(123, 4096)));
    REQUIRE(readMapping->WritableBytes().empty());

    const auto invalidMapping = PAL::FileMapping::Map(
        *file, {.access = static_cast<PAL::FileMappingAccess>(0xFF), .offset = 0, .size = 1});
    REQUIRE_FALSE(invalidMapping.has_value());
    REQUIRE(invalidMapping.error() == Sora::ErrorCode::InvalidArgument);

    auto writeMapping = PAL::FileMapping::Map(*file, {
                                                         .access = PAL::FileMappingAccess::ReadWrite,
                                                         .offset = 127,
                                                         .size = 64,
                                                     });
    REQUIRE(writeMapping.has_value());
    REQUIRE(file->Close().has_value());
    writeMapping->WritableBytes()[0] = std::byte{0x7F};
    REQUIRE(writeMapping->Flush().has_value());
    writeMapping->Reset();

    const std::vector<std::byte> bytes = ReadFile(path);
    REQUIRE(bytes.size() == source.size());
    REQUIRE(bytes[127] == std::byte{0x7F});
}

TEST_CASE("FileMapping represents an empty end-of-file range without retaining its source", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.Path() / "empty.bin";
    auto file = PAL::File::Open(path, PAL::FileOpenOptions{
                                          .access = PAL::FileAccess::Read | PAL::FileAccess::Write,
                                          .creation = PAL::FileCreation::CreateAlways,
                                      });
    REQUIRE(file.has_value());
    auto mapping = PAL::FileMapping::Map(*file);
    REQUIRE(mapping.has_value());
    REQUIRE(mapping->Bytes().empty());
    REQUIRE(file->Close().has_value());
    REQUIRE(static_cast<bool>(*mapping));
    mapping->Reset();
}

TEST_CASE("DirectIORequirements enforce address, offset, and size alignment", "[Sora.PAL.File]") {
    constexpr PAL::DirectIORequirements requirements{
        .memoryAlignment = 4096,
        .offsetAlignment = 512,
        .sizeAlignment = 512,
    };
    auto buffer = Sora::DirectBuffer::Allocate(4096, requirements.memoryAlignment);
    REQUIRE(buffer.has_value());
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer->Bytes().data()) % requirements.memoryAlignment == 0);
    REQUIRE(requirements.Accepts(buffer->Bytes().data(), 1024, buffer->Size()));
    REQUIRE_FALSE(requirements.Accepts(buffer->Bytes().data() + 1, 1024, buffer->Size()));
    REQUIRE_FALSE(requirements.Accepts(buffer->Bytes().data(), 1, buffer->Size()));
    REQUIRE_FALSE(requirements.Accepts(buffer->Bytes().data(), 1024, buffer->Size() - 1));
}

TEST_CASE("File performs aligned Direct I/O when the native volume supports it", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.Path() / "direct.bin";
    auto file = PAL::File::Open(path, PAL::FileOpenOptions{
                                          .access = PAL::FileAccess::Read | PAL::FileAccess::Write,
                                          .creation = PAL::FileCreation::CreateAlways,
                                          .flags = PAL::FileOpenFlag::Direct | PAL::FileOpenFlag::Positional,
                                      });
    if (!file) {
        SUCCEED("The temporary volume does not support native Direct I/O.");
        return;
    }
    const PAL::DirectIORequirements requirements = file->DirectRequirements();
    const size_t size = std::lcm(requirements.sizeAlignment, size_t{4096});
    auto source = Sora::DirectBuffer::Allocate(size, requirements.memoryAlignment);
    auto destination = Sora::DirectBuffer::Allocate(size, requirements.memoryAlignment);
    REQUIRE(source.has_value());
    REQUIRE(destination.has_value());
    std::ranges::fill(source->Bytes(), std::byte{0x5A});
    REQUIRE(file->WriteAllAt(source->Bytes(), 0).has_value());
    const auto readSize = file->ReadAt(destination->Bytes(), 0);
    REQUIRE(readSize.has_value());
    REQUIRE(*readSize == size);
    REQUIRE(std::ranges::equal(source->Bytes(), destination->Bytes()));
}

TEST_CASE("AtomicFileWriter commits complete files and removes aborted temporaries", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path destination = temporary.Path() / L"\u539f\u5b50.bin";
    constexpr std::string_view first = "first-version";
    constexpr std::string_view second = "second-version";

    auto initial = PAL::AtomicFileWriter::Create(destination);
    REQUIRE(initial.has_value());
    REQUIRE(initial->Output().WriteAllAt(std::as_bytes(std::span{first}), 0).has_value());
    REQUIRE(initial->Commit().has_value());

    auto replacement = PAL::AtomicFileWriter::Create(destination);
    REQUIRE(replacement.has_value());
    const std::filesystem::path temporaryPath = replacement->TemporaryPath();
    REQUIRE(replacement->Output().WriteAllAt(std::as_bytes(std::span{second}), 0).has_value());
    REQUIRE(replacement->Commit().has_value());
    REQUIRE_FALSE(std::filesystem::exists(temporaryPath));

    const std::vector<std::byte> bytes = ReadFile(destination);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} == second);

    auto aborted = PAL::AtomicFileWriter::Create(destination);
    REQUIRE(aborted.has_value());
    const std::filesystem::path abortedPath = aborted->TemporaryPath();
    aborted->Abort();
    REQUIRE_FALSE(std::filesystem::exists(abortedPath));
}

TEST_CASE("AtomicReplaceFile publishes a complete sibling and consumes its source name", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const std::filesystem::path destination = temporary.Path() / "destination.bin";
    const std::filesystem::path replacement = temporary.Path() / "replacement.bin";
    constexpr std::string_view contents = "replacement";

    auto output = PAL::File::Open(replacement, PAL::FileOpenOptions{
                                                   .access = PAL::FileAccess::Write,
                                                   .creation = PAL::FileCreation::CreateNew,
                                               });
    REQUIRE(output.has_value());
    REQUIRE(output->WriteAllAt(std::as_bytes(std::span{contents}), 0).has_value());
    REQUIRE(output->Close().has_value());
    REQUIRE(PAL::AtomicReplaceFile(replacement, destination).has_value());
    REQUIRE_FALSE(std::filesystem::exists(replacement));

    const std::vector<std::byte> bytes = ReadFile(destination);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} == contents);
}

TEST_CASE("FileWatcher delivers a change batch or an explicit rescan request", "[Sora.PAL.File]") {
    TemporaryDirectory temporary;
    const auto invalidWatcher = PAL::FileWatcher::Open(
        temporary.Path(), {.filter = static_cast<PAL::FileWatchFilter>(1u << 7u)});
    REQUIRE_FALSE(invalidWatcher.has_value());
    REQUIRE(invalidWatcher.error() == Sora::ErrorCode::InvalidArgument);

    auto watcher = PAL::FileWatcher::Open(temporary.Path());
    REQUIRE(watcher.has_value());

    const std::filesystem::path created = temporary.Path() / "watched.txt";
    std::jthread producer{[created] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        auto file = PAL::File::Open(created, PAL::FileOpenOptions{
                                                .access = PAL::FileAccess::Write,
                                                .creation = PAL::FileCreation::CreateAlways,
                                            });
        if (file) {
            static_cast<void>(file->WriteAllAt(std::as_bytes(std::span{std::string_view{"changed"}}), 0));
        }
    }};

    const auto changes = watcher->Wait(std::chrono::seconds{5});
    REQUIRE(changes.has_value());
    REQUIRE_FALSE(changes->empty());
    REQUIRE(std::ranges::any_of(*changes, [&](const PAL::FileChange& change) {
        return change.kind == PAL::FileChangeKind::RescanRequired || change.path.filename() == created.filename();
    }));
}
