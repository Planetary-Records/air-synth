#include "HouseSkin.h"

#include "HouseSkinData.h"
#include "ParameterRegistry.h"

#include <cmath>
#include <cstring>

namespace airack::studio::house
{
static float num (const juce::var& v) { return (float) (double) v; }
static Pt point (const juce::var& v)
{
    if (! v.isArray() || v.size() != 2 || ! std::isfinite (num (v[0])) || ! std::isfinite (num (v[1])))
        throw std::runtime_error ("skin point must contain two finite coordinates");
    return { num (v[0]), num (v[1]) };
}
static juce::Rectangle<float> rectOf (const juce::var& v)
{
    if (! v.isArray() || v.size() != 4)
        throw std::runtime_error ("skin rectangle must contain four coordinates");
    for (const auto& n : *v.getArray())
        if (! std::isfinite (num (n))) throw std::runtime_error ("skin rectangle must be finite");
    return { num (v[0]), num (v[1]), num (v[2]), num (v[3]) };
}

static Element::ShownWhen nativeBinding (const juce::var& v)
{
    const auto unit = (int) v.getProperty ("unit", -1);
    const auto key = v["key"].toString();
    const auto value = (double) v.getProperty ("value", 0.0);
    if (! v.isObject() || unit < 0 || unit > 255 || key.isEmpty() || key.length() > 128 || ! std::isfinite (value))
        throw std::runtime_error ("native face binding is invalid");
    return { unit, key, value, 0 };
}

static std::size_t registryIndex (const String& id)
{
    const auto registry = parameterRegistry();
    for (std::size_t i = 0; i < registry.size(); ++i)
        if (registry[i].id == id)
            return i;
    throw std::runtime_error (("the house skin binds a missing parameter " + id).toStdString());
}

/** The narrow-width rule's strip: the record's view's plate without the cuts' columns, each cut joined by a linear
    cross-fade of `blend` columns centred on it, split at `fill_at` into an ear and a plate, with the bare panel that
    fills a wider column; and the view's elements and groups copied to view "narrow", each element moved left by the
    cuts ending at or left of its centre. */
static void addNarrow (Model& m, const juce::var& record)
{
    const auto from = record["view"].toString();
    const auto& source = m.asset (from + ":plate").image;
    std::vector<std::pair<int, int>> cuts;
    for (const auto& c : *record["cuts"].getArray())
        cuts.emplace_back ((int) c[0], (int) c[1]);
    const int blend = (int) record["blend"], half = blend / 2, at = (int) record["fill_at"], h = source.getHeight();
    int width = source.getWidth();
    for (const auto& [a, b] : cuts)
        width -= b - a;
    if (width != (int) record["width"] || at <= 0 || at >= width)
        throw std::runtime_error ("the house skin's narrow strip is not its recorded width");

    juce::Image plate (source.getFormat(), width, h, false);
    {
        const juce::Image::BitmapData in (source, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData out (plate, juce::Image::BitmapData::writeOnly);
        jassert (in.pixelStride == out.pixelStride);
        const auto copy = [&] (int to, int at0, int n)
        {
            for (int y = 0; y < h; ++y)
                std::memcpy (out.getPixelPointer (to, y), in.getPixelPointer (at0, y), (size_t) (n * in.pixelStride));
        };
        int x = 0, s = 0;
        for (const auto& [a, b] : cuts)
        {
            copy (x, s, a - half - s);
            x += a - half - s;
            for (int i = 0; i < blend; ++i, ++x)
            {
                const auto t = ((float) i + 0.5f) / (float) blend;
                for (int y = 0; y < h; ++y)
                {
                    const auto *p = in.getPixelPointer (a - half + i, y), *q = in.getPixelPointer (b - half + i, y);
                    auto* o = out.getPixelPointer (x, y);
                    for (int c = 0; c < in.pixelStride; ++c)
                        o[c] = (juce::uint8) juce::roundToInt ((float) p[c] * (1.0f - t) + (float) q[c] * t);
                }
            }
            s = b + half;
        }
        copy (x, s, source.getWidth() - s);
    }
    Asset ear, rest, fill;
    ear.image = plate.getClippedImage ({ 0, 0, at, h }).createCopy();
    rest.image = plate.getClippedImage ({ at, 0, width - at, h }).createCopy();
    rest.anchor = { (float) at, 0.0f };
    fill.image = source.getClippedImage ({ (int) record["fill"][0], 0, (int) record["fill"][1] - (int) record["fill"][0], h }).createCopy();
    m.assets["narrow:ear"] = ear;
    m.assets["narrow:plate"] = rest;
    m.assets["narrow:fill"] = fill;

    const auto renamed = [] (const String& id) { return "narrow." + id.fromFirstOccurrenceOf (".", false, false); };
    std::vector<Element> copies;
    for (const auto& e : m.elements)
    {
        if (e.view != from)
            continue;
        const auto f = e.footprint();
        float dx = 0;
        for (const auto& [a, b] : cuts)
        {
            if (f.getX() < (float) b && f.getRight() > (float) a)
                throw std::runtime_error (("the house skin's narrow strip cuts " + e.id).toStdString());
            if ((float) b <= f.getCentreX())
                dx -= (float) (b - a);
        }
        auto n = e;
        n.id = renamed (e.id);
        n.view = "narrow";
        n.group = renamed (e.group);
        for (auto* p : { &n.pivot, &n.tickCentre, &n.led, &n.centre, &n.dot })
            p->x += dx;
        n.capCentreX += dx;
        for (auto* r : { &n.rect, &n.hitRect, &n.dash, &n.tapeRect })
            if (! r->isEmpty())
                *r = r->translated (dx, 0.0f);
        if (n.nativeEngraving)
        {
            n.nativeEngraving->labelRect = n.nativeEngraving->labelRect.translated (dx, 0.0f);
            n.nativeEngraving->unitRect = n.nativeEngraving->unitRect.translated (dx, 0.0f);
        }
        for (auto& p : n.positions)
            p.legendRect = p.legendRect.translated (dx, 0.0f);
        copies.push_back (std::move (n));
    }
    m.elements.insert (m.elements.end(), copies.begin(), copies.end());
    std::vector<Group> groups;
    for (const auto& g : m.groups)
        if (g.view == from)
        {
            Group n { renamed (g.id), "narrow", g.label, {}, {} };
            for (const auto& id : g.members)
                n.members.add (renamed (id));
            for (const auto& id : g.focusOrder)
                n.focusOrder.add (renamed (id));
            groups.push_back (n);
        }
    m.groups.insert (m.groups.end(), groups.begin(), groups.end());
}

Model loadModel (const std::function<juce::MemoryBlock (const String&)>& file, juce::Typeface::Ptr tapeFace)
{
    const auto doc = juce::JSON::parse (file ("elements.json").toString());
    if (! doc.isObject())
        throw std::runtime_error ("cannot read the house skin's elements.json");
    Model m;
    m.panelWidth = num (doc["rack_pitch"]["panel_w"]);
    m.unitHeight = num (doc["rack_pitch"]["u_px"]);
    if (! (m.panelWidth > 0.0f && m.unitHeight > 0.0f))
        throw std::runtime_error ("the house skin's rack pitch is missing");
    if (doc.hasProperty ("design_id"))
    {
        m.designId = juce::Uuid (doc["design_id"].toString());
        m.maker = doc["maker"].toString();
        m.modelName = doc["model"].toString();
        m.units = (int) doc["units"];
    }
    if (const auto face = doc["native_face"]; face.isObject())
    {
        Model::NativeFace native;
        native.statusWhen = nativeBinding (face["active_when"]);
        native.pilotWhen = nativeBinding (face["pilot"]);
        native.statusRect = rectOf (face["status_rect"]);
        native.status = face["status"].toString();
        native.inactiveStatus = face["inactive_status"].toString();
        native.pilotCentre = point (face["pilot"]["center"]);
        native.pilotRadius = num (face["pilot"]["radius"]);
        if (! std::isfinite (native.pilotRadius) || native.pilotRadius <= 0 || native.pilotRadius > 80
            || ! std::isfinite (native.pilotCentre.x) || ! std::isfinite (native.pilotCentre.y)
            || native.statusRect.isEmpty() || ! std::isfinite (native.statusRect.getX()) || ! std::isfinite (native.statusRect.getY())
            || ! std::isfinite (native.statusRect.getWidth()) || native.statusRect.getWidth() > 1000
            || ! std::isfinite (native.statusRect.getHeight()) || native.statusRect.getHeight() > 200
            || native.status.isEmpty() || native.inactiveStatus.isEmpty() || native.status.length() > 128 || native.inactiveStatus.length() > 128)
            throw std::runtime_error ("native face geometry is invalid");
        const auto* controls = face["controls"].getArray();
        if (controls == nullptr || controls->isEmpty() || controls->size() > 128)
            throw std::runtime_error ("native face controls are missing or unbounded");
        for (const auto& control : *controls)
        {
            auto binding = nativeBinding (control);
            if (std::any_of (native.controls.begin(), native.controls.end(), [&] (const auto& previous) { return previous.unit == binding.unit && previous.key == binding.key; }))
                throw std::runtime_error ("native face repeats a control");
            native.controls.push_back (std::move (binding));
        }
        m.nativeFace = std::move (native);
    }
    const auto strings = [] (const juce::var& v)
    {
        juce::StringArray out;
        if (auto* a = v.getArray())
            for (const auto& s : *a)
                out.add (s.toString());
        return out;
    };
    if (const auto r = doc["reveal"]; r.isObject())
    {
        m.reveal = { r["kind"].toString(), r["view"].toString(), r.getProperty ("front", {}).toString(), r["opener"].toString(),
                     strings (r["closers"]), strings (r["titles"]), (int) r.getProperty ("units", 0),
                     r.getProperty ("hang", {}).toString(), r.getProperty ("drawn_screen", {}).toString(), r.getProperty ("drawn_touch", {}).toString() };
        if (const auto mo = r.getProperty ("motion", {}); mo.isObject())
        {
            auto& motion = m.reveal.motion;
            motion.ms = (int) mo["ms"];
            if (auto* turns = mo["turns"].getArray())
                for (const auto& t : *turns)
                    motion.turns.push_back (juce::Rectangle<float> (2.0f * num (t[2]), 2.0f * num (t[2])).withCentre ({ num (t[0]), num (t[1]) }));
            if (mo.hasProperty ("lid"))
                motion.lid = rectOf (mo["lid"]);
            motion.corner = num (mo.getProperty ("corner", 0));
            if (mo.hasProperty ("door"))
                motion.door = rectOf (mo["door"]);
            if (mo.hasProperty ("slide"))
                motion.slide = point (mo["slide"]);
            motion.hinge = mo.getProperty ("hinge", {}).toString();
            motion.openHeight = num (mo.getProperty ("open_height", 0));
        }
    }
    if (auto* pages = doc["pages"].getArray())
        for (const auto& p : *pages)
            m.pages.push_back ({ p["id"].toString(), p["button"].toString(), p["label"].toString(), strings (p["screens"]) });
    m.strip = doc.getProperty ("strip", {}).toString();
    m.hostReadout = doc.getProperty ("readout", {}).toString() != "display";
    if (auto* obj = doc["assets"].getDynamicObject())
        for (const auto& nv : obj->getProperties())
        {
            const auto& a = nv.value;
            Asset as;
            if (a.hasProperty ("file"))
            {
                const auto data = file (a["file"].toString());
                as.image = juce::ImageFileFormat::loadFrom (data.getData(), data.getSize());
                if (! as.image.isValid())
                    throw std::runtime_error (("cannot load " + a["file"].toString()).toStdString());
            }
            if (a.hasProperty ("anchor"))
                as.anchor = point (a["anchor"]);
            as.lensR = num (a.getProperty ("lens_r", 0.0));
            if (a.hasProperty ("body"))
                as.body = juce::Rectangle<int>::leftTopRightBottom ((int) a["body"][0], (int) a["body"][1], (int) a["body"][2], (int) a["body"][3]);
            if (a.hasProperty ("slice"))
                for (int i = 0; i < 4; ++i)
                    as.slice[(size_t) i] = (int) a["slice"][i];
            m.assets[nv.name.toString()] = as;
        }

    if (const auto t = doc["assets"]["unit:tape"]["text"]; t.isObject())
    {
        auto& l = m.lettering;
        l.face = tapeFace;
        if (l.face == nullptr)
            throw std::runtime_error ("the house skin's tape font is missing");
        l.capOneLine = num (t["cap_height_of_body"]["one line"]);
        l.capTwoLines = num (t["cap_height_of_body"]["two lines"]);
        l.pitchOfCap = num (t["line_pitch_of_cap"]);
        l.maxWidthOfBody = num (t["max_width_of_body"]);
        l.strokeOfCap = num (t.getProperty ("stroke_of_cap", 0.0));
        l.ink = juce::Colour::fromString ("ff" + t["color"].toString().fromFirstOccurrenceOf ("#", false, false));
        l.alpha = num (t["alpha"]);
    }

    for (const auto& v : *doc["elements"].getArray())
    {
        Element e;
        e.id = v["id"].toString();
        e.view = v["view"].toString();
        e.kind = v["kind"].toString();
        e.label = v["label"].toString();
        e.group = v["group"].toString();
        if (v.hasProperty ("binding"))
        {
            const auto b = v["binding"];
            if (b.hasProperty ("rack_unit"))
            {
                e.rackUnit = (int) b["rack_unit"];
                e.key = b["key"].toString();
            }
            else
            {
                e.paramId = b["param"].toString();
                e.paramIndex = registryIndex (e.paramId);
            }
            e.logarithmic = b["response"].toString() == "logarithmic";
            e.registryResponse = b["response"].toString() == "parameter";
            e.skew = (double) b.getProperty ("skew", 1.0);
            e.bindingType = b["type"].toString();
            e.defaultValue = (double) b["default"];
            e.onIndex = (int) b.getProperty ("on_index", 1);
            if (e.bindingType == "float")
            {
                e.range = { (double) b["range"][0], (double) b["range"][1] };
                e.unit = b["unit"].toString();
            }
            else if (auto* choices = b["choices"].getArray())
                for (const auto& c : *choices)
                    e.choices.add (c.toString());
        }
        e.screen = v.getProperty ("screen", {}).toString();
        e.slot = (int) v.getProperty ("slot", 0);
        e.vfdName = v.getProperty ("vfd", {}).toString();
        e.lcdLabel = v.getProperty ("lcd", {}).toString();
        e.detents = (bool) v.getProperty ("detents", false)
                    || (bool) v["binding"].getProperty ("integer_physical_value", false);
        if (e.detents && ! e.choices.isEmpty())
            e.range = { 0.0, (double) std::max (1, e.choices.size() - 1) };
        if (const auto w = v["shown_when"]; w.isObject())
            e.shownWhen = { (int) w["unit"], w["key"].toString(), (double) w["value"], 0 };
        if (const auto active = v["native_active_when"]; active.isObject())
        {
            e.nativeActiveWhen = nativeBinding (active);
            e.nativeInactiveDescription = active.getProperty ("inactive_description", "Value retained; unused by the current mode. Editable for later changes.").toString();
            e.nativeInactiveAlpha = num (active.getProperty ("inactive_alpha", 0.4));
            if (! std::isfinite (e.nativeInactiveAlpha) || e.nativeInactiveAlpha < 0.15f || e.nativeInactiveAlpha > 1
                || e.nativeInactiveDescription.isEmpty() || e.nativeInactiveDescription.length() > 256)
                throw std::runtime_error ("native inactive control metadata is invalid");
        }
        const auto g = v["geometry"];
        const auto readTicks = [&e, &v]
        {
            if (auto* ticks = v["scale"]["ticks"].getArray())
                for (const auto& t : *ticks)
                    e.ticks.emplace_back ((double) t[0], num (t[1]));
        };
        if (e.kind == "rotary")
        {
            e.pivot = point (g["pivot"]);
            e.faceR = num (g["face_r"]);
            e.tickCentre = point (g["tick_center"]);
            e.tickR = num (g["tick_r"]);
            e.hitR = num (g["hit_r"]);
            e.sweepLo = (double) v["scale"]["sweep"][0];
            e.sweepHi = (double) v["scale"]["sweep"][1];
            e.drawnValue = (double) v["mockup"]["drawn_value"];
            readTicks();
            if (const auto r = v["route"]; r.isObject())
                e.printedRoute = { r["source"].toString(), r["target"].toString(), r["bank"].toString() };
            if (const auto k = v["knurl"]; k.isObject())
            {
                e.knurl = k["asset"].toString();
                e.knurlFrames = (int) k["frames"];
                e.knurlTeeth = (int) k["teeth"];
                e.knurlRest = num (k["rest_angle"]);
            }
        }
        else if (e.kind == "selector")
        {
            e.pivot = point (g["pivot"]);
            e.rect = rectOf (g["rect"]);
            e.faceR = num (g["face_r"]);
            e.hitR = num (g["hit_r"]);
            e.drawnValue = (double) v["mockup"]["drawn_value"];
        }
        else if (e.kind == "toggle" || e.kind == "action")
        {
            e.rect = rectOf (g["rect"]);
            if (g.hasProperty ("led"))
            {
                e.led = point (g["led"]);
                e.lensR = num (g["lens_r"]);
            }
            if (g.hasProperty ("lit_rect"))
                e.litRect = rectOf (g["lit_rect"]);
        }
        else if (e.kind == "fader")
        {
            e.capCentreX = num (g["cap_center_x"]);
            e.y0 = num (g["y0"]);
            e.y1 = num (g["y1"]);
            e.drawnValue = (double) v["mockup"]["drawn_value"];
        }
        else if (e.kind == "lever")
        {
            e.pivot = point (g["pivot"]);
            e.rect = rectOf (g["rect"]);
            e.drawnValue = (double) v["mockup"]["drawn_value"];
        }
        else if (e.kind == "lamp")
        {
            e.led = point (g["led"]);
            e.lensR = num (g["lens_r"]);
            e.litValue = (double) v["lit_value"];
        }
        else if (e.kind == "needle")
        {
            e.y0 = num (g["y"]);
            e.drawnValue = (double) v["mockup"]["drawn_value"];
            readTicks();
        }
        else if (e.kind == "still")
        {
            e.pivot = point (g["pivot"]);
            e.faceR = num (g["face_r"]);
        }
        else if (e.kind == "display")
        {
            e.rect = rectOf (g["glass"]);
            e.role = v["role"].toString();
            e.display = v["display"];
        }
        else if (e.kind == "tape")
        {
            e.centre = point (g["center"]);
            e.size = point (g["size"]);
            e.angle = num (g["angle"]);
            e.text = v.getProperty ("text", {}).toString();
            if (g.hasProperty ("dash"))
            {
                e.dash = rectOf (g["dash"]);
                e.hasDash = true;
            }
        }
        if (auto* positions = v["positions"].getArray())
            for (const auto& p : *positions)
            {
                Position pos { (double) p["value"], p["legend"].toString(), p.getProperty ("asset", {}).toString(), num (p.getProperty ("angle", 0.0)), {} };
                if (p.hasProperty ("legend_rect"))
                    pos.legendRect = rectOf (p["legend_rect"]);
                e.positions.push_back (pos);
            }
        e.legendClick = v["input"].hasProperty ("legend_click");
        if (g.hasProperty ("hit_rect"))
            e.hitRect = rectOf (g["hit_rect"]);
        if (g.hasProperty ("hit_center"))
            e.hitAt = point (g["hit_center"]);
        if (g.hasProperty ("tape"))
            e.tapeRect = rectOf (g["tape"]);
        if (g.hasProperty ("dot"))
        {
            e.dot = point (g["dot"]);
            e.hasDot = true;
        }
        if (auto* obj = v["layers"].getDynamicObject())
            for (const auto& nv : obj->getProperties())
                e.layers[nv.name.toString()] = nv.value.toString();
        if (const auto print = v["native_engraving"]; print.isObject())
        {
            if (e.kind != "rotary" && e.kind != "selector")
                throw std::runtime_error ("native engraving requires a rotary or selector");
            NativeEngraving n;
            const auto ink = print.getProperty ("ink", doc["native_engraving"].getProperty ("ink", "#D6B27D")).toString();
            const auto hex = ink.startsWithChar ('#') ? ink.substring (1) : ink;
            if (hex.length() != 6 || ! hex.containsOnly ("0123456789abcdefABCDEF"))
                throw std::runtime_error ("native engraving ink is not RGB hex");
            n.ink = juce::Colour::fromString ("ff" + hex);
            n.labelRect = rectOf (print["label_rect"]);
            n.unitRect = rectOf (print["unit_rect"]);
            n.labelFontHeight = num (print.getProperty ("label_font_height", 20));
            n.scaleFontHeight = num (print.getProperty ("scale_font_height", 18));
            n.labelRadius = num (print.getProperty ("tick_label_radius", 0));
            n.labelInset = num (print.getProperty ("tick_label_inset", 0));
            n.baseRadius = num (print.getProperty ("stationary_base_radius", 0));
            n.minorStep = num (print.getProperty ("minor_tick_step_degrees", 0));
            n.majorLength = num (print.getProperty ("major_tick_length", 11));
            n.minorLength = num (print.getProperty ("minor_tick_length", 6));
            const auto positive = [] (float x, float maximum) { return std::isfinite (x) && x > 0 && x <= maximum; };
            const auto nonnegative = [] (float x, float maximum) { return std::isfinite (x) && x >= 0 && x <= maximum; };
            if (! positive (n.labelFontHeight, 64) || ! positive (n.scaleFontHeight, 64)
                || ! nonnegative (n.labelRadius, 300) || ! nonnegative (n.labelInset, 60) || ! nonnegative (n.baseRadius, 200)
                || ! nonnegative (n.minorStep, 90) || (n.minorStep > 0 && n.minorStep < 1)
                || ! nonnegative (n.majorLength, 40) || ! nonnegative (n.minorLength, 40))
                throw std::runtime_error ("native engraving geometry is invalid");
            for (const auto& rect : { n.labelRect, n.unitRect })
                if (! std::isfinite (rect.getX()) || ! std::isfinite (rect.getY())
                    || ! nonnegative (rect.getWidth(), 1000) || ! nonnegative (rect.getHeight(), 200))
                    throw std::runtime_error ("native engraving text rectangle is invalid");
            if (auto* labels = print["tick_labels"].getArray())
            {
                if (labels->size() > 32)
                    throw std::runtime_error ("too many native scale labels");
                for (const auto& label : *labels)
                {
                    const auto value = (double) label["value"];
                    const auto text = label["text"].toString();
                    if (! std::isfinite (value) || (! e.range.contains (value) && value != e.range.getEnd())
                        || text.isEmpty() || text.length() > 16)
                        throw std::runtime_error ("native scale label is invalid");
                    n.tickLabels.emplace_back (value, text);
                }
            }
            if (e.kind == "rotary" && (e.ticks.size() < 2 || ! std::isfinite (e.sweepLo) || ! std::isfinite (e.sweepHi)
                                      || e.sweepHi <= e.sweepLo || e.sweepHi - e.sweepLo > 360))
                throw std::runtime_error ("native rotary scale is invalid");
            if (e.kind == "selector" && (e.positions.empty() || e.positions.size() > 16))
                throw std::runtime_error ("native selector positions are invalid");
            e.nativeEngraving = std::move (n);
        }
        if (v.hasProperty ("state_assets"))
        {
            const auto* states = v["state_assets"].getDynamicObject();
            if (states == nullptr)
                throw std::runtime_error (("invalid generated states for " + e.id).toStdString());
            for (const auto& nv : states->getProperties())
            {
                const auto key = nv.value.toString();
                if (! nv.value.isString() || key.isEmpty() || ! m.asset (key).image.isValid())
                    throw std::runtime_error (("invalid generated state for " + e.id).toStdString());
                e.stateAssets[nv.name.toString()] = key;
            }
            const auto require = [&e] (const juce::StringArray& names)
            {
                for (const auto& name : names)
                    if (e.stateAsset (name).isEmpty())
                        throw std::runtime_error (("missing generated state " + e.id + ":" + name).toStdString());
            };
            if (e.kind == "toggle")
                require ({ "off", "on", "hover_off", "hover_on", "pressed_off", "pressed_on", "disabled" });
            else if (e.kind == "fader" || e.kind == "action")
                require ({ "idle", "hover", "pressed", "disabled" });
        }
        m.elements.push_back (std::move (e));
    }

    // The image printed the dash only for macros 5 to 8, its unassigned ones, in their knobs' columns at their tapes'
    // height. Macros 1 to 4 take the dash of the macro four on, placed the same way.
    const auto element = [&m] (const String& id) -> const Element*
    {
        for (const auto& e : m.elements)
            if (e.id == id)
                return &e;
        return nullptr;
    };
    for (auto& e : m.elements)
    {
        const auto n = e.localId().fromFirstOccurrenceOf ("macro-tape.", false, false).getIntValue();
        if (e.kind != "tape" || e.hasDash || n < 1 || n > 4)
            continue;
        const auto* printed = element (e.view + ".macro-tape." + String (n + 4));
        const auto* knob = element (e.view + ".macro." + String (n));
        if (printed != nullptr && printed->hasDash && knob != nullptr)
        {
            e.dash = printed->dash.withCentre ({ knob->pivot.x, e.centre.y });
            e.hasDash = true;
        }
    }

    for (const auto& v : *doc["groups"].getArray())
    {
        Group gr { v["id"].toString(), v["view"].toString(), v["label"].toString(), {}, {} };
        for (const auto& id : *v["members"].getArray())
            gr.members.add (id.toString());
        for (const auto& id : *v["focus_order"].getArray())
            gr.focusOrder.add (id.toString());
        m.groups.push_back (gr);
    }
    if (const auto narrow = doc["narrow"]; narrow.isObject())
        addNarrow (m, narrow);
    return m;
}

juce::MemoryBlock houseSkinFile (const String& path)
{
    // The binary data keeps file names only; the house skin and branding file names are unique.
    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    for (int i = 0; i < HouseSkinData::namedResourceListSize; ++i)
        if (name == HouseSkinData::getNamedResourceOriginalFilename (HouseSkinData::namedResourceList[i]))
        {
            int size = 0;
            if (const auto* data = HouseSkinData::getNamedResource (HouseSkinData::namedResourceList[i], size))
                return { data, (size_t) size };
        }
    return {};
}

double proportionOf (const Element& e, double value)
{
    const auto lo = e.range.getStart(), hi = e.range.getEnd();
    if (e.logarithmic)
        return std::log (value / lo) / std::log (hi / lo);
    if (e.registryResponse && e.skew != 1.0)
        return std::pow (juce::jlimit (0.0, 1.0, (value - lo) / (hi - lo)), e.skew);
    return (value - lo) / (hi - lo);
}

int gripFrame (const Element& e, float angle)
{
    if (e.knurlFrames <= 0 || e.knurlTeeth <= 0)
        return 0;
    const auto teeth = (double) e.knurlTeeth * ((double) angle - (double) e.knurlRest) / 360.0;
    return juce::roundToInt ((teeth - std::floor (teeth)) * e.knurlFrames) % e.knurlFrames;
}

float pointerAngleFor (const Element& e, double proportion)
{
    if (! e.ticks.empty())
    {
        auto below = proportionOf (e, e.ticks.front().first);
        if (proportion <= below)
            return e.ticks.front().second;
        for (std::size_t i = 1; i < e.ticks.size(); ++i)
        {
            const auto above = proportionOf (e, e.ticks[i].first);
            if (proportion <= above)
                return juce::jmap ((float) ((proportion - below) / (above - below)), e.ticks[i - 1].second, e.ticks[i].second);
            below = above;
        }
        return e.ticks.back().second;
    }
    const auto th = juce::degreesToRadians (e.sweepLo + (e.sweepHi - e.sweepLo) * proportion);
    const auto px = e.tickCentre.x + e.tickR * (float) std::sin (th);
    const auto py = e.tickCentre.y - e.tickR * (float) std::cos (th);
    return juce::radiansToDegrees (std::atan2 (px - e.pivot.x, -(py - e.pivot.y)));
}

String formatValue (const Element& e, double v)
{
    if (e.bindingType == "toggle")
        return v > 0.5 ? "on" : "off";
    if (e.bindingType == "choice")
        return e.choices[juce::roundToInt (v)];
    if (e.detents && e.choices.isEmpty())
        return String (juce::roundToInt (v));
    if (e.unit == "s")
        return v < 1.0 ? String (juce::roundToInt (v * 1000.0)) + " ms" : String (v, 2) + " s";
    if (e.unit == "dB")
        return (v >= 0.0 ? "+" : "") + String (v, 1) + " dB";
    if (e.unit == "st" || e.unit == "cent" || e.unit == "oct")
    {
        // Signed as the deck prints them: cents and whole semitones as integers, octaves and other semitones to a tenth.
        const auto whole = e.unit == "cent" || (e.unit == "st" && std::abs (v - std::round (v)) < 0.05);
        const auto text = whole ? String (juce::roundToInt (v)) : String (v, 1);
        return (text.startsWithChar ('-') ? "" : "+") + text + (e.unit == "cent" ? " ct" : " " + e.unit);
    }
    if (e.paramId.endsWith (".pan"))
    {
        const auto pc = juce::roundToInt (std::abs (v) * 100.0);
        return pc == 0 ? String ("C") : (v < 0 ? "L " : "R ") + String (pc);
    }
    if (e.paramId.startsWith ("macro.") || e.unit == "%")
        return String (juce::roundToInt (v * 100.0)) + "%";
    if (e.unit == "ms")
        return v >= 1000.0 ? String (v / 1000.0, 2) + " s" : v < 10.0 ? String (v, 1) + " ms" : String (juce::roundToInt (v)) + " ms";
    if (e.unit == "Hz")
        return v >= 1000.0 ? String (v / 1000.0, 1) + " kHz" : v < 10.0 ? String (v, 2) + " Hz" : v < 100.0 ? String (v, 1) + " Hz" : String (juce::roundToInt (v)) + " Hz";
    return String (v, 2);
}

String compactValue (const Element& e, double v)
{
    auto text = formatValue (e, v);
    if (e.bindingType == "toggle" || e.bindingType == "choice")
        return text.toUpperCase();
    // A pan's "L 20" keeps its space.
    for (int i = text.length() - 2; i > 0; --i)
        if (text[i] == ' ' && juce::CharacterFunctions::isDigit (text[i - 1]))
            text = text.substring (0, i) + text.substring (i + 1);
    return text;
}
} // namespace airack::studio::house
