#include "AppConfiguration.h"
#include "../Main.h"

#include <Containers/StringConcatenable.h>
#include <IO/FileSystem.h>
#include <Utf8.h>

using namespace Death::Containers::Literals;
using namespace Death::IO;

namespace nCine
{
	AppConfiguration::AppConfiguration()
		: Resolution(0, 0),
		  WindowPosition(WindowPositionIgnore, WindowPositionIgnore),
		  FrameLimit(0),
		  FrameTimerLogInterval(5.0f),
		  Fullscreen(false),
		  Resizable(true),
		  WindowScaling(true),
		  UseBufferMapping(false),
		  UseBufferStorage(true),
	// Only the build-time override lives here. A batch size that follows from the rendering device is the
	// device's own business and is published as IntValues::MaxBatchSize by the backend that knows it
	// (GLRhiCapabilities for the ANGLE/Emscripten/WinRT targets, GxmRhiCapabilities, RsxRhiCapabilities);
	// everything that sizes a batch consults that, so leaving this 0 is what lets the device speak.
#if defined(WITH_FIXED_BATCH_SIZE) && WITH_FIXED_BATCH_SIZE > 0
		  FixedBatchSize(WITH_FIXED_BATCH_SIZE),
#else
		  FixedBatchSize(0),
#endif
	// These two also decide how many draws an aggregated mesh costs: a tile layer goes out as one command
	// per RenderResources::GetMaxQuadsPerDraw() quads, and a quad is 128 bytes of vertices and 12 of
	// indices. The 64 KB / 8 KB pair held 512 of the ~2200 quads a 1080p viewport of a layer covers, so a
	// layer took several draws, each out of a buffer newly created because nothing in hand had room - and
	// how many of those a frame needed moved with the camera. 512 KB / 128 KB is a whole layer in one
	// draw at any resolution the game runs at, for under 2 MB. The console backends keep the small pair,
	// where a few hundred quads fit either way and buffer memory is what is short, keyed on the backend
	// like `UseBufferMapping` below, the ES2 profile being Vita's vitaGL.
#if !(defined(WITH_IMGUI)) && defined(RHI_GL_PROFILE_ES2)
		  VboSize(64 * 1024),
		  IboSize(8 * 1024),
#else
		  VboSize(512 * 1024),
		  IboSize(128 * 1024),
#endif
		  VaoPoolSize(16),
		  RenderCommandPoolSize(32),
#if defined(WITH_IMGUI)
		  WithDebugOverlay(false),
#endif
		  WithAudio(true),
		  WithGraphics(true),
		  WithScenegraph(true),
		  WithThreads(false),
		  WithVSync(true),
		  WithGlDebugContext(false),

		  // Compile-time variables
		  _glCoreProfile(true),
		  _glForwardCompatible(true),
#if defined(RHI_GL_PROFILE_ES2)
		  // Real OpenGL|ES 2.0 profile (PS Vita target): ESSL 100, no UBOs, no gl_VertexID
		  _glMajorVersion(2),
		  _glMinorVersion(0),
#elif defined(RHI_GL_PROFILE_ES) || defined(DEATH_TARGET_EMSCRIPTEN)
		  _glMajorVersion(3),
		  _glMinorVersion(0),
#else
		  _glMajorVersion(3),
		  _glMinorVersion(3),
#endif
		  _argv(nullptr)
	{
#if defined(DEATH_TARGET_EMSCRIPTEN)
		GetDataPath() = fs::PathSeparator;
		// Always disable mapping on Emscripten as it is not supported by WebGL 2
		UseBufferMapping = false;
#else
		GetDataPath() = "assets"_s + fs::PathSeparator;
#endif

#if defined(RHI_GL_PROFILE_ES2)
		// glMapBufferRange()/glUnmapBuffer() are ES 3.0 (OES_mapbuffer is write-only and not assumed), so the
		// streaming buffers must use the glBufferSubData() host-copy path on the ES2 profile
		UseBufferMapping = false;
#endif

#if defined(DEATH_TARGET_UNIX) && (defined(WITH_SDL2) || defined(WITH_SDL3))
		// DPI queries do not seem to work reliably on X11 with SDL2
		WindowScaling = false;
#endif
	}

	const String& AppConfiguration::GetDataPath() const
	{
		return _dataPath;
	}

	String& AppConfiguration::GetDataPath()
	{
		return _dataPath;
	}

	const StringView AppConfiguration::GetArgv(std::size_t index) const
	{
		return _argv[index];
	}
}
