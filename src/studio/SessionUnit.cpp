#include "SessionUnit.h"
#include "HouseSkin.h"
#include "FactorySounds.h"

namespace airack::studio
{
namespace
{
const juce::String loadErrorSentence = "Could not load saved sound: invalid or unsupported data. Current sound preserved.";

/** The preset file's stem in capitals, as the drawn "WARM CIRCUIT"; INIT when no path is set. */
String nameFromPath (const String& path)
{
    auto stem = path.trim().fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
    if (stem.containsChar ('.'))
        stem = stem.upToLastOccurrenceOf (".", false, false);
    return stem.trim().isEmpty() ? String ("INIT") : stem.trim().toUpperCase();
}

/** Today's text against the message table: exact entries first, then "{op} ..." templates, whose op is returned. */
const Message* matchMessage (const std::vector<Message>& messages, const String& today, String& op)
{
    op = {};
    for (auto& m : messages)
        if (m.today == today)
            return &m;
    for (auto& m : messages)
        if (m.today.startsWith ("{op}"))
        {
            const auto suffix = m.today.fromFirstOccurrenceOf ("{op}", false, false);
            if (today.length() > suffix.length() && today.endsWith (suffix))
            {
                op = today.dropLastCharacters (suffix.length());
                return &m;
            }
        }
    return nullptr;
}

String expandMessage (const String& pattern, const Message& m, const String& op, const String& name, bool headline)
{
    if (headline)
        if (auto it = m.overrides.find (op); it != m.overrides.end())
            return it->second;
    return pattern.replace ("{OP}", op.toUpperCase()).replace ("{NAME}", name);
}

void identify (juce::Component& c, const juce::String& id, const juce::String& name)
{
    c.setComponentID (id);
    c.setName (name);
}
} // namespace

SessionShown sessionShown (const SessionFacts& f, const DisplaySpec& d)
{
    SessionShown out;
    out.path = f.presetPath;
    out.name = f.soundName.isNotEmpty() ? f.soundName.toUpperCase() : nameFromPath (f.presetPath);
    out.tempoKey = f.hostBpm ? "host" : f.waiting ? "waiting" : "none";
    out.tempoText = f.hostBpm ? String (juce::roundToInt (*f.hostBpm)) + " BPM HOST" : String ("NO HOST TEMPO");
    String op;
    const Message* m = nullptr;
    if (f.status.isNotEmpty())
        if (auto* t = matchMessage (d.messages, f.status, op); t != nullptr && ! t->persistent && f.now - f.statusAt < t->holdMs)
            m = t;
    if (m == nullptr && f.loadError)
        m = matchMessage (d.messages, loadErrorSentence, op);
    out.layout = m != nullptr ? "message" : "name";
    if (m != nullptr)
    {
        out.headline = expandMessage (m->headline, *m, op, out.name, true);
        out.detail = expandMessage (m->detail, *m, op, out.name, false);
    }
    out.errorLed = f.loadError || (m != nullptr && m->errorLed);
    juce::StringArray parts;
    parts.add (f.presetPath.isEmpty() ? String ("No preset path") : f.presetPath);
    parts.add (f.tempoText);
    if (f.loadError)
        parts.add (loadErrorSentence);
    if (f.status.isNotEmpty())
        parts.add (f.status);
    out.accessible = parts.joinIntoString (". ");
    return out;
}

SessionUnit::SessionUnit (InstrumentProcessor& p, const Model& model, StudioServices s)
    : processor (p), services (std::move (s)), view (model, "session")
{
    setComponentID ("session");
    badge = model.house.asset ("brand:badge").image;
    const auto logo = house::houseSkinFile ("ai-rack-wordmark.svg");
    if (logo.getSize() > 0)
        wordmark = juce::Drawable::createFromImageData (logo.getData(), logo.getSize());
    addAndMakeVisible (view);
    setSize (view.getWidth(), view.getHeight());
    identify (status, "status", "Edit result");
    identify (tempo, "tempo.status", "Tempo availability");
    identify (loadError, "state.load.error", "Saved sound load error");
    for (auto* label : { &status, &tempo, &loadError })
        addChildComponent (label);
    if (auto* b = button ("preset.save"))
        b->onClick = [this] { preset (true); };
    if (auto* b = button ("preset.load"))
        b->onClick = [this] { preset (false); };
    if (auto* b = button ("preset.browse"))
        b->onClick = [this] { browseSounds(); };
    if (auto* b = button ("undo"))
        b->onClick = [this] { result (processor.undoSoundEdit(), "Undo"); };
    if (auto* b = button ("redo"))
        b->onClick = [this] { result (processor.redoSoundEdit(), "Redo"); };
    if (auto* b = button ("ai.toggle"))
        b->onClick = [this, b] { if (onAi) onAi (! b->getToggleState()); };
    if (auto* b = button ("rack.toggle"))
        b->onClick = [this, b] { if (onLever) onLever (b->getToggleState()); };
    if (auto* d = view.display.get())
    {
        d->onPath = [this] (const juce::String& path) { setPath (path); };
        d->onContext = [this] { copyName(); };
        d->entry.onTextChange = [this, d]
        {
            if (! d->entry.isVisible())
                setPath (d->entry.getText());
        };
    }
    refresh();
}

void SessionUnit::paintOverChildren (juce::Graphics& g)
{
    if (! badge.isValid() || wordmark == nullptr)
        return;
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    // Clear the old printed name; the plate and lettering remain separate for sharp display scaling.
    g.drawImage (view.model.plates.at ("session"), 100, 24, 190, 32, 100, 88, 190, 12);
    g.drawImageWithin (badge, 98, 12, 178, 84, juce::RectanglePlacement::centred);
    wordmark->drawWithin (g, { 128, 26, 116, 52 }, juce::RectanglePlacement::centred, 1.0f);
}

juce::Button* SessionUnit::button (const char* id) const { return dynamic_cast<juce::Button*> (view.findChildWithID (id)); }

void SessionUnit::refresh()
{
    loadError.setText (processor.stateLoadFailed() ? loadErrorSentence : juce::String(), juce::dontSendNotification);
    tempo.setText (processor.tempoUnavailable() ? "Host tempo unavailable: synced modulation/effects waiting" : "Offline / manual controls available",
                   juce::dontSendNotification);
    SessionFacts f;
    f.presetPath = processor.soundPresetPath();
    if (f.presetPath != shownPath)
    {
        shownPath = f.presetPath;
        if (auto* d = view.display.get(); d != nullptr && ! d->entry.isVisible())
            d->entry.setText (f.presetPath, false);
    }
    f.soundName = processor.soundName();
    f.status = status.getText();
    f.tempoText = tempo.getText();
    f.statusAt = statusAt;
    f.now = services.nowMs();
    f.hostBpm = processor.currentHostBpm();
    f.waiting = processor.tempoUnavailable();
    f.loadError = processor.stateLoadFailed();
    const auto shown = sessionShown (f, view.model.displays.at ("session"));
    if (view.display != nullptr)
        view.display->show (shown);
    if (view.error != nullptr)
        view.error->setLit (shown.errorLed);
}

void SessionUnit::report (const juce::String& sentence)
{
    status.setText (sentence, juce::dontSendNotification);
    statusAt = services.nowMs();
    refresh();
}

void SessionUnit::result (bool ok, const juce::String& operation)
{
    report (operation + (ok ? " complete" : " failed; sound preserved"));
    if (onEdit)
        onEdit();
}

void SessionUnit::preset (bool writing)
{
    commitEntry();
    const auto path = processor.soundPresetPath();
    if (! juce::File::isAbsolutePath (path))
    {
        if (writing) { browse(true); return; }
        report ("Preset failed: enter an absolute file path");
        return;
    }
    const juce::File file (path);
    result (writing ? processor.saveSoundPreset (file) : processor.loadSoundPreset (file), writing ? "Save preset" : "Load preset");
}

void SessionUnit::browseSounds()
{
    if (! services.chooseMenu) { browse(); return; }
    if (choosing) return;
    commitEntry();
    SoundDocument captured;std::uint64_t edit=0,reset=0;
    if (! processor.trySoundSnapshot(captured,edit,reset)) return;
    const auto generation=pathGeneration;
    choosing=true;
    juce::StringArray items { "Open a saved sound..." };
    for (const auto& sound:factorySounds()) items.add(sound.name);
    const juce::Component::SafePointer<SessionUnit> safe(this);
    services.chooseMenu(button("preset.browse"),items,[safe,edit,reset,generation](int selected)
    {
        if (safe==nullptr) return;
        safe->choosing=false;
        SoundDocument now;std::uint64_t currentEdit=0,currentReset=0;
        if (selected<1||generation!=safe->pathGeneration
            ||!safe->processor.trySoundSnapshot(now,currentEdit,currentReset)||currentReset!=reset||currentEdit!=edit)
        { safe->report("Preset selection stale/cancelled; sound preserved");return; }
        if (selected==1) { safe->browse();return; }
        const auto index=std::size_t(selected-2);
        if (index>=factorySounds().size()) return;
        const bool loaded=safe->processor.loadFactorySound(index);
        if (loaded) safe->setPath({});
        safe->result(loaded,"Load preset");
    });
}

void SessionUnit::browse(bool saving)
{
    if (choosing)
        return;
    commitEntry();
    SoundDocument captured;
    std::uint64_t edit = 0, reset = 0;
    if (! processor.trySoundSnapshot (captured, edit, reset))
        return;
    const auto generation = pathGeneration;
    choosing = true;
    const juce::Component::SafePointer<SessionUnit> safe (this);
    auto folder = processor.soundFolder();
    if (! folder.isDirectory()) folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    services.chooseFile ({ saving ? "Save a sound preset" : "Choose a sound preset",
                           saving ? "*.airack" : "*.airack;*.soundstate", saving, saving,
                           saving ? folder.getChildFile("New sound.airack") : juce::File(), this },
                         [safe, reset, generation, saving] (const juce::File& file)
                         {
                             if (safe == nullptr)
                                 return;
                             safe->choosing = false;
                             SoundDocument now;
                             std::uint64_t edit = 0, token = 0;
                             if (file == juce::File() || generation != safe->pathGeneration
                                 || ! safe->processor.trySoundSnapshot (now, edit, token) || token != reset)
                                 safe->report ("Preset selection stale/cancelled; sound preserved");
                             else {
                                 safe->setPath (file.getFullPathName());
                                 if (saving) safe->preset(true);
                             }
                         });
}

void SessionUnit::commitEntry()
{
    if (auto* d = view.display.get(); d != nullptr && d->entry.isVisible())
        d->commitPath();
}

void SessionUnit::setPath (const juce::String& path)
{
    processor.setSoundPresetPath (path);
    ++pathGeneration;
    if (view.display != nullptr)
        view.display->entry.setText (path, false);
    refresh();
}

void SessionUnit::copyName()
{
    const auto name = processor.soundName();
    if (name.isEmpty() || ! services.copyText)
        return;
    services.copyText (name);
    report ("Name copied");
}

void SessionUnit::setLever (bool advanced)
{
    if (auto* lever = view.get<Lever> ("session.rack"))
        lever->setPosition (advanced ? 1 : 0, juce::dontSendNotification);
}

void SessionUnit::setAiOpen (bool open)
{
    if (auto* ai = view.get<SkinButton> ("session.ai"))
        ai->setToggleState (open, juce::dontSendNotification);
}
} // namespace airack::studio
