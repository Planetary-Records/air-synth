#pragma once

#include <functional>
#include <optional>

// The faceplates' motion (docs/superpowers/specs/2026-10-13-faceplate-motion.md): the clock each reveal moves by,
// Windows' reduced-motion setting, and the easing.
namespace airack::studio::motion
{
/** Milliseconds on the motion's clock: a steady clock, or the tests' clock. */
double now();
/** Whether Windows asks for reduced motion, its Animation effects being off, or the tests' choice. */
bool reduced();
/** For tests: a clock read instead of the steady one, and reduced motion forced on or off; empty restores each. */
void setForTests (std::function<double()> clock, std::optional<bool> reduced);
/** The eased position at fraction t of a move, clamped to 0 to 1: 4t^3 up to the middle and mirrored after it, so
    ease (1 - t) == 1 - ease (t) and a move turned back part-way continues from where it is drawn. */
float ease (float t);
} // namespace airack::studio::motion
