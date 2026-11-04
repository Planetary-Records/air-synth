#pragma once

// The version a musician is told about. CMake supplies it; the numeric PROJECT_VERSION stays 0.1.0 so that the
// plug-in's identity in hosts does not move between prereleases. A build without it reports "dev".
#ifndef RACKSYNTH_RELEASE_VERSION
#define RACKSYNTH_RELEASE_VERSION "dev"
#endif

namespace airack
{
inline constexpr const char* releaseVersion = RACKSYNTH_RELEASE_VERSION;
}
