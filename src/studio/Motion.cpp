#include "Motion.h"

#ifdef _WIN32
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#include <algorithm>
#include <chrono>

namespace airack::studio::motion
{
namespace
{
std::function<double()> testClock;
std::optional<bool> testReduced;
} // namespace

double now()
{
    if (testClock)
        return testClock();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool reduced()
{
    if (testReduced)
        return *testReduced;
#ifdef _WIN32
    BOOL animations = TRUE;
    return SystemParametersInfoW (SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) != 0 && animations == FALSE;
#else
    return false;
#endif
}

void setForTests (std::function<double()> clock, std::optional<bool> r)
{
    testClock = std::move (clock);
    testReduced = r;
}

float ease (float t)
{
    t = std::clamp (t, 0.0f, 1.0f);
    if (t < 0.5f)
        return 4.0f * t * t * t;
    const auto u = 2.0f - 2.0f * t;
    return 1.0f - u * u * u / 2.0f;
}
} // namespace airack::studio::motion
