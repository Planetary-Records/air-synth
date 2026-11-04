#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <iostream>

using namespace airack;
int main(int argc, char** argv)
{
    if (argc != 2 || parameterCount != 2215 || currentSoundStateVersion != 22
        || currentProjectStateVersion != 21) return 2;
    const juce::File directory(juce::String::fromUTF8(argv[1]));
    if (directory.exists() || !directory.createDirectory().wasOk()) return 3;
    const auto write = [&](const char* name, const juce::MemoryBlock& bytes) {
        const auto file = directory.getChildFile(name);
        if (file.exists() || !file.replaceWithData(bytes.getData(), bytes.getSize())) return false;
        std::cout << name << " bytes=" << bytes.getSize()
                  << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
        return true;
    };
    juce::Array<juce::var> rows;
    for (const auto& d : parameterRegistry()) {
        auto* row = new juce::DynamicObject;
        row->setProperty("id", d.id); row->setProperty("name", d.name); row->setProperty("unit", d.unit);
        row->setProperty("kind", int(d.kind)); row->setProperty("minimum", double(d.minimum));
        row->setProperty("maximum", double(d.maximum)); row->setProperty("default", double(d.defaultValue));
        row->setProperty("skew", double(d.skew)); row->setProperty("key", juce::int64(d.stateKey));
        juce::Array<juce::var> choices; for (const auto& c : d.choices) choices.add(c);
        row->setProperty("choices", choices); rows.add(juce::var(row));
    }
    const auto json = juce::JSON::toString(rows, true, 17);
    if (!write("registry-v22.json", juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()))) return 4;
    SoundDocument empty, rich;
    RackEdit edit; edit.kind = RackEditKind::addEffect; edit.slot = 2; edit.effect = EffectKind::delay;
    if (editRackLayout(rich.layout, edit, rich.layout) != RackEditResult::applied) return 5;
    for (std::size_t i = dynamicEqExtensionParameterStart; i < delayStereoParameterStart; ++i) {
        const auto count = parameterRegistry()[i].choices.size();
        if (count < 2) return 6;
        rich.values[i] = float(1 + i % (count - 1)) / float(count - 1);
        rich.controlLocks[i] = i % 3 == 0;
    }
    for (std::size_t i = delayStereoParameterStart; i < parameterCount; ++i) {
        const auto& d = parameterRegistry()[i];
        rich.values[i] = d.kind == ParameterKind::continuous ? .37f + .01f * float(i % 8)
            : d.choices.size() > 2 ? float(1 + i % (d.choices.size() - 1)) / float(d.choices.size() - 1) : 1.f;
        if (rich.values[i] == empty.values[i]) return 7;
        rich.controlLocks[i] = i % 2 == 0;
    }
    const auto right = rackParameterIndex(NativeModule::delay, 2, "delay.right_ratio");
    const auto feedback = rackParameterIndex(NativeModule::delay, 2, "delay.feedback");
    if (!right || !feedback) return 8;
    rich.values[0] = .3f; rich.values[*feedback] = .71f; rich.effectLocks[2] = true;
    rich.macros[0] = {parameterRegistry()[*right].stateKey, .21f};
    rich.macros[1] = {parameterRegistry()[*feedback].stateKey, -.17f};
    const auto examples = exampleRackDesigns();
    const auto oxide = std::find_if(examples.begin(), examples.end(), [](const auto& d) { return d.name == "Oxide Return"; });
    if (oxide == examples.end() || oxide->units.size() != 1 || oxide->units[0].defaults.size() != 14) return 9;
    RackOperation place; place.kind = RackOperationKind::place; place.design = *oxide;
    place.newInstance = juce::Uuid("6e1d4f8a-3c9b-4d72-af58-b2c7e4d93e1a");
    place.destination = {RackRole::fx, masterChain, 0}; place.allocation = {{0, 0}};
    SoundDocument embedded; RackDifference delta;
    if (makeRackCandidate(rich, place, SoundEditOrigin::manual, embedded, delta) != RackCandidateStatus::ready) return 10;
    juce::MemoryBlock bytes;
    for (const auto& item : std::array<std::pair<const char*, const SoundDocument*>, 3>{{
             {"default-v22.soundstate", &empty}, {"rich-v22.soundstate", &rich}, {"embedded-v22.soundstate", &embedded}}}) {
        SoundDocument decoded;
        if (!encodeSoundState(*item.second, bytes)
            || !decodeSoundState(bytes.getData(), bytes.getSize(), decoded) || decoded != *item.second
            || !write(item.first, bytes)) return 11;
    }
    for (const auto& item : std::array<std::pair<const char*, const SoundDocument*>, 2>{{
             {"rich-v21.airackproject", &rich}, {"embedded-v21.airackproject", &embedded}}}) {
        ProjectDocument project; project.sound = *item.second; project.mode = PlaybackMode::internalPattern;
        project.pattern.noteCount = 2; project.pattern.notes[0] = {0, .125, 65, 91, 1};
        project.pattern.notes[1] = {.25, .25, 72, 83, 2};
        if (item.second == &rich) {
            GroupIntent group; group.group = juce::Uuid("8ca90aee-620f-4c60-88cb-afec77bb7093");
            group.member = juce::Uuid("707662ee-f03f-44d6-b71b-d51a831181d1");
            if (!setGroupText("Accepted stereo", group.name) || !setGroupText("lead", group.role)) return 12;
            project.group = group;
        }
        ProjectDocument decoded;
        if (!encodeProjectState(project, bytes)
            || !decodeProjectState(bytes.getData(), bytes.getSize(), decoded) || decoded != project
            || !write(item.first, bytes)) return 13;
    }
    return 0;
}
