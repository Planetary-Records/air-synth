#include "FrozenRackExamples.h"
// Public product witness: unchanged across model-only RED and routed-runtime GREEN.
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include "RoutedSynthEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace airack;
using Audio=std::array<std::vector<float>,2>;
int checks=0,failures=0;
void check(bool value,const char* expression,int line){++checks;if(!value){++failures;std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
Audio render(RoutedSynthEngine& engine){
    constexpr int frames=32768,block=512;
    Audio result;for(auto& channel:result)channel.resize(frames);
    juce::AudioBuffer<float> buffer(2,block);juce::MidiBuffer midi;
    for(int first=0;first<frames;first+=block){
        midi.clear();if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        if(first==2048)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
        engine.render(buffer,midi,120.);
        for(int channel=0;channel<2;++channel)std::copy_n(buffer.getReadPointer(channel),block,result[std::size_t(channel)].data()+first);
    }
    return result;
}
bool bits(const Audio& a,const Audio& b){for(std::size_t channel=0;channel<2;++channel)if(std::memcmp(a[channel].data(),b[channel].data(),a[channel].size()*sizeof(float))!=0)return false;return true;}
double energy(const Audio& audio,std::size_t first=0){double result=0;for(const auto& channel:audio)for(std::size_t n=first;n<channel.size();++n){CHECK(std::isfinite(channel[n]));result+=double(channel[n])*channel[n];}return result;}
double residual(const Audio& a,const Audio& b,std::size_t first=0){double result=0;for(std::size_t channel=0;channel<2;++channel)for(std::size_t n=first;n<a[channel].size();++n){const double delta=double(a[channel][n])-b[channel][n];result+=delta*delta;}return result;}
void mappedFactoryProducesWetAudio(){
    const auto designs=airack::test::frozenFactoryRackDesigns();
    const auto found=std::find_if(designs.begin(),designs.end(),[](const auto& design){return design.name=="Lumen Field";});
    CHECK(found!=designs.end());if(found==designs.end())return;
    SoundDocument original,placed;RackOperation operation;operation.design=*found;operation.newInstance=juce::Uuid();
    operation.destination={RackRole::fx,masterChain,0};operation.allocation={{0,0}};RackDifference difference;
    const auto status=makeRackCandidate(original,operation,SoundEditOrigin::manual,placed,difference);
    CHECK(status==RackCandidateStatus::ready);if(status!=RackCandidateStatus::ready)return;
    RoutedPatch wet,dryReference;CHECK(makeRoutedPatch(placed,wet));CHECK(makeRoutedPatch(original,dryReference));
    CHECK(wet.layout.effects[0].use==SlotUse::present&&wet.layout.effects[0].kind==EffectKind::spaceReverb);
    CHECK(wet.effects[0].spaceReverb.mix>0&&wet.effects[0].spaceReverb.mix<1);
    CHECK(wet.synth==dryReference.synth);auto mixZero=wet;mixZero.effects[0].spaceReverb.mix=0;
    RoutedSynthEngine actual,zero,reference;
    const bool configured=actual.setPatch(wet)&&zero.setPatch(mixZero)&&reference.setPatch(dryReference);
    CHECK(configured);if(!configured)return;
    const bool prepared=actual.prepare(48000,512)&&zero.prepare(48000,512)&&reference.prepare(48000,512);
    CHECK(prepared);if(!prepared)return;
    actual.reset();zero.reset();reference.reset();
    const auto audio=render(actual),zeroAudio=render(zero),referenceAudio=render(reference);
    CHECK(energy(referenceAudio)>1e-6);CHECK(energy(audio)>1e-6);
    CHECK(bits(zeroAudio,referenceAudio));
    const double wetResidual=residual(audio,zeroAudio),dryTail=energy(referenceAudio,24576),wetTail=energy(audio,24576);
    // A passthrough or an attenuated dry signal cannot supply a reverb-only tail.
    CHECK(wetResidual>1e-8);CHECK(dryTail<1e-16);CHECK(wetTail>1e-12);
    actual.reset();CHECK(bits(render(actual),audio));
    std::cout<<"SPACE_WET_SMOKE wet_residual="<<wetResidual<<" dry_tail="<<dryTail<<" wet_tail="<<wetTail<<'\n';
}
}
int main(){mappedFactoryProducesWetAudio();std::cout<<"SPACE_WET_SMOKE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
