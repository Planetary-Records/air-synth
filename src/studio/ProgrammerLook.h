#pragma once

#include "ScreenControls.h"
#include "WheelLatch.h"

#include <vector>

// The PROGRAMMER's drawing (window/skin/app/Main.cpp, stage C) for JUCE's own controls: the rack panel's knobs, faders,
// sub-page buttons, glass fields and lettering, from the PROGRAMMER's approved parts.
namespace airack::studio
{
/** What the look asks a control it draws: whether to ring it in the accent. */
struct Ringed
{
    virtual ~Ringed() = default;
    virtual bool ringed() const = 0;
};

/** A JUCE control ringed, as the studio's other controls are, only while it holds focus the Tab key gave it. */
template <class Control>
class KeyFocus : public Control, public Ringed
{
public:
    using Control::Control;

    bool ringed() const override { return fromKeyboard && this->hasKeyboardFocus (false); }

    void focusGained (juce::Component::FocusChangeType cause) override
    {
        fromKeyboard = cause == juce::Component::focusChangedByTabKey;
        Control::focusGained (cause);
        this->repaint();
    }

    void focusLost (juce::Component::FocusChangeType cause) override
    {
        fromKeyboard = false;
        Control::focusLost (cause);
        this->repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (rackScrolling())
            juce::Component::mouseWheelMove (e, w);
        else
            Control::mouseWheelMove (e, w);
    }

private:
    bool fromKeyboard = false;
};

/** A 1 px engraved line: its highlight here, its shadow a pixel under it (right of it when upright). */
struct Engraving
{
    Rect line;
    bool upright = false;
};

/** Where a row's controls go across: each section's centres and pitch, and the dividers between sections. */
struct RowLayout
{
    std::vector<std::vector<float>> centres;
    std::vector<float> pitches, dividers;
};

/** The PROGRAMMER's row rule (`buildRow`) for sections of `counts` controls between `left` and `right`: from first_dx
    at pitch, gap more at each section's start; a row that would pass last_inset squeezes its last section's pitch
    first, down to squeeze_min, then pitch and gap alike; a divider midway between sections. A section with no controls
    takes one place. */
RowLayout layoutRow (const juce::var& programmer, const std::vector<int>& counts, float left, float right);

struct Title
{
    String text;
    juce::Path path;
    Rect ink;
};

struct TitleRow
{
    std::vector<Title> titles;
    std::vector<Engraving> lines;
};

/** The PROGRAMMER's section titles across a row (`addTitles`): each title_dx past its divider's far side (title_dx_first
    past `left` for the first), underlined a pixel wider than its ink, a rule at `ruleY` from rule_gap after it to
    rule_end short of the next divider; the last title's rule runs to `right`. A title that would pass where its rule
    ends is squeezed to 0.8 and then elided. */
TitleRow layoutTitles (const juce::var& programmer, const Faces& faces, const juce::StringArray& names, const std::vector<float>& dividers,
                       float left, float right, float baseline, float ruleY);

class ProgrammerLook final : public juce::LookAndFeel_V4
{
public:
    explicit ProgrammerLook (const Model& m);

    /** What a label shows, and so how it is lettered: a control's name or value, lines of text, or a title. */
    enum class Text { plain, name, value, lines, title };
    static void setRole (juce::Label& label, Text role);
    /** A name with the AI lock's dot left of it. */
    static void setLocked (juce::Label& label, bool on);
    static bool locked (const juce::Label& label);
    /** The legend `drawLegend` letters over a field. */
    static void setLegend (juce::Component& field, const String& text);
    static String legend (const juce::Component& field);
    static bool ringed (const juce::Component& c);
    static void setCompactButton (juce::TextButton& b) { b.getProperties().set ("programmer.compactButton", true); }

    /** The first control of an emphasised design is drawn this much larger. */
    static constexpr float emphasis = 1.15f;

    void drawFace (juce::Graphics& g, Rect area) const;
    void drawGlass (juce::Graphics& g, Rect area) const;
    void drawEngraving (juce::Graphics& g, const Engraving& line) const;
    void drawTitles (juce::Graphics& g, const TitleRow& row) const;
    /** A field's legend over it, in its parent's coordinates. */
    void drawLegend (juce::Graphics& g, const juce::Component& field) const;
    juce::Font fieldFont() const;
    /** `width` px dashes along the path, `on` px drawn and `off` px left. */
    static void drawDashes (juce::Graphics&, const juce::Path&, juce::Colour, float on, float off, float width = 2.0f);
    /** drawLinearSlider's cap rectangle at the slider position y, in the slider's coordinates. */
    Rect faderCap (const juce::Slider&, float sliderPos) const;
    /** drawRotarySlider's turn in radians clockwise from straight up, for a proportion of the travel, and its skirt's radius. */
    float knobAngle (float proportion) const;
    float knobRadius (const juce::Slider&) const;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool down, int, int, int, int, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart, int thumbSize,
                        bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 12; }
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (26.0f)); }

    const Model& model;
    const Lettering caps, mono;
    const Faces faces;
    const TextStyle labelStyle, titleStyle, legendStyle, optionStyle, bodyStyle;
    const juce::Colour ink;

private:
    void drawSubpage (juce::Graphics&, juce::Button&, float scale, bool lit, const String& legend, bool highlighted, bool down) const;

    const juce::Image face, glass;
};
} // namespace airack::studio
