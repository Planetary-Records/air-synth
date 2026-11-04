#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

// The approved AI Rack Synth unit skin (docs/design/racks/2026-10-09-studio-rack/house/skin), embedded in the plugin.
namespace airack::studio::house
{
using juce::String;
using Pt = juce::Point<float>;

inline const juce::Colour accent (0xffe8a23a);

//============================================================================== model (elements.json)

struct Asset
{
    juce::Image image;
    Pt anchor;
    float lensR = 0;
    juce::Rectangle<int> body;
    std::array<int, 4> slice {};
};

/** A lever's or selector's position: its value, its printed legend and the asset of its bat; a selector's pointer
    angle there, and where its legend is printed. */
struct Position
{
    double value = 0;
    String legend, asset;
    float angle = 0;
    juce::Rectangle<float> legendRect;
};

/** Optional native print for a stationary plate supplied without control artwork. Existing maker skins do not
    request it. Tick-label values use the bound element's registry response and existing printed-angle mapping. */
struct NativeEngraving
{
    juce::Colour ink { 0xffd6b27d };
    juce::Rectangle<float> labelRect, unitRect;
    float labelFontHeight = 20, scaleFontHeight = 18, labelRadius = 0, labelInset = 0, baseRadius = 0;
    float minorStep = 0, majorLength = 11, minorLength = 6;
    std::vector<std::pair<double, String>> tickLabels;
};

struct Element
{
    String id, view, kind, label, group;
    String paramId, bindingType, unit;
    /** paramId's index in the parameter registry, which the loader resolves. */
    std::size_t paramIndex = 0;
    juce::Range<double> range { 0.0, 1.0 };
    juce::StringArray choices;
    double defaultValue = 0, drawnValue = 0, sweepLo = 0, sweepHi = 0;
    int onIndex = 1;
    Pt pivot, tickCentre, led, centre, size;
    /** A knob's hit circle's centre where it is not its pivot ("hit_center"): OXIDE RETURN's TIME bezel, above the
        needle's hub. */
    std::optional<Pt> hitAt;
    float faceR = 0, tickR = 0, hitR = 0, lensR = 0, angle = 0, capCentreX = 0, y0 = 0, y1 = 0;
    juce::Rectangle<float> rect, hitRect, dash;
    bool hasDash = false;
    std::map<String, String> layers;
    /** Generated images for the control's idle, hover, pressed, disabled and toggle states. */
    std::map<String, String> stateAssets;
    Pt stateScale { 1.0f, 1.0f };
    /** A generated control caption registered independently of its hit rectangle. */
    std::optional<Pt> stateCaptionAt;
    /** A maker skin's binding: the design's unit and its module's key, which each placed rack resolves to its own
        slot's parameter. A house element binds paramId instead and keeps rackUnit at -1. */
    int rackUnit = -1;
    String key;
    bool logarithmic = false;
    /** The registry's normalisation ("response": "parameter"): the range and skew are the bound parameter's. */
    bool registryResponse = false;
    std::optional<NativeEngraving> nativeEngraving;
    double skew = 1;
    /** A maker's printed scale: the pointer's angle, or a needle's x, at each step's value, rising. A needle stands at
        y0. */
    std::vector<std::pair<double, float>> ticks;
    /** A grip ring: its strip of frames over one tooth period, its teeth, and the angle at which frame 0 is drawn. */
    String knurl;
    int knurlFrames = 0, knurlTeeth = 0;
    float knurlRest = 0;
    /** Where the host's console tape may cover a printed legend that is no longer true. */
    juce::Rectangle<float> tapeRect;
    /** The route that legend prints, by the registry's choice names. */
    struct PrintedRoute
    {
        String source { "Velocity" }, target { "Cutoff" }, bank { "Analog" };
    } printedRoute;
    /** A maker's tape lettering, which the host letters on its own blank while the tape is shown. */
    String text;
    /** An action's place while lit, where it then takes presses and draws its hover, press and focus: a pull standing
        raised while its drawer is open. */
    juce::Rectangle<float> litRect;
    /** Where the stack draws a locked control's dot. */
    Pt dot;
    bool hasDot = false;
    std::vector<Position> positions;
    /** A press on a position's printed legend chooses it ("legend_click"), and a lever's press elsewhere does nothing. */
    bool legendClick = false;
    /** The value at which a lamp is lit. */
    double litValue = 0;
    /** A soft knob's screen ("delay.1") and knob place (1 to 4), or a spare knob's place; its names on the VFD and
        the LCD. A face knob has only its VFD name. */
    String screen, vfdName, lcdLabel;
    int slot = 0;
    /** A choice knob, stepping from choice to choice over the range 0 to n - 1. */
    bool detents = false;
    /** A control shown only while a parameter of the rack has a value (TIME while DELAY SYNC is off); unit -1 for
        always. The index is the parameter's in the registry, which binding resolves. */
    struct ShownWhen
    {
        int unit = -1;
        String key;
        double value = 0;
        std::size_t index = 0;
    } shownWhen;
    ShownWhen nativeActiveWhen;
    String nativeInactiveDescription;
    float nativeInactiveAlpha = 0.4f, nativeAlpha = 1.0f;
    /** A display's role (vfd, lcd or name), and its spec in the window skin's form: its glass, dots, ink, lines and
        minimum horizontal scale, and the VFD's bar. Its glass is `rect`. */
    String role;
    juce::var display;

    String layer (const String& role) const
    {
        auto it = layers.find (role);
        return it == layers.end() ? String() : it->second;
    }

    String stateAsset (const String& state) const
    {
        const auto it = stateAssets.find (state);
        return it == stateAssets.end() ? String() : it->second;
    }

    Pt hitCentre() const { return hitAt.value_or (pivot); }

    /** "unit.macro.1" -> "macro.1": the same control in either view. */
    String localId() const { return id.fromFirstOccurrenceOf (".", false, false); }

    /** What the element occupies on the panel, in view pixels: knob circle, fader track, button with its LED, tape body. */
    juce::Rectangle<float> footprint() const
    {
        if (kind == "rotary" || kind == "selector")
            return juce::Rectangle<float> (2 * hitR, 2 * hitR).withCentre (hitCentre());
        if (kind == "still")
            return juce::Rectangle<float> (2 * faceR, 2 * faceR).withCentre (pivot);
        if (kind == "fader")
            return hitRect;
        if (kind == "tape")
            return juce::Rectangle<float> (size.x, size.y).withCentre (centre)
                .transformedBy (juce::AffineTransform::rotation (juce::degreesToRadians (angle), centre.x, centre.y));
        if (lensR > 0)
            return rect.getUnion (juce::Rectangle<float> (2 * lensR, 2 * lensR).withCentre (led));
        return rect;
    }
};

struct Group
{
    String id, view, label;
    juce::StringArray members, focusOrder;
};

/** The tapes' marker lettering: the tape asset's text spec, with the skin's font. */
struct Lettering
{
    juce::Typeface::Ptr face;
    float capOneLine = 0, capTwoLines = 0, pitchOfCap = 0, maxWidthOfBody = 0, strokeOfCap = 0, alpha = 1;
    juce::Colour ink;
};

struct Model
{
    std::map<String, Asset> assets;
    std::vector<Element> elements;
    std::vector<Group> groups;
    Lettering lettering;
    /** The existing condensed screen face, supplied only for a model requesting native control print. */
    juce::Typeface::Ptr nativeEngravingFace;
    struct NativeFace
    {
        Element::ShownWhen statusWhen, pilotWhen;
        juce::Rectangle<float> statusRect;
        String status, inactiveStatus;
        Pt pilotCentre;
        float pilotRadius = 0;
        std::vector<Element::ShownWhen> controls;
    };
    std::optional<NativeFace> nativeFace;
    /** Supported Tile Array records, validated during worker preparation; paint never reparses JSON. */
    juce::var nativeExtensionRecords;
    /** The rack's pitch on the plates (rack_pitch): the 19 in panel's width and the 1.75 in unit's height, in pixels. */
    float panelWidth = 0, unitHeight = 0;
    /** A maker skin's identity: the design it faces, its maker, its model and its height in rack units. The house
        skin has none. */
    juce::Uuid designId = juce::Uuid::null();
    String maker, modelName;
    int units = 0;
    /** How a maker's faceplate opens: a "recess" view over the face, closed by its closers; a "drawer" view that
        goes between the face and its front view, adding its units, opened and closed by the opener; or a "door" whose
        view goes over the face and whose hanging view goes below it, adding its units. A door names the screen and
        the control its drawing shows touched. */
    struct Reveal
    {
        String kind, view, front, opener;
        juce::StringArray closers, titles;
        int units = 0;
        String hang, drawnScreen, drawnTouch;
        /** The reveal's motion: its time; THREAD PRESS's fasteners' discs, which turn a quarter, and the cover that
            lifts, with its corners' radius; GLASS CURRENT's closed door on the face; OXIDE RETURN's tambour, the lid
            that slides by `slide` as it opens; SMALL HOURS's lid, which turns up about its "top" `hinge` until it
            stands `openHeight` tall. */
        struct Motion
        {
            int ms = 0;
            std::vector<juce::Rectangle<float>> turns;
            juce::Rectangle<float> lid;
            float corner = 0;
            juce::Rectangle<float> door;
            Pt slide;
            String hinge;
            float openHeight = 0;
        } motion;
    } reveal;
    /** The pages a door's panel shows, in order: each page's button and its screens. */
    struct Page
    {
        String id, button, label;
        juce::StringArray screens;
    };
    std::vector<Page> pages;
    /** The view shown alone while the rack is folded, or empty. */
    String strip;
    /** False when the faceplate reads out on its own display ("readout": "display"): the house readout does not show. */
    bool hostReadout = true;

    const Asset& asset (const String& key) const
    {
        auto it = assets.find (key);
        if (it == assets.end())
            throw std::runtime_error (("missing asset " + key).toStdString());
        return it->second;
    }
};

/** Parses the skin from `file` (a skin file by its path in elements.json), lettering its tapes in `tapeFace`, the
    font the window skin embeds for the bundle's. Throws on a missing file, image or parameter. */
Model loadModel (const std::function<juce::MemoryBlock (const String&)>& file, juce::Typeface::Ptr tapeFace);

/** An embedded house skin file by its path in elements.json ("plates/unit.png"); empty if absent. */
juce::MemoryBlock houseSkinFile (const String& path);

/** The pointer angle (degrees clockwise from 12 o'clock) for a normalised value: the value picks a point on the
    printed tick ring, and the pointer, painted on the face, aims at it from its pivot. On a maker's printed scale the
    angle is linear in the proportion between steps and pins at the end steps. */
float pointerAngleFor (const Element& e, double proportion);

/** A value's proportion of an element's range, logarithmic or linear as its binding's response, or through the
    registry's skew as the parameter's NormalisableRange gives it. */
double proportionOf (const Element& e, double value);

/** A grip ring's frame at a pointer angle: round(frac(teeth * (angle - rest) / 360) * frames) mod frames, so that its
    teeth travel with the knob while the lighting drawn into each frame stays put. */
int gripFrame (const Element& e, float angle);

/** A value readout's text: on or off, the choice's name, ms below a second, signed dB, pan as C, L n or R n,
    macros and fractions in percent, Hz and kHz, otherwise two decimals. */
String formatValue (const Element& e, double v);

/** formatValue as a maker's display writes it: no space between a number and its unit ("300ms", "+7st", "20.0Hz"),
    and choices and on or off in capitals. */
String compactValue (const Element& e, double v);
} // namespace airack::studio::house
