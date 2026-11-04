#include "FrozenRackExamples.h"
#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include <bit>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <juce_cryptography/juce_cryptography.h>
#include <set>
using namespace airack;
namespace
{
constexpr std::size_t expectedSaturationStart=4239, expectedSaturationBank=18;
int checks = 0, failures = 0;
void check(bool b, const char *m)
{
    ++checks;
    if (!b)
    {
        ++failures;
        std::cerr << "FAIL " << m << '\n';
    }
}
juce::MemoryBlock load(const juce::File &d, const juce::String &n)
{
    juce::MemoryBlock b;
    check(d.getChildFile(n).loadFileAsData(b), "fixture load");
    return b;
}
void rehash(juce::MemoryBlock &b)
{
    auto hash = juce::SHA256(b.getData(), b.getSize() - 32).getRawData();
    std::memcpy(static_cast<char *>(b.getData()) + b.getSize() - 32, hash.getData(), 32);
}
void intAt(juce::MemoryBlock &b, int pos, int value)
{
    const auto x = juce::ByteOrder::swapIfBigEndian(static_cast<std::uint32_t>(value));
    std::memcpy(static_cast<char *>(b.getData()) + pos, &x, 4);
}
void shortAt(juce::MemoryBlock &b, int pos, int value)
{
    const auto x = juce::ByteOrder::swapIfBigEndian(static_cast<std::uint16_t>(value));
    std::memcpy(static_cast<char *>(b.getData()) + pos, &x, 2);
}
void rejectSound(juce::MemoryBlock b)
{
    rehash(b);
    SoundDocument out;
    out.values[0] = .123f;
    const auto before = out;
    check(!decodeSoundState(b.getData(), b.getSize(), out) && out == before,
          "forged sound rejects atomically");
}
void rejectProject(juce::MemoryBlock b)
{
    rehash(b);
    ProjectDocument out;
    out.sound.values[0] = .123f;
    const auto before = out;
    check(!decodeProjectState(b.getData(), b.getSize(), out) && out == before,
          "forged project rejects atomically");
}
void compare(const SoundDocument &s, const juce::var &expected, int banks)
{
    check(s.layout.synthCount == 1, "synth count retained");
    std::set<std::size_t> present;
    const auto r = parameterRegistry();
    const auto values = expected["values"];
    for (const auto &row : *values.getArray())
    {
        const auto key = static_cast<std::uint32_t>(juce::int64(row["key"]));
        const auto i = parameterIndexForKey(key);
        check(i.has_value(), "old key retained");
        if (!i)
            continue;
        present.insert(*i);
        check(r[*i].id == row["id"].toString(), "old host ID retained");
        check(s.values[*i] == float(row["value"]), "old value retained by ID");
        check(s.controlLocks[*i] == bool(row["locked"]), "old lock retained by ID");
    }
    const SoundDocument defaults;
    for (std::size_t i = 0; i < parameterCount; ++i)
        if (!present.contains(i))
        {
            check(s.values[i] == defaults.values[i], "new field gets current default");
            check(!s.controlLocks[i], "new field unlocked");
        }
    for (int i = 0; i < 8; ++i)
    {
        check(s.macros[i].targetKey ==
                      static_cast<std::uint32_t>(juce::int64(expected["macros"][i]["key"])) &&
                  s.macros[i].amount == float(expected["macros"][i]["amount"]),
              "macro identity and amount retained");
        check(s.effectLocks[i] == bool(expected["effect_locks"][i]), "effect lock retained");
    }
    for (int i = 0; i < 4; ++i)
        check(s.synthLocks[i] == bool(expected["synth_locks"][i]), "synth lock retained");
    int n = 0;
    const auto revisions = expected["compatibility"];
    for (auto b : s.compatibility.synth)
        for (auto x : b)
            check(x == int(revisions[n++]), "synth revision retained");
    for (auto b : s.compatibility.effects)
    {
        for (int j = 0; j < banks; ++j)
            check(b[banks==17&&j==16?expectedSaturationBank:std::size_t(j)] == int(revisions[n++]), "effect bank revision retained");
        for (std::size_t j = 0; j < effectBankCount; ++j)
            if(j>=std::size_t(banks) || (banks==17&&j==16))
                if(!(banks==17&&j==expectedSaturationBank))check(b[j] == legacyAlgorithmRevision, "new bank native1");
    }
    n = 0;
    auto layout = expected["layout"];
    for (auto x : s.layout.synthUse)
        check(int(x) == int(layout[n++]), "synth use");
    for (auto x : s.layout.synthOrder)
        check(int(x) == int(layout[n++]), "synth order");
    for (auto x : s.layout.effects)
    {
        check(int(x.use) == int(layout[n++]), "effect use");
        const auto oldKind=int(layout[n++]);
        check(int(x.kind) == (banks==17&&oldKind==16?expectedSaturationBank:oldKind), "effect kind");
        check(int(x.chain) == int(layout[n++]), "effect chain");
    }
    for (auto x : s.layout.effectCount)
        check(int(x) == int(layout[n++]), "chain count");
    for (auto b : s.layout.effectOrder)
        for (auto x : b)
            check(int(x) == int(layout[n++]), "effect order");
    const auto personalities = expected["personalities"];
    check((s.personalities ? int(s.personalities->instances.size()) : 0) == personalities.size(),
          "personality count retained");
    if (s.personalities)
        for (int i = 0; i < personalities.size() && i < int(s.personalities->instances.size()); ++i)
        {
            const auto &x = s.personalities->instances[std::size_t(i)];
            check(x.id.toDashedString() == personalities[i]["id"].toString(), "personality UUID");
            check(!x.edited, "personality edit state retained");
            juce::MemoryBlock b;
            check(b.fromBase64Encoding(personalities[i]["design"].toString()), "frozen design");
            RackDesign design;
            check(decodeRackDesign(b.getData(), b.getSize(), design, RackValidationMode::instanceRecall) &&
                      design == x.design,
                  "personality recipe and bindings retained");
            auto bindings = personalities[i]["bindings"];
            check(int(x.bindings.size()) * 2 == bindings.size(), "binding count");
            for (int j = 0; j < int(x.bindings.size()); ++j)
                check(x.bindings[j].unit == int(bindings[j * 2]) &&
                          x.bindings[j].slot == int(bindings[j * 2 + 1]),
                      "physical binding retained");
        }
}
void predecessor(const char *writer, int banks)
{
    const auto d = juce::File(RACKSYNTH_MAIN_SYNC_FIXTURE_DIR).getChildFile(writer);
    auto registry = juce::JSON::parse(d.getChildFile("registry.json"));
    check(registry.isArray(), "frozen registry");
    const auto r = parameterRegistry();
    std::size_t oldIndex = 0;
    for (const auto &old : *registry.getArray())
    {
        const auto i = parameterIndexForKey(static_cast<std::uint32_t>(juce::int64(old["key"])));
        check(i.has_value(), "descriptor key");
        if (!i)
            continue;
        const bool saturationWriter = juce::String(writer).startsWith("saturation");
        const std::size_t oldStart = registry.size() == 2303 ? 2023 : registry.size() == 2471 ? 2191 : 2535;
        const auto expectedIndex = saturationWriter && oldIndex >= oldStart
                                       ? expectedSaturationStart + oldIndex - oldStart : oldIndex;
        check(*i == expectedIndex, "predecessor registry order and saturation suffix retained");
        ++oldIndex;
        const auto &p = r[*i];
        check(p.id == old["id"].toString() && p.name == old["name"].toString() &&
                  p.unit == old["unit"].toString() && p.minimum == float(old["minimum"]) &&
                  p.maximum == float(old["maximum"]) && p.defaultValue == float(old["default"]) &&
                  p.skew == float(old["skew"]) && int(p.kind) == int(old["kind"]),
              "stable descriptor");
        check(int(p.choices.size()) == old["choices"].size(), "choice count");
        for (int j = 0; j < int(p.choices.size()); ++j)
            check(p.choices[j] == old["choices"][j].toString(), "choice identity");
    }
    for (const auto *name : {"default", "rich", "inactive", "embedded"})
    {
        auto b = load(d, juce::String(name) + ".soundstate");
        auto expected = juce::JSON::parse(d.getChildFile(juce::String(name) + ".json"));
        SoundDocument s;
        const bool decoded = decodeSoundState(b.getData(), b.getSize(), s);
        check(decoded, "genuine predecessor sound decodes");
        if (decoded)
        {
            compare(s, expected, banks);
            juce::MemoryBlock current;
            SoundDocument roundtrip;
            check(encodeSoundState(s, current) &&
                      decodeSoundState(current.getData(), current.getSize(), roundtrip) && roundtrip == s,
                  "migrated current roundtrip");
        }
        auto p = load(d, juce::String(name) + ".airackproject");
        ProjectDocument project;
        const bool projectDecoded = decodeProjectState(p.getData(), p.getSize(), project);
        check(projectDecoded, "genuine predecessor project decodes");
        if (projectDecoded)
        {
            compare(project.sound, expected, banks);
            check(project.pattern == PatternDocument{} && project.mode == PlaybackMode::externalMidi,
                  "pattern/mode retained");
            check(project.group.has_value() == (juce::String(name) == "rich"), "group presence retained");
            if (project.group)
                check(project.group->bpm == 123.75 &&
                          project.group->group == juce::Uuid("4ec4d2b1-6517-4563-9023-f69f9623b401") &&
                          project.group->member == juce::Uuid("a3c63713-4294-4af0-bcc9-c6c62775ae47"),
                      "group identities retained");
            if (project.group)
                check(groupTextView(project.group->name) == "Main sync" &&
                          groupTextView(project.group->role) == "Frozen predecessor" &&
                          groupTextView(project.group->key) == "F#" &&
                          groupTextView(project.group->scale) == "minor", "group text retained");
        }
        // A valid size/count must not admit a key, macro or compatibility stride from the other history.
        juce::MemoryInputStream oldHeader(b, false);
        oldHeader.skipNextBytes(8);
        const int oldCount = oldHeader.readInt();
        const auto foreign =
            std::find_if(r.begin(), r.end(),
                         [&](const auto &descriptor)
                         {
                             return descriptor.id == juce::String(banks != 17
                                                                      ? "effect.1.saturation.input_trim_db"
                                                                      : oldCount == 2303 ? "effect.1.delay.saturation"
                                                                                         : "effect.1.delay.ping_pong");
                         });
        check(foreign != r.end(), "foreign domain key exists");
        if (foreign != r.end())
        {
            auto bad = b;
            intAt(bad, 16, static_cast<int>(foreign->stateKey));
            rejectSound(bad);
            bad = b;
            intAt(bad, 16 + oldCount * 8 + 78, static_cast<int>(foreign->stateKey));
            rejectSound(bad);
        }
        auto badLocks = b;
        static_cast<unsigned char *>(badLocks.getData())[16 + oldCount * 8 + 78 + 64] = 2;
        rejectSound(badLocks);
        auto badStride = b;
        static_cast<unsigned char *>(badStride.getData())[16 + oldCount * 9 + 154 + 9] =
            banks == 16 ? 17 : 16;
        rejectSound(badStride);
        for (int v : {19, 20, 21, 22, 23, 24, 29, 30})
        {
            juce::MemoryInputStream h(b, false);
            h.skipNextBytes(4);
            const int actual = h.readInt();
            if (v != actual)
            {
                auto bad = b;
                intAt(bad, 4, v);
                rejectSound(bad);
            }
        }
        for (int count : {2063, 2151, 2191, 2215, 2303, 2471, 2535, 2815, 4239, 4519})
        {
            juce::MemoryInputStream h(b, false);
            h.skipNextBytes(8);
            if (count != h.readInt())
            {
                auto bad = b;
                intAt(bad, 8, count);
                rejectSound(bad);
            }
        }
        for (int v : {18, 19, 20, 21, 22, 23, 28, 29})
        {
            juce::MemoryInputStream h(p, false);
            h.skipNextBytes(4);
            if (v != h.readShort())
            {
                auto bad = p;
                shortAt(bad, 4, v);
                rejectProject(bad);
            }
        }
        auto truncated = b;
        truncated.setSize(b.getSize() - 1);
        rejectSound(truncated);
        auto extra = b;
        extra.append("x", 1);
        rejectSound(extra);
    }
}
void releasedMainFactoriesAndAudio()
{
    const auto root=juce::File(RACKSYNTH_MAIN_SYNC_FIXTURE_DIR);
    const auto oracles=root.getChildFile("main-20261026-oracles");
    const auto factories=airack::test::frozenFactoryRackDesigns();check(factories.size()==21,"main19 plus two saturation factories");
    for(int n=0;n<19&&n<int(factories.size());++n){
        juce::MemoryBlock actual;check(encodeRackDesign(factories[std::size_t(n)],actual),"main factory encodes");
        check(actual==load(oracles,"factory-"+juce::String(n)+".airackrack"),"released factory prefix byte exact");
    }
    for(const auto* name:{"default","rich","inactive","embedded"}){
        const auto input=load(root.getChildFile("main-20261026"),juce::String(name)+".soundstate");
        SoundDocument sound;check(decodeSoundState(input.getData(),input.getSize(),sound),"released main audio input decodes");
        for(const auto quality:{RenderQuality::realtime,RenderQuality::higher}){
            const auto reference=load(oracles,juce::String(name)+(quality==RenderQuality::realtime?"-realtime.f32":"-higher.f32"));
            for(const auto block:{64,256}){
                RoutedPatch patch;check(makeRoutedPatch(sound,patch),"released main patch materializes");
                auto engine=std::make_unique<RoutedSynthEngine>();check(engine->prepare(48000,512,quality)&&engine->setPatch(patch),"released main render prepares");engine->reset();
                juce::MemoryInputStream expected(reference,false);juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(256);
                for(int first=0;first<12288;first+=block){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(first==4096)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);engine->render(audio,midi,120.);
                    for(int frame=0;frame<block;++frame)for(int channel=0;channel<2;++channel)
                        check(std::bit_cast<std::uint32_t>(audio.getSample(channel,frame))==std::bit_cast<std::uint32_t>(expected.readFloat()),"released main audio byte exact");
                }
                check(expected.isExhausted(),"complete released main audio extent");
            }
        }
    }
}
} // namespace
int main()
{
    check(parameterCount == 4519 && expectedSaturationStart == 4239 && currentSoundStateVersion == 31 && currentProjectStateVersion == 30 &&
              effectBankCount == 19,
          "unique combined schema");
    predecessor("saturation", 17);
    predecessor("main", 16);
    predecessor("saturation-20261021", 17);
    predecessor("main-20261021", 16);
    predecessor("saturation-20261026", 17);
    predecessor("main-20261026", 18);
    releasedMainFactoriesAndAudio();
    std::cout << "MainSyncState " << checks << " checks " << failures << " failures\n";
    return failures ? 1 : 0;
}
