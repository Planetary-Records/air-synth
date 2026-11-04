#pragma once

#include "StudioSkin.h"

namespace airack::studio
{
class Lettering
{
public:
    explicit Lettering (juce::Typeface::Ptr f) : face (std::move (f))
    {
        capAt100 = path (font (100.0f), "H").getBounds().getHeight();
        advanceAt100 = juce::GlyphArrangement::getStringWidth (font (100.0f), "00") - juce::GlyphArrangement::getStringWidth (font (100.0f), "0");
    }

    juce::Font fontForCap (float cap) const { return font (100.0f * cap / capAt100); }
    float advance (float cap, float hscale) const { return advanceAt100 * cap / capAt100 * hscale; }

    static juce::Path path (const juce::Font& f, const String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, s, 0.0f, 0.0f);
        juce::Path p;
        ga.createPath (p);
        return p;
    }

    juce::Typeface::Ptr face;

private:
    juce::Font font (float h) const { return juce::Font (juce::FontOptions (face).withHeight (h)); }
    float capAt100 = 1.0f, advanceAt100 = 1.0f;
};

struct Placed
{
    juce::Path path;
    String shown;
    float hscale = 1.0f, pen = 0.0f, advance = 0.0f;
};

/** Text on the line's character grid. Left lines keep the pen; right and centre lines keep the drawn text's last or
    middle cell. A left line too long for max_right is squeezed to the display's minimum scale, then elided. A character
    the face has no glyph for letters as '?'. */
Placed place (const Lettering& let, const Line& l, const String& text, float minHscale);

/** The line with the cap, horizontal scale, pen and baseline at which its drawn text's outline fills its ink box, for
    a skin that gives a line by its drawn text and ink alone. */
Line fitLine (const Lettering& let, Line drawn);

/** Lit text on a glass: glyph coverage in the ink colour, dimmed away from the lattice's dots by the ripple.
    `dst` covers the plate from `origin` at `scale` pixels per design pixel. The ripple is sampled half a design pixel
    up and left of each pixel's centre, which is where the prototype's 1x measurements put it. */
void drawLit (juce::Image& dst, Pt origin, const juce::Path& path, const Lattice& l, float scale = 1.0f);

void drawSprite (juce::Graphics& g, const Asset& a, Pt at, float scale = 1.0f);

// Reuse the INIT glass crease around digital panels, preserving its corners.
void drawDisplayRim(juce::Graphics&,const juce::Image&,juce::Rectangle<int>);

/** The pixels per design pixel a glass renders at in this context: the device's, never below 1, in steps of 1/64. */
float renderScaleFor (juce::Graphics& g);

/** The plate's area `r`, resampled to `scale` pixels per design pixel. */
juce::Image scaledArea (const juce::Image& plate, juce::Rectangle<int> r, float scale);
} // namespace airack::studio
