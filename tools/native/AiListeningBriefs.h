#pragma once

#include "ParameterRegistry.h"
#include "ProjectDocument.h"
#include "RackExamples.h"
#include <array>
#include <string>
#include <string_view>

// The sound-quality packet's AI briefs (docs/verification/2026-10-09-sound-quality-listening.md), shared by the
// account probe that sends them and the renderer that plays them back. C4 is MIDI 60.
namespace airack::listening
{
struct AiBrief
{
    const char* id;
    std::string instruction, role, key, scale;
    double bpm;
    PatternDocument phrase;
    double seconds;
};
inline std::array<AiBrief,2> aiBriefs()
{
    const auto note=[](double start,double duration,int pitch,int velocity){return PatternNote{start,duration,std::uint8_t(pitch),std::uint8_t(velocity),1};};
    AiBrief a{"A","Create a warm evolving pad with a clear upper shimmer, soft attacks and broad stereo space; keep the low mids clear in mono.","pad","C","minor",100,{},16};
    a.phrase.bars=4;
    for(int pitch:{48,51,55,58})a.phrase.notes[a.phrase.noteCount++]=note(0,8,pitch,70);
    for(int pitch:{44,48,51,55})a.phrase.notes[a.phrase.noteCount++]=note(8,8,pitch,70);
    AiBrief b{"B","Create a tight expressive pluck or lead that speaks clearly, preserves attacks and leaves a restrained stereo tail without washing out repeated notes.","pluck or lead","C","minor",120,{},8};
    b.phrase.bars=2;
    constexpr int line[]{60,63,67,70,67,63};
    for(double repeat:{0.0,4.0})for(int n=0;n<6;++n)b.phrase.notes[b.phrase.noteCount++]=note(repeat+.5*n,.25,line[n],n%2?70:90);
    return {a,b};
}
inline const AiBrief* aiBrief(std::string_view id)
{
    static const auto briefs=aiBriefs();
    for(const auto& brief:briefs)if(id==brief.id)return &brief;
    return nullptr;
}
// What the brief starts from: a new instance's sound at the packet's +24 dB with the protector on, the phrase in its pattern.
inline ProjectDocument aiBriefSource(const AiBrief& brief)
{
    ProjectDocument project;project.sound=copperArcSound();project.pattern=brief.phrase;
    const auto registry=parameterRegistry();
    for(std::size_t n=0;n<registry.size();++n){
        const auto& p=registry[n];
        if(p.id=="output.gain_db")project.sound.values[n]=juce::NormalisableRange<float>(p.minimum,p.maximum,0,p.skew).convertTo0to1(24.0f);
        if(p.id=="output.protection")project.sound.values[n]=1;
    }
    return project;
}
}
