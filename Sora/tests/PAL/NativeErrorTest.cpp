#include <Sora/Core/PAL/NativeError.h>
#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <string>
#include <system_error>
#include <type_traits>

static_assert(sizeof(Sora::PAL::NativeError) == sizeof(std::error_code));
static_assert(std::is_trivially_copyable_v<Sora::PAL::NativeError>);

TEST_CASE("NativeError preserves category, value, and normalized messages", "[Sora.PAL.NativeError]") {
    const Sora::PAL::NativeError missing = Sora::PAL::NativeError::FromErrno(ENOENT);

    REQUIRE(missing);
    REQUIRE(missing.Value() == ENOENT);
    REQUIRE(&missing.Category() == &std::generic_category());
    REQUIRE_FALSE(missing.Message().empty());
    REQUIRE(missing.ToString().contains(missing.Message()));
    REQUIRE(missing.ToString().contains(missing.Category().name()));
}

TEST_CASE("NativeError captures the calling thread native error slot", "[Sora.PAL.NativeError]") {
#ifdef _WIN32
    constexpr Sora::PAL::WindowsSystem::DWord kAccessDenied = 5;
    const Sora::PAL::NativeErrorSystemAPI& api = Sora::PAL::LoadNativeErrorSystemAPI();
    REQUIRE(api.setLastError != nullptr);
    api.setLastError(kAccessDenied);
    const Sora::PAL::NativeError captured = Sora::PAL::CaptureLastNativeError();
    REQUIRE(captured.Value() == kAccessDenied);
    REQUIRE(&captured.Category() == &std::system_category());
#else
    errno = EACCES;
    const Sora::PAL::NativeError captured = Sora::PAL::CaptureLastNativeError();
    REQUIRE(captured.Value() == EACCES);
    REQUIRE(&captured.Category() == &std::generic_category());
#endif
    REQUIRE(captured);
    REQUIRE_FALSE(captured.Message().empty());
}

TEST_CASE("NativeError bridges to std::system_error without duplicating the native message", "[Sora.PAL.NativeError]") {
    const Sora::PAL::NativeError error = Sora::PAL::NativeError::FromErrno(EINVAL);
    const std::system_error exception = Sora::PAL::MakeSystemError(error, "opening configuration");
    const std::string message = exception.what();

    REQUIRE(exception.code() == error.Code());
    REQUIRE(message.contains("opening configuration"));
    REQUIRE(message.contains(error.Message()));
}

TEST_CASE("ModuleLoader reports the common module-load error", "[Sora.PAL.Module]") {
    constexpr std::string_view missing = "sora-native-error-test-module-that-does-not-exist";
    const Sora::PAL::ModuleLoadOptions options{
        .nameKind = Sora::PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = Sora::PAL::ModuleCachePolicy::Private,
    };

    const Sora::Result<Sora::PAL::ModulePtr> loaded = Sora::PAL::LoadModule({missing}, options);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error() == Sora::ErrorCode::ModuleLoadFailed);
}
