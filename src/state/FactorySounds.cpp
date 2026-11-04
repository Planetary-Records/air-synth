#include "FactorySounds.h"
#include "RackExamples.h"
#include "RackReconciliation.h"
#include "ParameterRegistry.h"
#include <algorithm>
#include <array>

namespace airack {
namespace {
constexpr std::array<FactorySoundInfo,12> sounds {{
    {"Soft Pluck", "A rounded short note for gentle melodic phrases."},
    {"Round Bass", "A warm low voice with a quick start and steady body."},
    {"Hollow Bass", "A darker hollow voice for sparse bass lines."},
    {"Paper Keys", "Soft, lightly struck keys with a short fading tail."},
    {"Slow Glow", "A warm held chord that opens gently."},
    {"Gentle Lead", "A focused sustained melody with a softened top."},
    {"Clear Bell", "A bright fading note with an upper octave."},
    {"Bright Steps", "A crisp short digital pluck for repeating phrases."},
    {"Wide Chord", "A sustained, lightly detuned chord voice."},
    {"Velvet Pad", "A dark slow pad for held notes."},
    {"Glass Pulse", "A hollow digital stab with a little sustain."},
    {"Dusk Brass", "A rounded swelling chord with a firm body."}
}};
struct Settings {
    float cutoff, attack, decay, sustain, release, color, secondLevel, secondTune, level;
};
// Authored here with ordinary controls: first six Copper Arc, last six Prism Relay. No external audio.
constexpr std::array<Settings,12> settings {{
    {1800,.003f,.34f,0,.14f,1,.18f,0,.52f},
    {850,.006f,.18f,.8f,.12f,1,.12f,-12,.56f},
    {1250,.004f,.22f,.65f,.1f,3,.10f,-12,.48f},
    {2600,.008f,.65f,.12f,.32f,1,.21f,12,.50f},
    {2400,.8f,.9f,.8f,1.4f,2,.28f,0,.48f},
    {3800,.018f,.24f,.72f,.2f,2,.16f,0,.50f},
    {11000,.002f,.85f,0,.65f,.82f,.18f,12,.38f},
    {6200,.002f,.22f,0,.12f,.72f,.12f,0,.40f},
    {7400,.12f,.5f,.78f,.8f,.48f,.28f,0,.36f},
    {1600,1.1f,.8f,.82f,1.6f,.22f,.20f,0,.42f},
    {4800,.004f,.26f,.2f,.16f,.94f,.16f,12,.38f},
    {3200,.18f,.35f,.68f,.45f,.35f,.24f,0,.40f}
}};
static_assert(settings.size()==sounds.size());

bool set(SoundDocument& sound,const juce::String& id,float physical) {
    const auto registry=parameterRegistry();
    const auto p=std::find_if(registry.begin(),registry.end(),[&](const auto& item){return item.id==id;});
    if(p==registry.end()||physical<p->minimum||physical>p->maximum)return false;
    sound.values[std::size_t(p-registry.begin())]=juce::NormalisableRange<float>(p->minimum,p->maximum,0,p->skew).convertTo0to1(physical);
    return true;
}
}
std::span<const FactorySoundInfo> factorySounds() noexcept { return sounds; }
bool makeFactorySound(std::size_t index, SoundDocument& output) {
    if(index>=sounds.size())return false;
    const bool digital=index>=6;const auto& s=settings[index];
    RackOperation op;op.design=factoryRackDesigns()[digital?1:0];
    op.newInstance=juce::Uuid("61c587e5-2aa0-4d51-9df0-"+juce::String(index+1).paddedLeft('0',12));
    op.destination={RackRole::synth,masterChain,0};op.allocation={{0,0}};op.allowSlotReuse=true;
    SoundDocument sound;RackDifference difference;
    if(makeRackCandidate(newInstanceSound(),op,SoundEditOrigin::manual,sound,difference)!=RackCandidateStatus::ready)return false;
    const juce::String bank=digital?"synth.1.wavetable.":"synth.1.analog.";
    if(!set(sound,bank+"filter.cutoff",s.cutoff)||!set(sound,bank+"amp.attack",s.attack)
        ||!set(sound,bank+"amp.decay",s.decay)||!set(sound,bank+"amp.sustain",s.sustain)
        ||!set(sound,bank+"amp.release",s.release)||!set(sound,bank+"osc.2.level",s.secondLevel)
        ||!set(sound,bank+"osc.2.tune",s.secondTune)||!set(sound,"synth.1.level",s.level)
        ||!set(sound,bank+(digital?"osc.1.position":"osc.1.waveform"),s.color)
        ||!set(sound,"global.voice_mode",index==1||index==2?1.0f:0.0f))return false;
    sound.name=sounds[index].name;
    if(!validateSoundDocument(sound))return false;
    output=std::move(sound);return true;
}
}
