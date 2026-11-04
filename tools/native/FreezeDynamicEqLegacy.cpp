#include "ProjectDocument.h"
#include "SoundState.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace airack;
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
void save(const juce::File& directory, const char* name, const juce::MemoryBlock& bytes)
{
    const auto file = directory.getChildFile(name);
    require(!file.exists(), "Frozen fixture already exists; refusing overwrite");
    require(file.replaceWithData(bytes.getData(),bytes.getSize()), "Cannot write fixture");
    std::cout << name << " bytes=" << bytes.getSize() << " sha256=" << juce::SHA256(bytes).toHexString() << '\n';
}
void saveSound(const juce::File& directory, const char* name, const SoundDocument& document)
{
    juce::MemoryBlock bytes; SoundDocument decoded;
    require(encodeSoundState(document,bytes), "Old sound encode failed");
    juce::MemoryInputStream header(bytes,false);
    require(header.readInt()==0x49535241 && header.readInt()==16 && header.readInt()==1231, "Capture requires actual Sound16/1231 encoder");
    require(decodeSoundState(bytes.getData(),bytes.getSize(),decoded) && decoded==document, "Old sound roundtrip differs");
    save(directory,name,bytes);
}
}
int main(int argc, char** argv)
{
    try {
        require(argc==2, "Usage: RackSynthFreezeDynamicEqLegacy ABSOLUTE_EMPTY_OUTPUT_DIRECTORY");
        require(parameterCount==1231 && effectBankCount==15, "Legacy capture only runs before the append");
        const juce::File directory(juce::String::fromUTF8(argv[1]));
        require(directory.isAbsolutePath(directory.getFullPathName()) && directory.createDirectory().wasOk(), "Invalid output directory");
        require(directory.getNumberOfChildFiles(juce::File::findFilesAndDirectories)==0, "Capture directory must be empty");
        saveSound(directory,"default-v16.soundstate",SoundDocument{});
        auto rich = copperArcSound(); rich.personalities.reset();
        rich.compatibility = newInstanceKernelCompatibility();
        const auto registry = parameterRegistry();
        for (std::size_t i = eqParameterStart; i < parameterCount; ++i) {
            const auto& d = registry[i];
            rich.values[i] = d.kind == ParameterKind::continuous ? float(i%7)/6.f
                : float(i % d.choices.size())/float(d.choices.size()-1);
        }
        const std::array<EffectKind,8> kinds {EffectKind::drive,EffectKind::delay,EffectKind::reverb,EffectKind::compressor,
            EffectKind::eq,EffectKind::filter,EffectKind::frequencySplit,EffectKind::midSideSplit};
        for (std::uint8_t slot = 0; slot < 8; ++slot) {
            RackEdit edit; edit.kind = RackEditKind::addEffect; edit.slot = slot;
            edit.destination = slot%2 ? masterChain : 0; edit.position = rich.layout.effectCount[edit.destination]; edit.effect = kinds[slot];
            RackLayout next; require(editRackLayout(rich.layout,edit,next)==RackEditResult::applied,"Old FX layout rejected"); rich.layout=next;
        }
        rich.controlLocks[parameterCount-1] = true; rich.controlLocks[eqParameterStart+1] = true;
        rich.effectLocks[7] = true; rich.synthLocks[0] = true;
        rich.macros[0] = {registry[midSideSplitParameterStart].stateKey,.125f}; rich.values[1] = .25f;
        require(validateSoundDocument(rich), "Rich old document invalid");
        saveSound(directory,"rich-v16.soundstate",rich);
        auto embedded = copperArcSound(); bool found = false;
        for (const auto& design : factoryRackDesigns()) if (design.units.size()==1 && design.units[0].module==NativeModule::midSideSplit) {
            RackOperation operation; operation.kind=RackOperationKind::place; operation.design=design;
            operation.newInstance=juce::Uuid("71abac19-7e09-4a4e-a683-1900615a0081"); operation.destination={RackRole::fx,masterChain,0};
            SoundDocument next; RackDifference difference;
            require(makeRackCandidate(embedded,operation,SoundEditOrigin::manual,next,difference)==RackCandidateStatus::ready,"Old embedded terminal module rejected");
            embedded=std::move(next); found=true; break;
        }
        require(found,"Missing old terminal factory module"); saveSound(directory,"embedded-v16.soundstate",embedded);
        ProjectDocument project; project.sound=rich;
        juce::MemoryBlock projectBytes; ProjectDocument recalled;
        require(encodeProjectState(project,projectBytes),"Old project encode failed");
        juce::MemoryInputStream projectHeader(projectBytes,false); projectHeader.skipNextBytes(4);
        require(projectHeader.readShort()==15 && decodeProjectState(projectBytes.getData(),projectBytes.getSize(),recalled) && recalled==project,"Capture requires Project15 exact roundtrip");
        save(directory,"rich-v15.airackproject",projectBytes);
        juce::Array<juce::var> catalog;
        for (const auto& d : registry) {
            auto* row = new juce::DynamicObject; row->setProperty("id",d.id); row->setProperty("name",d.name); row->setProperty("unit",d.unit);
            row->setProperty("kind",int(d.kind)); row->setProperty("minimum",d.minimum); row->setProperty("maximum",d.maximum);
            row->setProperty("default",d.defaultValue); row->setProperty("skew",d.skew); row->setProperty("key",juce::int64(d.stateKey));
            juce::Array<juce::var> choices; for (const auto& choice : d.choices) choices.add(choice); row->setProperty("choices",choices); catalog.add(row);
        }
        const auto json = juce::JSON::toString(catalog,false,17);
        save(directory,"registry-v16.json",juce::MemoryBlock(json.toRawUTF8(),json.getNumBytesAsUTF8()));
        RoutedPatch patch; require(makeRoutedPatch(rich,patch),"Old carrier mapping failed");
        RoutedSynthEngine engine; require(engine.prepare(48000,512) && engine.setPatch(patch),"Old carrier preparation failed"); engine.reset();
        juce::MemoryBlock audio; juce::MemoryOutputStream stream(audio,false);
        constexpr int frames=12032, block=64;
        stream.writeInt(0x30465241); stream.writeInt(48000); stream.writeInt(frames); stream.writeInt(block);
        juce::AudioBuffer<float> buffer(2,block); juce::MidiBuffer midi; midi.ensureSize(256);
        double peak=0;
        for (int start=0; start<frames; start+=block) {
            midi.clear(); if(start==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
            if(start==4096)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
            engine.render(buffer,midi,120.);
            for(int n=0;n<block;++n)for(int ch=0;ch<2;++ch){const auto x=buffer.getSample(ch,n);require(std::isfinite(x),"Old raw carrier nonfinite");peak=std::max(peak,std::abs(double(x)));stream.writeFloat(x);}
        }
        require(peak>1e-6,"Old carrier must sound"); stream.flush(); save(directory,"rich-v16-48k-64.raw",audio);
        std::cout << "Legacy actual capture complete, peak=" << peak << '\n'; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
