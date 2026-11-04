#include "StudioSkin.h"

#include "StudioSkinData.h"
#include "RackDesign.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace airack::studio
{
static juce::var readJson (const String& file)
{
    auto v = juce::JSON::parse (studioSkinFile (file).toString());
    if (! v.isObject())
        throw std::runtime_error (("cannot read " + file).toStdString());
    return v;
}

static juce::Image loadImage (const String& file)
{
    const auto data = studioSkinFile (file);
    auto img = juce::ImageFileFormat::loadFrom (data.getData(), data.getSize());
    if (! img.isValid())
        throw std::runtime_error (("cannot load " + file).toStdString());
    return img.convertedToFormat (juce::Image::ARGB);
}

static float num (const juce::var& v) { return (float) (double) v; }
static Pt point (const juce::var& v) { return { num (v[0]), num (v[1]) }; }
static Rect rectOf (const juce::var& v) { return { num (v[0]), num (v[1]), num (v[2]), num (v[3]) }; }
static Rect ltrb (const juce::var& v) { return Rect::leftTopRightBottom (num (v[0]), num (v[1]), num (v[2]), num (v[3])); }
static juce::Colour fromBgr (const juce::var& v) { return juce::Colour ((juce::uint8) (int) v[2], (juce::uint8) (int) v[1], (juce::uint8) (int) v[0]); }

Lattice latticeFrom (const juce::var& d)
{
    Lattice l;
    l.glass = rectOf (d["glass"]);
    l.origin = point (d["dots"]["origin"]);
    l.pitch = point (d["dots"]["pitch"]);
    for (auto& v : *d["dots"]["profile"].getArray())
        l.profile.push_back (num (v));
    l.step = num (d["dots"]["profile_step"]);
    for (int i = 0; i < 3; ++i)
        l.tint[(size_t) i] = num (d["dots"]["tint_bgr"][i]);
    l.ink = fromBgr (d["ink"]["bgr"]);
    l.ripple = num (d["ink"]["ripple"]);
    l.rippleCore = num (d["ink"]["ripple_radii"][0]);
    l.rippleEdge = num (d["ink"]["ripple_radii"][1]);
    return l;
}

Line lineFrom (const juce::var& v)
{
    Line l;
    l.text = v["text"].toString();
    l.align = v["align"].toString();
    l.cap = num (v["cap"]);
    l.hscale = num (v["hscale"]);
    l.penX = num (v["pen_x"]);
    l.baseline = num (v["baseline"]);
    l.maxRight = num (v.getProperty ("max_right", 0.0));
    l.ink = ltrb (v["ink"]);
    if (v.hasProperty ("indicator"))
    {
        l.indicator = v["indicator"]["part"].toString();
        l.indicatorCentre = point (v["indicator"]["center"]);
    }
    return l;
}

static void bakeDots (juce::Image& plate, const Lattice& l);

static Model loadModel()
{
    const auto doc = readJson ("elements.json");
    Model m;
    if (auto* views = doc["views"].getDynamicObject())
        for (const auto& nv : views->getProperties())
        {
            auto img = loadImage (nv.value["plate"].toString());
            if (img.getWidth() != (int) nv.value["size"][0] || img.getHeight() != (int) nv.value["size"][1])
                throw std::runtime_error (("plate size differs from elements.json: " + nv.name.toString()).toStdString());
            m.plates[nv.name.toString()] = img;
        }
    if (auto* obj = doc["assets"].getDynamicObject())
        for (const auto& nv : obj->getProperties())
        {
            Asset a;
            a.image = loadImage (nv.value["file"].toString());
            a.anchor = point (nv.value["anchor"]);
            a.lensR = num (nv.value.getProperty ("lens_r", 0.0));
            a.ringR = num (nv.value.getProperty ("ring_r", 0.0));
            a.spec = nv.value;
            m.assets[nv.name.toString()] = a;
        }

    const auto loadFace = [&] (const char* key)
    {
        juce::Typeface::Ptr face;
        const auto file = doc["fonts"][key]["file"].toString();
        const auto data = studioSkinFile (file);
        if (! data.isEmpty())
            face = juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize());
        if (face == nullptr)
            throw std::runtime_error ((String ("cannot load the ") + key + " font " + file).toStdString());
        return face;
    };
    m.face = loadFace ("display");
    m.screenFace = loadFace ("screen");
    m.markerFace = loadFace ("marker");

    for (const auto& v : *doc["elements"].getArray())
    {
        Element e;
        e.id = v["id"].toString();
        e.view = v["view"].toString();
        e.kind = v["kind"].toString();
        e.label = v["label"].toString();
        e.group = v["group"].toString();
        e.control = v["binding"].getProperty ("control", "").toString();
        if (auto* p = v["binding"]["positions"].getArray())
            for (auto& s : *p)
                e.positions.add (s.toString());
        const auto g = v["geometry"];
        if (g.hasProperty ("rect")) e.rect = rectOf (g["rect"]);
        if (g.hasProperty ("hit_rect")) e.hitRect = rectOf (g["hit_rect"]);
        if (g.hasProperty ("footprint")) e.footprint = rectOf (g["footprint"]);
        if (g.hasProperty ("glass")) e.glass = rectOf (g["glass"]);
        if (g.hasProperty ("triangle")) e.triangle = rectOf (g["triangle"]);
        if (g.hasProperty ("led")) e.led = point (g["led"]);
        if (g.hasProperty ("pivot")) e.pivot = point (g["pivot"]);
        if (g.hasProperty ("legend")) e.legend = ltrb (g["legend"]);
        if (g.hasProperty ("center")) e.centre = point (g["center"]);
        e.lensR = num (g.getProperty ("lens_r", 0.0));
        e.ringR = num (g.getProperty ("ring_r", 0.0));
        e.skirtR = num (g.getProperty ("skirt_r", 0.0));
        if (g.hasProperty ("sweep")) e.sweep = num (g["sweep"][1]);
        e.display = v["display"].toString();
        e.status = v["binding"].getProperty ("status", "").toString();
        e.ledStatus = v["binding"].getProperty ("led", "").toString();
        const auto value = v["binding"].getProperty ("value", -1);
        e.value = value.isInt() || value.isDouble() ? (int) value : -1;
        if (auto* obj = v["layers"].getDynamicObject())
            for (const auto& nv : obj->getProperties())
                e.layers[nv.name.toString()] = nv.value.toString();
        m.elements.push_back (std::move (e));
    }

    juce::StringArray displayKeys;
    if (auto* obj = doc["displays"].getDynamicObject())
        for (const auto& nv : obj->getProperties())
            displayKeys.add (nv.name.toString());
    for (const auto& key : displayKeys)
    {
        const auto d = doc["displays"][juce::Identifier (key)];
        DisplaySpec s;
        s.lattice = latticeFrom (d);
        if (auto* obj = d["lines"].getDynamicObject())
            for (const auto& nv : obj->getProperties())
                s.lines[nv.name.toString()] = lineFrom (nv.value);
        s.minHscale = num (d["min_hscale"]);
        if (auto* a = d["messages"].getArray())
            for (auto& v : *a)
            {
                Message msg { v["today"].toString(), v["headline"].toString(), v["detail"].toString(), (bool) v["error_led"],
                              v.hasProperty ("hold"), (int) v.getProperty ("hold_ms", 0), {} };
                if (auto* o = v["overrides"].getDynamicObject())
                    for (const auto& nv : o->getProperties())
                        msg.overrides[nv.name.toString()] = nv.value.toString();
                s.messages.push_back (msg);
            }
        if (auto* a = d["playback"].getArray())
            for (auto& v : *a)
                s.playback.push_back ({ v["today"].toString(), v["alert"].isString() ? v["alert"].toString() : String() });
        if (auto* a = d["operations"].getArray())
            for (auto& v : *a)
                s.operations.push_back ({ v["today"].toString(), v["text"].isString() ? v["text"].toString() : String(), (bool) v["amber"],
                                          (int) v["hold_ms"] });
        s.spec = d;
        m.displays[key] = s;
    }

    {
        const auto o = doc["overview"];
        auto& ov = m.overview;
        ov.lattice = latticeFrom (o);
        ov.columns = (int) o["columns"];
        ov.rows = (int) o["rows"];
        ov.barsX0 = num (o["bars"]["x0"]);
        ov.lineTop = num (o["bars"]["line_rows"][0]);
        ov.lineBottom = num (o["bars"]["line_rows"][1]);
        ov.lineWidth = num (o["bars"]["line_width"]);
        ov.linePeak = (int) o["bars"]["line_peak"];
        ov.numberOffsetX = num (o["numbers"]["offset_x"]);
        ov.numberCap = num (o["numbers"]["cap"]);
        ov.numberBaseline = num (o["numbers"]["baseline"]);
        ov.numberPeak = (int) o["numbers"]["peak"];
        ov.rowsPerNote = (int) o["notes"]["rows_per_note"];
        const auto lit = o["notes"]["lit"];
        for (int i = 0; i < 4; ++i)
            ov.litInset[(size_t) i] = num (lit["inset"][i]);
        ov.seamDepth = num (lit["seam"]["depth"]);
        ov.seamWidth = std::max (0.25f, num (lit["seam"]["width"]));
        ov.lit = fromBgr (lit["bgr"]);
    }

    for (const auto& v : *doc["groups"].getArray())
    {
        Group gr { v["id"].toString(), v["view"].toString(), v["label"].toString(), {} };
        for (const auto& id : *v["members"].getArray())
            gr.members.add (id.toString());
        m.groups.push_back (gr);
    }
    if (auto* obj = doc["focus_order"].getDynamicObject())
        for (const auto& nv : obj->getProperties())
            for (const auto& id : *nv.value.getArray())
                m.focusOrder[nv.name.toString()].add (id.toString());

    {
        const auto s = doc["mockup"]["session"], p = doc["mockup"]["strip"];
        auto& mk = m.mockup;
        mk.path = s["path"].toString();
        mk.bpm = (double) s["bpm"];
        mk.bars = (int) p["bars"];
        mk.numerator = (int) p["meter"][0];
        mk.denominator = (int) p["meter"][1];
        mk.grid = p["grid"].toString();
        mk.swing = (double) p["swing"];
        mk.mappedCells = (int) p["mapped_cells"];
        for (auto& n : *p["notes"].getArray())
            mk.notes.push_back ({ (double) n[0], (double) n[1], (int) n[2] });
    }

    m.screens = doc["screens"];
    m.layout = doc["layout"];
    m.sideStates = doc["mockup"]["side"];
    if (! m.screens.isObject() || ! m.layout.isObject() || ! m.sideStates.isObject())
        throw std::runtime_error ("elements.json has no side units (screens, layout, mockup.side)");

    m.programmer = doc["programmer"];
    m.rolls = doc["rolls"];
    m.keyboards = doc["keyboards"];
    m.patternMockup = doc["mockup"]["pattern"];
    m.programmerMockup = doc["mockup"]["programmer"];
    if (! m.programmer.isObject() || ! m.rolls.isObject() || ! m.keyboards.isObject() || ! m.patternMockup.isObject() || ! m.programmerMockup.isObject())
        throw std::runtime_error ("elements.json has no stage C (programmer, rolls, keyboards, mockup.pattern, mockup.programmer)");
    {
        const auto pd = m.programmer["display"];
        DisplaySpec s;
        s.lattice = latticeFrom (pd);
        if (auto* obj = pd["lines"].getDynamicObject())
            for (const auto& nv : obj->getProperties())
                s.lines[nv.name.toString()] = lineFrom (nv.value);
        s.minHscale = num (pd["min_hscale"]);
        s.spec = pd;
        m.displays["advanced"] = s;
    }
    const auto sectionsOf = [] (const juce::var& rows)
    {
        std::vector<std::vector<SectionSpec>> out;
        for (auto& row : *rows.getArray())
        {
            auto& r = out.emplace_back();
            for (auto& sec : *row.getArray())
            {
                SectionSpec s { sec["id"].toString(), sec["title"].toString(), {}, (bool) sec.getProperty ("macros", false) };
                for (auto& k : *sec["knobs"].getArray())
                    s.knobs.push_back ({ k["label"].toString(), (int) k["index"] });
                r.push_back (s);
            }
        }
        return out;
    };
    for (auto& p : *m.programmer["page_spec"].getArray())
    {
        PageSpec ps;
        ps.id = p["id"].toString();
        ps.label = p["label"].toString();
        ps.synth = (int) p.getProperty ("synth", -1);
        for (auto& b : *m.programmer["pages"].getArray())
            if (b["id"].toString() == ps.id)
            {
                ps.rect = rectOf (b["rect"]);
                ps.led = point (b["led"]);
                ps.lensR = num (b["lens_r"]);
            }
        for (auto& s : *p["subpages"].getArray())
        {
            SubpageSpec ss;
            ss.id = s["id"].toString();
            ss.label = s["label"].toString();
            ss.engine = (int) s.getProperty ("engine", -1);
            ss.effect = (int) s.getProperty ("effect", -1);
            ss.rackLayout = (bool) s.getProperty ("rack_layout", false);
            if (auto* routes = s["pages"].getArray())
                for (auto& r : *routes)
                {
                    SubpageSpec rs;
                    rs.id = r["id"].toString();
                    rs.label = r["label"].toString();
                    rs.effect = ss.effect;
                    if (auto* range = r["range"].getArray(); range != nullptr && range->size() == 2)
                    {
                        rs.first = (int) (*range)[0];
                        rs.last = (int) (*range)[1];
                    }
                    rs.rows = sectionsOf (r["rows"]);
                    ss.routes.push_back (rs);
                }
            else
                ss.rows = sectionsOf (s["rows"]);
            ps.subpages.push_back (ss);
        }
        m.pages.push_back (ps);
    }
    // Fixed native EQ and delay tape pages share the registry with automation, rack defaults and AI edits.
    // Keep the existing SLOT bypass in its original leaf, with each new index present once.
    for (auto& page : m.pages)
        if (page.id == "effects")
            for (auto& slot : page.subpages)
            {
                if (! juce::isPositiveAndBelow (slot.effect, 8))
                    continue;
                const auto base = dynamicEqParameterStart + (std::size_t) slot.effect * dynamicEqParametersPerSlot;
                const char* labels[] { "ACTIVE", "BYPASS", "SHAPE", "FREQUENCY", "GAIN", "Q", "CUT SLOPE", "RANGE", "THRESHOLD", "ATTACK", "RELEASE" };
                for (int band = 0; band < 8; ++band)
                {
                    SubpageSpec leaf;
                    leaf.id = "dynamic_eq.band." + String (band + 1);
                    leaf.label = "EQ BAND " + String (band + 1);
                    leaf.effect = slot.effect;
                    leaf.first = (int) (base + (std::size_t) band * dynamicEqParametersPerBand);
                    leaf.last = leaf.first + (int) dynamicEqParametersPerBand - 1;
                    SectionSpec tone { leaf.id, band == 0 ? String ("DYNAMIC EQ") : "DYNAMIC EQ BAND " + String (band + 1), {} };
                    SectionSpec dynamics { leaf.id + ".dynamics", "CHANNEL / DYNAMICS", {} };
                    dynamics.knobs.push_back ({ "PLACEMENT", (int) (dynamicEqPlacementParameterStart
                        + (std::size_t) slot.effect * dynamicEqPlacementParametersPerSlot + (std::size_t) band) });
                    for (int field = 0; field < (int) dynamicEqParametersPerBand; ++field)
                        (field < 6 ? tone : dynamics).knobs.push_back ({ labels[field], leaf.first + field });
                    const auto extension=dynamicEqExtensionParameterStart+(std::size_t)slot.effect*dynamicEqExtensionParametersPerSlot
                        +(std::size_t)band*dynamicEqExtensionParametersPerBand;
                    tone.knobs.push_back({"ALT SHAPE",(int)extension});
                    dynamics.knobs.push_back({"ALT SLOPE",(int)extension+1});
                    leaf.rows = { { std::move (tone), std::move (dynamics) } };
                    slot.routes.push_back (std::move (leaf));
                }
                SubpageSpec output;
                output.id = "dynamic_eq.output";
                output.label = "EQ OUTPUT";
                output.effect = slot.effect;
                output.first = output.last = (int) (base + dynamicEqParametersPerSlot - 1);
                output.rows = { { { output.id, "DYNAMIC EQ OUTPUT", { { "OUTPUT", output.first } } } } };
                slot.routes.push_back (std::move (output));
                SubpageSpec gain;
                gain.id = "dynamic_eq.gain";
                gain.label = "EQ GAIN";
                gain.effect = slot.effect;
                gain.first = (int) (dynamicEqGainParameterStart + (std::size_t) slot.effect * dynamicEqGainParametersPerSlot);
                gain.last = gain.first + 1;
                gain.rows = { { { gain.id, "DYNAMIC EQ GAIN", { { "AUTO GAIN", gain.first }, { "GAIN SCALE", gain.last } } } } };
                slot.routes.push_back (std::move (gain));
                for(int band=0;band<8;++band){
                    SubpageSpec detector;detector.id="dynamic_eq.detector."+juce::String(band+1);detector.label="DETECTOR "+juce::String(band+1);detector.effect=slot.effect;
                    detector.first=(int)(dynamicEqDetectorParameterStart+(std::size_t)slot.effect*dynamicEqDetectorParametersPerSlot+(std::size_t)band*dynamicEqDetectorParametersPerBand);
                    detector.last=detector.first+4;SectionSpec section{detector.id,"DETECTOR",{}};
                    const char* labels[]{"AUTO TH","ADAPTIVE","SOURCE","LOW","HIGH"};
                    for(int field=0;field<5;++field)section.knobs.push_back({labels[field],detector.first+field});
                    detector.rows={{std::move(section)}};slot.routes.push_back(std::move(detector));
                }
                for(int band=0;band<8;++band){
                    SubpageSpec spectral;spectral.id="dynamic_eq.spectral."+juce::String(band+1);spectral.label="SPECTRAL "+juce::String(band+1);spectral.effect=slot.effect;
                    const auto mode=dynamicEqSpectralParameterStart+std::size_t(slot.effect)*dynamicEqSpectralParametersPerSlot;
                    spectral.first=int(mode+1+std::size_t(band)*3);spectral.last=spectral.first+2;
                    SectionSpec section{spectral.id,"SPECTRAL",{{"SPECTRAL",spectral.first},{"DENSITY",spectral.first+1},{"THRESHOLD TILT",spectral.first+2}}};
                    if(band==0)section.knobs.push_back({"RACK PROCESSING",int(mode)});
                    spectral.rows={{std::move(section)}};slot.routes.push_back(std::move(spectral));
                }
                SubpageSpec tape;
                tape.id = "delay.tape";
                tape.label = "DELAY TAPE";
                tape.effect = slot.effect;
                tape.first = (int) (delayTapeParameterStart + (std::size_t) slot.effect * delayTapeParametersPerSlot);
                tape.last = tape.first + (int) delayTapeParametersPerSlot - 1;
                SectionSpec tapeSection { tape.id, "DELAY TAPE", {} };
                const char* tapeLabels[] { "SATURATION", "LOW CUT", "WOBBLE", "RATE", "GLIDE" };
                for (int field = 0; field < (int) delayTapeParametersPerSlot; ++field)
                    tapeSection.knobs.push_back ({ tapeLabels[field], tape.first + field });
                tape.rows = { { std::move (tapeSection) } };
                slot.routes.push_back (std::move (tape));
                SubpageSpec stereo;
                stereo.id = "delay.modes";
                stereo.label = "DELAY STEREO";
                stereo.effect = slot.effect;
                stereo.first = (int) (delayStereoParameterStart + (std::size_t) slot.effect * delayStereoParametersPerSlot);
                stereo.last = stereo.first + (int) delayStereoParametersPerSlot - 1;
                SectionSpec stereoSection { stereo.id, "DELAY STEREO", {} };
                const char* stereoLabels[] { "RIGHT", "PING-PONG", "NOTE" };
                for (int field = 0; field < (int) delayStereoParametersPerSlot; ++field)
                    stereoSection.knobs.push_back ({ stereoLabels[field], stereo.first + field });
                stereo.rows = { { std::move (stereoSection) } };
                slot.routes.push_back (std::move (stereo));
            }
    // Space uses separate retained decay-time and wet-amplitude bands. Existing pages keep their order.
    for (auto& page : m.pages)
        if (page.id == "effects")
            for (auto& slot : page.subpages)
            {
                if (! juce::isPositiveAndBelow (slot.effect, 8)) continue;
                const auto base = spaceReverbParameterStart + (std::size_t) slot.effect * spaceReverbParametersPerSlot;
                const char* globalLabels[] { "SIZE", "DECAY", "PREDELAY", "BRIGHTNESS", "CHARACTER", "DISTANCE", "WIDTH", "MIX", "DUCK RANGE", "THRESHOLD", "ATTACK", "RELEASE" };
                for (int group = 0; group < 2; ++group)
                {
                    SubpageSpec leaf;leaf.id = group == 0 ? "space_reverb.main" : "space_reverb.output";
                    leaf.label = group == 0 ? "SPACE" : "SPACE OUTPUT";leaf.effect = slot.effect;
                    leaf.first = (int) base + group * 6;leaf.last = leaf.first + 5;
                    SectionSpec section { leaf.id, group == 0 ? effectDisplayName(EffectKind::spaceReverb).toUpperCase() : String("SPACE OUTPUT / DUCK"), {} };
                    for (int field = 0; field < 6; ++field) section.knobs.push_back ({globalLabels[group * 6 + field], leaf.first + field});
                    leaf.rows = { { std::move(section) } };slot.routes.push_back(std::move(leaf));
                }
                const char* decayLabels[] { "ACTIVE", "BYPASS", "SHAPE", "FREQUENCY", "MULTIPLIER", "Q" };
                const char* postLabels[] { "ACTIVE", "BYPASS", "SHAPE", "FREQUENCY", "GAIN", "BELL Q", "SHELF Q", "CUT SLOPE" };
                for (int family = 0; family < 2; ++family)
                    for (int band = 0; band < 6; ++band)
                    {
                        const int count = family == 0 ? 6 : 8;
                        SubpageSpec leaf;leaf.id = String(family == 0 ? "space_reverb.decay." : "space_reverb.post.") + String(band + 1);
                        leaf.label = String(family == 0 ? "DECAY " : "POST EQ ") + String(band + 1);leaf.effect = slot.effect;
                        leaf.first = (int) base + (family == 0 ? 12 : 48) + band * count;leaf.last = leaf.first + count - 1;
                        SectionSpec section { leaf.id, leaf.label, {} };
                        for (int field = 0; field < count; ++field) section.knobs.push_back ({ family == 0 ? decayLabels[field] : postLabels[field], leaf.first + field });
                        leaf.rows = { { std::move(section) } };slot.routes.push_back(std::move(leaf));
                    }
            }
    // Each synth's warmth keys follow its WAVETABLE subpage, which keeps its layout.
    for (auto& page : m.pages)
        if (juce::isPositiveAndBelow (page.synth, 4))
        {
            const auto at = std::find_if (page.subpages.begin(), page.subpages.end(), [] (const SubpageSpec& s) { return s.id == "wavetable"; });
            if (at == page.subpages.end())
                throw std::runtime_error ("a synth page lacks its WAVETABLE subpage");
            SubpageSpec warmth;
            warmth.id = "wavetable.warmth";
            warmth.label = "WARMTH";
            warmth.engine = 1;
            warmth.first = (int) (wavetableWarmthParameterStart + (std::size_t) page.synth * wavetableWarmthParametersPerSlot);
            warmth.last = warmth.first + (int) wavetableWarmthParametersPerSlot - 1;
            const auto knob = [&] (const char* label, int field) { return KnobSpec { label, warmth.first + field }; };
            warmth.rows = { { { warmth.id + ".unison", "UNISON", { knob ("OSC 1 VOICES", 0), knob ("OSC 1 SPREAD", 1), knob ("OSC 2 VOICES", 2),
                                                                   knob ("OSC 2 SPREAD", 3), knob ("WIDTH", 4) } },
                              { warmth.id + ".filter", "FILTER", { knob ("TYPE", 6), knob ("VOWEL SIZE", 7) } },
                              { warmth.id + ".motion", "MOTION", { knob ("DRIFT", 5), knob ("ATTACK CURVE", 8) } } } };
            page.subpages.insert (at + 1, std::move (warmth));
        }
    // Era Reverb: its voice, time and mix on the first page; wobble, grit and tone on the second.
    for (auto& page : m.pages)
        if (page.id == "effects")
            for (auto& slot : page.subpages)
            {
                if (! juce::isPositiveAndBelow (slot.effect, 8)) continue;
                const auto base = (int) (eraReverbParameterStart + (std::size_t) slot.effect * eraReverbParametersPerSlot);
                const std::pair<int, const char*> mainFields[] { { 0, "VOICING" }, { 1, "AGE" }, { 2, "DECAY" }, { 3, "PRE-DELAY" },
                                                                 { 4, "SIZE" }, { 5, "ATTACK" }, { 6, "DIFFUSION" }, { 14, "MIX" } };
                const char* toneLabels[] { "WOBBLE", "RATE", "GRIT", "BASS", "DAMPING", "LOW CUT", "HIGH CUT" };
                SubpageSpec main;main.id = "era_reverb.main";main.label = "ERA REVERB";main.effect = slot.effect;
                main.first = base;main.last = base + (int) eraReverbParametersPerSlot - 1;
                SectionSpec mainSection { main.id, effectDisplayName (EffectKind::eraReverb).toUpperCase(), {} };
                for (const auto& [field, label] : mainFields) mainSection.knobs.push_back ({ label, base + field });
                main.rows = { { std::move (mainSection) } };slot.routes.push_back (std::move (main));
                SubpageSpec tone;tone.id = "era_reverb.tone";tone.label = "ERA TONE";tone.effect = slot.effect;
                tone.first = base + 7;tone.last = base + 13;
                SectionSpec toneSection { tone.id, "ERA TONE", {} };
                for (int field = 0; field < 7; ++field) toneSection.knobs.push_back ({ toneLabels[field], tone.first + field });
                tone.rows = { { std::move (toneSection) } };slot.routes.push_back (std::move (tone));
            }
    for(auto& page:m.pages)if(page.synth>=0&&page.synth<4){
        const auto bank=std::size_t(page.synth);
        SubpageSpec drum;drum.id="drum";drum.label="DRUM";
        SubpageSpec mode;mode.id="drum.mode";mode.label="SOURCE";
        mode.first=mode.last=int(drumModeParameterStart+bank);
        mode.rows={{{"drum.source","DRUM SOURCE",{{"DRUM MODE",mode.first}}}}};
        drum.routes.push_back(std::move(mode));
        for(int pad=0;pad<16;++pad){
            const auto base=int(drumParameterStart+bank*drumParametersPerSlot+std::size_t(pad)*drumParametersPerPad);
            SubpageSpec leaf;leaf.id="drum.pad."+String(pad+1);leaf.label="PAD "+String(pad+1);leaf.first=base;leaf.last=base+8;
            leaf.rows={
                {{"drum.level","PAD "+String(pad+1),{{"GAIN",base},{"PAN",base+1},{"TUNE",base+2}}}},
                {{"drum.trim","TRIM",{{"START",base+3},{"END",base+4}}},
                 {"drum.envelope","ENVELOPE",{{"ATTACK",base+5},{"DECAY",base+6}}},
                 {"drum.mapping","MAPPING",{{"MIDI NOTE",base+7},{"CHOKE",base+8}}}}
            };
            drum.routes.push_back(std::move(leaf));
        }
        page.subpages.push_back(std::move(drum));
    }
    for (auto& page : m.pages)
        if (page.id == "effects")
            for (auto& slot : page.subpages)
            {
                if (! juce::isPositiveAndBelow (slot.effect, 8))
                    continue;
                const auto knob = [&] (const char* label, const String& key)
                {
                    const auto index = rackParameterIndex (NativeModule::saturation, (std::uint8_t) slot.effect, key);
                    if (! index)
                        throw std::runtime_error ("saturation numeric page binds a missing key");
                    return KnobSpec { label, (int) *index };
                };
                SubpageSpec global;
                global.id = "saturation.global"; global.label = "SAT GLOBAL"; global.effect = slot.effect;
                global.rows = {
                    { { "saturation.mode", "SATURATION", { knob ("MODE", "saturation.mode"), knob ("DRIVE COMP", "saturation.drive_compensation") } },
                      { "saturation.trims", "INPUT / OUTPUT", { knob ("INPUT", "saturation.input_trim_db"), knob ("OUTPUT", "saturation.output_trim_db"), knob ("GLOBAL MIX", "saturation.mix") } } },
                    { { "saturation.crossover", "CROSSOVERS", { knob ("LOW", "saturation.low_crossover_hz"), knob ("HIGH", "saturation.high_crossover_hz") } },
                      { "saturation.cuts", "WET CUTS", { knob ("PRE CUT ON", "saturation.pre_low_cut_enabled"), knob ("PRE LOW CUT", "saturation.pre_low_cut_hz"), knob ("POST CUT ON", "saturation.post_high_cut_enabled"), knob ("POST HIGH CUT", "saturation.post_high_cut_hz") } } }
                };
                slot.routes.push_back (std::move (global));
                for (const auto* name : { "full", "low", "mid", "high" })
                {
                    const auto lane = String (name), prefix = "saturation." + lane + ".";
                    SubpageSpec bank;
                    bank.id = "saturation." + lane; bank.label = "SAT " + lane.toUpperCase(); bank.effect = slot.effect;
                    bank.rows = { { { bank.id, lane.toUpperCase() + " CHARACTER", { knob ("BYPASS", prefix + "bypassed"), knob ("CHARACTER", prefix + "character"), knob ("DRIVE", prefix + "drive_db") } },
                                    { bank.id + ".blend", lane.toUpperCase() + " BLEND", { knob ("PRE TILT", prefix + "pre_tilt_db"), knob ("LANE MIX", prefix + "mix"), knob ("WET TRIM", prefix + "trim_db") } } } };
                    slot.routes.push_back (std::move (bank));
                }
            }
    for (const auto& p : m.pages)
        for (const auto& s : p.subpages)
            for (int r = 0; r < std::max (1, (int) s.routes.size()); ++r)
            {
                const auto& leaf = s.routes.empty() ? s : s.routes[(size_t) r];
                std::map<String, int> counts;
                for (auto& row : leaf.rows)
                    for (auto& sec : row)
                        for (auto& k : sec.knobs)
                            ++counts[k.label];
                for (auto& row : leaf.rows)
                    for (auto& sec : row)
                        for (auto& k : sec.knobs)
                            if (juce::isPositiveAndBelow (k.index, (int) airack::parameterCount))
                            {
                                m.knobLabels[(size_t) k.index] = k.label;
                                m.knobNames[(size_t) k.index] = counts[k.label] > 1 ? sec.title + " " + k.label : k.label;
                            }
            }

    // The strip beside a side unit: both ends and EXPORT as they are, a slice of the overview's glass taken out.
    {
        const auto& raw = m.plates.at ("strip");
        const auto n = m.layout["strip_narrow"];
        const auto c0 = (int) n["cut"][0], c1 = (int) n["cut"][1], cut = c1 - c0;
        juce::Image narrow (juce::Image::ARGB, raw.getWidth() - cut, raw.getHeight(), true);
        {
            juce::Graphics g (narrow);
            g.drawImageAt (raw.getClippedImage ({ 0, 0, c0, raw.getHeight() }), 0, 0);
            g.drawImageAt (raw.getClippedImage ({ c1, 0, raw.getWidth() - c1, raw.getHeight() }), c0, 0);
        }
        if (narrow.getWidth() != (int) n["width"])
            throw std::runtime_error ("the narrowed strip's width differs from elements.json");
        m.plates["strip.narrow"] = narrow;
        const auto shiftRect = [&] (Rect r)
        {
            if (r.getX() >= (float) c1)
                return r.translated ((float) -cut, 0.0f);
            if (r.getX() < (float) c0 && r.getRight() > (float) c1)
                return r.withWidth (r.getWidth() - (float) cut);
            return r;
        };
        const auto shiftPoint = [&] (Pt p) { return p.x >= (float) c1 ? p.translated ((float) -cut, 0.0f) : p; };
        std::vector<Element> copies;
        for (const auto& e : m.elements)
            if (e.view == "strip")
            {
                auto c = e;
                c.view = "strip.narrow";
                for (auto* r : { &c.rect, &c.hitRect, &c.footprint, &c.glass, &c.triangle, &c.legend })
                    *r = shiftRect (*r);
                c.led = shiftPoint (c.led);
                c.pivot = shiftPoint (c.pivot);
                copies.push_back (c);
            }
        m.elements.insert (m.elements.end(), copies.begin(), copies.end());
        m.overviewNarrow = m.overview;
        m.overviewNarrow.lattice.glass = shiftRect (m.overview.lattice.glass);
        m.overviewNarrow.columns = (int) n["columns"];
        m.displays["strip.narrow"] = m.displays.at ("strip");
        m.focusOrder["strip.narrow"] = m.focusOrder.at ("strip");
    }

    bakeDots (m.plates["session"], m.displays["session"].lattice);
    bakeDots (m.plates["strip"], m.displays["strip"].lattice);
    bakeDots (m.plates["strip"], m.overview.lattice);
    bakeDots (m.plates["strip.narrow"], m.displays["strip"].lattice);
    bakeDots (m.plates["strip.narrow"], m.overviewNarrow.lattice);
    bakeDots (m.plates["pattern"], m.displays["pattern.settings"].lattice);
    bakeDots (m.plates["pattern"], m.displays["pattern.note"].lattice);
    bakeDots (m.plates["pattern"], latticeFrom (m.rolls["pattern"]));
    bakeDots (m.plates["advanced"], m.displays["advanced"].lattice);
    m.house = house::loadModel (house::houseSkinFile, m.markerFace);
    const auto badge = house::houseSkinFile ("ai-rack-badge.png");
    m.house.assets["brand:badge"].image = juce::ImageFileFormat::loadFrom (badge.getData(), badge.getSize());
    return m;
}

//============================================================================== dot-matrix drawing

/** The profile is sampled in bins of `step` px; values are read between bin centres. */
float profileAt (const std::vector<float>& p, float step, float r)
{
    const auto u = r / step - 0.5f;
    if (u <= 0.0f)
        return p.front();
    const auto i = (size_t) u;
    if (i + 1 >= p.size())
        return p.back();
    const auto f = u - (float) i;
    return p[i] * (1.0f - f) + p[i + 1] * f;
}

/** Distance from a pixel (index coordinates, as measured) to its nearest lattice point, and that point's cell. */
float latticeDistance (const Lattice& l, float x, float y, int* col, int* row)
{
    const auto c = std::round ((x - l.origin.x) / l.pitch.x), r = std::round ((y - l.origin.y) / l.pitch.y);
    if (col != nullptr) *col = (int) c;
    if (row != nullptr) *row = (int) r;
    return std::hypot (x - l.origin.x - c * l.pitch.x, y - l.origin.y - r * l.pitch.y);
}

static juce::uint8 addTo (juce::uint8 channel, float v) { return (juce::uint8) juce::jlimit (0, 255, juce::roundToInt ((float) channel + v)); }

/** The unlit lattice back on the plate's smoothed glass, inside the 2 px rim the extraction kept. */
static void bakeDots (juce::Image& plate, const Lattice& l)
{
    const auto lumTint = std::max (1e-3f, 0.114f * l.tint[0] + 0.587f * l.tint[1] + 0.299f * l.tint[2]);
    const auto g = l.glass.toNearestInt();
    juce::Image::BitmapData bd (plate, juce::Image::BitmapData::readWrite);
    for (int y = g.getY() + 2; y < g.getBottom() - 2; ++y)
        for (int x = g.getX() + 2; x < g.getRight() - 2; ++x)
        {
            const auto v = profileAt (l.profile, l.step, latticeDistance (l, (float) x, (float) y)) / lumTint;
            const auto c = bd.getPixelColour (x, y);
            bd.setPixelColour (x, y, juce::Colour (addTo (c.getRed(), v * l.tint[2]), addTo (c.getGreen(), v * l.tint[1]), addTo (c.getBlue(), v * l.tint[0])));
        }
}

juce::MemoryBlock studioSkinFile (const juce::String& path)
{
    // The binary data keeps file names only; the skin's 45 file names are unique.
    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    for (int i = 0; i < StudioSkinData::namedResourceListSize; ++i)
        if (name == StudioSkinData::getNamedResourceOriginalFilename (StudioSkinData::namedResourceList[i]))
        {
            int size = 0;
            if (const auto* data = StudioSkinData::getNamedResource (StudioSkinData::namedResourceList[i], size))
                return { data, (size_t) size };
        }
    return {};
}

namespace
{
/** The parsed skin or why it failed, deleted when JUCE shuts down so that its images and typefaces go first. */
class LoadedSkin final : private juce::DeletedAtShutdown
{
public:
    LoadedSkin()
    {
        try
        {
            model = std::make_unique<Model> (loadModel());
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        catch (...)
        {
            error = "unknown error";
        }
    }

    ~LoadedSkin() override { clearSingletonInstance(); }

    std::unique_ptr<Model> model;
    String error;

    JUCE_DECLARE_SINGLETON_INLINE (LoadedSkin, false)
};
} // namespace

const Model* studioSkin() noexcept { return LoadedSkin::getInstance()->model.get(); }

juce::String studioSkinError() { return LoadedSkin::getInstance()->error; }
} // namespace airack::studio
