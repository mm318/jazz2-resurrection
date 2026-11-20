#if defined(WITH_TRACY) && defined(WITH_TRACY_OPENGL)
#include "tracy/TracyOpenGL.hpp"
#else
#include "tracy.h"

#define TracyGpuContext
#define TracyGpuContextName(x, y)
#define TracyGpuNamedZone(x, y, z)
#define TracyGpuNamedZoneC(x, y, z, w)
#define TracyGpuZone(x)
#define TracyGpuZoneC(x, y)
#define TracyGpuZoneTransient(x, y, z)
#define TracyGpuCollect

#define TracyGpuNamedZoneS(x, y, z, w)
#define TracyGpuNamedZoneCS(x, y, z, w, a)
#define TracyGpuZoneS(x, y)
#define TracyGpuZoneCS(x, y, z)
#define TracyGpuZoneTransientS(x, y, z, w)

#ifndef DOXYGEN_GENERATING_OUTPUT
namespace tracy
{
	struct SourceLocationData;

	class GpuCtxScope
	{
	public:
		GpuCtxScope(const SourceLocationData*, bool)
		{
		}
		GpuCtxScope(const SourceLocationData*, int, bool)
		{
		}
	};
}
#endif

#endif
