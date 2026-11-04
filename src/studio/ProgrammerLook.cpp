#include "ProgrammerLook.h"

#include <algorithm>
#include <cmath>

namespace airack::studio
{
static float num (const juce::var& v) { return (float) (double) v; }
static juce::Colour grey (int level) { return juce::Colour::greyLevel ((float) level / 255.0f); }

static const juce::Identifier roleProperty ("programmer.text"), lockedProperty ("programmer.locked"), legendProperty ("programmer.legend");
// The plate's bare body: under the display glass, inside the rails, clear of the two bottom screws.
static const juce::Rectangle<int> bareBody (110, 220, 1320, 516);
// The display's glass with its lip, nine-sliced with `glassBorder` px borders.
static const juce::Rectangle<int> displayGlass (93, 146, 1361, 70);
static constexpr int glassBorder = 10;
static const juce::Colour veil (0xff2b2c2e);
static constexpr int retainedGrey = 120;

RowLayout layoutRow (const juce::var& programmer, const std::vector<int>& counts, float left, float right)
{
    RowLayout out;
    if (counts.empty())
        return out;
    const auto L = programmer["body"]["layout"];
    int n = 0;
    for (auto c : counts)
        n += std::max (1, c);
    const auto lastN = std::max (1, counts.back());
    auto pitch = num (L["pitch"]), gap = num (L["gap"]), lastPitch = pitch;
    const auto x0 = left + num (L["first_dx"]), room = right - num (L["last_inset"]) - x0;
    const auto span = [&] { return (float) (n - lastN) * pitch + (float) (lastN - 1) * lastPitch + (float) (counts.size() - 1) * gap; };
    if (lastN > 1 && span() > room)
        lastPitch = std::max (pitch * num (L.getProperty ("squeeze_min", 0.8)), lastPitch - (span() - room) / (float) (lastN - 1));
    if (n > 1 && span() > room)
    {
        const auto s = room / span();
        pitch *= s;
        gap *= s;
        lastPitch *= s;
    }
    auto x = x0;
    std::vector<std::pair<float, float>> extents;
    for (size_t si = 0; si < counts.size(); ++si)
    {
        const auto step = si + 1 == counts.size() ? lastPitch : pitch;
        if (si > 0)
            x += pitch + gap;
        const auto first = x;
        out.pitches.push_back (step);
        out.centres.emplace_back();
        for (int ki = 0; ki < counts[si]; ++ki)
        {
            if (ki > 0)
                x += step;
            out.centres.back().push_back (x);
        }
        extents.push_back ({ first, x });
    }
    for (size_t si = 0; si + 1 < extents.size(); ++si)
        out.dividers.push_back (std::round ((extents[si].second + extents[si + 1].first) * 0.5f));
    return out;
}

TitleRow layoutTitles (const juce::var& programmer, const Faces& faces, const juce::StringArray& names, const std::vector<float>& dividers,
                       float left, float right, float baseline, float ruleY)
{
    TitleRow out;
    const auto L = programmer["body"]["layout"];
    const auto style = styleOf (programmer, "title");
    const auto underY = std::round (baseline + num (L["underline_dy"]));
    const auto rule = std::round (ruleY);
    for (int si = 0; si < names.size(); ++si)
    {
        const auto from = si == 0 ? left : dividers[(size_t) si - 1] + 2.0f;
        const auto end = si + 1 == names.size() ? right : dividers[(size_t) si] - num (L["rule_end"]);
        const auto t = setText (faces, style, names[si], from + num (si == 0 ? L["title_dx_first"] : L["title_dx"]), baseline, end, 0.8f);
        out.titles.push_back ({ names[si], t.path, t.ink() });
        out.lines.push_back ({ Rect::leftTopRightBottom (std::round (t.ink().getX()) - 1.0f, underY, std::round (t.ink().getRight()) + 1.0f, underY + 1.0f), false });
        const auto r0 = std::round (t.ink().getRight() - 1.0f + num (L["rule_gap"])), r1 = std::round (end);
        if (r1 > r0 + 4.0f)
            out.lines.push_back ({ Rect::leftTopRightBottom (r0, rule, r1, rule + 1.0f), false });
    }
    return out;
}

//============================================================================== ProgrammerLook

ProgrammerLook::ProgrammerLook (const Model& m)
    : model (m), caps (m.screenFace), mono (m.face), faces { caps, mono },
      labelStyle (styleOf (m.programmer, "label")), titleStyle (styleOf (m.programmer, "title")), legendStyle (styleOf (m.programmer, "legend")),
      optionStyle (styleOf (m.screens, "label")), bodyStyle (styleOf (m.screens, "body")), ink (m.displays.at ("advanced").lattice.ink),
      face (m.plates.at ("advanced").getClippedImage (bareBody)), glass (m.plates.at ("advanced").getClippedImage (displayGlass))
{
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1d1e21));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::textColourId, ink);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, ink);
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::ComboBox::textColourId, ink);
    setColour (juce::Label::textColourId, grey (labelStyle.grey));
}

void ProgrammerLook::setRole (juce::Label& label, Text role) { label.getProperties().set (roleProperty, (int) role); }
void ProgrammerLook::setLocked (juce::Label& label, bool on)
{
    if (locked (label) == on)
        return;
    label.getProperties().set (lockedProperty, on);
    label.repaint();
}
bool ProgrammerLook::locked (const juce::Label& label) { return (bool) label.getProperties()[lockedProperty]; }
void ProgrammerLook::setLegend (juce::Component& field, const String& text) { field.getProperties().set (legendProperty, text); }
String ProgrammerLook::legend (const juce::Component& field) { return field.getProperties()[legendProperty].toString(); }

bool ProgrammerLook::ringed (const juce::Component& c)
{
    auto* r = dynamic_cast<const Ringed*> (&c);
    return r != nullptr && r->ringed();
}

void ProgrammerLook::drawFace (juce::Graphics& g, Rect area) const
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (face, juce::AffineTransform::scale (area.getWidth() / (float) face.getWidth(), area.getHeight() / (float) face.getHeight())
                                      .translated (area.getX(), area.getY()));
}

void ProgrammerLook::drawGlass (juce::Graphics& g, Rect area) const
{
    const auto r = area.toNearestIntEdges();
    const auto b = std::min ({ glassBorder, r.getWidth() / 2, r.getHeight() / 2 });
    const int sw = glass.getWidth(), sh = glass.getHeight();
    const int xs[] { 0, b, sw - b, sw }, ys[] { 0, b, sh - b, sh };
    const int xd[] { r.getX(), r.getX() + b, r.getRight() - b, r.getRight() }, yd[] { r.getY(), r.getY() + b, r.getBottom() - b, r.getBottom() };
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            if (xd[col + 1] > xd[col] && yd[row + 1] > yd[row])
                g.drawImage (glass, xd[col], yd[row], xd[col + 1] - xd[col], yd[row + 1] - yd[row], xs[col], ys[row], xs[col + 1] - xs[col], ys[row + 1] - ys[row]);
}

void ProgrammerLook::drawEngraving (juce::Graphics& g, const Engraving& line) const
{
    const auto& L = model.programmer["body"]["line"];
    g.setColour (grey ((int) L["highlight"]));
    g.fillRect (line.line);
    g.setColour (grey ((int) L["shadow"]));
    g.fillRect (line.line.translated (line.upright ? 1.0f : 0.0f, line.upright ? 0.0f : 1.0f));
}

void ProgrammerLook::drawTitles (juce::Graphics& g, const TitleRow& row) const
{
    for (const auto& l : row.lines)
        drawEngraving (g, l);
    g.setColour (grey (titleStyle.grey));
    for (const auto& t : row.titles)
        g.fillPath (t.path);
}

void ProgrammerLook::drawLegend (juce::Graphics& g, const juce::Component& field) const
{
    const auto text = legend (field);
    if (text.isEmpty() || ! field.isVisible())
        return;
    const auto b = field.getBounds().toFloat();
    const auto t = setText (faces, legendStyle, text.toUpperCase(), b.getX(), b.getY() - 8.0f, b.getRight(), 0.8f);
    g.setColour (grey (field.isEnabled() ? legendStyle.grey : retainedGrey));
    g.fillPath (t.path);
}

juce::Font ProgrammerLook::fieldFont() const { return mono.fontForCap (bodyStyle.cap).withHorizontalScale (bodyStyle.hscale); }

void ProgrammerLook::drawDashes (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float on, float off, float width)
{
    juce::Path dashed;
    const float pattern[] { on, off };
    juce::PathStrokeType (width).createDashedStroke (dashed, path, pattern, 2);
    g.setColour (colour);
    g.fillPath (dashed);
}

//============================================================================== sliders

float ProgrammerLook::knobAngle (float proportion) const
{
    return juce::degreesToRadians ((2.0f * proportion - 1.0f) * num (model.programmer["knob"]["sweep"][1]));
}

float ProgrammerLook::knobRadius (const juce::Slider& slider) const
{
    const auto& body = model.asset (model.programmer["knob"]["body"].toString());
    return num (body.spec["skirt_r"]) * (float) slider.getWidth() / (float) body.image.getWidth();
}

void ProgrammerLook::drawRotarySlider (juce::Graphics& g, int, int, int, int, float sliderPos, float, float, juce::Slider& slider)
{
    const auto K = model.programmer["knob"];
    const auto& body = model.asset (K["body"].toString());
    const auto& pointer = model.asset (K["pointer"].toString());
    const auto k = (float) slider.getWidth() / (float) body.image.getWidth();
    const Pt c ((float) slider.getWidth() * 0.5f, (float) slider.getHeight() * 0.5f);
    drawSprite (g, body, c, k);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (pointer.image, juce::AffineTransform::translation (-pointer.anchor.x, -pointer.anchor.y)
                                               .scaled (k)
                                               .rotated (knobAngle (sliderPos))
                                               .translated (c.x, c.y));
    const auto skirt = knobRadius (slider);
    const auto skirtFace = Rect (2.0f * skirt, 2.0f * skirt).withCentre (c);
    if (! slider.isEnabled())
    {
        g.setColour (veil.withAlpha (0.55f));
        g.fillEllipse (skirtFace.expanded (1.0f));
    }
    if (ringed (slider))
    {
        g.setColour (accent);
        g.drawEllipse (skirtFace.expanded (4.0f), 2.0f);
    }
}

// A fader's cap is the PROGRAMMER's button at three quarters of its size; at 1x the slider is 44 px wide.
static constexpr float capScale = 0.75f, faderWidth = 44.0f;

Rect ProgrammerLook::faderCap (const juce::Slider& slider, float sliderPos) const
{
    const auto sp = model.programmer["subpages"];
    const auto s = capScale * (float) slider.getWidth() / faderWidth;
    const auto w = num (sp["face"][0]) * s, h = num (sp["face"][1]) * s;
    return { (float) slider.getWidth() * 0.5f - w * 0.5f, sliderPos - h * 0.5f, w, h };
}

void ProgrammerLook::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    const auto sp = model.programmer["subpages"];
    const auto s = capScale * (float) slider.getWidth() / faderWidth;
    const auto cx = (float) slider.getWidth() * 0.5f;
    drawEngraving (g, { Rect (std::round (cx), (float) y, 1.0f, (float) height), true });
    const auto cap = faderCap (slider, sliderPos);
    drawSprite (g, model.asset (sp["button"].toString()), cap.getTopLeft(), s);
    drawFaceStates (g, cap, slider.isEnabled(), slider.isMouseOverOrDragging() && ! slider.isMouseButtonDown(), slider.isMouseButtonDown(), ringed (slider), veil);
}

int ProgrammerLook::getSliderThumbRadius (juce::Slider& slider)
{
    if (slider.getSliderStyle() != juce::Slider::LinearVertical)
        return LookAndFeel_V4::getSliderThumbRadius (slider);
    return juce::roundToInt (num (model.programmer["subpages"]["face"][1]) * capScale * (float) slider.getWidth() / faderWidth * 0.5f);
}

//============================================================================== buttons

// At 1x a button is 84 x 96 and a toggle without a legend 56 x 81; the face's top is 30 down, under the lit LED's glow.
static constexpr float buttonHeight = 96.0f, bareToggleHeight = 81.0f, faceTop = 30.0f;

void ProgrammerLook::drawSubpage (juce::Graphics& g, juce::Button& button, float scale, bool lit, const String& legend, bool highlighted, bool down) const
{
    const auto sp = model.programmer["subpages"];
    const auto w = num (sp["face"][0]) * scale, h = num (sp["face"][1]) * scale;
    const auto faceRect = Rect (((float) button.getWidth() - w) * 0.5f, faceTop * scale, w, h);
    drawSprite (g, model.asset (sp["button"].toString()), faceRect.getTopLeft(), scale);
    const Pt led (faceRect.getCentreX(), faceRect.getY() + num (sp["led_dy"]) * scale);
    drawSprite (g, model.asset (sp["led"].toString()), led, scale);
    if (lit)
        drawSprite (g, model.asset (sp["lit"].toString()), led, scale);
    if (legend.isNotEmpty())
    {
        auto style = legendStyle;
        style.cap *= scale;
        style.tracking *= scale;
        const auto baseline = faceRect.getY() + (num (sp["legend_baseline"]) - num (sp["top"])) * scale;
        const auto t = setText (faces, style, legend.toUpperCase(), 0.0f, baseline, (float) button.getWidth(), 0.8f, faceRect.getCentreX());
        g.setColour (grey (button.isEnabled() ? legendStyle.grey : retainedGrey));
        g.fillPath (t.path);
    }
    drawFaceStates (g, faceRect, button.isEnabled(), highlighted, down, ringed (button), veil);
}

void ProgrammerLook::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    if ((bool) button.getProperties()["programmer.compactButton"])
    {
        drawGlass (g, button.getLocalBounds().toFloat());
        if (highlighted || down) { g.setColour (juce::Colours::white.withAlpha (down ? .07f : .035f)); g.fillRect (button.getLocalBounds().reduced (2)); }
        if (ringed (button)) { g.setColour (accent); g.drawRect (button.getLocalBounds().reduced (1)); }
        return;
    }
    drawSubpage (g, button, (float) button.getHeight() / buttonHeight, button.getToggleState(), button.getButtonText(), highlighted, down);
}

void ProgrammerLook::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    if (! (bool) button.getProperties()["programmer.compactButton"]) return;
    g.setColour (juce::Colour (0xffd4dfd7));
    if (button.getButtonText() == "<" || button.getButtonText() == ">")
    {
        const auto centre = button.getLocalBounds().toFloat().getCentre();
        const float direction = button.getButtonText() == "<" ? -1.f : 1.f;
        juce::Path arrow; arrow.startNewSubPath (centre.x - direction * 2, centre.y - 5);
        arrow.lineTo (centre.x + direction * 3, centre.y); arrow.lineTo (centre.x - direction * 2, centre.y + 5);
        g.strokePath (arrow, juce::PathStrokeType (1.5f)); return;
    }
    const bool disclosure = (bool) button.getProperties()["programmer.disclosure"];
    auto text = button.getLocalBounds().reduced (8, 2);
    if (disclosure) text.removeFromRight (18);
    g.setFont (caps.fontForCap (16));
    g.drawFittedText (button.getButtonText(), text, juce::Justification::centred, 1, .8f);
    if (disclosure)
    {
        const float x = (float) button.getWidth() - 14, y = (float) button.getHeight() * .5f;
        const float direction = button.getToggleState() ? -1.f : 1.f;
        juce::Path arrow; arrow.startNewSubPath (x - 4, y - direction * 2);
        arrow.lineTo (x, y + direction * 2); arrow.lineTo (x + 4, y - direction * 2);
        g.strokePath (arrow, juce::PathStrokeType (1.2f));
    }
}

void ProgrammerLook::drawToggleButton (juce::Graphics& g, juce::ToggleButton& toggle, bool highlighted, bool down)
{
    const auto legend = toggle.getButtonText();
    drawSubpage (g, toggle, (float) toggle.getHeight() / (legend.isEmpty() ? bareToggleHeight : buttonHeight), toggle.getToggleState(), legend,
                 highlighted, down);
}

//============================================================================== glass fields

void ProgrammerLook::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = Rect ((float) width, (float) height);
    drawGlass (g, area);
    const auto cx = (float) width - 15.0f, cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.addTriangle (cx - 3.0f, cy - 2.0f, cx + 3.0f, cy - 2.0f, cx, cy + 2.0f);
    g.setColour (box.isEnabled() ? ink : grey (retainedGrey));
    g.fillPath (arrow);
    if (ringed (box))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (area.reduced (1.0f), 4.0f, 2.0f);
    }
}

void ProgrammerLook::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (0, 0, std::max (0, box.getWidth() - 24), box.getHeight());
    label.setFont (getComboBoxFont (box));
}

juce::Font ProgrammerLook::getComboBoxFont (juce::ComboBox&) { return caps.fontForCap (optionStyle.cap); }

void ProgrammerLook::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
{
    drawGlass (g, Rect ((float) width, (float) height));
}

void ProgrammerLook::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled() || ! editor.hasKeyboardFocus (true))
        return;
    g.setColour (accent);
    g.drawRoundedRectangle (Rect ((float) width, (float) height).reduced (1.0f), 4.0f, 2.0f);
}

//============================================================================== lettering

void ProgrammerLook::drawLabel (juce::Graphics& g, juce::Label& label)
{
    const auto w = (float) label.getWidth(), h = (float) label.getHeight();
    const auto text = label.getText();
    const auto shownGrey = [&] (int level) { return grey (label.isEnabled() ? level : retainedGrey); };
    if (auto* box = dynamic_cast<juce::ComboBox*> (label.getParentComponent()))
    {
        const auto t = setText (faces, optionStyle, text.toUpperCase(), 10.0f, std::round (h * 0.5f + optionStyle.cap * 0.5f), w, 0.8f);
        g.setColour (box->isEnabled() ? ink : grey (retainedGrey));
        g.fillPath (t.path);
        return;
    }
    switch ((Text) (int) label.getProperties().getWithDefault (roleProperty, (int) Text::plain))
    {
        case Text::name:
        {
            auto style=labelStyle;if((bool)label.getProperties()["programmer.equipment"])style.cap=14;
            const auto t = setText (faces, style, text.toUpperCase(), 0.0f, 16.0f, w, 0.8f, w * 0.5f);
            g.setColour ((bool)label.getProperties()["programmer.equipment"]?juce::Colour(0xffe5e6e1):shownGrey (labelStyle.grey));
            g.fillPath (t.path);
            if (locked (label) && ! t.path.isEmpty())
            {
                const auto lk = model.programmer["lock"];
                drawSprite (g, model.asset (lk["part"].toString()), { t.ink().getX() + num (lk["dx"]), t.ink().getCentreY() + num (lk["dy"]) });
            }
            return;
        }
        case Text::value:
        {
            const auto t = setText (faces, legendStyle, text.upToFirstOccurrenceOf (" | ", false, false).toUpperCase(), 0.0f, 14.0f, w, 0.8f, w * 0.5f);
            g.setColour (shownGrey (legendStyle.grey));
            g.fillPath (t.path);
            return;
        }
        case Text::lines:
        {
            g.setColour (shownGrey (labelStyle.grey));
            auto lines = juce::StringArray::fromLines (text);
            for (int i = 0; i < lines.size(); ++i)
                g.fillPath (setText (faces, labelStyle, lines[i].toUpperCase(), 0.0f, 14.0f + 17.0f * (float) i, w, 0.8f).path);
            return;
        }
        case Text::title:
            drawTitles (g, layoutTitles (model.programmer, faces, { text.toUpperCase() }, {}, 0.0f, w, 16.0f, 9.0f));
            return;
        case Text::plain:
            break;
    }
    LookAndFeel_V4::drawLabel (g, label);
}

//============================================================================== scroll bars

void ProgrammerLook::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart, int thumbSize,
                                    bool over, bool down)
{
    const auto area = Rect ((float) x, (float) y, (float) width, (float) height);
    if (vertical)
        drawEngraving (g, { Rect (std::round (area.getCentreX()), area.getY(), 1.0f, area.getHeight()), true });
    else
        drawEngraving (g, { Rect (area.getX(), std::round (area.getCentreY()), area.getWidth(), 1.0f), false });
    if (thumbSize <= 0)
        return;
    const auto thumb = vertical ? Rect (area.getCentreX() - 3.0f, (float) thumbStart, 6.0f, (float) thumbSize)
                                : Rect ((float) thumbStart, area.getCentreY() - 3.0f, (float) thumbSize, 6.0f);
    g.setColour (grey (over || down ? 160 : retainedGrey));
    g.fillRoundedRectangle (thumb, 3.0f);
}
} // namespace airack::studio
