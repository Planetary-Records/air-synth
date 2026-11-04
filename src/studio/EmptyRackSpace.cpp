#include "EmptyRackSpace.h"

#include "HouseUnit.h"

namespace airack::studio
{
namespace
{
// EIA-310, in millimetres: a unit's height; a front rail's face and its holes, from its end of the panel; the holes'
// side; and their rows, from a unit's top.
constexpr float unitMm = 44.45f, faceIn = 0.8125f, faceOut = 16.6875f, holeIn = 8.75f, holeSide = 9.5f;
constexpr float holeRows[] { 6.35f, 22.225f, 38.1f };

// The lettering's and the hint's cap heights, in stage pixels, and the lettering's tracking (of its font's height).
constexpr float letterCap = 16.0f, hintCap = 13.0f, tracking = 0.22f;
// Taller than this, the hint stands under the lettering.
constexpr float stackedAbove = 100.0f;

const juce::Colour column (0xff141517), columnEdge (0xff2c2d31);
const juce::Colour seam (0xff040405), face (0xff18191b), faceLit (0xff27282b), hole (0xff030304), holeLip (0xff2a2b2e);
const juce::Colour wall (0xff0b0c0d), wallLit (0xff141517), rearSlot (0xff020203);
const juce::Colour back (0xff1c1d1f), shade (0xff060708), floorTop (0xff121315), floorLip (0xff2c2d30);
const juce::Colour ink (0xffc9cacd), inkLit (0xfff2f2ee), inkPressed (0xff8a8c92), outline (0xff6f7176);

// The slot's outline: 2 px dashes run along each edge from corner to corner, so that each corner is a dash's.
void dashOutline (juce::Graphics& g, Rect r)
{
    constexpr float dash = 12.0f, gap = 8.0f, t = 2.0f;
    const auto along = [] (float length, auto&& at)
    {
        const auto n = juce::jmax (2, juce::roundToInt ((length + gap) / (dash + gap)));
        for (int i = 0; i < n; ++i)
            at ((float) i * (length - dash) / (float) (n - 1));
    };
    along (r.getWidth(), [&] (float x)
    {
        g.fillRect (Rect (r.getX() + x, r.getY(), dash, t));
        g.fillRect (Rect (r.getX() + x, r.getBottom() - t, dash, t));
    });
    along (r.getHeight(), [&] (float y)
    {
        g.fillRect (Rect (r.getX(), r.getY() + y, t, dash));
        g.fillRect (Rect (r.getRight() - t, r.getY() + y, t, dash));
    });
}
} // namespace

EmptyRackSpace::EmptyRackSpace (const Model& m) : add (*this), model (m)
{
    setComponentID ("rack.space");
    add.setComponentID ("rack.add");
    add.setTitle ("Add rack");
    add.setButtonText (m.layout["add_rack"]["text"].toString());
    addAndMakeVisible (add);
    hint.setComponentID ("rack.hint");
    hint.setName ("Rack hint");
    hint.setInterceptsMouseClicks (false, false);
    hint.setWantsKeyboardFocus (false);
    hint.setColour (juce::Label::textColourId, juce::Colour (0xff9a9ca1));
    hint.setFont (Lettering (m.screenFace).fontForCap (hintCap));
    hint.cap = hintCap;
    addAndMakeVisible (hint);
}

void EmptyRackSpace::setRack (float s, float d)
{
    scale = s;
    depth = d;
    layout();
    repaint();
}

void EmptyRackSpace::setHint (const String& text)
{
    hint.setText (text, juce::dontSendNotification);
    layout();
    repaint();
}

bool EmptyRackSpace::stacked() const { return (float) getHeight() > stackedAbove; }

void EmptyRackSpace::resized() { layout(); }

float EmptyRackSpace::rightRail() const
{
    const auto panel = (float) HouseUnit::railW + model.house.panelWidth;
    return (float) getWidth() < std::round (panel * scale) ? (float) getWidth() / scale : panel;
}

void EmptyRackSpace::layout()
{
    const auto mm = model.house.unitHeight / unitMm, h = (float) getHeight();
    const auto left = (float) HouseUnit::railW, right = rightRail();
    const auto inner = Rect::leftTopRightBottom ((left + faceOut * mm) * scale, 0.0f, (right - faceOut * mm) * scale, h);
    add.setBounds (inner.toNearestIntEdges());

    holeRects.clear();
    const auto side = holeSide * mm * scale, pitch = model.house.unitHeight * scale;
    for (auto u = juce::jmax (0, (int) std::floor (depth / pitch) - 1); (float) u * pitch < depth + h + side; ++u)
        for (const auto row : holeRows)
        {
            const auto y = ((float) u * unitMm + row) * mm * scale - depth;
            if (y + side / 2.0f <= 0.0f || y - side / 2.0f >= h)
                continue;
            for (const auto x : { left + holeIn * mm, right - holeIn * mm })
                holeRects.push_back (Rect (side, side).withCentre ({ x * scale, y }));
        }

    const Lettering caps (model.screenFace);
    auto letterPath = Lettering::path (caps.fontForCap (letterCap).withExtraKerningFactor (tracking), add.getButtonText());
    const auto box = letterPath.getBounds();
    const auto hintW = std::ceil (juce::GlyphArrangement::getStringWidth (hint.getFont(), hint.getText())) + 2.0f;
    const auto hintH = std::ceil (hint.getFont().getHeight() + hint.getFont().getDescent());
    const auto centre = inner.getCentre();
    Rect hintBox;
    if (stacked())
    {
        const auto gap = 14.0f, top = centre.y - (box.getHeight() + gap + hintH) / 2.0f;
        letters = box.withCentre ({ centre.x, top + box.getHeight() / 2.0f });
        hintBox = Rect (hintW, hintH).withCentre ({ centre.x, letters.getBottom() + gap + hintH / 2.0f });
    }
    else
    {
        const auto gap = 28.0f, start = centre.x - (box.getWidth() + gap + hintW) / 2.0f;
        letters = box.withCentre ({ start + box.getWidth() / 2.0f, centre.y });
        hintBox = Rect (hintW, hintH).withCentre ({ letters.getRight() + gap + hintW / 2.0f, centre.y });
    }
    letterPath.applyTransform (juce::AffineTransform::translation (letters.getX() - box.getX(), letters.getY() - box.getY()));
    shape = letterPath;
    hint.setBounds (hintBox.toNearestIntEdges());

    const auto inset = stacked() ? Pt (14.0f, 14.0f) : Pt (10.0f, 7.0f);
    slotRect = inner.reduced (inset.x, inset.y).toNearestIntEdges().toFloat();
}

void EmptyRackSpace::paint (juce::Graphics& g)
{
    const auto mm = model.house.unitHeight / unitMm, h = (float) getHeight();
    const auto left = (float) HouseUnit::railW, right = rightRail();

    // The house unit's fold rail, continued.
    g.setColour (column);
    g.fillRect (Rect (0.0f, 0.0f, left * scale, h));
    g.setColour (columnEdge);
    g.fillRect (Rect ((left - 2.0f) * scale, 0.0f, 2.0f * scale, h));
    g.setColour (seam);
    g.fillRect (Rect::leftTopRightBottom (left * scale, 0.0f, (float) getWidth(), h));

    // The rack's back, shaded by the side walls and the unit above, with the floor at its foot.
    const auto faceW = (faceOut - faceIn) * mm * scale, wallW = 0.9f * faceW;
    const auto inner = Rect::leftTopRightBottom ((left + faceOut * mm) * scale, 0.0f, (right - faceOut * mm) * scale, h);
    const auto backWall = inner.reduced (wallW, 0.0f);
    g.setColour (back);
    g.fillRect (backWall);
    const auto spread = 1.2f * faceW;
    g.setGradientFill (juce::ColourGradient (shade, backWall.getX(), 0.0f, back.withAlpha (0.0f), backWall.getX() + spread, 0.0f, false));
    g.fillRect (backWall.withWidth (spread));
    g.setGradientFill (juce::ColourGradient (shade, backWall.getRight(), 0.0f, back.withAlpha (0.0f), backWall.getRight() - spread, 0.0f, false));
    g.fillRect (backWall.withLeft (backWall.getRight() - spread));
    const auto floorH = juce::jlimit (4.0f, 14.0f, 0.06f * h);
    juce::Path ledge;
    ledge.addQuadrilateral (backWall.getX(), h - floorH, backWall.getRight(), h - floorH, inner.getRight(), h, inner.getX(), h);
    g.setColour (floorTop);
    g.fillPath (ledge);
    g.setColour (floorLip);
    g.fillRect (Rect (inner.getX(), h - 1.0f, inner.getWidth(), 1.0f));
    g.setColour (seam);
    g.fillRect (Rect (backWall.getX(), h - floorH - 1.0f, backWall.getWidth(), 1.0f));

    // The side walls inside the front rails, with the rear rails' slots on the holes' rows.
    for (const auto x : { inner.getX(), inner.getRight() - wallW })
    {
        const auto w = Rect (x, 0.0f, wallW, h);
        g.setColour (wall);
        g.fillRect (w);
        g.setColour (wallLit);
        g.fillRect (x < inner.getCentreX() ? w.withWidth (2.0f) : w.withLeft (w.getRight() - 2.0f));
        g.setColour (rearSlot);
        for (const auto& r : holeRects)
            if ((r.getCentreX() < inner.getCentreX()) == (x < inner.getCentreX()))
                g.fillRoundedRectangle (Rect (0.42f * wallW, 0.55f * r.getHeight()).withCentre ({ w.getCentreX(), r.getCentreY() }), 1.0f);
    }

    const auto top = juce::jmin (22.0f, 0.4f * h);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.75f), 0.0f, 0.0f, juce::Colours::transparentBlack, 0.0f, top, false));
    g.fillRect (inner.withHeight (top));

    // The front rails, with their square holes.
    for (const auto x : { left + faceIn * mm, right - faceOut * mm })
    {
        const auto r = Rect (x * scale, 0.0f, faceW, h);
        g.setColour (face);
        g.fillRect (r);
        g.setColour (faceLit);
        g.fillRect (r.withWidth (1.0f));
        g.setColour (seam);
        g.fillRect (r.withLeft (r.getRight() - 1.5f));
    }
    for (const auto& r : holeRects)
    {
        g.setColour (holeLip);
        g.fillRoundedRectangle (r.translated (0.0f, 1.0f), 1.5f);
        g.setColour (hole);
        g.fillRoundedRectangle (r, 1.5f);
    }
}

EmptyRackSpace::Add::Add (EmptyRackSpace& s) : KeyFocus<juce::Button> ("Add rack"), space (s) {}

void EmptyRackSpace::Add::paintButton (juce::Graphics& g, bool over, bool down)
{
    g.addTransform (juce::AffineTransform::translation (-getPosition().toFloat()));
    const auto held = getToggleState();
    if (held || over)
    {
        g.setColour (held ? ink : outline);
        dashOutline (g, space.slotRect);
    }
    g.setColour (down ? inkPressed : over || held ? inkLit : ink);
    g.fillPath (space.shape);
    if (ringed())
    {
        g.setColour (accent);
        g.drawRoundedRectangle (space.letters.getUnion (space.hint.getBounds().toFloat()).expanded (10.0f, 6.0f), 5.0f, 2.0f);
    }
}

void EmptyRackSpace::Hint::paint (juce::Graphics& g)
{
    const auto f = getFont();
    juce::GlyphArrangement line;
    const auto width = juce::GlyphArrangement::getStringWidth (f, getText());
    line.addLineOfText (f, getText(), ((float) getWidth() - width) / 2.0f, ((float) getHeight() + cap) / 2.0f);
    g.setColour (findColour (textColourId));
    line.draw (g);
}
} // namespace airack::studio
