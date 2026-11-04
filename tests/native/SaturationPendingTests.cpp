#include "SoundState.h"
#include <bit>
#include <cmath>
#include <iostream>

namespace {
using namespace airack;
std::size_t checks=0,failures=0;
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL:"<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
constexpr auto pending=EffectKind::saturation;
bool place(SoundAudioState& sound,std::uint8_t slot){
    RackEdit edit;edit.kind=RackEditKind::addEffect;edit.slot=slot;edit.effect=pending;edit.destination=masterChain;edit.position=sound.layout.effectCount[masterChain];
    const auto result=editRackLayout(sound.layout,edit,sound.layout);CHECK(result==RackEditResult::applied);return result==RackEditResult::applied;
}
void retentionAndHost(){
    CHECK(parameterCount==4519);
    if(parameterCount!=4519)return;
    const auto registry=parameterRegistry();
    for(std::uint8_t slot=0;slot<8;++slot){
        SoundDocument sound;if(!place(sound,slot))continue;
        for(std::size_t field=0;field<35;++field){const auto i=saturationParameterStart+slot*35+field;const auto& d=registry[i];sound.values[i]=d.choices.empty()?.73f:1.f;}
        for(float raw:{0.f,1.f}){
            sound.values[572+slot*23]=raw;sound.values[756]=.25f;
            const auto before=sound;RoutedPatch patch;CHECK(validateSoundDocument(sound));CHECK(makeRoutedPatch(sound,patch));CHECK(patch.effects[slot].bypassed==(raw==1));CHECK(sound==before);
            CHECK(patch.outputGainDb==parameterPhysicalValue(sound.values,756));
            SoundAudioState host=sound;CHECK(makeRoutedPatch(host,patch));CHECK(patch.effects[slot].bypassed==(raw==1));CHECK(host==static_cast<const SoundAudioState&>(before));
            juce::MemoryBlock bytes;SoundDocument recalled;CHECK(encodeSoundState(sound,bytes));CHECK(decodeSoundState(bytes.getData(),bytes.getSize(),recalled)&&recalled==sound);CHECK(recalled.values[572+slot*23]==raw);
            // Continuous macro edits still publish unrelated changes while requested bypass stays retained.
            sound.macros[0]={registry[saturationParameterStart+slot*35+13].stateKey,.5f};sound.values[1]=1;
            sound.macros[1]={registry[756].stateKey,.25f};sound.values[2]=1;
            const auto modulated=sound;CHECK(makeRoutedPatch(sound,patch));CHECK(patch.effects[slot].bypassed==(raw==1));CHECK(sound==modulated);CHECK(patch.outputGainDb>parameterPhysicalValue(sound.values,756));
            sound.macros={};
        }
    }
}
void directAdmission(){
    for(std::uint8_t slot=0;slot<8;++slot){
        SoundAudioState sound;if(!place(sound,slot))continue;RoutedPatch patch;CHECK(makeRoutedPatch(sound,patch));
        RoutedSynthEngine engine;CHECK(engine.setPatch(patch));const auto before=engine.patch();
        patch.effects[slot].bypassed=false;CHECK(validateRoutedPatch(patch));CHECK(engine.setPatch(patch));CHECK(!engine.patch().effects[slot].bypassed);
        auto invalid=patch;invalid.effects[slot].saturation.lanes[3].mix=2;CHECK(!validateRoutedPatch(invalid));CHECK(!engine.setPatch(invalid));CHECK(engine.patch()==before);
        patch.effects[slot].bypassed=true;CHECK(validateRoutedPatch(patch));CHECK(engine.setPatch(patch));
        // An absent slot has no audio path; retained metadata does not block unrelated patches.
        RackEdit remove;remove.kind=RackEditKind::removeEffect;remove.slot=slot;CHECK(editRackLayout(patch.layout,remove,patch.layout)==RackEditResult::applied);
        patch.effects[slot].bypassed=false;CHECK(validateRoutedPatch(patch));CHECK(engine.setPatch(patch));
    }
    SoundAudioState old;RackEdit add;add.kind=RackEditKind::addEffect;add.slot=7;add.effect=EffectKind::drive;CHECK(editRackLayout(old.layout,add,old.layout)==RackEditResult::applied);
    RoutedPatch patch;CHECK(makeRoutedPatch(old,patch));CHECK(!patch.effects[7].bypassed);CHECK(validateRoutedPatch(patch));
}
void identicalAudio(){
    SoundAudioState sound;RoutedPatch dry;CHECK(makeRoutedPatch(sound,dry));
    for(std::uint8_t slot=0;slot<8;++slot)if(!place(sound,slot))return;
    for(std::uint8_t slot=0;slot<8;++slot)sound.values[572+slot*23]=1;
    RoutedPatch pendingPatch;CHECK(makeRoutedPatch(sound,pendingPatch));for(const auto& e:pendingPatch.effects)CHECK(e.bypassed);
    RoutedSynthEngine reference,staged;CHECK(reference.setPatch(dry)&&staged.setPatch(pendingPatch));CHECK(reference.prepare(48000,64)&&staged.prepare(48000,64));reference.reset();staged.reset();
    juce::AudioBuffer<float> a(2,64),b(2,64);juce::MidiBuffer midi;midi.ensureSize(256);double peak=0;
    for(int first=0;first<2048;first+=64){midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);if(first==1024)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
        reference.render(a,midi,120.);staged.render(b,midi,120.);
        for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n){peak=std::max(peak,std::abs(double(a.getSample(ch,n))));CHECK(std::bit_cast<std::uint32_t>(a.getSample(ch,n))==std::bit_cast<std::uint32_t>(b.getSample(ch,n)));}}
    CHECK(peak>1e-6);
}
}
int main(){retentionAndHost();directAdmission();identicalAudio();std::cout<<"SATURATION_PENDING checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
