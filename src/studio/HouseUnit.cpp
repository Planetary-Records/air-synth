#include "HouseUnit.h"

#include "InstrumentProcessor.h"
#include "RackReconciliation.h"

#include <algorithm>
#include <cmath>

namespace airack::studio
{
namespace
{
const juce::Rectangle<int> unitTitleBounds { 116, 24, 310, 120 }, stripTitleBounds { 114, 48, 228, 104 };

juce::Image titleMaterial (const juce::Image& plate, juce::Rectangle<int> source, juce::Rectangle<int> title)
{
    juce::Image image (juce::Image::ARGB, title.getWidth(), title.getHeight(), true);
    {
        juce::Graphics g (image);
        g.drawImage (plate, 0, 0, image.getWidth(), image.getHeight(),
                     source.getX(), source.getY(), source.getWidth(), source.getHeight());
    }
    juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < pixels.height; ++y)
        for (int x = 0; x < pixels.width; ++x)
        {
            const auto edge = std::min ({ x, y, pixels.width - 1 - x, pixels.height - 1 - y });
            pixels.setPixelColour (x, y, pixels.getPixelColour (x, y).withMultipliedAlpha (std::min (1.0f, (float) edge / 12.0f)));
        }
    return image;
}

const RackUnit* unitOf (const RackInstance& i, std::uint8_t id)
{
    const auto u = std::find_if (i.design.units.begin(), i.design.units.end(), [id] (const auto& v) { return v.id == id; });
    return u == i.design.units.end() ? nullptr : &*u;
}

/** Each present synth bank's rack name in capitals: the first synth rack whose first unit is bound there. */
std::array<String, 4> slotNames (const SoundDocument& d)
{
    std::array<String, 4> out;
    if (! d.personalities)
        return out;
    for (const auto& i : d.personalities->instances)
    {
        if (i.design.role != RackRole::synth || i.design.units.empty())
            continue;
        const auto* b = rackUnitBinding (i, i.design.units.front().id);
        if (b != nullptr && b->slot < 4 && d.layout.synthUse[b->slot] == SlotUse::present && out[b->slot].isEmpty())
            out[b->slot] = i.design.name.toUpperCase();
    }
    return out;
}

house::Assignment assignmentOf (const MacroBinding& b) { return { parameterIndexForKey (b.targetKey), b.amount }; }

bool same (const house::Assignment& a, const house::Assignment& b)
{
    return a.target == b.target && std::abs (a.amount - b.amount) < 0.0005f;
}
} // namespace

std::vector<house::RackTarget> rackTargets (const SoundDocument& d)
{
    std::vector<house::RackTarget> out;
    if (! d.personalities)
        return out;
    for (const auto& i : d.personalities->instances)
        for (std::size_t n = 0; n < i.design.panel.controls.size(); ++n)
        {
            if (! rackControlActive (d, i, n))
                continue;
            const auto& c = i.design.panel.controls[n];
            const auto* u = unitOf (i, c.unit);
            const auto* b = rackUnitBinding (i, c.unit);
            if (u == nullptr || b == nullptr)
                continue;
            if (const auto index = rackParameterIndex (u->module, b->slot, c.key); index && isContinuousParameter (*index))
                out.push_back ({ c.label.toUpperCase(), i.design.name, *index });
        }
    return out;
}

String targetName (std::size_t index, const std::vector<house::RackTarget>& targets, const Model& model)
{
    for (const auto& t : targets)
        if (t.index == index)
            return t.label;
    const auto& d = parameterRegistry()[index];
    auto name = index < model.knobNames.size() && model.knobNames[index].isNotEmpty() ? model.knobNames[index] : d.name.toUpperCase();
    for (const auto& [prefix, letters] : { std::pair<const char*, const char*> { "synth.", "S" }, { "effect.", "FX" } })
        if (d.id.startsWith (prefix))
            return letters + d.id.fromFirstOccurrenceOf (prefix, false, false).upToFirstOccurrenceOf (".", false, false) + " " + name;
    return name;
}

HouseUnit::HouseUnit (InstrumentProcessor& p, const Model& m, house::Host h)
    : model (m), host (std::move (h)), context { m.house, p, host, {} }, unit (context, "unit"), strip (context, "strip"),
      narrow (context, "narrow")
{
    context.tapeText = [this] (const house::Element& e) { return tapeText (e.localId()); };
    setComponentID ("house.panel");
    setTitle ("air unit");
    const auto logo = house::houseSkinFile ("ai-rack-wordmark.svg");
    wordmark = juce::Drawable::createFromImageData (logo.getData(), logo.getSize());
    if (wordmark != nullptr)
    {
        wordmark->replaceColour (juce::Colour (0xff282a29), juce::Colour (0xffeeeae0));
        unitTitle = titleMaterial (model.house.asset ("unit:plate").image, { 116, 110, 310, 80 }, unitTitleBounds);
        stripTitle = titleMaterial (model.house.asset ("strip:plate").image, { 114, 140, 228, 40 }, stripTitleBounds);
    }
    setFocusContainerType (FocusContainerType::focusContainer);
    rail.setExplicitFocusOrder (1);
    unit.setExplicitFocusOrder (2);
    strip.setExplicitFocusOrder (2);
    narrow.setExplicitFocusOrder (2);
    addAndMakeVisible (rail);
    addAndMakeVisible (unit);
    addChildComponent (strip);
    addChildComponent (narrow);
    // Over the 3U unit while it is scrolled sideways under the rail.
    rail.toFront (false);
    unit.addChildComponent (editor);
    editor.setExplicitFocusOrder (unit.focusOrder.size() + 1);
    rail.onToggle = [this]
    {
        if (onRail)
            onRail();
        else if (column > 0)
            setOpen (! open);
        else
            setFolded (! folded);
    };
    for (auto& b : unit.buttons)
        if (b->element.kind == "action")
        {
            const auto n = b->element.localId().fromFirstOccurrenceOf (".", false, false).getIntValue();
            b->onAction = [this, n, bp = b.get()]
            {
                if (editor.macro == n)
                {
                    editor.close();
                    return;
                }
                auto column = bp->element.footprint();
                if (auto* k = unit.slider ("macro." + String (n)))
                    column = column.getUnion (k->element.footprint());
                if (auto* t = unit.tape ("macro-tape." + String (n)))
                    column = column.getUnion (t->element.footprint());
                editor.open (n, *bp, column);
                editor.show (assignmentOf (shown.macros[(size_t) n - 1]));
            };
        }
    editor.onOpenChanged = [this] (int n, bool opened)
    {
        if (auto* b = unit.button ("assign." + String (n)))
            b->setActionLit (opened);
        if (opened && onOpened)
            onOpened();
    };
    editor.onAssign = [this] (int n, const house::Assignment& a) { assign (n, a); };
    editor.onPreview = [this] (int n, std::optional<house::Assignment> a)
    {
        if (a)
            preview.emplace (n, *a);
        else
            preview.reset();
        unit.refreshTapes();
        strip.refreshTapes();
        narrow.refreshTapes();
    };
    layout();
}

HouseUnit::~HouseUnit() = default;

void HouseUnit::setFolded (bool f)
{
    if (f == folded)
        return;
    auto& was = current();
    folded = f;
    switchView (was);
}

void HouseUnit::setStripOnly (bool s)
{
    if (s == stripOnly)
        return;
    auto& was = current();
    stripOnly = s;
    switchView (was);
}

void HouseUnit::setNarrow (int width)
{
    width = std::max (0, width);
    if (width == column)
        return;
    auto& was = current();
    column = width;
    open = open && column > 0;
    shiftPx = std::min (shiftPx, across());
    switchView (was);
}

void HouseUnit::setOpen (bool o)
{
    o = o && column > 0;
    if (o == open)
        return;
    auto& was = current();
    open = o;
    switchView (was);
}

void HouseUnit::setShift (int px)
{
    px = juce::jlimit (0, across(), px);
    if (px == shiftPx)
        return;
    shiftPx = px;
    layout();
}

int HouseUnit::across() const
{
    return column > 0 && open && ! stripOnly ? std::max (0, unit.getWidth() - column) : 0;
}

void HouseUnit::refresh (const SoundDocument& d)
{
    auto letter = ! refreshed || d.macros != shown.macros;
    if (! refreshed || d.personalities != shown.personalities || d.layout != shown.layout || d.values != shown.values)
    {
        auto now = rackTargets (d);
        const auto names = slotNames (d);
        if (now != targets)
        {
            targets = std::move (now);
            editor.setRackTargets (targets);
            letter = true;
        }
        if (names != slots)
        {
            slots = names;
            letter = true;
        }
    }
    shown.macros = d.macros;
    shown.personalities = d.personalities;
    shown.layout = d.layout;
    shown.values = d.values;
    refreshed = true;
    if (letter)
    {
        unit.refreshTapes();
        strip.refreshTapes();
        narrow.refreshTapes();
    }
    if (editor.macro != 0 && ! editor.isDragging())
        if (const auto a = assignmentOf (d.macros[(size_t) editor.macro - 1]); ! same (editor.assignment(), a))
            editor.show (a);
}

void HouseUnit::visibilityChanged()
{
    if (! isVisible())
        editor.close (false);
}

void HouseUnit::paint (juce::Graphics& g)
{
    // The narrow strip's ear, and bare panel from it to the narrow strip in a wider column.
    if (! narrow.isVisible())
        return;
    const auto& fill = model.house.asset ("narrow:fill").image;
    const auto at = railW + juce::roundToInt (model.house.asset ("narrow:plate").anchor.x), extra = narrow.getX() - railW;
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageAt (model.house.asset ("narrow:ear").image, railW, 0);
    if (extra <= 0)
        return;
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (at, 0, extra, fill.getHeight());
    for (int x = at, i = 0; x < at + extra; x += fill.getWidth(), ++i)
        if (i % 2 == 0)
            g.drawImageTransformed (fill, juce::AffineTransform::scale (-1.0f, 1.0f).translated ((float) (x + fill.getWidth()), 0.0f));
        else
            g.drawImageAt (fill, x, 0);
}

void HouseUnit::paintOverChildren (juce::Graphics& g)
{
    // The narrow strip deliberately omits the title; the other views replace only their printed name.
    if (wordmark == nullptr || narrow.isVisible())
        return;
    const bool foldedView = strip.isVisible();
    const auto& view = foldedView ? strip : unit;
    const auto title = foldedView ? stripTitleBounds : unitTitleBounds;
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (getLocalBounds().withTrimmedLeft (railW));
    g.setOrigin (view.getPosition());
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageAt (foldedView ? stripTitle : unitTitle, title.getX(), title.getY());
    wordmark->drawWithin (g, foldedView ? juce::Rectangle<float> (132, 65, 188, 70)
                                      : juce::Rectangle<float> (132, 40, 240, 90),
                          juce::RectanglePlacement::centred, 1.0f);
}

void HouseUnit::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : e.mods.isShiftDown() ? w.deltaY : 0.0f;
    if (delta != 0.0f && across() > 0 && onAcross)
        onAcross (-wheelDistance (delta));
    else
        juce::Component::mouseWheelMove (e, w);
}

house::View& HouseUnit::current()
{
    return ! showsStrip() ? unit : column > 0 ? narrow : strip;
}

void HouseUnit::switchView (house::View& from)
{
    auto& to = current();
    rail.folded = showsStrip();
    rail.repaint();
    if (&to == &from)
    {
        layout();
        return;
    }
    editor.close();
    if (&to == &unit)
        shiftPx = 0;
    String focused;
    if (auto* fc = juce::Component::getCurrentlyFocusedComponent(); fc != nullptr && from.isParentOf (fc))
        focused = fc->getProperties()["element"].toString().fromFirstOccurrenceOf (".", false, false);
    for (auto* v : { &unit, &strip, &narrow })
        v->setVisible (v == &to);
    layout();
    if (focused.isNotEmpty())
    {
        if (auto* c = to.control (focused))
            c->grabKeyboardFocus();
        else
            rail.grabKeyboardFocus();
    }
    if (onLayout)
        onLayout();
}

void HouseUnit::layout()
{
    const auto& v = current();
    const auto shifted = &v == &unit ? shiftPx : 0;
    rail.setBounds (0, 0, railW, v.getHeight());
    unit.setTopLeftPosition (railW - shifted, 0);
    unit.setCovered (shifted);
    strip.setTopLeftPosition (railW, 0);
    narrow.setTopLeftPosition (railW + std::max (0, column - narrow.getWidth()), 0);
    setSize (railW + (column > 0 ? column : v.getWidth()), v.getHeight());
    repaint();
}

String HouseUnit::tapeText (const String& localId) const
{
    const auto n = localId.fromLastOccurrenceOf (".", false, false).getIntValue();
    if (localId.startsWith ("slot-tape.") && n >= 1 && n <= 4)
        return slots[(size_t) n - 1];
    if (! localId.startsWith ("macro-tape.") || n < 1 || n > 8)
        return {};
    const auto a = preview && preview->first == n ? preview->second : assignmentOf (shown.macros[(size_t) n - 1]);
    if (! a.target)
        return {};
    const auto pc = juce::roundToInt (a.amount * 100.0f);
    return targetName (*a.target, targets, model) + " " + (pc < 0 ? "-" : "+") + String (std::abs (pc));
}

void HouseUnit::assign (int macro, const house::Assignment& a)
{
    const auto slot = (size_t) macro - 1;
    const auto binding = a.target ? MacroBinding { parameterRegistry()[*a.target].stateKey, a.amount } : MacroBinding {};
    if (! host.change || binding == shown.macros[slot])
        return;
    host.change ([slot, binding] (SoundDocument& d) { d.macros[slot] = binding; }, a.target ? "Macro assignment" : "Clear macro");
}

int wheelDistance (float delta)
{
    if (delta == 0.0f)
        return 0;
    const auto d = delta * 14.0f * 16.0f;
    return juce::roundToInt (d < 0.0f ? std::min (d, -1.0f) : std::max (d, 1.0f));
}
} // namespace airack::studio
