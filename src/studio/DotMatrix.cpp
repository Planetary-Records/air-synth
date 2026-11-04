#include "DotMatrix.h"

namespace airack::studio
{
void drawDisplayRim(juce::Graphics& g,const juce::Image& glass,juce::Rectangle<int> r){
 if(!glass.isValid()||r.getWidth()<14||r.getHeight()<14)return;
 constexpr int border=7;const int w=glass.getWidth(),h=glass.getHeight();
 const int xs[]{0,border,w-border,w},ys[]{0,border,h-border,h};
 const int xd[]{r.getX(),r.getX()+border,r.getRight()-border,r.getRight()},yd[]{r.getY(),r.getY()+border,r.getBottom()-border,r.getBottom()};
 g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
 for(int row=0;row<3;++row)for(int col=0;col<3;++col)if(row!=1||col!=1)
  g.drawImage(glass,xd[col],yd[row],xd[col+1]-xd[col],yd[row+1]-yd[row],xs[col],ys[row],xs[col+1]-xs[col],ys[row+1]-ys[row]);
}

static float smoothstep (float a, float b, float x)
{
    const auto t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

/** Without this, JUCE would letter a missing character from a fallback system font. */
static String withFaceGlyphs (const juce::Typeface& face, const String& text)
{
    String out;
    for (auto t = text.getCharPointer(); ! t.isEmpty();)
    {
        const auto c = t.getAndAdvance();
        out += face.getNominalGlyphForCodepoint (c).has_value() ? String::charToString (c) : String ("?");
    }
    return out;
}

Placed place (const Lettering& let, const Line& l, const String& text, float minHscale)
{
    Placed out;
    auto hs = l.hscale;
    auto shown = withFaceGlyphs (*let.face, text);
    auto adv = let.advance (l.cap, hs);
    const auto n = l.text.length(), m = shown.length();
    auto pen = l.penX;
    if (l.align == "right")
        pen = l.penX + (float) (n - m) * adv;
    else if (l.align == "centre")
        pen = l.penX + (float) (n - m) * adv * 0.5f;
    else if (l.maxRight > 0.0f && pen + (float) m * adv > l.maxRight)
    {
        hs = std::max (minHscale, hs * (l.maxRight - pen) / ((float) m * adv));
        adv = let.advance (l.cap, hs);
        if (pen + (float) m * adv > l.maxRight)
        {
            const auto keep = std::max (1, (int) std::floor ((l.maxRight - pen) / adv) - 1);
            shown = shown.substring (0, keep).trimEnd() + String::charToString ((juce::juce_wchar) 0x2026);
        }
    }
    out.path = Lettering::path (let.fontForCap (l.cap), shown);
    out.path.applyTransform (juce::AffineTransform::scale (hs, 1.0f).translated (pen, l.baseline));
    out.shown = shown;
    out.hscale = hs;
    out.pen = pen;
    out.advance = adv;
    return out;
}

Line fitLine (const Lettering& let, Line l)
{
    const auto b = Lettering::path (let.fontForCap (100.0f), withFaceGlyphs (*let.face, l.text)).getBounds();
    if (b.isEmpty() || l.ink.isEmpty())
        return l;
    l.cap = 100.0f * l.ink.getHeight() / b.getHeight();
    const auto k = l.cap / 100.0f;
    l.hscale = l.ink.getWidth() / (b.getWidth() * k);
    l.penX = l.ink.getX() - b.getX() * k * l.hscale;
    l.baseline = l.ink.getY() - b.getY() * k;
    return l;
}

void drawLit (juce::Image& dst, Pt origin, const juce::Path& path, const Lattice& l, float scale)
{
    juce::Image cov (juce::Image::SingleChannel, dst.getWidth(), dst.getHeight(), true);
    {
        juce::Graphics g (cov);
        g.setColour (juce::Colours::white);
        g.fillPath (path, juce::AffineTransform::translation (-origin.x, -origin.y).scaled (scale));
    }
    const juce::Image::BitmapData cd (cov, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dd (dst, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < dst.getHeight(); ++y)
        for (int x = 0; x < dst.getWidth(); ++x)
        {
            auto a = cd.getPixelColour (x, y).getFloatAlpha();
            if (a <= 0.0f)
                continue;
            const auto px = origin.x + ((float) x + 0.5f) / scale - 0.5f, py = origin.y + ((float) y + 0.5f) / scale - 0.5f;
            a *= 1.0f - l.ripple * smoothstep (l.rippleCore, l.rippleEdge, latticeDistance (l, px, py));
            dd.setPixelColour (x, y, dd.getPixelColour (x, y).interpolatedWith (l.ink, a));
        }
}

void drawSprite (juce::Graphics& g, const Asset& a, Pt at, float scale)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (a.image, juce::AffineTransform::translation (-a.anchor.x, -a.anchor.y).scaled (scale).translated (at.x, at.y));
}

float renderScaleFor (juce::Graphics& g)
{
    return std::max (1.0f, std::round (g.getInternalContext().getPhysicalPixelScaleFactor() * 64.0f) / 64.0f);
}

juce::Image scaledArea (const juce::Image& plate, juce::Rectangle<int> r, float scale)
{
    if (scale == 1.0f)
        return plate.getClippedImage (r).createCopy();
    // Resampling from a margin around r gives the edge pixels their real neighbours.
    const auto src = r.expanded (2).getIntersection (plate.getBounds());
    juce::Image out (plate.getFormat(), (int) std::ceil ((float) r.getWidth() * scale), (int) std::ceil ((float) r.getHeight() * scale), true);
    juce::Graphics g (out);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (plate.getClippedImage (src),
                            juce::AffineTransform::translation ((float) (src.getX() - r.getX()), (float) (src.getY() - r.getY())).scaled (scale));
    return out;
}
} // namespace airack::studio
