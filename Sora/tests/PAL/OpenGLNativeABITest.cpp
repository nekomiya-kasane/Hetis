/**
 * @file OpenGLNativeABITest.cpp
 * @brief Compare PAL graphics structures with the native SDK ABI where native declarations are available.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/OpenGLAPI.h>
#include <Sora/Platform.h>

#if defined(PLATFORM_WINDOWS)
#  include <Windows.h>
#elif defined(PLATFORM_LINUX) && !defined(__ANDROID__)
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <GL/glx.h>
#  include <GL/glxext.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace OpenGL = Sora::PAL::OpenGL;

#if defined(PLATFORM_WINDOWS)
static_assert(sizeof(OpenGL::Rect) == sizeof(::RECT));
static_assert(alignof(OpenGL::Rect) == alignof(::RECT));
static_assert(sizeof(OpenGL::PixelFormatDescriptor) == sizeof(::PIXELFORMATDESCRIPTOR));
static_assert(alignof(OpenGL::PixelFormatDescriptor) == alignof(::PIXELFORMATDESCRIPTOR));
static_assert(sizeof(OpenGL::LayerPlaneDescriptor) == sizeof(::LAYERPLANEDESCRIPTOR));
static_assert(sizeof(OpenGL::GlyphMetricsFloat) == sizeof(::GLYPHMETRICSFLOAT));
#elif defined(PLATFORM_LINUX) && !defined(__ANDROID__)
static_assert(sizeof(OpenGL::XVisualInfo) == sizeof(::XVisualInfo));
static_assert(alignof(OpenGL::XVisualInfo) == alignof(::XVisualInfo));
static_assert(sizeof(OpenGL::GLXPbufferClobberEvent) == sizeof(::GLXPbufferClobberEvent));
static_assert(alignof(OpenGL::GLXPbufferClobberEvent) == alignof(::GLXPbufferClobberEvent));
static_assert(sizeof(OpenGL::GLXBufferClobberEventSGIX) == 72);
static_assert(alignof(OpenGL::GLXBufferClobberEventSGIX) == 8);
static_assert(offsetof(OpenGL::GLXBufferClobberEventSGIX, serial) == 8);
static_assert(offsetof(OpenGL::GLXBufferClobberEventSGIX, display) == 24);
static_assert(offsetof(OpenGL::GLXBufferClobberEventSGIX, count) == 68);
static_assert(sizeof(OpenGL::GLXBufferSwapComplete) == sizeof(::GLXBufferSwapComplete));
static_assert(sizeof(OpenGL::GLXStereoNotifyEventEXT) == sizeof(::GLXStereoNotifyEventEXT));
static_assert(sizeof(OpenGL::GLXEvent) == sizeof(::GLXEvent));
static_assert(sizeof(OpenGL::GLXHyperpipeNetwork) == sizeof(::GLXHyperpipeNetworkSGIX));
static_assert(sizeof(OpenGL::GLXHyperpipeConfig) == sizeof(::GLXHyperpipeConfigSGIX));
static_assert(sizeof(OpenGL::GLXPipeRect) == sizeof(::GLXPipeRect));
static_assert(sizeof(OpenGL::GLXPipeRectLimits) == sizeof(::GLXPipeRectLimits));
static_assert(sizeof(OpenGL::EGLClientPixmapHI) == sizeof(::EGLClientPixmapHI));
static_assert(alignof(OpenGL::EGLClientPixmapHI) == alignof(::EGLClientPixmapHI));
#endif

TEST_CASE("Graphics PAL native ABI probes compiled", "[Sora.PAL.OpenGLAPI]") { SUCCEED(); }
