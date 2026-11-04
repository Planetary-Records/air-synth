#include "FrozenRackExamples.h"
#include "SoundState.h"
#include "ProjectDocument.h"
#include "AiCatalog.h"
#include "CompanionClient.h"
#include "CompanionWire.h"
#include "InstrumentProcessor.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "RackReconciliation.h"
#include "ProgrammerUnit.h"
#include "StudioSkin.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>
#ifndef RACKSYNTH_FIXTURE_DIR
#define RACKSYNTH_FIXTURE_DIR ""
#endif

namespace
{
using namespace airack;
int checks = 0, failures = 0;
void check(bool good, const char* expression, int line)
{
    ++checks;
    if (!good && ++failures < 90) std::cerr << "FAIL " << line << ' ' << expression << '\n';
}
#define CHECK(x) check(bool(x), #x, __LINE__)

#ifdef NDEBUG
constexpr const char* configuration = "release";
#else
constexpr const char* configuration = "debug";
#endif
// Format 26 is Space Reverb's main format; format 27, main's wavetable warmth append, is the last one before Era
// Reverb's. Both keep 17 effect banks.
struct FrozenFormat { const char* directory; std::size_t parameters; int sound, project; std::vector<const char*> sounds; };
const FrozenFormat frozenFormats[] {
    { "era-reverb-sound26", 3503, 26, 25, { "default", "copper-arc", "lumen-field" } },
    { "era-reverb-sound27", 3539, 27, 26, { "default", "copper-arc", "lumen-field", "warm" } }
};
constexpr std::size_t frozenParameters = 3539;

juce::File fixtureDirectory(const FrozenFormat& format) { return juce::File(juce::String(RACKSYNTH_FIXTURE_DIR)).getChildFile(format.directory); }

SoundDocument lumenFieldSound()
{
    const auto designs = airack::test::frozenFactoryRackDesigns();
    const auto found = std::find_if(designs.begin(), designs.end(), [](const auto& d) { return d.name == "Lumen Field"; });
    CHECK(found != designs.end());
    if (found == designs.end()) return copperArcSound();
    RackOperation operation;
    operation.design = *found;
    operation.newInstance = juce::Uuid("2f1d6c5e-8a40-4c55-9a51-3e6e2b7d0e26");
    operation.destination = { RackRole::fx, masterChain, 0 };
    SoundDocument placed;
    RackDifference delta;
    CHECK(makeRackCandidate(copperArcSound(), operation, SoundEditOrigin::manual, placed, delta) == RackCandidateStatus::ready);
    return placed;
}
// The wavetable warmth legacy "rich" sound: slot 2 at revision 2 with every synth.2.wavetable key, the warmth keys
// included, away from its default.
SoundDocument warmSound()
{
    SoundDocument rich;
    RackEdit edit; edit.kind = RackEditKind::addSynth; edit.slot = 1;
    CHECK(editRackLayout(rich.layout, edit, rich.layout) == RackEditResult::applied);
    const auto enabled = rackParameterIndex(NativeModule::wavetable, 1, "enabled"), engine = rackParameterIndex(NativeModule::wavetable, 1, "engine");
    CHECK(enabled && engine);
    if (!enabled || !engine) return rich;
    rich.values[*enabled] = 1; rich.values[*engine] = 1;
    const auto registry = parameterRegistry();
    const auto defaults = defaultParameterValues();
    int warmth = 0;
    for (std::size_t i = 0; i < registry.size(); ++i) {
        if (!registry[i].id.startsWith("synth.2.wavetable.")) continue;
        if (const auto steps = int(registry[i].choices.size()); steps > 1)
            rich.values[i] = float((int(std::lround(defaults[i] * float(steps - 1))) + 1) % steps) / float(steps - 1);
        else rich.values[i] = defaults[i] < .5f ? defaults[i] + .31f : defaults[i] - .29f;
        CHECK(rich.values[i] != defaults[i]);
        warmth += i >= wavetableWarmthParameterStart;
    }
    CHECK(warmth == int(wavetableWarmthParametersPerSlot));
    rich.compatibility.synth[1][1] = modernSynthAlgorithmRevision;
    return rich;
}
SoundDocument frozenDocument(std::string_view name)
{
    return name == "default" ? SoundDocument {} : name == "copper-arc" ? copperArcSound() : name == "lumen-field" ? lumenFieldSound() : warmSound();
}

// Three notes held half a second, two seconds at 48 kHz in 64-sample blocks; the float bits of both channels.
juce::String renderHash(const SoundDocument& sound)
{
    RoutedPatch patch;
    CHECK(makeRoutedPatch(sound, patch));
    RoutedSynthEngine engine;
    CHECK(engine.prepare(48000, 64, RenderQuality::realtime) && engine.setPatchQuiescent(patch));
    engine.reset();
    constexpr int frames = 96000, block = 64, release = 24000;
    std::vector<float> output(std::size_t(frames) * 2);
    juce::AudioBuffer<float> storage(2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start < frames; start += block) {
        midi.clear();
        if (start == 0) for (const int note : { 48, 55, 60 }) midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(100)), 0);
        if (start <= release && release < start + block)
            for (const int note : { 48, 55, 60 }) midi.addEvent(juce::MidiMessage::noteOff(1, note), release - start);
        engine.render(storage, midi, 120.);
        for (int n = 0; n < block; ++n)
            for (int ch = 0; ch < 2; ++ch) output[std::size_t(start + n) * 2 + std::size_t(ch)] = storage.getSample(ch, n);
    }
    return juce::SHA256(output.data(), output.size() * sizeof(float)).toHexString();
}

juce::var registryRows(std::size_t count)
{
    juce::Array<juce::var> rows;
    const auto registry = parameterRegistry();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& d = registry[i];
        auto* row = new juce::DynamicObject();
        row->setProperty("id", d.id); row->setProperty("name", d.name); row->setProperty("unit", d.unit);
        row->setProperty("kind", int(d.kind)); row->setProperty("minimum", d.minimum); row->setProperty("maximum", d.maximum);
        row->setProperty("default", d.defaultValue); row->setProperty("skew", d.skew); row->setProperty("key", juce::int64(d.stateKey));
        juce::Array<juce::var> choices;
        for (const auto& c : d.choices) choices.add(c);
        row->setProperty("choices", choices);
        rows.add(juce::var(row));
    }
    return rows;
}

// Writes the frozen format the tree's writer produces.
int freeze(const juce::File& directory)
{
    const auto format = std::find_if(std::begin(frozenFormats), std::end(frozenFormats), [](const auto& f) {
        return f.parameters == parameterCount && f.sound == currentSoundStateVersion && f.project == currentProjectStateVersion;
    });
    if (format == std::end(frozenFormats)) {
        std::cerr << "--freeze captures a frozen format and only runs on a tree whose writer produces it\n";
        return 2;
    }
    const auto sound = juce::String(format->sound), project = juce::String(format->project);
    CHECK(directory.createDirectory().wasOk());
    auto* renders = new juce::DynamicObject();
    for (const auto* name : format->sounds) {
        const auto document = frozenDocument(name);
        CHECK(validateSoundDocument(document));
        juce::MemoryBlock bytes;
        CHECK(encodeSoundState(document, bytes));
        CHECK(directory.getChildFile(juce::String(name) + "-v" + sound + ".soundstate").replaceWithData(bytes.getData(), bytes.getSize()));
        ProjectDocument wrapped;
        wrapped.sound = document;
        CHECK(encodeProjectState(wrapped, bytes));
        CHECK(directory.getChildFile(juce::String(name) + "-v" + project + ".airackproject").replaceWithData(bytes.getData(), bytes.getSize()));
        renders->setProperty(name, renderHash(document));
    }
    if (juce::String(configuration) == "debug")
        CHECK(directory.getChildFile("registry-v" + sound + ".json").replaceWithText(juce::JSON::toString(registryRows(format->parameters), false)));
    CHECK(directory.getChildFile(juce::String("renders-") + configuration + ".json").replaceWithText(juce::JSON::toString(juce::var(renders), false)));
    std::cout << "Era Reverb freeze (format " << format->sound << ", " << configuration << "): " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

void frozenRegistryPrefix(const FrozenFormat& format)
{
    std::cout << "SECTION frozen_format" << format.sound << "_registry_prefix\n";
    const auto rows = juce::JSON::parse(fixtureDirectory(format).getChildFile("registry-v" + juce::String(format.sound) + ".json"));
    const auto registry = parameterRegistry();
    const auto count = format.parameters;
    CHECK(rows.isArray() && rows.size() == int(count) && registry.size() >= count);
    if (!rows.isArray() || rows.size() != int(count) || registry.size() < count) return;
    for (std::size_t i = 0; i < count; ++i) {
        const auto row = rows[int(i)];
        const auto& d = registry[i];
        CHECK(row["id"].toString() == d.id && row["name"].toString() == d.name && row["unit"].toString() == d.unit);
        CHECK(int(row["kind"]) == int(d.kind) && juce::int64(row["key"]) == juce::int64(d.stateKey));
        CHECK(float(row["minimum"]) == d.minimum && float(row["maximum"]) == d.maximum && float(row["default"]) == d.defaultValue && float(row["skew"]) == d.skew);
        const auto choices = row["choices"];
        CHECK(choices.isArray() && choices.size() == int(d.choices.size()));
        for (int c = 0; c < std::min(choices.size(), int(d.choices.size())); ++c) CHECK(choices[c].toString() == d.choices[std::size_t(c)]);
    }
}

void frozenStatesDecodeAndRenderUnchanged(const FrozenFormat& format)
{
    std::cout << "SECTION frozen_format" << format.sound << "_states_decode_upgrade_and_render_unchanged\n";
    const auto directory = fixtureDirectory(format);
    const auto hashes = juce::JSON::parse(directory.getChildFile(juce::String("renders-") + configuration + ".json"));
    CHECK(hashes.isObject());
    const auto defaults = defaultParameterValues();
    const auto count = format.parameters;
    for (const auto* name : format.sounds) {
        juce::MemoryBlock bytes;
        CHECK(directory.getChildFile(juce::String(name) + "-v" + juce::String(format.sound) + ".soundstate").loadFileAsData(bytes));
        if (bytes.getSize() < 16 + count * 8) continue;
        juce::MemoryInputStream header(bytes, false);
        header.setPosition(4);
        CHECK(header.readInt() == format.sound && header.readInt() == int(count));
        auto decoded = std::make_unique<SoundDocument>();
        const bool read = decodeSoundState(bytes.getData(), bytes.getSize(), *decoded);
        CHECK(read);
        if (!read) continue;
        header.setPosition(16);
        for (std::size_t record = 0; record < count; ++record) {
            const auto key = std::uint32_t(header.readInt());
            const auto value = header.readFloat();
            const auto index = parameterIndexForKey(key);
            CHECK(index && *index < count && decoded->values[*index] == value);
        }
        for (std::size_t i = count; i < parameterCount; ++i) CHECK(decoded->values[i] == defaults[i] && !decoded->controlLocks[i]);
        for (const auto& slot : decoded->compatibility.effects)
            for (std::size_t bank = 17; bank < effectBankCount; ++bank) CHECK(slot[bank] == 1);
        if (std::string_view(name) == "lumen-field") {
            CHECK(decoded->personalities != nullptr);
            bool space = false;
            if (decoded->personalities)
                for (const auto& instance : decoded->personalities->instances)
                    for (const auto& unit : instance.design.units) space = space || unit.module == NativeModule::spaceReverb;
            CHECK(space);
        }
        if (std::string_view(name) == "warm") {
            int moved = 0;
            for (std::size_t i = wavetableWarmthParameterStart; i < wavetableWarmthParameterStart + 4 * wavetableWarmthParametersPerSlot; ++i)
                moved += decoded->values[i] != defaults[i];
            CHECK(moved == int(wavetableWarmthParametersPerSlot) && decoded->compatibility.synth[1][1] == modernSynthAlgorithmRevision);
        }
        CHECK(renderHash(*decoded) == hashes[juce::Identifier(name)].toString());
        juce::MemoryBlock upgraded;
        CHECK(encodeSoundState(*decoded, upgraded));
        juce::MemoryInputStream modern(upgraded, false);
        modern.setPosition(4);
        CHECK(modern.readInt() == currentSoundStateVersion && modern.readInt() == int(parameterCount));
        auto again = std::make_unique<SoundDocument>();
        CHECK(decodeSoundState(upgraded.getData(), upgraded.getSize(), *again) && *again == *decoded);

        juce::MemoryBlock projectBytes;
        CHECK(directory.getChildFile(juce::String(name) + "-v" + juce::String(format.project) + ".airackproject").loadFileAsData(projectBytes));
        if (projectBytes.getSize() < 8) continue;
        CHECK(static_cast<const unsigned char*>(projectBytes.getData())[4] == format.project);
        auto project = std::make_unique<ProjectDocument>();
        CHECK(decodeProjectState(projectBytes.getData(), projectBytes.getSize(), *project) && project->sound == *decoded);
        juce::MemoryBlock projectAgain;
        CHECK(encodeProjectState(*project, projectAgain));
        CHECK(static_cast<const unsigned char*>(projectAgain.getData())[4] == currentProjectStateVersion);
        auto reread = std::make_unique<ProjectDocument>();
        CHECK(decodeProjectState(projectAgain.getData(), projectAgain.getSize(), *reread) && *reread == *project);
    }
}

struct EraField { const char* key; float minimum, maximum, initial, skew; int choices; };
constexpr EraField eraFields[] {
    { "voicing", 0, 5, 0, 1, 6 }, { "age", 0, 2, 0, 1, 3 }, { "decay_seconds", .2f, 70, 4, .2977f, 0 },
    { "predelay_ms", 0, 500, 20, .4307f, 0 }, { "size", 0, 1, 1, 1, 0 }, { "attack", 0, 1, .5f, 1, 0 },
    { "diffusion", 0, 1, 1, 1, 0 }, { "wobble", 0, 1, .38f, 1, 0 }, { "wobble_rate_hz", .1f, 10, 2.53f, 1, 0 },
    { "grit", 0, 1, 0, 1, 0 }, { "bass_multiplier", .25f, 4, 1.5f, .4307f, 0 }, { "damping_hz", 100, 20000, 6000, .5701f, 0 },
    { "low_cut_hz", 10, 1500, 10, 1, 0 }, { "high_cut_hz", 100, 20000, 8000, .5701f, 0 }, { "mix", 0, 1, .25f, 1, 0 }
};
void setEra(SoundDocument& sound, std::uint8_t slot, const char* key, float physical)
{
    const auto index = rackParameterIndex(NativeModule::eraReverb, slot, juce::String("era_reverb.") + key);
    CHECK(index.has_value());
    if (!index) return;
    const auto& d = parameterRegistry()[*index];
    sound.values[*index] = juce::NormalisableRange<float>(d.minimum, d.maximum, 0, d.skew).convertTo0to1(physical);
}
SoundDocument eraSound()
{
    SoundDocument sound;
    RackEdit edit;
    edit.kind = RackEditKind::addEffect; edit.slot = 2; edit.destination = masterChain; edit.position = 0; edit.effect = EffectKind::eraReverb;
    CHECK(editRackLayout(sound.layout, edit, sound.layout) == RackEditResult::applied);
    setEra(sound, 2, "voicing", 1); setEra(sound, 2, "age", 0); setEra(sound, 2, "decay_seconds", 8); setEra(sound, 2, "predelay_ms", 25);
    setEra(sound, 2, "size", .85f); setEra(sound, 2, "attack", .6f); setEra(sound, 2, "diffusion", 1); setEra(sound, 2, "wobble", .4f);
    setEra(sound, 2, "wobble_rate_hz", .7f); setEra(sound, 2, "grit", .15f); setEra(sound, 2, "bass_multiplier", 1.2f);
    setEra(sound, 2, "damping_hz", 5000); setEra(sound, 2, "low_cut_hz", 150); setEra(sound, 2, "high_cut_hz", 9000); setEra(sound, 2, "mix", .3f);
    return sound;
}
bool near(float a, float b) { return std::abs(a - b) <= 2e-4f * std::max(1.f, std::abs(b)); }
void resign(juce::MemoryBlock& bytes)
{
    const auto digest = juce::SHA256(bytes.getData(), bytes.getSize() - 32).getRawData();
    std::memcpy(static_cast<char*>(bytes.getData()) + bytes.getSize() - 32, digest.getData(), 32);
}

void registryIdentityAndState()
{
    std::cout << "SECTION era_reverb_registry_identity_mapping_and_state28\n";
    CHECK(eraReverbParameterStart == frozenParameters && eraReverbParametersPerSlot == 15 && parameterCount == 4519);
    CHECK(int(EffectKind::eraReverb) == 17 && eraReverbEffectBank == 17 && effectBankCount == 19);
    CHECK(rackUnitParameterCapacity(NativeModule::eraReverb) == 16 && rackUnitParameterCapacity(NativeModule(22)) == 0);
    CHECK(moduleDisplayName(NativeModule::eraReverb) == "Era Reverb" && effectDisplayName(EffectKind::eraReverb) == "Era Reverb");
    CHECK(rackParameterActive(NativeModule::eraReverb, "bypassed") && rackParameterActive(NativeModule::eraReverb, "era_reverb.mix"));
    CHECK(!rackParameterActive(NativeModule::eraReverb, "space_reverb.mix"));
    const auto registry = parameterRegistry();
    for (std::uint8_t slot = 0; slot < 8; ++slot)
        for (std::size_t field = 0; field < std::size(eraFields); ++field) {
            const auto& f = eraFields[field];
            const auto index = eraReverbParameterStart + slot * eraReverbParametersPerSlot + field;
            const auto& d = registry[index];
            CHECK(d.id == "effect." + juce::String(slot + 1) + ".era_reverb." + f.key);
            CHECK(d.minimum == f.minimum && d.maximum == f.maximum && d.defaultValue == f.initial && std::abs(d.skew - f.skew) < 1e-4f);
            CHECK(int(d.choices.size()) == (f.choices > 0 ? f.choices : 0));
            CHECK(rackParameterIndex(NativeModule::eraReverb, slot, juce::String("era_reverb.") + f.key) == std::optional<std::size_t>(index));
        }
    CHECK(registry[eraReverbParameterStart].choices == std::vector<juce::String>({ "Hall", "Space", "Tape", "Plate", "Room", "Gate" }));
    CHECK(registry[eraReverbParameterStart + 1].choices == std::vector<juce::String>({ "Dark", "Warm", "Clear" }));

    const auto sound = eraSound();
    CHECK(validateSoundDocument(sound));
    RoutedPatch patch;
    CHECK(makeRoutedPatch(sound, patch));
    const auto& e = patch.effects[2].eraReverb;
    CHECK(e.voicing == EraVoicing::space && e.age == EraAge::dark);
    CHECK(near(e.decaySeconds, 8) && near(e.predelayMs, 25) && near(e.size, .85f) && near(e.attack, .6f) && near(e.diffusion, 1));
    CHECK(near(e.wobble, .4f) && near(e.wobbleRateHz, .7f) && near(e.grit, .15f) && near(e.bassMultiplier, 1.2f));
    CHECK(near(e.dampingHz, 5000) && near(e.lowCutHz, 150) && near(e.highCutHz, 9000) && near(e.mix, .3f));
    const auto& d0 = patch.effects[0].eraReverb;
    const EraReverbSettings initial;
    CHECK(d0.voicing == initial.voicing && d0.age == initial.age && near(d0.decaySeconds, initial.decaySeconds) && near(d0.predelayMs, initial.predelayMs));
    CHECK(near(d0.size, initial.size) && near(d0.attack, initial.attack) && near(d0.diffusion, initial.diffusion) && near(d0.wobble, initial.wobble));
    CHECK(near(d0.wobbleRateHz, initial.wobbleRateHz) && near(d0.grit, initial.grit) && near(d0.bassMultiplier, initial.bassMultiplier));
    CHECK(near(d0.dampingHz, initial.dampingHz) && near(d0.lowCutHz, initial.lowCutHz) && near(d0.highCutHz, initial.highCutHz) && near(d0.mix, initial.mix));

    juce::MemoryBlock bytes;
    CHECK(encodeSoundState(sound, bytes));
    juce::MemoryInputStream header(bytes, false);
    header.setPosition(4);
    CHECK(header.readInt() == 31 && header.readInt() == 4519 && bytes.getSize() == 41053 && currentSoundStateSize == 41053);
    auto decoded = std::make_unique<SoundDocument>();
    CHECK(decodeSoundState(bytes.getData(), bytes.getSize(), *decoded) && *decoded == sound);
    ProjectDocument project;
    project.sound = sound;
    CHECK(encodeProjectState(project, bytes));
    CHECK(static_cast<const unsigned char*>(bytes.getData())[4] == 30 && bytes.getSize() == 41161 && currentMinimumProjectStateBytes == 41161);
    auto reread = std::make_unique<ProjectDocument>();
    CHECK(decodeProjectState(bytes.getData(), bytes.getSize(), *reread) && *reread == project);

    for (const auto& format : frozenFormats) {
        juce::MemoryBlock old;
        CHECK(fixtureDirectory(format).getChildFile("default-v" + juce::String(format.sound) + ".soundstate").loadFileAsData(old));
        const auto kind = 16 + format.parameters * 8 + 10;
        if (old.getSize() <= kind) continue;
        static_cast<unsigned char*>(old.getData())[kind] = 17;
        resign(old);
        auto before = std::make_unique<SoundDocument>(*decoded);
        CHECK(!decodeSoundState(old.getData(), old.getSize(), *before) && *before == *decoded);
        static_cast<unsigned char*>(old.getData())[kind] = 16;
        resign(old);
        CHECK(decodeSoundState(old.getData(), old.getSize(), *before));
    }
}

RackDesign eraProbe()
{
    RackDesign d;
    d.id = juce::Uuid("5b0e8c1a-7d2f-4e3a-9b6c-1f2e3d4c5b6a");
    d.name = "Era probe";
    d.intent = "Era Reverb catalog probe";
    d.role = RackRole::fx;
    d.units = { { 0, NativeModule::eraReverb, { { 0, "era_reverb.voicing", .4f }, { 0, "era_reverb.mix", .3f } }, 1 } };
    d.panel.sections = { "Era" };
    d.panel.controls = { { "Mix", 0, 0, "era_reverb.mix", FocusWidget::rotary, FocusResponse::linear, 0, 1 } };
    return d;
}

void catalogProposalsAndWire()
{
    std::cout << "SECTION era_reverb_catalog_proposals_and_wire\n";
    using namespace ai;
    const auto design = eraProbe();
    CHECK(validateRackDesign(design, RackValidationMode::portableDesign));
    const ProjectDocument original;
    juce::var catalog, provider;
    CHECK(catalogFor(original, catalog) && providerCatalogFor(original, provider));
    const auto* modules = catalog["modules"].getArray();
    CHECK(modules != nullptr && modules->contains("era_reverb"));
    const auto* nativeModules = provider["projection_enums"]["native_module"].getArray();
    CHECK(nativeModules != nullptr && nativeModules->size() == 22 && (*nativeModules)[19] == juce::var("era_reverb")
          && (*nativeModules)[20].isVoid() && (*nativeModules)[21] == juce::var("saturation"));
    const auto* kinds = provider["projection_enums"]["effect_kind"].getArray();
    CHECK(kinds != nullptr && kinds->size() == 19 && (*kinds)[17] == juce::var("era_reverb")
          && (*kinds)[18] == juce::var("saturation"));
    CHECK(provider["module_keys"]["era_reverb"].size() == int(rackUnitParameterCapacity(NativeModule::eraReverb)));

    RackCatalogSnapshot snapshot;
    CHECK(captureRackCatalog(std::vector<RackDesign> { design }, {}, {}, snapshot));
    if (snapshot.entries.size() != 1) { CHECK(false); return; }
    juce::var inventory;
    CHECK(catalogInventoryFor(snapshot, inventory));
    GenerationRequest request;
    request.tag = { juce::Uuid(), 61, 3, 2 };
    request.model = "fixture-model";
    request.instruction = "Add a dark hall";
    auto authority = std::make_shared<CatalogAuthority>();
    authority->root = request.tag;
    authority->snapshot = snapshot.id;
    authority->descriptorJson = juce::JSON::toString(inventory, true, 22).toStdString();
    const auto& entry = snapshot.entries.front();
    CatalogDefinition definition { design.id, design.revision, entry.contentSha256, {} };
    CHECK(encodeRackDesign(design, definition.canonical));
    authority->definitions.push_back(definition);
    CHECK(ai::detail::catalogAuthorityValid(*authority));

    const ProposalPolicy policy;
    ProposalEdits selection;
    selection.catalogSelections = { { 7, design.id, design.revision, entry.contentSha256, { RackRole::fx, masterChain, 0 } } };
    selection.catalogParameters = { { 7, 0, "era_reverb.mix", .5f } };
    auto placed = std::make_unique<ProjectDocument>();
    CHECK(admitProposal(original, policy, selection, *authority, *placed) == ProposalStatus::accepted);
    std::optional<std::uint8_t> slot;
    for (std::uint8_t s = 0; s < 8; ++s)
        if (placed->sound.layout.effects[s].use == SlotUse::present && placed->sound.layout.effects[s].kind == EffectKind::eraReverb) slot = s;
    CHECK(slot.has_value());
    if (slot) {
        const auto mix = rackParameterIndex(NativeModule::eraReverb, *slot, "era_reverb.mix");
        CHECK(mix && near(placed->sound.values[*mix], .5f));
    }

    ProposalEdits decoded;
    auto json = juce::JSON::parse(R"({"version":1,"parameter_edits":[],"rack_edits":[{"kind":"add_effect","slot":1,"chain":4,"position":0,"effect":"era_reverb"}],)"
                                  R"("macro_edits":[],"pattern":null,"characteristics":"dark","explanation":"era"})");
    const auto text = juce::JSON::toString(json, true).toStdString();
    CHECK(decodeProposal(text, decoded) && decoded.racks.size() == 1 && decoded.racks.front().effect == EffectKind::eraReverb);
    // Like every other kind, the AI reaches Era Reverb through catalog racks, not bare slot edits.
    auto added = std::make_unique<ProjectDocument>();
    CHECK(admitProposal(original, policy, decoded, *added) == ProposalStatus::outOfReach);
    CHECK(juce::JSON::toString(proposalSchema()).contains("\"era_reverb\""));
    auto beyond = juce::String(text).replace("\"era_reverb\"", "\"era_reverb_2\"").toStdString();
    ProposalEdits rejected;
    CHECK(!decodeProposal(beyond, rejected));

    CatalogRequestContext context;
    context.root = request.tag;
    context.authority = authority;
    context.baseIntent.racks = { { RackEditKind::addEffect, 1, masterChain, 0, EffectKind::eraReverb, false } };
    request.catalog = context;
    juce::MemoryBlock wire;
    CHECK(companion::encodeRequest(request, wire));
    const companion::WireBytes wireBytes { static_cast<const std::byte*>(wire.getData()), wire.getSize() };
    auto read = std::make_unique<GenerationRequest>();
    CHECK(companion::decodeRequest(wireBytes, *read) && read->catalog && read->catalog->baseIntent.racks.size() == 1
          && read->catalog->baseIntent.racks.front().effect == EffectKind::eraReverb);
    auto saturation = std::make_unique<GenerationRequest>(request);
    saturation->catalog->baseIntent.racks.front().effect = EffectKind::saturation;
    CHECK(int(saturation->catalog->baseIntent.racks.front().effect) == 18);
    juce::MemoryBlock saturationWire;
    CHECK(companion::encodeRequest(*saturation, saturationWire));
    const companion::WireBytes saturationBytes { static_cast<const std::byte*>(saturationWire.getData()), saturationWire.getSize() };
    auto saturationRead = std::make_unique<GenerationRequest>();
    CHECK(companion::decodeRequest(saturationBytes, *saturationRead) && saturationRead->catalog
          && saturationRead->catalog->baseIntent.racks.size() == 1
          && saturationRead->catalog->baseIntent.racks.front().effect == EffectKind::saturation
          && int(saturationRead->catalog->baseIntent.racks.front().effect) == 18);
    auto invalid = request;
    invalid.catalog->baseIntent.racks.front().effect = EffectKind(19);
    juce::MemoryBlock untouched("keep", 4);
    CHECK(!companion::encodeRequest(invalid, untouched) && untouched == juce::MemoryBlock("keep", 4));
}

void studioPages()
{
    std::cout << "SECTION era_reverb_programmer_and_all_controls_pages\n";
    const auto* model = studio::studioSkin();
    CHECK(model != nullptr);
    if (model == nullptr) return;
    const juce::StringArray mainLabels { "VOICING", "AGE", "DECAY", "PRE-DELAY", "SIZE", "ATTACK", "DIFFUSION", "MIX" };
    const juce::StringArray toneLabels { "WOBBLE", "RATE", "GRIT", "BASS", "DAMPING", "LOW CUT", "HIGH CUT" };
    int slots = 0;
    for (const auto& sub : model->page("effects").subpages) {
        if (!juce::isPositiveAndBelow(sub.effect, 8)) continue;
        ++slots;
        const auto base = int(eraReverbParameterStart + std::size_t(sub.effect) * eraReverbParametersPerSlot);
        std::vector<int> seen(eraReverbParametersPerSlot, 0);
        int mainPage = -1, tonePage = -1;
        for (std::size_t p = 0; p < sub.routes.size(); ++p) {
            const auto& page = sub.routes[p];
            const bool main = page.id == "era_reverb.main", tone = page.id == "era_reverb.tone";
            if (main) mainPage = int(p);
            if (tone) tonePage = int(p);
            juce::StringArray labels, titles;
            for (const auto& row : page.rows)
                for (const auto& section : row) {
                    titles.add(section.title);
                    for (const auto& knob : section.knobs) {
                        const bool era = knob.index >= base && knob.index < base + int(eraReverbParametersPerSlot);
                        CHECK(era == (main || tone));
                        if (era) { ++seen[std::size_t(knob.index - base)]; labels.add(knob.label); }
                    }
                }
            if (main) CHECK(titles == juce::StringArray { "ERA REVERB" } && labels == mainLabels);
            if (tone) CHECK(titles == juce::StringArray { "ERA TONE" } && labels == toneLabels);
        }
        CHECK(mainPage >= 0 && tonePage == mainPage + 1);
        CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
        CHECK(studio::pageOfKind(sub, EffectKind::eraReverb) == mainPage);
        CHECK(model->knobLabels[std::size_t(base)] == "VOICING" && model->knobLabels[std::size_t(base + 14)] == "MIX"
              && model->knobLabels[std::size_t(base + 13)] == "HIGH CUT");
    }
    CHECK(slots == 8);
}

struct Render { double rate = 48000; RenderQuality quality = RenderQuality::realtime; int block = 64, frames = 96000; };
// Three notes held 1.5 s. When given, `changed` replaces the patch at frame `changeAt` without a reset.
std::vector<float> renderRouted(const SoundDocument& sound, Render r, const SoundDocument* changed = nullptr, int changeAt = 0)
{
    std::vector<float> output(std::size_t(r.frames) * 2);
    RoutedPatch patch, next;
    CHECK(makeRoutedPatch(sound, patch));
    if (changed) CHECK(makeRoutedPatch(*changed, next));
    RoutedSynthEngine engine;
    const bool ready = engine.prepare(r.rate, r.block, r.quality) && engine.setPatchQuiescent(patch);
    CHECK(ready);
    if (!ready) return output;
    engine.reset();
    const int release = int(r.rate * 1.5);
    juce::AudioBuffer<float> storage(2, r.block);
    juce::MidiBuffer midi;
    for (int start = 0; start < r.frames; start += r.block) {
        const int n = std::min(r.block, r.frames - start);
        if (changed && start <= changeAt && changeAt < start + n) CHECK(engine.setPatch(next));
        midi.clear();
        if (start == 0) for (const int note : { 48, 55, 60 }) midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(90)), 0);
        if (start <= release && release < start + n)
            for (const int note : { 48, 55, 60 }) midi.addEvent(juce::MidiMessage::noteOff(1, note), release - start);
        storage.setSize(2, n, false, false, true);
        engine.render(storage, midi, 120.);
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) output[std::size_t(start + i) * 2 + std::size_t(ch)] = storage.getSample(ch, i);
    }
    return output;
}
SoundDocument withEra(SoundDocument sound, const char* key, float physical) { setEra(sound, 2, key, physical); return sound; }

void routing()
{
    std::cout << "SECTION era_reverb_engine_slot_and_reported_tail\n";
    const auto hall = withEra(eraSound(), "voicing", 0);
    auto none = hall;
    none.layout = SoundDocument {}.layout;
    auto bypassed = hall;
    const auto bypass = rackParameterIndex(NativeModule::eraReverb, 2, "bypassed");
    CHECK(bypass.has_value());
    if (bypass) bypassed.values[*bypass] = 1;
    const Render base;
    const auto wet = renderRouted(hall, base), dry = renderRouted(none, base);
    CHECK(wet != dry);
    CHECK(renderRouted(bypassed, base) == dry);
    CHECK(renderRouted(withEra(hall, "mix", 0), base) == dry);
    for (const int block : { 17, 512 }) {
        auto r = base;
        r.block = block;
        CHECK(renderRouted(hall, r) == wet);
    }

    // Hall to Plate half a second into the held notes. Mix is an equal-power law applied after the wet path, so with
    // the dry term unfaded, the Mix 0.3 render is the cos/sin blend of the Mix 0 and Mix 1 renders of the same switch.
    const auto switched = [&](float mix) {
        const auto from = withEra(hall, "mix", mix), to = withEra(withEra(hall, "voicing", 3), "mix", mix);
        return renderRouted(from, base, &to, 24000);
    };
    const auto blend = switched(.3f), wetOnly = switched(1);
    CHECK(switched(0) == dry);
    CHECK(blend != wet);
    const double c = std::cos(std::numbers::pi / 2 * double(.3f)), s = std::sin(std::numbers::pi / 2 * double(.3f));
    double peak = 0, error = 0;
    for (std::size_t i = 0; i < blend.size(); ++i) {
        peak = std::max(peak, std::abs(double(blend[i])));
        error = std::max(error, std::abs(blend[i] - (c * dry[i] + s * wetOnly[i])));
    }
    std::cout << "SWITCH peak=" << peak << " blend_error=" << error << '\n';
    CHECK(peak > .01 && error <= 1e-5 * peak);

    const std::pair<double, RenderQuality> rates[] { { 32000, RenderQuality::realtime }, { 192000, RenderQuality::higher }, { 384000, RenderQuality::higher } };
    for (const auto& [rate, quality] : rates) {
        const Render r { rate, quality, 256, int(rate / 8) };
        const auto audio = renderRouted(hall, r);
        CHECK(std::all_of(audio.begin(), audio.end(), [](float v) { return std::isfinite(v); }));
        CHECK(audio != renderRouted(none, r));
    }

    const auto rootPath = juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", {});
    const auto root = (rootPath.isEmpty() ? juce::File::getSpecialLocation(juce::File::tempDirectory) : juce::File(rootPath))
                          .getChildFile("era-reverb-tail-" + juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());
    companion::ClientOptions offline;
    offline.allowLaunch = false;
    offline.launch.pipeSuffix = juce::Uuid().toString().removeCharacters("-");
    offline.launch.fixtureRoot = root;
    auto processor = std::make_unique<InstrumentProcessor>(offline);
    juce::MemoryBlock bytes;
    CHECK(encodeSoundState(hall, bytes));
    CHECK(processor->restoreSoundState(bytes.getData(), int(bytes.getSize())));
    processor->prepareToPlay(48000, 64);
    RoutedPatch patch;
    CHECK(makeRoutedPatch(hall, patch));
    double release = 0;
    for (std::size_t i = 0; i < 4; ++i)
        if (patch.layout.synthUse[i] == SlotUse::present && patch.synth.layers[i].enabled && !patch.mute[i])
            release = std::max(release, double(patch.synth.layers[i].ampEnvelope.release));
    const double expected = release + eraReverbTailSeconds(patch.effects[2].eraReverb) + .01;
    std::cout << "TAIL host_seconds=" << processor->getTailLengthSeconds() << " expected=" << expected << '\n';
    CHECK(std::abs(processor->getTailLengthSeconds() - expected) <= 1e-9);
    processor->releaseResources();
    processor.reset();
    root.deleteRecursively();
}
}

int main(int argc, char** argv)
{
    const std::string_view section = argc > 1 ? argv[1] : "";
    if (section == "--freeze" && argc == 3) return freeze(juce::File(juce::String(argv[2])));
    constexpr std::string_view sections[] { "--legacy", "--state", "--catalog", "--studio", "--routing" };
    if (!section.empty() && std::find(std::begin(sections), std::end(sections), section) == std::end(sections)) {
        std::cerr << "Usage: RackSynthEraReverbProductTests [--legacy|--state|--catalog|--studio|--routing] | --freeze ABSOLUTE_DIR\n";
        return 2;
    }
    const auto run = [&](std::string_view name) { return section.empty() || section == name; };
    if (run("--legacy")) for (const auto& format : frozenFormats) { frozenRegistryPrefix(format); frozenStatesDecodeAndRenderUnchanged(format); }
    if (run("--state")) registryIdentityAndState();
    if (run("--catalog")) catalogProposalsAndWire();
    if (run("--studio")) { juce::ScopedJuceInitialiser_GUI gui; studioPages(); }
    if (run("--routing")) { juce::ScopedJuceInitialiser_GUI gui; routing(); }
    std::cout << "Era Reverb product: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
