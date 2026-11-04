#include "MakerFace.h"

#include "DotMatrix.h"
#include "InstrumentProcessor.h"
#include "Motion.h"
#include "ProgrammerLook.h"
#include "RackExamples.h"
#include "RackReview.h"
#include "SoundDocument.h"
#include "TileArrayFace.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace airack::studio
{


namespace
{
std::optional<MakerSkin::NativeEditor> nativeEditorFor (const juce::var& doc, const house::Model& model, const RackDesign& design)
{
    const auto reveal = doc["reveal"];
    if (! reveal.hasProperty ("native_editor"))
        return std::nullopt;
    const auto editor = reveal["native_editor"];
    const auto* rect = editor["rect"].getArray();
    if (! editor.isObject() || editor["type"].toString() != "space_reverb" || editor["theme"].toString() != "amber_crt"
        || ! editor["rack_unit"].isInt() || rect == nullptr || rect->size() != 4 || model.reveal.kind != "drawer")
        throw std::runtime_error ("invalid maker native editor");
    for (const auto& value : *rect)
        if ((! value.isInt() && ! value.isDouble()) || ! std::isfinite ((double) value))
            throw std::runtime_error ("invalid maker native editor rectangle");
    MakerSkin::NativeEditor result { editor["type"].toString(), editor["theme"].toString(), (int) editor["rack_unit"],
                                    { (float) (double) (*rect)[0], (float) (double) (*rect)[1], (float) (double) (*rect)[2], (float) (double) (*rect)[3] } };
    const auto unit = std::find_if (design.units.begin(), design.units.end(), [&result] (const auto& u) { return u.id == result.rackUnit && u.module == NativeModule::spaceReverb; });
    const auto& plate = model.asset (model.reveal.view + ":plate");
    const juce::Rectangle<float> area { plate.anchor.x, plate.anchor.y, (float) plate.image.getWidth(), (float) plate.image.getHeight() };
    if (unit == design.units.end() || result.rect.isEmpty() || ! area.contains (result.rect))
        throw std::runtime_error ("maker native editor is outside its bound reveal");
    return result;
}

struct PreparedSkin {
    juce::String key;
    RackDesign design;
    std::optional<RackPackageRef> package;
    std::weak_ptr<const MakerSkin> owner;
};
std::mutex preparedMutex;
std::vector<PreparedSkin> prepared;
MakerSkinPtr lookup(const RackInstance& rack) {
    const std::lock_guard lock(preparedMutex);
    for(const auto& entry:prepared)if(entry.package==rack.package&&entry.design==rack.design)
        if(auto skin=entry.owner.lock())return skin;
    return {};
}
} // namespace

MakerSkinPtr makerFaceFor(const RackDesign& design) {RackInstance rack;rack.design=design;return lookup(rack);}
MakerSkinPtr makerFaceFor(const RackInstance& rack,const RackInventorySnapshot&) {return lookup(rack);}

MakerSkinPtr prepareMakerSkin(const RackInstance& rack,const RackInventorySnapshot* inventory,const RackLibraryCancel& cancel) {
    const auto cancelled=[&]{return cancel&&cancel();};
    const auto* messages=juce::MessageManager::getInstanceWithoutCreating();
    if((messages&&messages->isThisTheMessageThread())||cancelled())return {};
    if(auto skin=lookup(rack))return skin;
    try {
        RackPackageLeasePtr lease;juce::String key,name;std::function<juce::MemoryBlock(const juce::String&)> reader;
        const auto reference=rack.package?rack.package:legacyRackPackageRef(rack.design);
        if(reference){
            if(!inventory||!validRackPackageRef(*reference))return {};
            lease=inventory->openPackage(*reference,nullptr,cancel);if(!lease||cancelled())return {};
            const auto& manifest=lease->manifest();if(manifest.skinPath.isEmpty())return {};
            RackDesign exact;const auto recipe=lease->canonicalRecipe();
            if(!decodeRackDesign(recipe.data(),recipe.size(),exact,RackValidationMode::instanceRecall)||exact!=rack.design)return {};
            key=reference->sha256+":"+manifest.skinPath;
            reader=[lease,cancelled](const juce::String& relative){
                if(cancelled())throw std::runtime_error("skin preparation cancelled");
                const auto& m=lease->manifest();const auto parent=m.skinPath.containsChar('/')?m.skinPath.upToLastOccurrenceOf("/",true,false):juce::String();
                const auto path=relative=="elements.json"?m.skinPath:parent+relative;
                juce::MemoryBlock bytes;
                if(!validRackPackagePath(path)||lease->readListed(path,bytes)!=RackLibraryStatus::ready)throw std::runtime_error("unavailable listed skin file");
                return bytes;
            };
        }else return {};
        const auto json=reader("elements.json");const auto doc=juce::JSON::parse(json.toString());
        auto model=std::make_shared<house::Model>(house::loadModel(reader,nullptr));
        if(model->designId!=rack.design.id||model->units<1||model->units>64||cancelled())return {};
        if(std::any_of(model->elements.begin(),model->elements.end(),[](const house::Element& e){return e.nativeEngraving.has_value();})){
            const auto* window=studioSkin();if(!window||!window->screenFace)return {};model->nativeEngravingFace=window->screenFace;
        }
        TileArrayFace::adaptModel(*model,doc);
        const auto native=nativeEditorFor(doc,*model,rack.design);
        if(native&&lease&&lease->manifest().capabilities["nativeEditor"].toString()!=native->type)return {};
        auto owner=std::make_shared<const MakerSkin>(MakerSkin{model->designId,model->maker,model->modelName,model->units,model,*model,native,lease});
        const std::lock_guard lock(preparedMutex);
        std::erase_if(prepared,[](const auto& entry){return entry.owner.expired();});
        for(const auto& entry:prepared)if(entry.key==key&&entry.package==rack.package&&entry.design==rack.design)
            if(auto held=entry.owner.lock())return held;
        prepared.push_back({key,rack.design,rack.package,owner});return owner;
    }catch(const std::exception&){return {};}
}
namespace
{
/** A recess's fasteners have turned by this drawn fraction, and its cover then lifts, growing by `coverGrowth`. */
constexpr float fastenersTurned = 0.4f, coverGrowth = 0.04f;

/** The skin with each control bound to the rack's banks, or none when one of them has no parameter there. */
std::optional<house::Model> bound (const MakerSkin& skin, const RackInstance& rack)
{
    auto model = skin.skin;
    if (skin.nativeEditor)
    {
        const auto unit = std::find_if (rack.design.units.begin(), rack.design.units.end(), [&skin] (const auto& u) { return u.id == skin.nativeEditor->rackUnit && u.module == NativeModule::spaceReverb; });
        if (unit == rack.design.units.end() || rackUnitBinding (rack, unit->id) == nullptr)
            return std::nullopt;
    }
    const auto indexOf = [&rack] (int rackUnit, const juce::String& key) -> std::optional<std::size_t>
    {
        const auto unit = std::find_if (rack.design.units.begin(), rack.design.units.end(), [rackUnit] (const RackUnit& u) { return u.id == rackUnit; });
        const auto* binding = rackUnitBinding (rack, (std::uint8_t) rackUnit);
        return unit != rack.design.units.end() && binding != nullptr ? rackParameterIndex (unit->module, binding->slot, key) : std::nullopt;
    };
    for (auto& e : model.elements)
    {
        if (e.nativeActiveWhen.unit >= 0)
        {
            const auto active = indexOf (e.nativeActiveWhen.unit, e.nativeActiveWhen.key);
            if (! active)
                return std::nullopt;
            e.nativeActiveWhen.index = *active;
        }
        if (e.shownWhen.unit >= 0)
        {
            const auto shown = indexOf (e.shownWhen.unit, e.shownWhen.key);
            if (! shown)
                return std::nullopt;
            e.shownWhen.index = *shown;
        }
        if (e.rackUnit < 0)
            continue;
        const auto index = indexOf (e.rackUnit, e.key);
        if (! index)
            return std::nullopt;
        e.paramIndex = *index;
        if (e.detents && ! parameterRegistry()[*index].choices.empty())
        {
            e.choices.clear();
            for (const auto& c : parameterRegistry()[*index].choices)
                e.choices.add (c);
            e.range = { 0.0, (double) std::max (1, e.choices.size() - 1) };
        }
        if (e.registryResponse)
        {
            const auto& d = parameterRegistry()[*index];
            e.range = { d.minimum, d.maximum };
            e.skew = d.skew;
            if (! e.ticks.empty())
            {
                e.ticks.front().first = d.minimum;
                e.ticks.back().first = d.maximum;
            }
        }
    }
    if (model.nativeFace)
    {
        auto bind = [&] (house::Element::ShownWhen& local)
        {
            const auto index = indexOf (local.unit, local.key);
            if (! index)
                return false;
            local.index = *index;
            return true;
        };
        if (! bind (model.nativeFace->statusWhen) || ! bind (model.nativeFace->pilotWhen))
            return std::nullopt;
        for (auto& local : model.nativeFace->controls)
            if (! bind (local))
                return std::nullopt;
    }
    return model;
}

/** Whether a control shows: always, or while the parameter its `shown_when` names has its value. */
bool shownAt (const house::Element& e, const ParameterValues& values)
{
    return e.shownWhen.unit < 0 || juce::roundToInt (parameterPhysicalValue (values, e.shownWhen.index)) == juce::roundToInt (e.shownWhen.value);
}

bool nativeAt (const house::Element::ShownWhen& condition, const ParameterValues& values)
{
    return condition.unit < 0 || juce::roundToInt (parameterPhysicalValue (values, condition.index)) == juce::roundToInt (condition.value);
}

float nativeAlpha (const house::Element& e, const ParameterValues& values)
{
    return nativeAt (e.nativeActiveWhen, values) ? 1.0f : e.nativeInactiveAlpha;
}

juce::String nativeStatus (const house::Model& model, const ParameterValues& values)
{
    if (! model.nativeFace)
        return {};
    const auto& n = *model.nativeFace;
    return nativeAt (n.statusWhen, values) ? n.status : n.inactiveStatus;
}

void paintNativeState (juce::Graphics& g, const house::Model& model, const ParameterValues& values, house::Pt origin = {})
{
    if (! model.nativeFace)
        return;
    const auto& n = *model.nativeFace;
    juce::Graphics::ScopedSaveState saved (g);
    g.addTransform (juce::AffineTransform::translation (-origin.x, -origin.y));
    if (model.nativeEngravingFace != nullptr)
    {
        g.setColour (juce::Colour (0xffd6b27d));
        g.setFont (juce::Font (juce::FontOptions (model.nativeEngravingFace).withHeight (26.0f)));
        g.drawFittedText (nativeStatus (model, values), n.statusRect.toNearestInt(), juce::Justification::centred, 3, 0.9f);
    }
}

/** The face's first bound control in focus order, which the displays read until one is touched. */
const house::Element* firstControl (const house::Model& m)
{
    for (const auto& g : m.groups)
        if (g.view == "face")
            for (const auto& id : g.focusOrder)
                for (const auto& e : m.elements)
                    if (e.id == id && e.rackUnit >= 0)
                        return &e;
    return nullptr;
}

/** The control a still picture's displays read: the drawing's touched one while open, else the face's first. */
const house::Element* drawnTouch (const house::Model& m, bool open)
{
    if (open)
        for (const auto& e : m.elements)
            if (e.id == m.reveal.drawnTouch)
                return &e;
    return firstControl (m);
}

/** A maker display: its element, its lattice, and its lines fitted to their drawn ink. */
struct DisplayGlass
{
    DisplayGlass (const house::Element& e, const Lettering* let)
        : element (e), lattice (latticeFrom (e.display)), minHscale ((float) (double) e.display.getProperty ("min_hscale", 1.0))
    {
        if (auto* o = e.display["lines"].getDynamicObject())
            for (const auto& nv : o->getProperties())
            {
                lines[nv.name.toString()] = let != nullptr ? fitLine (*let, lineFrom (nv.value)) : lineFrom (nv.value);
                if (const auto* c = nv.value["column"].getArray(); c != nullptr && c->size() == 2)
                    columns[nv.name.toString()] = { (float) (double) (*c)[0], (float) (double) (*c)[1] };
            }
    }

    /** The line that letters `text` at `key`: a line with a column is centred on its drawn word and squeezed into the
        column, down to the display's minimum scale. */
    Line lineFor (const Lettering& let, const juce::String& key, const juce::String& text) const
    {
        auto l = lines.at (key);
        const auto c = columns.find (key);
        if (c == columns.end())
            return l;
        const auto adv = let.advance (l.cap, l.hscale);
        const auto centre = l.penX + (float) l.text.length() * adv * 0.5f;
        const auto column = c->second;
        const auto m = (float) juce::jmax (1, text.length());
        l.hscale = std::max (minHscale, l.hscale * std::min (1.0f, column.getLength() / (m * adv)));
        const auto width = m * let.advance (l.cap, l.hscale);
        l.penX = juce::jlimit (column.getStart(), std::max (column.getStart(), column.getEnd() - width), centre - width * 0.5f);
        l.align = "left";
        l.maxRight = column.getEnd();
        return l;
    }

    /** The cells a left line holds at its cap and the display's minimum horizontal scale; its drawn text's without
        the lettering. */
    int cells (const Lettering* let, const juce::String& key) const
    {
        const auto l = lines.find (key);
        if (l == lines.end())
            return 0;
        if (let == nullptr || l->second.maxRight <= l->second.penX)
            return l->second.text.length();
        return (int) std::floor ((l->second.maxRight - l->second.penX) / let->advance (l->second.cap, minHscale) + 1.0e-3f);
    }

    const house::Element& element;
    const Lattice lattice;
    const float minHscale;
    std::map<juce::String, Line> lines;
    std::map<juce::String, juce::Range<float>> columns;
};

/** What a display shows: each line's text, and how many of its bar's segments are lit, or -1 for no bar. */
struct Lettered
{
    std::map<juce::String, juce::String> lines;
    int lit = -1;
    bool operator== (const Lettered&) const = default;
};

/** vfdLines with the cells the VFD's own lines hold. */
VfdText vfdOn (const house::Model& m, const ParameterValues& values, const DisplayGlass& d, const Lettering* let, const house::Element& touched)
{
    return vfdLines (m, values, touched, d.cells (let, "line1"), d.cells (let, "line2"));
}

/** What a display shows at the values, `touched` being the control last touched and `screen` the door's screen. An
    LCD reads the label of each item shown on the screen at its slot, and nothing at a slot the screen leaves unused. */
Lettered letteredFor (const house::Model& m, const ParameterValues& values, const DisplayGlass& d, const Lettering* let,
                      const house::Element* touched, const juce::String& screen)
{
    if (d.element.role == "lcd")
    {
        Lettered out;
        for (const auto& e : m.elements)
            if (e.screen.isNotEmpty() && e.screen == screen && e.rackUnit >= 0 && shownAt (e, values))
                out.lines["slot" + juce::String (e.slot)] = e.lcdLabel;
        return out;
    }
    // A name display reads what its drawing reads.
    if (d.element.role == "name")
    {
        Lettered out;
        for (const auto& [key, l] : d.lines)
            out.lines[key] = l.text;
        return out;
    }
    if (d.element.role != "vfd" || touched == nullptr)
        return {};
    const auto t = vfdOn (m, values, d, let, *touched);
    return { { { "line1", t.line1 }, { "line2", t.line2 } }, t.lit };
}

/** The display's glass at `scale` pixels per face pixel: its view's plate under it, its lines lettered and lit on its
    lattice, and for a bar the lit segment at its first `lit` centres and the unlit segment at the rest. */
juce::Image glassImage (const house::Model& m, const DisplayGlass& d, const Lettering* let, const Lettered& text, float scale)
{
    const auto& e = d.element;
    const auto& plate = m.asset (e.view + ":plate");
    const auto r = e.rect.getSmallestIntegerContainer();
    auto image = scaledArea (plate.image, r - plate.anchor.roundToInt(), scale);
    const auto origin = r.getPosition().toFloat();
    if (let != nullptr)
        for (const auto& [key, t] : text.lines)
            if (d.lines.count (key) != 0 && t.isNotEmpty())
                drawLit (image, origin, place (*let, d.lineFor (*let, key, t), t, d.minHscale).path, d.lattice, scale);
    if (text.lit < 0)
        return image;
    const auto bar = e.display["bar"];
    const auto& lit = m.asset (bar["lit"].toString());
    const auto& unlit = m.asset (bar["unlit"].toString());
    juce::Graphics g (image);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (const auto* centres = bar["centers"].getArray())
        for (int i = 0; i < centres->size(); ++i)
        {
            const auto& a = i < text.lit ? lit : unlit;
            const auto at = (Pt ((float) (double) (*centres)[i][0], (float) (double) (*centres)[i][1]) - origin) * scale;
            g.drawImageTransformed (a.image, juce::AffineTransform::translation (-a.anchor.x, -a.anchor.y).scaled (scale).translated (at.x, at.y));
        }
    return image;
}

/** The views a faceplate shows, top down, each with its top in the faceplate: the face; over it a recess or a door's
    panel while open, and the door, which lies in the faceplate's pixels below the face; under it a drawer's deck while
    open, then the drawer's front. */
std::vector<std::pair<juce::String, int>> stacked (const house::Model& m, bool open)
{
    std::vector<std::pair<juce::String, int>> out { { "face", 0 } };
    if (m.reveal.kind != "drawer")
    {
        if (open)
            out.push_back ({ m.reveal.view, 0 });
        if (open && m.reveal.kind == "door" && m.reveal.hang.isNotEmpty())
            out.push_back ({ m.reveal.hang, 0 });
        return out;
    }
    const auto height = [&m] (const juce::String& view) { const auto& a = m.asset (view + ":plate"); return juce::roundToInt (a.anchor.y) + a.image.getHeight(); };
    auto y = height ("face");
    if (open)
    {
        out.push_back ({ m.reveal.view, y });
        y += height (m.reveal.view);
    }
    if (m.reveal.front.isNotEmpty())
        out.push_back ({ m.reveal.front, y });
    return out;
}

/** A faceplate's height for its views: a door's open faceplate is its units' share of the face's height. */
int heightOf (const house::Model& m, const std::vector<std::pair<juce::String, int>>& views, bool open)
{
    int height = 0;
    for (const auto& [view, top] : views)
    {
        const auto& a = m.asset (view + ":plate");
        height = std::max (height, top + juce::roundToInt (a.anchor.y) + a.image.getHeight());
    }
    if (open && m.reveal.kind == "door")
    {
        const auto& f = m.asset ("face:plate");
        height = std::max (height, (juce::roundToInt (f.anchor.y) + f.image.getHeight()) * (m.units + m.reveal.units) / m.units);
    }
    return height;
}

/** The page whose screens hold `screen`, or none. */
const house::Model::Page* pageOf (const house::Model& m, const juce::String& screen)
{
    for (const auto& p : m.pages)
        if (p.screens.contains (screen))
            return &p;
    return nullptr;
}

/** The height of a skin's strip: the face's over its units. */
int stripHeight (const house::Model& m)
{
    const auto& f = m.asset ("face:plate");
    return (juce::roundToInt (f.anchor.y) + f.image.getHeight()) / std::max (1, m.units);
}

juce::Image composedViews (const house::Model& model, const ParameterValues& values, const std::vector<std::pair<juce::String, int>>& views,
                           int height, bool open, const Lettering* let, const TileArrayFace* tile = nullptr)
{
    std::unique_ptr<house::GeneratedText> tileText;
    if (tile == nullptr && TileArrayFace::isTile (model)) tileText = std::make_unique<house::GeneratedText>();
    int width = 0;
    for (const auto& [view, top] : views)
    {
        const auto& a = model.asset (view + ":plate");
        width = std::max (width, juce::roundToInt (a.anchor.x) + a.image.getWidth());
    }
    juce::Image out (juce::Image::ARGB, width, height, true);
    juce::Graphics g (out);
    // Open, a door shows the drawing's screen with its page lit.
    const auto screen = open ? model.reveal.drawnScreen : juce::String();
    const auto* page = pageOf (model, screen);
    for (const auto& [view, top] : views)
    {
        const auto& plate = model.asset (view + ":plate");
        g.drawImageAt (plate.image, juce::roundToInt (plate.anchor.x), top + juce::roundToInt (plate.anchor.y));
        const house::Pt origin (0.0f, (float) -top);
        for (const auto& e : model.elements)
            if (e.view == view && e.nativeEngraving)
            {
                auto printed = e;
                printed.nativeAlpha = nativeAlpha (e, values);
                house::paintNativeEngraving (g, model, printed, origin);
            }
        house::paintNativeViewText (g, model, view, origin);
        if (view == "face")
            paintNativeState (g, model, values, origin);
        std::vector<int> used;
        // Tile's opaque control bodies sit beneath the independently moving rotors.
        if (TileArrayFace::isTile (model))
            for (const auto& e : model.elements)
                if (e.view == view && e.kind == "still" && TileArrayFace::shows (e, tile != nullptr ? tile->selectedPad() : 0))
                    house::paintPointer (g, model, e, 0.0f, origin);
        for (const auto& e : model.elements)
        {
            if (e.view != view)
                continue;
            if (TileArrayFace::isTile (model) && ! TileArrayFace::shows (e, tile != nullptr ? tile->selectedPad() : 0))
                continue;
            if (page != nullptr && e.id == page->button)
                house::paintLight (g, model, e, origin);
            if (open && model.reveal.kind == "drawer" && e.id == model.reveal.opener && e.layer ("lit").isNotEmpty())
                house::paintLight (g, model, e, origin);
            if (e.rackUnit < 0)
                continue;
            if (e.screen.isNotEmpty())
            {
                if (e.screen != screen || ! shownAt (e, values))
                    continue;
                used.push_back (e.slot);
            }
            const auto v = (double) parameterPhysicalValue (values, e.paramIndex);
            const auto opacity = nativeAlpha (e, values);
            if (opacity < 1.0f)
                g.beginTransparencyLayer (opacity);
            if (e.kind == "rotary")
                house::paintPointer (g, model, e, house::pointerAngleFor (e, house::proportionOf (e, e.range.clipValue (v))), origin);
            else if (e.kind == "selector")
                house::paintPointer (g, model, e, house::positionFor (e, v).angle, origin);
            else if (e.kind == "lever")
                house::paintBat (g, model, e, house::positionFor (e, v), origin);
            else if (e.kind == "fader")
                house::paintCap (g, model, e, house::proportionOf (e, e.range.clipValue (v)), origin);
            else if (e.kind == "toggle" && ! e.stateAssets.empty())
            {
                const auto& a = model.asset (e.stateAsset (v > 0.5 ? "on" : "off"));
                const auto at = e.rect.getCentre() - a.anchor - origin;
                g.drawImageTransformed (a.image, juce::AffineTransform::translation (at.x, at.y));
            }
            else if (e.kind == "lamp" && house::litAt (e, v))
                house::paintLight (g, model, e, origin);
            else if (e.kind == "needle")
                house::paintNeedle (g, model, e, house::proportionOf (e, e.range.clipValue (v)), origin);
            if (opacity < 1.0f)
                g.endTransparencyLayer();
        }
        for (const auto& e : model.elements)
            if (! TileArrayFace::isTile (model) && e.view == view && e.kind == "still" && std::find (used.begin(), used.end(), e.slot) == used.end())
                house::paintPointer (g, model, e, 0.0f, origin);
        if (TileArrayFace::isTile (model))
        {
            juce::Graphics::ScopedSaveState saved (g);
            g.addTransform (juce::AffineTransform::translation (0, (float) top));
            if (tile != nullptr) tile->paint (g, values, view);
            else TileArrayFace::paintExtras (g, model, values, view, 0, nullptr, nullptr, {}, true, tileText.get());
        }
        for (const auto& e : model.elements)
            if (e.view == view && e.kind == "display")
            {
                const DisplayGlass d (e, let);
                const auto r = e.rect.getSmallestIntegerContainer();
                g.drawImageAt (glassImage (model, d, let, letteredFor (model, values, d, let, drawnTouch (model, open), screen), 1.0f), r.getX(), top + r.getY());
            }
    }
    return out;
}

juce::Image composed (const house::Model& model, const ParameterValues& values, bool open, const Lettering* let)
{
    const auto views = stacked (model, open);
    return composedViews (model, values, views, heightOf (model, views, open), open, let);
}

std::unique_ptr<Lettering> windowLettering()
{
    const auto* window = studioSkin();
    return window != nullptr ? std::make_unique<Lettering> (window->face) : nullptr;
}
} // namespace

VfdText vfdLines (const house::Model& m, const ParameterValues& values, const house::Element& touched, int cells1, int cells2)
{
    const auto physical = [&values] (const house::Element& e) { return (double) parameterPhysicalValue (values, e.paramIndex); };
    VfdText out;
    auto name = touched.vfdName.isNotEmpty() ? touched.vfdName : touched.label;
    const auto value = house::compactValue (touched, physical (touched));
    if (name.length() + 1 + value.length() > cells1 && name.containsChar (' '))
        name = name.fromFirstOccurrenceOf (" ", false, false);
    out.line1 = name + " " + value;
    if (touched.screen.isEmpty())
    {
        if (touched.kind == "rotary")
            for (const auto& e : m.elements)
                if (const auto n = e.display["bar"]["centers"].size(); e.role == "vfd" && n > 0)
                {
                    const auto p = house::proportionOf (touched, touched.range.clipValue (physical (touched)));
                    out.lit = juce::jlimit (0, n, (int) std::ceil (p * n - 1.0e-6));
                }
        return out;
    }
    std::vector<const house::Element*> others;
    for (const auto& e : m.elements)
        if (&e != &touched && e.screen == touched.screen && ! e.detents && shownAt (e, values))
            others.push_back (&e);
    std::stable_sort (others.begin(), others.end(), [] (const auto* a, const auto* b) { return a->slot < b->slot; });
    for (const auto* e : others)
    {
        const auto pair = e->lcdLabel + " " + house::compactValue (*e, physical (*e));
        const auto next = out.line2.isEmpty() ? pair : out.line2 + " " + pair;
        if (next.length() > cells2)
            break;
        out.line2 = next;
    }
    return out;
}

/** A display over its view, its glass lettered at the device's scale. It takes no presses and no focus. */
class MakerFace::Glass final : public juce::Component
{
public:
    Glass (const house::Model& m, const house::Element& e, const Lettering* l) : display (e, l), model (m), let (l)
    {
        setComponentID (e.id);
        setInterceptsMouseClicks (false, false);
        setBounds (e.rect.getSmallestIntegerContainer());
    }

    void show (Lettered t)
    {
        if (t == text)
            return;
        text = std::move (t);
        image = {};
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (const auto s = renderScaleFor (g); s != scale || ! image.isValid())
        {
            scale = s;
            image = glassImage (model, display, let, text, scale);
        }
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale));
    }

    const Lettered& shown() const { return text; }

    const DisplayGlass display;

private:
    const house::Model& model;
    const Lettering* let;
    Lettered text;
    juce::Image image;
    float scale = 1.0f;
};

/** A layer of the reveal's move over the whole faceplate, which paints it. It takes no presses and no focus, and
    assistive tools skip it. */
class MakerFace::Layer final : public juce::Component
{
public:
    explicit Layer (std::function<void (juce::Graphics&)> p) : painter (std::move (p))
    {
        setInterceptsMouseClicks (false, false);
        setAccessible (false);
    }

    void paint (juce::Graphics& g) override { painter (g); }

private:
    std::function<void (juce::Graphics&)> painter;
};

/** A spare knob: its rotor at rest on a slot that the screen leaves unused. It takes no presses and no focus. */
class MakerFace::Still final : public juce::Component
{
public:
    Still (const house::Model& m, const house::Element& e) : element (e), model (m)
    {
        setComponentID (e.id);
        setInterceptsMouseClicks (false, false);
        const auto& a = m.asset (e.layer ("pointer"));
        const auto half = std::max ({ e.faceR, a.anchor.x, a.anchor.y, (float) a.image.getWidth() - a.anchor.x, (float) a.image.getHeight() - a.anchor.y }) + 2.0f;
        setBounds (juce::Rectangle<float> (2 * half, 2 * half).withCentre (e.pivot).getSmallestIntegerContainer());
    }

    void paint (juce::Graphics& g) override { house::paintPointer (g, model, element, 0.0f, getPosition().toFloat()); }

    const house::Element& element;

private:
    const house::Model& model;
};

std::unique_ptr<MakerFace> MakerFace::forRack (InstrumentProcessor& p, MakerSkinPtr skin, const RackInstance& rack, house::Host host)
{
    if(!skin||!skin->owner)return nullptr;
    auto model = bound (*skin, rack);
    // The design is its example's, whose units have these keys, and a placed rack binds each of its units.
    if (! model)
        return nullptr;
    return std::unique_ptr<MakerFace> (new MakerFace (p, std::move(skin), rack, std::move (*model), std::move (host)));
}

juce::Image MakerFace::composeFor (const MakerSkin& skin, const RackInstance& rack, const ParameterValues& values)
{
    const auto model = bound (skin, rack);
    return model ? composed (*model, values, false, windowLettering().get()) : juce::Image();
}

MakerFace::MakerFace (InstrumentProcessor& p, MakerSkinPtr s, const RackInstance& r, house::Model m, house::Host h)
    : skinOwner(std::move(s)), skin (*skinOwner), model (std::move (m)), host (std::move (h)),
      tileText (TileArrayFace::isTile (model) ? std::make_unique<house::GeneratedText>() : nullptr),
      context { model, p, host, {} }, face (context, "face", {}),
      reveal (std::make_unique<house::View> (context, model.reveal.view, juce::String())),
      drawerFront (isDrawer() && model.reveal.front.isNotEmpty() ? std::make_unique<house::View> (context, model.reveal.front, juce::String()) : nullptr),
      hang (isDoor() && model.reveal.hang.isNotEmpty() ? std::make_unique<house::View> (context, model.reveal.hang, juce::String()) : nullptr),
      stripView (model.strip.isNotEmpty() ? std::make_unique<house::View> (context, model.strip, juce::String()) : nullptr),
      rack (r.id), bindings (r.bindings)
{
    context.generatedText = tileText.get();
    setComponentID ("rack.face");
    setTitle (skin.maker.isEmpty() ? skin.model : skin.maker + ", " + skin.model);
    setFocusContainerType (FocusContainerType::focusContainer);
    // A press on the bare plate leaves the focus where it was, as on a house plate, rather than giving it to a control.
    for (auto* c : std::initializer_list<juce::Component*> { this, &face, reveal.get(), drawerFront.get(), hang.get(), stripView.get() })
        if (c != nullptr)
            c->setMouseClickGrabsKeyboardFocus (false);
    if (stripView != nullptr)
    {
        stripView->setBounds (0, 0, face.getWidth(), stripHeight (model));
        addChildComponent (*stripView);
    }
    face.setExplicitFocusOrder (1);
    reveal->setExplicitFocusOrder (2);
    addAndMakeVisible (face);
    addChildComponent (*reveal);
    if (isDrawer())
    {
        // The deck slides out from under the face.
        reveal->toBack();
        if (drawerFront != nullptr)
        {
            drawerFront->setExplicitFocusOrder (3);
            addAndMakeVisible (*drawerFront);
        }
    }
    else
    {
        // The recess or the door's panel lies over the face in its pixels, and the door below it in the faceplate's;
        // only their controls take presses.
        reveal->setBounds (face.getBounds());
        reveal->setInterceptsMouseClicks (false, true);
        if (hang != nullptr)
        {
            hang->setExplicitFocusOrder (3);
            hang->setInterceptsMouseClicks (false, true);
            addChildComponent (*hang);
        }
    }
    const auto local = [] (const juce::String& id) { return id.fromFirstOccurrenceOf (".", false, false); };
    opener = face.button (local (model.reveal.opener));
    if (opener == nullptr && drawerFront != nullptr)
        opener = drawerFront->button (local (model.reveal.opener));
    if (opener != nullptr)
    {
        opener->onAction = [this] { setOpen (isDrawer() ? ! open : true); };
        if (model.reveal.titles.size() == 2)
            opener->setTitle (model.reveal.titles[0]);
        // A drawer without a front is pulled from the face, its pull after the deck in Tab order. The face lies at the
        // faceplate's origin, so the pull keeps its bounds.
        if (isDrawer() && drawerFront == nullptr && opener->getParentComponent() == &face)
        {
            opener->setExplicitFocusOrder (3);
            addAndMakeVisible (*opener);
        }
    }
    if (auto* all = reveal->button ("all-controls"))
        all->onAction = [this] { const auto scope = context.frame(); if (! context.closing() && onSetup) onSetup(); };
    for (const auto& id : model.reveal.closers)
    {
        auto* b = reveal->button (local (id));
        if (b == nullptr && hang != nullptr)
            b = hang->button (local (id));
        if (b == nullptr)
            continue;
        b->onAction = [this] { setOpen (false); };
        if (isDoor() && model.reveal.titles.size() == 2)
            b->setTitle (model.reveal.titles[1]);
        if (closer == nullptr)
            closer = b;
    }
    for (std::size_t i = 0; i < model.pages.size(); ++i)
        if (auto* b = pageButton (i))
        {
            b->radio = true;
            b->invalidateAccessibilityHandler();
            b->onAction = [this, i] { choosePage (i, false); };
            b->onArrow = [this] (int by)
            {
                const auto n = (int) model.pages.size();
                choosePage ((std::size_t) (((int) pageIndex + by + n) % n), true);
            };
        }
    for (auto& s : reveal->sliders)
        if (s->element.screen.isNotEmpty() && s->element.vfdName.isNotEmpty())
            s->setTitle (s->element.vfdName);
    // A faceplate that reads out on its own display says each value as the display writes it.
    if (! model.hostReadout)
        for (auto* v : { &face, reveal.get() })
            for (auto& s : v->sliders)
                s->textFromValueFunction = [&e = s->element] (double x) { return house::compactValue (e, x); };
    for (const auto& e : model.elements)
        if (e.kind == "still")
            if (auto* v = viewNamed (e.view))
            {
                stills.push_back (std::make_unique<Still> (model, e));
                v->addChildComponent (*stills.back());
                if (TileArrayFace::isTile (model)) stills.back()->toBack();
            }
    for (const auto& e : model.elements)
    {
        const auto unit = std::find_if (r.design.units.begin(), r.design.units.end(), [&e] (const RackUnit& u) { return u.id == e.rackUnit; });
        const auto* binding = unit != r.design.units.end() ? rackUnitBinding (r, (std::uint8_t) e.rackUnit) : nullptr;
        if (e.tapeRect.isEmpty() || binding == nullptr || ! e.key.startsWith ("modulation."))
            continue;
        // "modulation.1.amount": the route's other fields share its prefix.
        const auto prefix = e.key.upToLastOccurrenceOf (".", true, false);
        const auto field = [&] (const char* name) { return rackParameterIndex (unit->module, binding->slot, prefix + name); };
        const auto enabled = field ("enabled"), source = field ("source"), target = field ("target"), bank = field ("bank");
        if (enabled && source && target && bank)
            route = Route { &e, *enabled, *source, *target, *bank, prefix.fromFirstOccurrenceOf (".", false, false).upToFirstOccurrenceOf (".", false, false) };
    }
    letters = windowLettering();
    for (const auto& e : model.elements)
        if (e.kind == "display")
            if (auto* v = viewNamed (e.view))
            {
                glasses.push_back (std::make_unique<Glass> (model, e, letters.get()));
                v->addAndMakeVisible (*glasses.back());
            }
    touched = firstControl (model);
    context.readouts = model.hostReadout;
    context.touched = [this] (const house::Element& e)
    {
        if (touched == &e)
            return;
        touched = &e;
        letter();
    };
    context.valueShown = [this] (const house::Element&)
    {
        if (tileArrayFace != nullptr)
            tileArrayFace->valuesChanged();
        showScreen();
        letter();
    };
    if (isRecess() && ! model.reveal.motion.turns.empty())
    {
        turnLayer = std::make_unique<Layer> ([this] (juce::Graphics& g) { paintFasteners (g); });
        addChildComponent (*turnLayer, getIndexOfChildComponent (reveal.get()));
    }
    if (isRecess() || isDoor())
    {
        moveLayer = std::make_unique<Layer> ([this] (juce::Graphics& g) { paintMove (g); });
        addChildComponent (*moveLayer);
    }
    if (TileArrayFace::isTile (model))
    {
        tileArrayFace = std::make_unique<TileArrayFace> (*this, context, face, *reveal, r.id, 0);
        tileArrayFace->selectPad (0);
    }
    showScreen();
    letter();
    layout();
}

MakerFace::~MakerFace()
{
    if (onDrumSourceClose)
        onDrumSourceClose();
    onDrumSource = nullptr;
    onDrumSourceClose = nullptr;
    tileArrayFace.reset();
    // The controls may show their parameters again as they go, after the displays.
    context.touched = context.valueShown = nullptr;
    suspendInputs();
}

void MakerFace::suspendInputs()
{
    const auto scope = context.frame();
    if (tileArrayFace != nullptr)
        tileArrayFace->suspend();
    if (onDrumSourceClose)
        onDrumSourceClose();
    std::vector<juce::Component::SafePointer<house::View>> views;
    for (auto* view : { &face, reveal.get(), drawerFront.get(), hang.get(), stripView.get() })
        if (view != nullptr)
            views.emplace_back (view);
    for (const auto& view : views)
        if (view != nullptr)
            view->suspendInputs();
}

void MakerFace::visibilityChanged()
{
    if (tileArrayFace != nullptr)
    {
        if (isVisible()) tileArrayFace->resume();
        else tileArrayFace->suspend();
    }
}

void MakerFace::retire()
{
    stopTimer();onOpen=onSetup=nullptr;context.touched=context.valueShown=nullptr;
    host.closing=[]{return true;};
    if(tileArrayFace)tileArrayFace->suspend();
    if(onDrumSourceClose)onDrumSourceClose();
    onDrumSource=nullptr;onDrumSourceClose=nullptr;tileArrayFace.reset();
    face.retire();for(auto* view:{reveal.get(),drawerFront.get(),hang.get(),stripView.get()})if(view)view->retire();
    host={};host.closing=[]{return true;};
}

ParameterValues MakerFace::liveValues() const
{
    ParameterValues live {};
    for (std::size_t i = 0; i < live.size(); ++i)
        live[i] = context.processor.control (i).getValue();
    return live;
}

void MakerFace::letter()
{
    if (glasses.empty())
        return;
    const auto live = liveValues();
    for (auto& g : glasses)
    {
        if (g->display.element.role == "vfd" && touched != nullptr)
            shownVfd = vfdOn (model, live, g->display, letters.get(), *touched);
        g->show (letteredFor (model, live, g->display, letters.get(), touched, screen()));
    }
}

house::SkinButton* MakerFace::pageButton (std::size_t i)
{
    return i < model.pages.size() ? reveal->button (model.pages[i].button.fromFirstOccurrenceOf (".", false, false)) : nullptr;
}

juce::String MakerFace::page() const
{
    return model.pages.empty() ? juce::String() : model.pages[pageIndex].id;
}

juce::String MakerFace::screen() const
{
    return model.pages.empty() ? juce::String() : model.pages[pageIndex].screens[(int) screenIndex];
}

juce::StringArray MakerFace::lcd() const
{
    juce::StringArray out;
    for (const auto& g : glasses)
        if (g->display.element.role == "lcd")
            for (const auto& [key, line] : g->display.lines)
            {
                const auto t = g->shown().lines.find (key);
                out.add (t != g->shown().lines.end() ? t->second : juce::String());
            }
    return out;
}

void MakerFace::choosePage (std::size_t i, bool focus)
{
    if (i >= model.pages.size())
        return;
    screenIndex = i == pageIndex ? (screenIndex + 1) % (std::size_t) model.pages[i].screens.size() : 0;
    pageIndex = i;
    const auto live = liveValues();
    const house::Element* first = nullptr;
    for (const auto& e : model.elements)
        if (e.screen == screen() && e.rackUnit >= 0 && shownAt (e, live) && (first == nullptr || e.slot < first->slot))
            first = &e;
    if (first != nullptr)
        touched = first;
    showScreen();
    letter();
    remark();
    if (focus)
        if (auto* b = pageButton (pageIndex))
            b->grabKeyboardFocus();
}

void MakerFace::showScreen()
{
    if (model.pages.empty())
    {
        for (auto& s : stills)
            s->setVisible (tileArrayFace == nullptr || tileArrayFace->shows (s->element));
        return;
    }
    const auto live = liveValues();
    const auto current = screen();
    std::vector<int> used;
    for (auto& s : reveal->sliders)
        if (s->element.screen.isNotEmpty())
        {
            const auto on = s->element.screen == current && shownAt (s->element, live);
            s->setVisible (on);
            if (on)
                used.push_back (s->element.slot);
        }
    for (auto& s : stills)
        s->setVisible (std::find (used.begin(), used.end(), s->element.slot) == used.end());
    for (std::size_t i = 0; i < model.pages.size(); ++i)
        if (auto* b = pageButton (i))
        {
            const auto chosen = i == pageIndex;
            const auto& p = model.pages[i];
            b->setActionLit (chosen);
            b->setWantsKeyboardFocus (chosen);
            b->setTitle (p.label + " page" + (chosen ? ", screen " + juce::String (screenIndex + 1) + " of " + juce::String (p.screens.size()) : juce::String()));
        }
    // A touched item that its shown_when hides gives way to the one shown in its slot.
    if (touched != nullptr && touched->screen.isNotEmpty() && ! shownAt (*touched, live))
        for (const auto& e : model.elements)
            if (&e != touched && e.screen == touched->screen && e.slot == touched->slot && e.rackUnit >= 0 && shownAt (e, live))
            {
                touched = &e;
                break;
            }
}

void MakerFace::layout()
{
    if (folded)
    {
        stripView->setTopLeftPosition (0, 0);
        setSize (face.getWidth(), stripView->getHeight());
        return;
    }
    const auto shown = open || isMoving();
    // The new electronic deck accepts edits once extended. Existing doors retain their close-key focus in motion.
    reveal->setEnabled (! skin.nativeEditor || settledOpen());
    const auto drawn = opening();
    int bottom = 0;
    for (const auto& [name, top] : stacked (model, shown))
        if (auto* view = viewNamed (name))
        {
            view->setTopLeftPosition (0, top);
            bottom = std::max (bottom, top + view->getHeight());
        }
    if (isDrawer() && shown)
    {
        // The deck's foot the drawn fraction of the deck below the face, on the front's top.
        const auto y = reveal->getY() + juce::roundToInt (drawn * (float) reveal->getHeight());
        reveal->setTopLeftPosition (0, y - reveal->getHeight());
        bottom = y;
        if (drawerFront != nullptr)
        {
            drawerFront->setTopLeftPosition (0, y);
            bottom = y + drawerFront->getHeight();
        }
    }
    const auto doorHeight = face.getHeight() + juce::roundToInt (drawn * (float) (face.getHeight() * model.reveal.units / skin.units));
    setSize (face.getWidth(), isDoor() && shown ? doorHeight : bottom);
    // The recess fades in as its cover lifts, or shows where its lid has slid or turned up from; the hanging door's
    // inside is drawn by the swing until it settles.
    if (isRecess())
        reveal->setAlpha (isMoving() && ! slides() && ! hinges() ? lift() : 1.0f);
    if (hang != nullptr)
        hang->setAlpha (isMoving() ? 0.0f : 1.0f);
    for (auto* l : { turnLayer.get(), moveLayer.get() })
        if (l != nullptr)
        {
            l->setBounds (getLocalBounds());
            l->setVisible (isMoving());
        }
}

float MakerFace::lift() const
{
    return juce::jlimit (0.0f, 1.0f, (opening() - fastenersTurned) / (1.0f - fastenersTurned));
}

float MakerFace::fastenerTurn() const
{
    return isRecess() && ! model.reveal.motion.turns.empty() ? 90.0f * std::min (1.0f, opening() / fastenersTurned) : 0.0f;
}

juce::Rectangle<float> MakerFace::movingArea() const
{
    if (! isMoving())
        return {};
    const auto& m = model.reveal.motion;
    if (slides())
        return m.lid.translated (m.slide.x * opening(), m.slide.y * opening()).getIntersection (m.lid);
    if (hinges())
    {
        // Turned up until it stands its open height tall, seen foreshortened by the angle's cosine.
        const auto upright = std::acos (juce::jlimit (0.0f, 1.0f, m.openHeight / m.lid.getHeight()));
        return m.lid.withHeight (m.lid.getHeight() * std::cos (opening() * upright));
    }
    if (isRecess())
    {
        const auto s = 1.0f + coverGrowth * lift();
        return m.lid.withSizeKeepingCentre (m.lid.getWidth() * s, m.lid.getHeight() * s);
    }
    if (! isDoor() || hang == nullptr)
        return {};
    const auto c = std::cos (juce::degreesToRadians (swing()));
    if (swing() < 90.0f)
        return m.door.withTop (m.door.getBottom() - m.door.getHeight() * c);
    const auto& a = model.asset (model.reveal.hang + ":plate");
    return { a.anchor.x, a.anchor.y, (float) a.image.getWidth(), -(float) a.image.getHeight() * c };
}

void MakerFace::paintFasteners (juce::Graphics& g) const
{
    const auto& plate = model.asset ("face:plate");
    // A quarter-turn fastener comes loose anticlockwise.
    const auto turn = juce::degreesToRadians (-fastenerTurn());
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    for (const auto& disc : model.reveal.motion.turns)
    {
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path p;
        p.addEllipse (disc);
        g.reduceClipRegion (p);
        g.drawImageTransformed (plate.image, juce::AffineTransform::translation (plate.anchor.x, plate.anchor.y).rotated (turn, disc.getCentreX(), disc.getCentreY()));
    }
}

void MakerFace::paintMove (juce::Graphics& g) const
{
    const auto area = movingArea();
    if (area.getHeight() <= 0.0f)
        return;
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (slides())
    {
        // The closed lid's pixels carried along, cut off where it runs under the lid's far edge.
        const auto& m = model.reveal.motion;
        const auto& plate = model.asset ("face:plate");
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path clip;
        clip.addRectangle (area);
        g.reduceClipRegion (clip);
        g.drawImageTransformed (plate.image, juce::AffineTransform::translation (plate.anchor.x + m.slide.x * opening(), plate.anchor.y + m.slide.y * opening()));
        return;
    }
    if (hinges())
    {
        // The closed lid's pixels squashed to its hinge, darker as it turns up from the light.
        const auto& m = model.reveal.motion;
        const auto& plate = model.asset ("face:plate");
        const auto seen = area.getHeight() / m.lid.getHeight();
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path clip;
        clip.addRectangle (area);
        g.reduceClipRegion (clip);
        g.drawImageTransformed (plate.image, juce::AffineTransform::translation (plate.anchor.x, plate.anchor.y - m.lid.getY())
                                                 .scaled (1.0f, seen)
                                                 .translated (0.0f, area.getY()));
        g.setColour (juce::Colours::black.withAlpha (0.35f * (1.0f - seen)));
        g.fillRect (area);
        return;
    }
    if (isRecess())
    {
        const auto p = lift();
        if (p <= 0.0f)
            return;
        const auto& m = model.reveal.motion;
        const auto& plate = model.asset ("face:plate");
        const auto grown = juce::AffineTransform::scale (area.getWidth() / m.lid.getWidth(), area.getHeight() / m.lid.getHeight(), m.lid.getCentreX(), m.lid.getCentreY());
        juce::Path outline;
        outline.addRoundedRectangle (area, m.corner * area.getWidth() / m.lid.getWidth());
        // The shadow spreads as the cover rises and is gone as it is.
        juce::DropShadow (juce::Colours::black.withAlpha (2.0f * p * (1.0f - p)), juce::roundToInt (4.0f + 28.0f * p), { 0, juce::roundToInt (2.0f + 10.0f * p) }).drawForPath (g, outline);
        juce::Graphics::ScopedSaveState saved (g);
        g.beginTransparencyLayer (1.0f - p);
        g.reduceClipRegion (outline);
        const auto placed = juce::AffineTransform::translation (plate.anchor.x, plate.anchor.y);
        g.drawImageTransformed (plate.image, placed.followedBy (grown));
        const auto turn = juce::degreesToRadians (-fastenerTurn());
        for (const auto& disc : m.turns)
        {
            juce::Graphics::ScopedSaveState inDisc (g);
            juce::Path d;
            d.addEllipse (disc);
            d.applyTransform (grown);
            g.reduceClipRegion (d);
            g.drawImageTransformed (plate.image, placed.rotated (turn, disc.getCentreX(), disc.getCentreY()).followedBy (grown));
        }
        g.endTransparencyLayer();
        return;
    }
    // The closed door's pixels squashed to its foot, then the hanging door's to its top, darkest edge-on.
    const auto closed = swing() < 90.0f;
    const auto& plate = model.asset (closed ? juce::String ("face:plate") : model.reveal.hang + ":plate");
    const auto source = closed ? model.reveal.motion.door
                               : juce::Rectangle<float> (plate.anchor.x, plate.anchor.y, (float) plate.image.getWidth(), (float) plate.image.getHeight());
    juce::Graphics::ScopedSaveState saved (g);
    juce::Path clip;
    clip.addRectangle (area);
    g.reduceClipRegion (clip);
    g.drawImageTransformed (plate.image, juce::AffineTransform::translation (plate.anchor.x, plate.anchor.y - source.getY())
                                             .scaled (1.0f, area.getHeight() / source.getHeight())
                                             .translated (0.0f, area.getY()));
    g.setColour (juce::Colours::black.withAlpha (0.4f * std::sin (juce::degreesToRadians (swing()))));
    g.fillRect (area);
}

float MakerFace::along() const
{
    if (! isMoving())
        return 1.0f;
    return juce::jlimit (0.0f, 1.0f, from + (float) ((motion::now() - since) / std::max (1, model.reveal.motion.ms)));
}

float MakerFace::opening() const
{
    const auto e = motion::ease (along());
    return open ? e : 1.0f - e;
}

float MakerFace::shownUnits() const
{
    return (float) skin.units + (isDrawer() || isDoor() ? opening() * (float) model.reveal.units : 0.0f);
}

void MakerFace::timerCallback()
{
    if (along() >= 1.0f)
    {
        stopTimer();
        reveal->setVisible (open);
        if (hang != nullptr)
            hang->setVisible (open);
    }
    layout();
    repaint();
    if (onOpen)
        onOpen();
}

house::View* MakerFace::viewNamed (const juce::String& view)
{
    for (auto* v : { &face, reveal.get(), drawerFront.get(), hang.get(), stripView.get() })
        if (v != nullptr && v->name == view)
            return v;
    return nullptr;
}

juce::Point<float> MakerFace::offsetOf (const juce::String& view) const
{
    for (const auto* v : std::initializer_list<const house::View*> { &face, reveal.get(), drawerFront.get(), hang.get(), stripView.get() })
        if (v != nullptr && v->name == view)
            return v->getPosition().toFloat();
    return {};
}

int MakerFace::units() const
{
    return skin.units + ((isDrawer() || isDoor()) && open ? model.reveal.units : 0);
}

juce::Rectangle<int> MakerFace::openedArea() const
{
    if (! open)
        return {};
    // While it moves, only what is drawn below the face: the door as far as the faceplate has grown, the deck from the
    // face's foot.
    if (hang != nullptr)
    {
        const auto& a = model.asset (model.reveal.hang + ":plate");
        return juce::Rectangle<int> (juce::roundToInt (a.anchor.x), juce::roundToInt (a.anchor.y), a.image.getWidth(), a.image.getHeight())
            .getIntersection (getLocalBounds());
    }
    if (! isDrawer())
        return {};
    const auto drawn = drawerFront != nullptr ? reveal->getBounds().getUnion (drawerFront->getBounds()) : reveal->getBounds();
    return drawn.withTop (std::max (drawn.getY(), face.getBottom()));
}

void MakerFace::setFolded (bool f)
{
    if (stripView == nullptr || f == folded)
        return;
    if (f)
    {
        suspendInputs();
        setOpen (false, false);
    }
    folded = f;
    face.setVisible (! folded);
    if (opener != nullptr && opener->getParentComponent() == this)
        opener->setVisible (! folded);
    stripView->setVisible (folded);
    layout();
    repaint();
}

juce::Image MakerFace::composeStrip (const ParameterValues& v) const
{
    if (model.strip.isEmpty())
        return {};
    return composedViews (model, v, { { model.strip, 0 } }, stripHeight (model), false, letters.get());
}

void MakerFace::setOpen (bool o, bool moving)
{
    if (o == open || (o && folded))
        return;
    const auto scope = context.frame();
    const juce::Component::SafePointer<MakerFace> alive (this);
    if (! o)
    {
        if (tileArrayFace != nullptr && onDrumSourceClose)
            onDrumSourceClose();
        reveal->suspendInputs();
        if (alive == nullptr)
            return;
        if (hang != nullptr)
            hang->suspendInputs();
        if (alive == nullptr)
            return;
    }
    if (moving && model.reveal.motion.ms > 0 && ! motion::reduced())
    {
        // Turned back part-way, the eased fraction mirrors: the reveal continues from where it is drawn.
        from = isMoving() ? 1.0f - along() : 0.0f;
        since = motion::now();
        if (from >= 1.0f)
            stopTimer();
        else
            startTimerHz (60);
    }
    else
        stopTimer();
    open = o;
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const auto inside = reveal->isParentOf (focused) || (hang != nullptr && hang->isParentOf (focused));
    const auto fromOpener = opener != nullptr && focused == opener;
    if (! open)
    {
        reveal->hideReadouts();
        if (hang != nullptr)
            hang->hideReadouts();
    }
    reveal->setVisible (open || isMoving());
    if (hang != nullptr)
        hang->setVisible (open || isMoving());
    // Closing, the revealed controls leave Tab order at the press though they are drawn until the move ends.
    const auto tabbed = open ? FocusContainerType::focusContainer : FocusContainerType::keyboardFocusContainer;
    reveal->setFocusContainerType (tabbed);
    if (hang != nullptr)
        hang->setFocusContainerType (tabbed);
    layout();
    if (opener != nullptr)
    {
        // While open a recess lies over its cover, which keeps its place in Tab order; a door's panel over its top
        // notch, which leaves it.
        if (! isDrawer())
            opener->setInterceptsMouseClicks (! open, false);
        else if (opener->element.layer ("lit").isNotEmpty())
            opener->setActionLit (open);
        if (isDoor())
            opener->setWantsKeyboardFocus (! open);
        else if (model.reveal.titles.size() == 2)
            opener->setTitle (model.reveal.titles[open ? 1 : 0]);
        if (inside)
            opener->grabKeyboardFocus();
        else if (open && fromOpener && closer != nullptr && isDoor())
            closer->grabKeyboardFocus();
    }
    remark();
    if (onOpen)
        onOpen();
}

bool MakerFace::keyPressed (const juce::KeyPress& key)
{
    if (! isRecess() || ! open || key.getKeyCode() != juce::KeyPress::escapeKey)
        return false;
    setOpen (false);
    return true;
}

juce::Image MakerFace::compose (const ParameterValues& v, bool opened) const
{
    if (tileArrayFace != nullptr)
    {
        const auto views = stacked (model, opened);
        return composedViews (model, v, views, heightOf (model, views, opened), opened, letters.get(), tileArrayFace.get());
    }
    return composed (model, v, opened, letters.get());
}

std::vector<bool> MakerFace::letteredShown() const
{
    std::vector<bool> out;
    for (const auto& [e, art] : lettered)
        out.push_back (shownAt (*e, values));
    return out;
}

void MakerFace::refresh (const SoundDocument& d)
{
    if (tileArrayFace != nullptr)
        tileArrayFace->refresh();
    const auto taped = routeTape();
    const auto tapesShown = letteredShown();
    values = d.values;
    locks = d.controlLocks;
    for (auto& e : model.elements)
        if (e.nativeActiveWhen.unit >= 0)
        {
            const auto alpha = nativeAlpha (e, values);
            if (e.nativeAlpha != alpha)
            {
                e.nativeAlpha = alpha;
                if (auto* view = viewNamed (e.view))
                {
                    if (auto* c = view->control (e.localId()))
                        c->setAlpha (alpha);
                    view->repaint();
                }
            }
        }
    if (model.nativeFace)
    {
        const auto description = nativeStatus (model, values) + "; settings retained for later processing";
        if (getDescription() != description)
        {
            setDescription (description);
            repaint();
        }
    }
    nativeLocked = false;
    if (d.personalities)
        for (const auto& instance : d.personalities->instances)
            if (instance.id == rack)
                nativeLocked = rackLocked (d, instance);
    std::vector<const house::Element*> now;
    for (const auto& e : model.elements)
        if (e.hasDot && e.rackUnit >= 0 && e.paramIndex < parameterCount && locks[e.paramIndex])
            now.push_back (&e);
    if (now != locked)
    {
        locked = std::move (now);
        repaint();
    }
    if (route)
    {
        if (auto* v = viewNamed (route->knob->view))
            if (auto* k = v->control (route->knob->localId()))
                k->setTitle (routeTape().isEmpty() ? route->knob->label : routeWords (false));
        if (routeTape() != taped)
            repaint();
    }
    if (letteredShown() != tapesShown)
        repaint();
    remark();
}

void MakerFace::setHostLook (const Asset& dot, float facePixels, const house::Model& houseSkin)
{
    if (lockDot == &dot && pixel == facePixels && tapeSkin == &houseSkin)
        return;
    lockDot = &dot;
    pixel = facePixels;
    tapeSkin = &houseSkin;
    if (route)
        tape.emplace (houseSkin, "unit:tape", house::Pt (route->knob->tapeRect.getWidth(), route->knob->tapeRect.getHeight()));
    lettered.clear();
    for (const auto& e : model.elements)
        if (e.kind == "tape" && e.text.isNotEmpty())
            lettered.emplace_back (&e, house::TapeArt (houseSkin, "unit:tape", e.size));
    repaint();
}

juce::String MakerFace::routeWords (bool upper) const
{
    if (! route)
        return {};
    const auto choice = [this] (std::size_t i)
    {
        const auto& names = parameterRegistry()[i].choices;
        const auto n = juce::roundToInt (parameterPhysicalValue (values, i));
        return n >= 0 && (std::size_t) n < names.size() ? names[(std::size_t) n] : juce::String();
    };
    const auto on = parameterPhysicalValue (values, route->enabled) > 0.5f;
    const auto source = choice (route->source), target = choice (route->target), bank = choice (route->bank);
    const auto& printed = route->knob->printedRoute;
    if (on && source == printed.source && target == printed.target && bank == printed.bank)
        return {};
    const auto words = on ? source + " to " + (bank == printed.bank ? juce::String() : bank + " ") + target : "Route " + route->number + " off";
    return upper ? words.toUpperCase() : words;
}

void MakerFace::showProposal (std::shared_ptr<const SoundDocument> d)
{
    if (proposal == d)
        return;
    proposal = std::move (d);
    remark();
}

std::vector<MakerFace::Mark> MakerFace::marks() const
{
    std::vector<Mark> out;
    for (const auto& [e, v] : marked)
        out.push_back ({ e->id, v });
    return out;
}

bool MakerFace::proposesPanel() const
{
    return outlined;
}

std::vector<std::size_t> MakerFace::parameters() const
{
    auto all = revealParameters;
    for (const auto& e : model.elements)
        if (e.rackUnit >= 0 && e.paramIndex < parameterCount)
            all.push_back (e.paramIndex);
    std::sort (all.begin(), all.end());
    all.erase (std::unique (all.begin(), all.end()), all.end());
    return all;
}

void MakerFace::setRevealParameters (std::vector<std::size_t> parameters)
{
    parameters.erase (std::remove_if (parameters.begin(), parameters.end(), [] (const auto p) { return p >= parameterCount; }), parameters.end());
    std::sort (parameters.begin(), parameters.end());
    parameters.erase (std::unique (parameters.begin(), parameters.end()), parameters.end());
    if (parameters != revealParameters)
    {
        revealParameters = std::move (parameters);
        remark();
    }
}

void MakerFace::remark()
{
    const RackInstance* kept = nullptr;
    if (proposal != nullptr && proposal->personalities != nullptr)
        for (const auto& i : proposal->personalities->instances)
            if (i.id == rack)
                kept = &i;
    std::vector<std::pair<const house::Element*, float>> now;
    bool outline = false;
    if (kept != nullptr && kept->design.id == skin.design)
    {
        if (lookup(*kept) != skinOwner || kept->bindings != bindings)
            outline = true;
        else
            for (const auto* view : { &face, reveal.get() })
                for (const auto& id : view->focusOrder)
                {
                    const auto e = std::find_if (model.elements.begin(), model.elements.end(), [&id] (const house::Element& x) { return x.id == id; });
                    if (e != model.elements.end() && e->rackUnit >= 0 && e->paramIndex < parameterCount && ! locks[e->paramIndex]
                        && std::abs (proposal->values[e->paramIndex] - values[e->paramIndex]) > 1e-6f)
                        now.push_back ({ &*e, parameterPhysicalValue (proposal->values, e->paramIndex) });
                }
    }
    // The reveal's changes that no face control shows, as a soft knob sharing FACET's parameter does not.
    int hidden = 0;
    if (kept != nullptr && kept->design.id == skin.design && ! outline && ! nativeLocked)
        for (const auto p : revealParameters)
            if (! locks[p] && std::abs (proposal->values[p] - values[p]) > 1e-6f)
                ++hidden;
    std::vector<int> pages (model.pages.size(), 0);
    const auto onScreen = [this] (const house::Element& e) { return (tileArrayFace == nullptr || tileArrayFace->shows (e))
        && (e.screen.isEmpty() || (e.screen == screen() && shownAt (e, values))); };
    const auto hiddenPads = tileArrayFace != nullptr ? (int) std::count_if (now.begin(), now.end(),
        [this] (const auto& item) { return ! tileArrayFace->shows (*item.first); }) : 0;
    for (const auto& x : now)
    {
        const auto* e = x.first;
        const auto onFace = std::any_of (now.begin(), now.end(), [e] (const auto& y) { return y.first->view == "face" && y.first->paramIndex == e->paramIndex; });
        if (e->view != model.reveal.view || onFace)
            continue;
        ++hidden;
        if (! onScreen (*e))
            for (std::size_t i = 0; i < model.pages.size(); ++i)
                pages[i] += model.pages[i].screens.contains (e->screen) ? 1 : 0;
    }
    if (model.nativeFace && kept != nullptr && kept->design.id == skin.design && ! outline && kept->bindings == bindings)
    {
        const auto front = parameters();
        for (const auto& local : model.nativeFace->controls)
            if (! locks[local.index] && ! std::binary_search (front.begin(), front.end(), local.index)
                && std::abs (proposal->values[local.index] - values[local.index]) > 1e-6f)
                ++hidden;
    }
    now.erase (std::remove_if (now.begin(), now.end(), [&onScreen] (const auto& x) { return ! onScreen (*x.first); }), now.end());
    for (auto* view : { &face, reveal.get() })
        for (const auto& id : view->focusOrder)
            if (auto* c = view->control (id.fromFirstOccurrenceOf (".", false, false)))
            {
                const auto m = std::find_if (now.begin(), now.end(), [&id] (const auto& x) { return x.first->id == id; });
                juce::String said;
                if (m != now.end())
                {
                    const auto& e = *m->first;
                    said = "AI proposes " + (e.positions.empty() ? house::formatValue (e, m->second) : house::positionFor (e, m->second).legend);
                }
                if (const auto item = std::find_if (model.elements.begin(), model.elements.end(), [&id] (const auto& e) { return e.id == id; }); item != model.elements.end() && ! nativeAt (item->nativeActiveWhen, values))
                    said = item->nativeInactiveDescription + (said.isEmpty() ? juce::String() : " " + said);
                c->setDescription (said);
            }
    const auto count = [] (int n) { return juce::String (n) + " control" + (n == 1 ? "" : "s"); };
    for (std::size_t i = 0; i < model.pages.size(); ++i)
        if (auto* b = pageButton (i))
            b->setDescription (pages[i] == 0 ? juce::String() : "AI proposes changes to " + count (pages[i]) + " on this page");
    if (opener != nullptr)
    {
        // The reveal's word as its opener prints it: "Open set-up", "Open programmer".
        const auto word = opener->element.label.fromFirstOccurrenceOf ("Open ", false, false);
        opener->setDescription (open || hidden == 0 ? juce::String() : model.nativeFace
            ? "AI proposes changes to " + count (hidden) + " under SET-UP"
            : "AI proposes changes to " + juce::String (hidden) + " " + word + " control" + (hidden == 1 ? "" : "s"));
    }
    if (now == marked && outline == outlined && hidden == revealChanges && pages == pageChanges && hiddenPads == tileHiddenChanges)
        return;
    marked = std::move (now);
    outlined = outline;
    revealChanges = hidden;
    pageChanges = std::move (pages);
    tileHiddenChanges = hiddenPads;
    repaint();
}

void MakerFace::paintOverChildren (juce::Graphics& g)
{
    paintNativeState (g, model, values);
    if (tape && route && (route->knob->view != model.reveal.view || settledOpen()))
        if (const auto text = routeTape(); text.isNotEmpty())
        {
            const auto r = route->knob->tapeRect + offsetOf (route->knob->view);
            tape->paint (g, r.getCentre(), 0.0f, text, house::TapeArt::widthFor (*tapeSkin, text, r.getHeight()) > r.getWidth());
        }
    for (const auto& [e, art] : lettered)
        if (! folded && (e->view != model.reveal.view || settledOpen()) && shownAt (*e, values))
            art.paint (g, e->centre + offsetOf (e->view), e->angle, e->text, house::TapeArt::widthFor (*tapeSkin, e->text, e->size.y) > e->size.x);
    // A soft knob's dot shows while its screen shows it.
    const auto showing = [this] (const house::Element& e)
    {
        if (e.screen.isEmpty())
            return true;
        auto* v = viewNamed (e.view);
        const auto* c = v != nullptr ? v->control (e.localId()) : nullptr;
        return c != nullptr && c->isVisible();
    };
    if (lockDot != nullptr && ! folded)
    {
        for (const auto* e : locked)
            if ((e->view != model.reveal.view || settledOpen()) && showing (*e))
                drawSprite (g, *lockDot, e->dot + offsetOf (e->view), pixel);
        if (tileArrayFace != nullptr && opener != nullptr && std::any_of (locked.begin(), locked.end(),
                [this] (const auto* e) { return e->view == model.reveal.view && (! open || ! tileArrayFace->shows (*e)); }))
            drawSprite (g, *lockDot, opener->element.rect.getTopRight() + house::Pt (-12, 12) + offsetOf (opener->element.view), pixel);
    }
    if (marked.empty() && ! outlined && revealChanges == 0)
        return;
    // Each mark dashed on its own, since a dashed stroke joins its sub-paths, in the host's pixels as the rows' are.
    juce::Graphics::ScopedSaveState saved (g);
    g.addTransform (juce::AffineTransform::scale (pixel));
    const auto dash = [&g, this] (juce::Path p)
    {
        p.applyTransform (juce::AffineTransform::scale (1.0f / pixel));
        // A dark edge keeps the white legible on a pale faceplate or a silver cap.
        ProgrammerLook::drawDashes (g, p, juce::Colours::black.withAlpha (0.6f), 6.0f, 4.0f, 4.0f);
        ProgrammerLook::drawDashes (g, p, juce::Colour (0xf2ffffff), 6.0f, 4.0f);
    };
    const auto box = [] (juce::Rectangle<float> r) { juce::Path p; p.addRectangle (r); return p; };
    if (folded)
    {
        for (const auto& e : model.elements)
            if (e.view == model.strip && e.kind == "display")
                dash (box (e.rect.expanded (3.0f * pixel) + offsetOf (e.view)));
        return;
    }
    if (outlined && settledOpen() && hang != nullptr)
    {
        // The face's outline, stepping down round the door.
        const auto top = face.getBounds().toFloat().reduced (4.0f * pixel), door = openedArea().toFloat().reduced (4.0f * pixel);
        juce::Path p;
        p.startNewSubPath (top.getTopLeft());
        for (const auto at : { top.getTopRight(), top.getBottomRight(), juce::Point<float> (door.getRight(), top.getBottom()), door.getBottomRight(),
                               door.getBottomLeft(), juce::Point<float> (door.getX(), top.getBottom()), top.getBottomLeft() })
            p.lineTo (at);
        p.closeSubPath();
        dash (p);
    }
    else if (outlined)
        dash (box (getLocalBounds().toFloat().reduced (4.0f * pixel)));
    for (const auto& [e, v] : marked)
    {
        const auto at = offsetOf (e->view);
        if (e->view == model.reveal.view && ! settledOpen())
            continue;
        if (e->kind == "rotary" || e->kind == "selector")
        {
            const auto degrees = e->kind == "selector" ? house::positionFor (*e, v).angle
                                                       : house::pointerAngleFor (*e, house::proportionOf (*e, e->range.clipValue ((double) v)));
            const auto a = juce::degreesToRadians (degrees);
            juce::Path p;
            p.startNewSubPath (e->pivot + at);
            p.lineTo (e->pivot + at + Pt (std::sin (a), -std::cos (a)) * e->faceR);
            dash (p);
        }
        else if (e->kind == "fader")
        {
            const auto& cap = model.asset (e->layer ("cap"));
            const auto y = house::Fader::capYAt (*e, house::proportionOf (*e, e->range.clipValue ((double) v)));
            dash (box (juce::Rectangle<float> ((float) cap.image.getWidth(), (float) cap.image.getHeight()).withPosition (e->capCentreX - cap.anchor.x, y - cap.anchor.y) + at));
        }
        else
            dash (box (e->rect.expanded (3.0f * pixel) + at));
    }
    if (opener != nullptr && (! open || tileHiddenChanges > 0) && revealChanges > 0)
        dash (box (opener->element.rect.expanded (3.0f * pixel) + offsetOf (opener->element.view)));
    if (settledOpen())
        for (std::size_t i = 0; i < pageChanges.size(); ++i)
            if (auto* b = pageButton (i); b != nullptr && pageChanges[i] > 0)
                dash (box (b->element.rect.expanded (3.0f * pixel) + offsetOf (b->element.view)));
}
} // namespace airack::studio
