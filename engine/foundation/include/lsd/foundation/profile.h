#pragma once
// CPU profiling zones for Tracy (https://github.com/wolfpld/tracy, BSD-3), Unreal Insights' role.
// Build with -DLSD_TRACY=ON, run, and connect the Tracy profiler (same version, v0.11.1) to see
// every frame's zones live. Without it the macros are empty: shipped builds open no network port.
#ifdef LSD_TRACY
#include <tracy/Tracy.hpp>
#define LSD_PROFILE_SCOPE(name) ZoneScopedN(name)
#define LSD_PROFILE_FRAME() FrameMark
#else
#define LSD_PROFILE_SCOPE(name) ((void)0)
#define LSD_PROFILE_FRAME() ((void)0)
#endif
