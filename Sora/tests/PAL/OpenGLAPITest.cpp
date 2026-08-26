/**
 * @file OpenGLAPITest.cpp
 * @brief Verify generated Khronos ABI declarations, dispatch resolution, and platform bootstrap tables.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/OpenGLAPI.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace OpenGL = Sora::PAL::OpenGL;

namespace {

    OpenGL::EGLBoolean StubSwapInterval(OpenGL::EGLDisplay, OpenGL::EGLint) { return 1; }

    static_assert(std::is_void_v<std::invoke_result_t<OpenGL::GL::glAccumFunction, OpenGL::GLenum,
                                                      OpenGL::GLfloat>>);
    static_assert(std::is_same_v<OpenGL::GL::Texture::glBindTextureFunction,
                                 OpenGL::GL::glBindTextureFunction>);
    static_assert(std::is_same_v<OpenGL::GL::Buffer::glBufferDataFunction,
                                 OpenGL::GL::glBufferDataFunction>);
    static_assert(std::is_same_v<OpenGL::GL::Shader::glCreateShaderFunction,
                                 OpenGL::GL::glCreateShaderFunction>);
    static_assert(std::is_same_v<OpenGL::GL::Draw::glDrawArraysFunction,
                                 OpenGL::GL::glDrawArraysFunction>);
    static_assert(std::is_same_v<std::invoke_result_t<OpenGL::GL::glGetStringFunction, OpenGL::GLenum>,
                                 const OpenGL::GLubyte*>);
    static_assert(std::is_same_v<OpenGL::GLhalfARB, uint16_t>);
    static_assert(std::is_same_v<OpenGL::GPU_DEVICE, OpenGL::GpuDevice>);
    static_assert(std::is_same_v<OpenGL::GLXFBConfigID, OpenGL::XID>);
    static_assert(std::is_same_v<OpenGL::GLXFBConfigIDSGIX, OpenGL::XID>);
    static_assert(std::is_same_v<OpenGL::PFNEGLBINDWAYLANDDISPLAYWL,
                                 OpenGL::EGL::eglBindWaylandDisplayWLFunction>);
    static_assert(sizeof(OpenGL::PixelFormatDescriptor) == 40);
    static_assert(sizeof(OpenGL::LayerPlaneDescriptor) == 32);
    static_assert(sizeof(OpenGL::GlyphMetricsFloat) == 24);
    static_assert(sizeof(OpenGL::GpuDevice) == 184);
    static_assert(offsetof(OpenGL::GpuDevice, virtualScreen) == 168);
    static_assert(OpenGL::GL::GL_TRIANGLES == 0x0004);
    static_assert(OpenGL::WGL::WGL_DRAW_TO_WINDOW_ARB == 0x2001);
    static_assert(OpenGL::GLX::GLX_X_RENDERABLE == 0x8012);
    static_assert(OpenGL::EGL::EGL_PLATFORM_WAYLAND_KHR == 0x31D8);

} // namespace

TEST_CASE("Generated EGL dispatch resolves exact registry names", "[Sora.PAL.OpenGLAPI]") {
    OpenGL::EGLAPI api{};
    api.Resolve([](const char* name) noexcept -> void* {
        return std::string_view{name} == "eglSwapInterval" ? reinterpret_cast<void*>(&StubSwapInterval) : nullptr;
    });

    REQUIRE(api.eglSwapInterval == reinterpret_cast<OpenGL::EGL::eglSwapIntervalFunction>(&StubSwapInterval));
    REQUIRE(api.eglInitialize == nullptr);
}

TEST_CASE("Graphics runtime tables have process-lifetime identities", "[Sora.PAL.OpenGLAPI]") {
    REQUIRE(&OpenGL::LoadWGLAPI() == &OpenGL::LoadWGLAPI());
    REQUIRE(&OpenGL::LoadGLXAPI() == &OpenGL::LoadGLXAPI());
    REQUIRE(&OpenGL::LoadEGLAPI() == &OpenGL::LoadEGLAPI());
}

#if defined(PLATFORM_WINDOWS)
TEST_CASE("Windows WGL bootstrap exposes opengl32 core exports", "[Sora.PAL.OpenGLAPI]") {
    const OpenGL::WGLAPI& api = OpenGL::LoadWGLAPI();
    REQUIRE(api.wglCreateContext != nullptr);
    REQUIRE(api.wglDeleteContext != nullptr);
    REQUIRE(api.wglGetCurrentContext != nullptr);
    REQUIRE(api.wglGetProcAddress != nullptr);
    REQUIRE(api.wglMakeCurrent != nullptr);
    REQUIRE(api.SwapBuffers != nullptr);
}
#endif
