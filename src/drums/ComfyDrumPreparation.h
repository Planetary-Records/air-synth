#pragma once
#include "DrumPreparation.h"
#include "../companion/ComfyHttpWindows.h"

namespace airack::drums {
bool inspectComfyDrumSource(std::unique_ptr<companion::ComfyRawFile>&,
    const GeneratedOrigin&, const std::atomic_bool&,
    std::unique_ptr<DrumSource>&, PreparationError&) noexcept;
}
