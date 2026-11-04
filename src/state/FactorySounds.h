#pragma once
#include "SoundDocument.h"
#include <span>

namespace airack {
struct FactorySoundInfo { const char* name; const char* description; };
std::span<const FactorySoundInfo> factorySounds() noexcept;
// Original control settings for built-in racks only; no samples, pattern or downloaded dependencies.
bool makeFactorySound(std::size_t index, SoundDocument&);
}
