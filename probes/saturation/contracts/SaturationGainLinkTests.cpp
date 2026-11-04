#include "SaturationGainRuntime.h"
#include <bit>
#include <cstdint>

// No ordinary math call can contribute the release math descriptor to this image.
int main() {
    volatile double exponent=0.;
    return std::bit_cast<std::uint64_t>(airack::gain_math::canonicalPower10(exponent))==0x3ff0000000000000ull ? 0 : 1;
}
