#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>
#include <optional>

// The PROGRAMMER display's values (window/skin/app/Main.cpp's shownValue), and their entry in the same form (the
// spec's decision 2). Values are physical, in the registry's units.
namespace airack::studio
{
/** A parameter's value as the display shows it: three significant figures; Hz as kHz from 999.5 Hz; seconds as ms
    below 0.9995 s, and ms as seconds from 999.5 ms; a sign on dB, st, cent and oct, and on a bipolar value without a
    unit; a choice's name in capitals. */
juce::String formatParameter (std::size_t index, float physical);

/** The unit formatParameter shows for that value ("kHz", "ms", "dB"); empty for a choice or a value without one. */
juce::String unitShown (std::size_t index, float physical);

/** A typed value read as the display's form gives it: a number with an optional prefix (k or m, on Hz and seconds)
    and an optional unit (Hz, s, dB, st, cent, oct or beats, in any case), or a choice's name. A bare number is in
    `shownUnit`, the unit the display showed when the entry opened. Nothing for a value out of range, not finite, in
    another unit, or not a value; a value just past the range that the display shows as the range's end is that end. */
std::optional<float> parseParameter (std::size_t index, const juce::String& text, const juce::String& shownUnit);
} // namespace airack::studio
