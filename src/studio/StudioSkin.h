#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "HouseSkin.h"
#include "ParameterRegistry.h"

#include <array>
#include <map>
#include <stdexcept>
#include <vector>

// The approved studio window skin (docs/design/racks/2026-10-09-studio-rack/window/skin), embedded in the plugin.
namespace airack::studio
{
using juce::String;
using Pt = juce::Point<float>;
using Rect = juce::Rectangle<float>;

//============================================================================== model (elements.json)

struct Asset
{
    juce::Image image;
    Pt anchor;
    float lensR = 0, ringR = 0;
    /** The asset's entry in elements.json, for the parts that carry more (the tape's body and slices). */
    juce::var spec;
};

struct Element
{
    String id, view, kind, label, group, control;
    /** The display spec a display element draws with; the status an indicator, or a button's LED, reads back. */
    String display, status, ledStatus;
    juce::StringArray positions;
    Rect rect, hitRect, footprint, glass, triangle, legend;
    Pt led, pivot, centre;
    float lensR = 0, ringR = 0, skirtR = 0, sweep = 140;
    /** The command's value: a mode button's request mode (1 to 3). */
    int value = -1;
    std::map<String, String> layers;

    /** "session.save" -> "save". */
    String localId() const { return id.fromFirstOccurrenceOf (".", false, false); }

    String layer (const String& role) const
    {
        auto it = layers.find (role);
        return it == layers.end() ? String() : it->second;
    }
};

/** One display line: the bundled font at a fitted cap height and horizontal scale, its pen on a character grid. */
struct Line
{
    String text, align, indicator;
    float cap = 0, hscale = 1, penX = 0, baseline = 0, maxRight = 0;
    Rect ink;
    Pt indicatorCentre;
};

/** A glass: where its dots sit, how an unlit dot looks by distance from its lattice point, and the lit ink, full
    within `rippleCore` of a lattice point and dimmed by `ripple` beyond `rippleEdge`. */
struct Lattice
{
    Rect glass;
    Pt origin, pitch;
    std::vector<float> profile;
    float step = 0.25f, ripple = 0, rippleCore = 1, rippleEdge = 2;
    std::array<float, 3> tint {};
    juce::Colour ink;
};

struct Message
{
    String today, headline, detail;
    bool errorLed = false, persistent = false;
    int holdMs = 0;
    std::map<String, String> overrides;
};

struct Playback { String today, alert; };
struct Operation { String today, text; bool amber = false; int holdMs = 0; };

struct DisplaySpec
{
    Lattice lattice;
    std::map<String, Line> lines;
    float minHscale = 0.8f;
    std::vector<Message> messages;
    std::vector<Playback> playback;
    std::vector<Operation> operations;
    /** The display's entry in elements.json, for what only one display has (the note marks, the bank line's gap). */
    juce::var spec;

    const Line& line (const String& key) const
    {
        auto it = lines.find (key);
        if (it == lines.end())
            throw std::runtime_error (("missing display line " + key).toStdString());
        return it->second;
    }
};

struct OverviewSpec
{
    Lattice lattice;
    int columns = 0, rows = 0, rowsPerNote = 2, linePeak = 240, numberPeak = 255;
    float barsX0 = 0, lineTop = 0, lineBottom = 0, lineWidth = 1.5f, numberOffsetX = 0, numberCap = 0, numberBaseline = 0;
    std::array<float, 4> litInset {}; // left, top, right, bottom
    float seamDepth = 0, seamWidth = 1;
    juce::Colour lit;
};

/** A pattern note as PatternNote holds it: start and duration in quarter beats. */
struct Note
{
    double start = 0, duration = 0.25;
    int pitch = 60, velocity = 100, channel = 1;
    bool operator== (const Note&) const = default;
};

struct Group { String id, view, label; juce::StringArray members; };

/** The PROGRAMMER's pages as elements.json's page_spec lays them out: rows of sections of knobs, a knob per registry
    parameter. ROUTES and FX N hold their four pages in `routes`, FX N's each with its `effect`. */
struct KnobSpec { String label; int index = 0; };
struct SectionSpec { String id, title; std::vector<KnobSpec> knobs; bool macros = false; };
struct SubpageSpec
{
    String id, label;
    std::vector<std::vector<SectionSpec>> rows;
    int engine = -1, effect = -1, first = 0, last = 0;
    bool rackLayout = false;
    std::vector<SubpageSpec> routes;
};
struct PageSpec
{
    String id, label;
    int synth = -1;
    std::vector<SubpageSpec> subpages;
    Rect rect;
    Pt led;
    float lensR = 0;
};

struct Mockup
{
    String path;
    double bpm = 120;
    int bars = 4, numerator = 4, denominator = 4, mappedCells = 0;
    String grid = "1/16";
    double swing = 0;
    std::vector<Note> notes;
};

struct Model
{
    std::map<String, juce::Image> plates;
    std::map<String, Asset> assets;
    std::vector<Element> elements;
    std::map<String, DisplaySpec> displays;
    /** The strip's overview, and the narrowed strip's (a slice of the glass taken out, fewer columns). */
    OverviewSpec overview, overviewNarrow;
    std::vector<Group> groups;
    std::map<String, juce::StringArray> focusOrder;
    juce::Typeface::Ptr face, screenFace;
    Mockup mockup;
    /** Stage B as elements.json gives it: the side units' page templates, styles and furniture; the stage layout;
        what each drawing shows. Read where they are drawn. */
    juce::var screens, layout, sideStates;
    /** Stage C as elements.json gives it: the PROGRAMMER's spec, the roll's and the keyboard's, the drawn states. */
    juce::var programmer, rolls, keyboards, patternMockup, programmerMockup;
    juce::Typeface::Ptr markerFace;
    std::vector<PageSpec> pages;
    /** A knob's name on the display: its label, or its section's title and label where the label repeats on its page. */
    std::array<String, airack::parameterCount> knobNames;
    std::array<String, airack::parameterCount> knobLabels;
    /** The AI Rack Synth unit, from its own skin, its tapes lettered in markerFace. */
    house::Model house;

    const OverviewSpec& overviewFor (const String& view) const { return view == "strip.narrow" ? overviewNarrow : overview; }

    const PageSpec& page (const String& id) const
    {
        for (auto& p : pages)
            if (p.id == id)
                return p;
        throw std::runtime_error (("missing PROGRAMMER page " + id).toStdString());
    }

    /** The sub-page a page shows, routes resolved to the page of four shown. */
    const SubpageSpec& leaf (const String& pageId, const String& subId, int routesPage) const
    {
        const auto& p = page (pageId);
        for (auto& s : p.subpages)
            if (s.id == subId)
                return s.routes.empty() ? s : s.routes[(size_t) juce::jlimit (0, (int) s.routes.size() - 1, routesPage)];
        return p.subpages.front().routes.empty() ? p.subpages.front() : p.subpages.front().routes.front();
    }

    const Asset& asset (const String& key) const
    {
        auto it = assets.find (key);
        if (it == assets.end())
            throw std::runtime_error (("missing asset '" + key + "'").toStdString());
        return it->second;
    }

    const Element& element (const String& id) const
    {
        for (auto& e : elements)
            if (e.id == id)
                return e;
        throw std::runtime_error (("missing element " + id).toStdString());
    }
};

/** The skin, parsed once from the plugin's binary data on the first call; nullptr if that failed. It lives until
    JUCE shuts down. */
const Model* studioSkin() noexcept;
/** Why the skin failed to load, or empty. */
juce::String studioSkinError();
/** An embedded skin file by its path in elements.json ("plates/session.png"); empty if absent. */
juce::MemoryBlock studioSkinFile (const juce::String& path);

/** A display's glass, dots and ink, and one of its lines, as elements.json gives them; a maker's faceplate gives its
    displays in the same form. A line without a cap, scale, pen or baseline reads them as 0. */
Lattice latticeFrom (const juce::var& display);
Line lineFrom (const juce::var& line);

/** The profile is sampled in bins of `step` px; values are read between bin centres. */
float profileAt (const std::vector<float>& p, float step, float r);
/** Distance from a pixel (index coordinates, as measured) to its nearest lattice point, and that point's cell. */
float latticeDistance (const Lattice& l, float x, float y, int* col = nullptr, int* row = nullptr);
} // namespace airack::studio
