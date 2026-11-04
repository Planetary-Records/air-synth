#include "DrumEngine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace airack::drums;
namespace airack::drums {
struct DrumEngineTestAccess {
    static bool begin(DrumEngine& e,juce::AudioBuffer<float>& b,const juce::MidiBuffer& m,int factor=1){return e.beginRender(b,m.cbegin(),m.cend(),factor);}
    static bool range(DrumEngine& e,int first,int count){return e.renderRange(first,count);}
    static void end(DrumEngine& e){e.endRender();}
    static bool beginExact(DrumEngine& e,juce::AudioBuffer<float>& b,std::span<const RenderEvent> events,int factor=1){return e.beginRender(b,events,0,factor);}
    static bool rangeExact(DrumEngine& e,int first,int count,const std::array<std::uint64_t,16>& tokens,bool& unavailable){return e.renderRange(first,count,true,true,tokens,unavailable);}
};
}
namespace
{
int checks=0, failures=0, sections=0;
void check(bool pass,const char* text,int line)
{ ++checks; if(!pass){++failures;std::cerr<<"FAIL "<<line<<": "<<text<<'\n';} }
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* text){++sections;std::cout<<text<<'\n';}
using Stereo=std::array<std::vector<float>,2>;
struct Event{int offset;juce::MidiMessage message;};
juce::MidiMessage on(int note=36,int channel=1,int velocity=127)
{return juce::MidiMessage::noteOn(channel,note,static_cast<juce::uint8>(velocity));}
AssetPtr asset(std::vector<float> data,std::uint32_t channels=1)
{
    AssetPtr p;AssetError error=AssetError::none;
    CHECK(makeSampleAsset(data,channels,p,error));CHECK(error==AssetError::none);return p;
}
RackSettings rack(const SampleAsset* sample)
{RackSettings p;p.pads[0].sample=sample;return p;}
Stereo render(DrumEngine& engine,int frames,const std::vector<Event>& events={},int block=512)
{
    Stereo result;for(auto& x:result)x.resize(static_cast<std::size_t>(frames));
    for(int first=0;first<frames;first+=block){
        const int count=std::min(block,frames-first);juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;
        for(const auto& e:events)if(e.offset>=first&&e.offset<first+count)midi.addEvent(e.message,e.offset-first);
        engine.render(audio,midi);
        for(int channel=0;channel<2;++channel)std::copy_n(audio.getReadPointer(channel),count,result[channel].data()+first);
    }
    return result;
}
bool near(double a,double b,double tolerance=2e-6){return std::abs(a-b)<=tolerance;}
bool silent(const juce::AudioBuffer<float>& b)
{for(int c=0;c<b.getNumChannels();++c)for(int n=0;n<b.getNumSamples();++n)if(b.getSample(c,n)!=0)return false;return true;}
void configure(DrumEngine& e,const RackSettings& p,double rate=48000,int block=512)
{CHECK(e.setSettings(p));CHECK(e.prepare(rate,block));}

void rangeSessions()
{
    section("owner-only contiguous ranges, scaled offsets and invalid-session freeze");
    auto a=asset({.25f,-.5f,.75f,.125f});auto b=asset({-.125f,.5f,-.25f,.75f},2);
    auto p=rack(a.get());p.pads[1].sample=b.get();p.pads[1].midiNote=37;p.pads[0].chokeGroup=p.pads[1].chokeGroup=1;
    for(int factor:{1,2})for(int partition:{1,7,16,17,64,512}){
        DrumEngine whole,ranges;configure(whole,p,48000*factor,1024);configure(ranges,p,48000*factor,1024);
        juce::AudioBuffer<float> expected(2,1024),actual(2,1024);juce::MidiBuffer host,scaled;
        for(int i:{0,7,15,16,17,255,511}){host.addEvent(on(i%2?37:36),i);scaled.addEvent(on(i%2?37:36),i*factor);}
        if(factor==1){host.addEvent(on(),1023);scaled.addEvent(on(),1023);}
        host.addEvent(juce::MidiMessage::controllerEvent(1,123,0),500);scaled.addEvent(juce::MidiMessage::controllerEvent(1,123,0),500*factor);
        whole.render(expected,scaled);CHECK(DrumEngineTestAccess::begin(ranges,actual,host,factor));
        CHECK(!DrumEngineTestAccess::begin(ranges,actual,host,factor));
        CHECK(!DrumEngineTestAccess::range(ranges,1,1));CHECK(!DrumEngineTestAccess::range(ranges,0,-1));
        for(int first=0;first<1024;first+=partition)CHECK(DrumEngineTestAccess::range(ranges,first,std::min(partition,1024-first)));
        CHECK(!DrumEngineTestAccess::range(ranges,1024,1));DrumEngineTestAccess::end(ranges);
        bool same=true;for(int ch=0;ch<2;++ch)for(int i=0;i<1024;++i)same&=expected.getSample(ch,i)==actual.getSample(ch,i);
        CHECK(same);if(factor==1)CHECK(actual.getSample(0,0)==.25f);CHECK(whole.activeHitCount()==ranges.activeHitCount());
        if(factor==1)CHECK(actual.getSample(0,1023)==.25f);
        CHECK(render(whole,32)==render(ranges,32));
    }
    for(int factor:{1,2})for(bool stereo:{false,true})for(int offset:{0,7,15,16,17,255,511}){
        std::vector<float> words(32*(stereo?2:1));
        for(int frame=0;frame<32;++frame)for(int ch=0;ch<(stereo?2:1);++ch)words[frame*(stereo?2:1)+ch]=frame>=8&&frame<16?(stereo?(ch?.5f:-.125f):.25f):(frame%2?16.f:-16.f);
        auto selected=asset(words,stereo?2:1);auto settings=rack(selected.get());settings.pads[0].trimStart=.25f;settings.pads[0].trimEnd=.5f;
        DrumEngine direct,ranges;configure(direct,settings,48000*factor,2048);configure(ranges,settings,48000*factor,2048);
        juce::AudioBuffer<float> a(2,2048),b(2,2048);juce::MidiBuffer host,scaled;host.addEvent(on(),offset);scaled.addEvent(on(),offset*factor);direct.render(a,scaled);
        CHECK(DrumEngineTestAccess::begin(ranges,b,host,factor));CHECK(DrumEngineTestAccess::range(ranges,0,2048));DrumEngineTestAccess::end(ranges);
        bool legal=true;for(int ch=0;ch<2;++ch)for(int n=0;n<2048;++n){const double expected=n>=offset*factor&&n<(offset+8)*factor?(stereo?(ch?.5:-.125):.25):0.;legal&=near(a.getSample(ch,n),expected)&&a.getSample(ch,n)==b.getSample(ch,n);}CHECK(legal);
        settings.pads[0].sample=nullptr;CHECK(direct.setSettings(settings)&&ranges.setSettings(settings));CHECK(direct.activeHitCount()==0&&ranges.activeHitCount()==0);CHECK(!direct.references(selected.get())&&!ranges.references(selected.get()));
    }
}

void validation()
{
    section("transactional preparation/settings and internal geometry");
    auto sample=asset(std::vector<float>(2000,.25f));auto p=rack(sample.get());DrumEngine e,reference;
    configure(e,p);configure(reference,p);CHECK(render(e,8,{{0,on()}})==render(reference,8,{{0,on()}}));
    for(double rate:{0.,7999.,768001.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})CHECK(!e.prepare(rate,512));
    for(int size:{-1,0,131073})CHECK(!e.prepare(48000,size));
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(int field=0;field<12;++field){
        auto bad=p;switch(field){
            case 0:bad.level=nan;break;case 1:bad.level=1.01f;break;case 2:bad.pan=-1.01f;break;
            case 3:bad.pads[0].gainDb=12.01f;break;case 4:bad.pads[0].gainDb=-60.01f;break;
            case 5:bad.pads[0].pan=nan;break;case 6:bad.pads[0].tuneSemitones=24.01f;break;
            case 7:bad.pads[0].trimStart=.5f;bad.pads[0].trimEnd=.5f;break;
            case 8:bad.pads[0].attackSeconds=-.01f;break;case 9:bad.pads[0].decaySeconds=10.01f;break;
            case 10:bad.pads[0].midiNote=128;break;case 11:bad.pads[0].chokeGroup=17;break;
        }CHECK(!e.setSettings(bad));
    }
    for(auto member:{&PadSettings::gainDb,&PadSettings::pan,&PadSettings::tuneSemitones,&PadSettings::trimStart,&PadSettings::trimEnd,&PadSettings::attackSeconds,&PadSettings::decaySeconds}){
        auto bad=p;bad.pads[15].*member=nan;CHECK(!e.setSettings(bad));
    }
    auto bad=p;bad.pads[0].trimStart=.75f;bad.pads[0].trimEnd=.25f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].trimStart=-.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].trimEnd=1.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].midiNote=-1;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].chokeGroup=-1;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].tuneSemitones=-24.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].pan=1.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].attackSeconds=1.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pads[0].decaySeconds=-.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.level=-.1f;CHECK(!e.setSettings(bad));
    bad=p;bad.pan=nan;CHECK(!e.setSettings(bad));
    CHECK(render(e,32)==render(reference,32));CHECK(e.references(sample.get()));CHECK(!e.references(nullptr));
    for(double rate:{8000.,44100.,48000.,96000.,768000.}){DrumEngine bounds;CHECK(bounds.prepare(rate,131072));}
    auto bounds=p;bounds.pads[0].gainDb=-60;bounds.pads[0].pan=-1;bounds.pads[0].tuneSemitones=-24;
    bounds.pads[0].attackSeconds=1;bounds.pads[0].decaySeconds=10;bounds.pads[0].chokeGroup=16;bounds.pads[0].midiNote=0;
    CHECK(e.setSettings(bounds));bounds.pads[0].gainDb=12;bounds.pads[0].pan=1;bounds.pads[0].tuneSemitones=24;bounds.pads[0].midiNote=127;CHECK(e.setSettings(bounds));
}

void literalPlayback()
{
    section("literal unity samples, offsets, stereo/mono, trim and velocity");
    auto sample=asset({.25f,-.5f,.75f,.125f,-.5f,1.f},2);DrumEngine e;configure(e,rack(sample.get()));
    auto y=render(e,9,{{3,on()},{4,juce::MidiMessage::noteOff(1,36)}});
    CHECK(y[0]==std::vector<float>({0,0,0,.25f,.75f,-.5f,0,0,0}));
    CHECK(y[1]==std::vector<float>({0,0,0,-.5f,.125f,1,0,0,0}));CHECK(e.activeHitCount()==0);
    auto ramp=asset({0,.1f,.2f,.3f,.4f,.5f,.6f,.7f});auto p=rack(ramp.get());p.pads[0].trimStart=.25f;p.pads[0].trimEnd=.75f;
    configure(e,p);y=render(e,6,{{0,on()}});CHECK(y[0]==std::vector<float>({.2f,.3f,.4f,.5f,0,0}));CHECK(y[0]==y[1]);
    p.pads[0].trimEnd=.25001f;configure(e,p);y=render(e,3,{{0,on()}});CHECK(y[0]==std::vector<float>({.2f,0,0}));
    auto mono=asset({1,1,1,1});p=rack(mono.get());configure(e,p);y=render(e,6,{{0,on(36,16,64)},{1,on(36,1,0)}});
    for(int n=0;n<4;++n)CHECK(near(y[0][n],64./127));CHECK(y[0][4]==0);CHECK(y[0]==y[1]);
    p.pads[0].pan=-1;configure(e,p);y=render(e,2,{{0,on()}});CHECK(y[0][0]==1&&y[1][0]==0);
    p.pads[0].pan=1;configure(e,p);y=render(e,2,{{0,on()}});CHECK(y[0][0]==0&&y[1][0]==1);
    p.pads[0].pan=0;p.pan=-1;configure(e,p);y=render(e,2,{{0,on()}});CHECK(y[0][0]==1&&y[1][0]==0);
    p=rack(mono.get());p.pads[0].gainDb=12;configure(e,p);y=render(e,2,{{0,on()}});CHECK(near(y[0][0],std::pow(10.,.6)));
}

void rateTuneEnvelope()
{
    section("physical rate/tuning law, natural duration and linear attack/decay");
    auto sample=asset({0,.02f,.04f,.06f,.08f,.10f,.12f,.14f,.16f,.18f,.20f});
    auto constant=asset(std::vector<float>(11,.25f));
    for(double rate:{44100.,48000.,96000.})for(float tune:{-24.f,-12.f,0.f,12.f,24.f}){
        const double step=48000/rate*std::exp2(tune/12.);
        auto p=rack(step==1?sample.get():constant.get());p.pads[0].tuneSemitones=tune;DrumEngine e;configure(e,p,rate);
        const int count=static_cast<int>(std::ceil(11/step));
        const auto y=render(e,count+4,{{0,on()}});
        for(int n=0;n<count;++n)CHECK(near(y[0][n],step==1?.02*n:.25));
        for(int n=count;n<count+4;++n)CHECK(y[0][n]==0);CHECK(e.activeHitCount()==0);
    }
    struct DurationCase{int frames,count,block;float start=0,end=1;};
    for(const auto test:std::array<DurationCase,8>{{{480,441,1},{480,441,7},{480,441,64},{480,441,511},
        {480,441,512},{48000,44100,512},{480000,441000,512},{960,441,7,.25f,.75f}}}){
        auto timed=asset(std::vector<float>(test.frames,1));auto settings=rack(timed.get());
        settings.pads[0].trimStart=test.start;settings.pads[0].trimEnd=test.end;DrumEngine exact;configure(exact,settings,44100);
        const auto prefix=render(exact,test.count-1,{{0,on()}},test.block);
        CHECK(std::all_of(prefix[0].begin(),prefix[0].end(),[](float value){return value==1;}));CHECK(prefix[0]==prefix[1]);
        CHECK(exact.activeHitCount()==1);settings.pads[0].sample=nullptr;CHECK(exact.setSettings(settings));
        CHECK(render(exact,1)[0][0]==1);CHECK(exact.activeHitCount()==0);CHECK(!exact.references(timed.get()));
        CHECK(render(exact,1)[0][0]==0);
    }
    std::vector<float> slope(960,.25f);
    auto tuned=asset(slope);struct TuneCase{float tune;int count;};
    for(const auto test:std::array<TuneCase,3>{{{-7.25f,671},{3.f,371},{11.5f,227}}}){
        auto settings=rack(tuned.get());settings.pads[0].trimStart=.25f;settings.pads[0].trimEnd=.75f;settings.pads[0].tuneSemitones=test.tune;
        DrumEngine reference;configure(reference,settings,44100);const auto expected=render(reference,test.count+2,{{0,on()}});
        for(int n=0;n<test.count;++n)CHECK(near(expected[0][n],.25));
        CHECK(expected[0][test.count-1]>0);CHECK(expected[0][test.count]==0&&expected[0][test.count+1]==0);CHECK(reference.activeHitCount()==0);
        for(int block:{1,7,64,511}){DrumEngine partition;configure(partition,settings,44100);CHECK(render(partition,test.count+2,{{0,on()}},block)==expected);}
    }
    auto flat=asset(std::vector<float>(24000,1));auto p=rack(flat.get());p.pads[0].attackSeconds=.125f;p.pads[0].decaySeconds=.125f;
    DrumEngine e;configure(e,p,8000);const auto y=render(e,2100,{{0,on()},{50,juce::MidiMessage::noteOff(1,36)}});
    for(int n=0;n<2100;++n)CHECK(near(y[0][n],n<1000?n/1000.:std::max(0.,1-(n-1000)/1000.)));
    CHECK(e.activeHitCount()==0);p.pads[0].attackSeconds=0;p.pads[0].decaySeconds=0;configure(e,p,8000);
    CHECK(render(e,4001,{{0,on()}})[0][3999]==1);CHECK(e.activeHitCount()==0);
}

void liveSettingsAndLifetime()
{
    section("5ms live gains; captured playback settings and borrowed-pointer retirement");
    auto a=asset(std::vector<float>(2000,1));auto b=asset(std::vector<float>(2000,.5f));auto p=rack(a.get());DrumEngine e;configure(e,p);
    CHECK(render(e,1,{{0,on()}})[0][0]==1);p.level=0;CHECK(e.setSettings(p));
    const auto ramp=render(e,241);for(int n=0;n<240;++n)CHECK(near(ramp[0][n],1-(n+1)/240.));CHECK(ramp[0][240]==0);
    CHECK(e.setSettings(p));CHECK(render(e,1)[0][0]==0);p.level=1;CHECK(e.setSettings(p));CHECK(near(render(e,1)[0][0],1./240));
    p.level=.5f;CHECK(e.setSettings(p));const double first=1./240+(.5-1./240)/240;CHECK(near(render(e,1)[0][0],first));
    p=rack(a.get());configure(e,p);CHECK(render(e,1,{{0,on()}})[0][0]==1);
    p.pads[0].gainDb=-6;p.pads[0].pan=1;CHECK(e.setSettings(p));const auto gains=render(e,240);
    const double target=std::pow(10.,-6./20);CHECK(near(gains[0][0],239./240));CHECK(gains[0][239]==0);CHECK(near(gains[1][239],target));
    p=rack(a.get());configure(e,p);CHECK(render(e,1,{{0,on()}})[0][0]==1);
    p.pads[0].sample=b.get();p.pads[0].trimStart=.5f;p.pads[0].tuneSemitones=24;p.pads[0].attackSeconds=1;p.pads[0].decaySeconds=.001f;
    CHECK(e.setSettings(p));CHECK(e.references(a.get()));CHECK(e.references(b.get()));CHECK(render(e,16)[0][0]==1);
    e.reset();CHECK(!e.references(a.get()));CHECK(e.references(b.get()));CHECK(e.activeHitCount()==0);
    CHECK(render(e,1,{{0,on()}})[0][0]==0);p.pads[0].sample=nullptr;CHECK(e.setSettings(p));CHECK(e.references(b.get()));
    e.reset();CHECK(!e.references(b.get()));
    p=rack(a.get());configure(e,p);(void)render(e,1,{{0,on()}});p.enabled=false;CHECK(e.setSettings(p));CHECK(e.activeHitCount()==0);
    CHECK(render(e,8,{{0,on()}})[0]==std::vector<float>(8,0));p.enabled=true;CHECK(e.setSettings(p));CHECK(render(e,1,{{0,on()}})[0][0]==1);
}

void overlapChokeSteal()
{
    section("overlap, duplicate mappings, 5ms choke and oldest32-hit steal");
    auto a=asset(std::vector<float>(4000,.25f));auto b=asset(std::vector<float>(4000,.5f));auto p=rack(a.get());DrumEngine e;configure(e,p);
    auto y=render(e,6,{{0,on()},{1,juce::MidiMessage::noteOff(1,36)},{2,on()},{3,on(36,1,0)}});
    CHECK(y[0]==std::vector<float>({.25f,.25f,.5f,.5f,.5f,.5f}));CHECK(e.activeHitCount()==2);
    p.pads[0].chokeGroup=3;p.pads[1].sample=b.get();p.pads[1].midiNote=36;p.pads[1].chokeGroup=3;
    configure(e,p);y=render(e,242,{{0,on()},{1,on(36,2)}});
    CHECK(y[0][0]==.75f);CHECK(y[0][1]==1.5f);CHECK(near(y[0][2],.75+.75*239./240));CHECK(y[0][241]==.75f);CHECK(e.activeHitCount()==2);
    // A re-choke does not extend an old hit's release; a different group survives.
    p=rack(a.get());p.pads[0].chokeGroup=1;p.pads[1].sample=b.get();p.pads[1].chokeGroup=2;configure(e,p);
    y=render(e,242,{{0,on()},{0,on(37)},{1,on()},{100,on()}});
    CHECK(near(y[0][1],1.));CHECK(near(y[0][241],.5+.25+.25*(240-(241-100))/240.));
    // Old pointer remains borrowed for exactly its choke release after replacement.
    p=rack(a.get());p.pads[0].chokeGroup=1;configure(e,p);(void)render(e,1,{{0,on()}});
    p.pads[0].sample=b.get();CHECK(e.setSettings(p));(void)render(e,239,{{0,on()}});CHECK(e.references(a.get()));
    (void)render(e,1);CHECK(!e.references(a.get()));CHECK(e.references(b.get()));
    // Saturate with one old A and31 B hits. Removing configured A isolates hit lifetime.
    p=rack(a.get());p.pads[1].sample=b.get();configure(e,p);std::vector<Event> full{{0,on()}};
    for(int n=0;n<31;++n)full.push_back({0,on(37)});y=render(e,1,full);CHECK(y[0][0]==15.75f);CHECK(e.activeHitCount()==32);
    p.pads[0].sample=nullptr;CHECK(e.setSettings(p));CHECK(e.references(a.get()));y=render(e,1,{{0,on(37)}});
    CHECK(y[0][0]==15.75f);CHECK(!e.references(a.get()));CHECK(e.activeHitCount()==32);
    const auto blend=render(e,240);for(int n=0;n<240;++n)CHECK(near(blend[0][n],15.75+.25*(n+1)/240.));CHECK(blend[0][239]==16);
    // Reuse a retired low-index slot: oldest must follow trigger order, not slot index.
    auto shortHit=asset({.125f});p=rack(shortHit.get());p.pads[1].sample=a.get();p.pads[2].sample=b.get();configure(e,p);
    CHECK(render(e,1,{{0,on()},{0,on(37)}})[0][0]==.375f);CHECK(e.activeHitCount()==1);
    std::vector<Event> refill;for(int n=0;n<31;++n)refill.push_back({0,on(38)});
    CHECK(render(e,1,refill)[0][0]==15.75f);p.pads[1].sample=nullptr;CHECK(e.setSettings(p));
    CHECK(render(e,1,{{0,on(38)}})[0][0]==15.75f);CHECK(!e.references(a.get()));CHECK(render(e,240)[0].back()==16);
    for(double rate:{44100.,48000.,96000.}){
        p=rack(a.get());p.pads[0].chokeGroup=1;configure(e,p,rate);(void)render(e,1,{{0,on()}});
        p.pads[0].sample=b.get();CHECK(e.setSettings(p));const int release=static_cast<int>(std::ceil(.005*rate));
        const auto start=render(e,release-1,{{0,on()}});CHECK(start[0][0]==.75f);CHECK(e.references(a.get()));
        CHECK(near(render(e,1)[0][0],.5+.25/release));CHECK(!e.references(a.get()));CHECK(e.activeHitCount()==1);
    }
}

void eventsAndInvalidOutput()
{
    section("CC120/123, Stop/equal-offset ordering, malformed MIDI and invalid-output freeze");
    auto a=asset(std::vector<float>(4000,.25f));auto p=rack(a.get());DrumEngine e;configure(e,p,48000,32);
    auto y=render(e,6,{{0,on(36,1)},{0,on(36,2)},{2,juce::MidiMessage::controllerEvent(1,120,0)},{4,juce::MidiMessage::controllerEvent(2,123,0)}},32);
    CHECK(y[0]==std::vector<float>({.5f,.5f,.25f,.25f,0,0}));CHECK(e.activeHitCount()==0);
    y=render(e,2,{{0,on()},{0,juce::MidiMessage::midiStop()},{0,on(36,2)}},32);CHECK(y[0][0]==.25f);CHECK(e.activeHitCount()==1);
    y=render(e,2,{{0,juce::MidiMessage::midiStop()}},32);CHECK(y[0]==std::vector<float>(2,0));
    juce::AudioBuffer<float> audio(2,4);juce::MidiBuffer invalid;const std::uint8_t malformed[]{0x90,0x80,0x7f};
    invalid.addEvent(on(),-1);invalid.addEvent(on(),4);invalid.addEvent(malformed,3,0);invalid.addEvent(juce::MidiMessage::controllerEvent(1,1,127),0);
    e.render(audio,invalid);CHECK(silent(audio));CHECK(e.activeHitCount()==0);
    DrumEngine reference;configure(reference,p,48000,32);CHECK(render(e,1,{{0,on()}},32)==render(reference,1,{{0,on()}},32));
    juce::MidiBuffer stop;stop.addEvent(juce::MidiMessage::midiStop(),0);
    juce::AudioBuffer<float> mono(1,4);mono.clear();mono.setSample(0,0,1);e.render(mono,stop);CHECK(silent(mono));
    juce::AudioBuffer<float> big(2,33);big.clear();big.setSample(1,32,1);e.render(big,stop);CHECK(silent(big));
    std::array<float,8> overlap{};float* pointers[]{overlap.data(),overlap.data()+1};juce::AudioBuffer<float> alias(pointers,2,4);
    overlap.fill(1);e.render(alias,stop);CHECK(silent(alias));
    juce::AudioBuffer<float> empty(2,0);e.render(empty,stop);CHECK(e.activeHitCount()==1);
    std::array<float,8> adjacent{};float* disjoint[]{adjacent.data(),adjacent.data()+4};juce::AudioBuffer<float> validAdjacent(disjoint,2,4);juce::MidiBuffer none;
    e.render(validAdjacent,none);const auto adjacentReference=render(reference,4,{},32);
    for(int c=0;c<2;++c)for(int n=0;n<4;++n)CHECK(validAdjacent.getSample(c,n)==adjacentReference[c][n]);
    CHECK(render(e,16,{},32)==render(reference,16,{},32));
    DrumEngine unprepared;audio.setSample(0,0,1);unprepared.render(audio,stop);CHECK(silent(audio));
    CHECK(e.prepare(96000,32));CHECK(e.activeHitCount()==0);CHECK(e.references(a.get()));
}

void partitionAndSpectrum()
{
    section("exact block partitions, rate/pitch spectrum characterization and finite headroom");
    std::vector<float> wave(20000);for(std::size_t n=0;n<wave.size();++n)wave[n]=static_cast<float>(std::sin(2*std::numbers::pi*.05*n));
    auto a=asset(wave);auto p=rack(a.get());p.pads[0].tuneSemitones=3;p.pads[0].attackSeconds=.001f;p.pads[0].decaySeconds=.02f;p.pads[0].chokeGroup=1;
    const std::vector<Event> events{{0,on()},{17,on(36,2,90)},{91,on()},{92,juce::MidiMessage::controllerEvent(2,123,0)},{700,juce::MidiMessage::midiStop()},{701,on(36,3)}};
    for(double rate:{44100.,48000.,96000.}){
        DrumEngine reference;configure(reference,p,rate);const auto expected=render(reference,1000,events,512);
        for(int block:{1,7,64,511}){DrumEngine e;configure(e,p,rate);CHECK(render(e,1000,events,block)==expected);}
        const auto withEdit=[&](int block){
            DrumEngine e;configure(e,p,rate);auto result=render(e,73,{{0,on()}},block);
            auto edited=p;edited.level=.5f;edited.pan=.3f;edited.pads[0].gainDb=-12;CHECK(e.setSettings(edited));
            const auto tail=render(e,927,{{7,on()},{700,on(36,2,90)}},block);
            for(int c=0;c<2;++c)result[c].insert(result[c].end(),tail[c].begin(),tail[c].end());return result;
        };
        const auto editReference=withEdit(512);for(int block:{1,7,64,511})CHECK(withEdit(block)==editReference);
    }
    for(double rate:{44100.,48000.,96000.})for(double f:{.05,.30}){
        for(std::size_t n=0;n<wave.size();++n)wave[n]=static_cast<float>(std::sin(2*std::numbers::pi*f*n));
        auto sample=asset(wave);DrumEngine e;configure(e,rack(sample.get()),rate);const auto y=render(e,4352,{{0,on()}});
        double worst=0;for(int n=256;n<4352;++n)worst=std::max(worst,std::abs(y[0][n]-std::sin(2*std::numbers::pi*f*n*48000/rate)));
        std::cout<<"RECONSTRUCTION rate="<<rate<<" source_cycles="<<f<<" max_error="<<worst<<'\n';
        CHECK(worst<=.001);if(rate==48000)CHECK(worst<1e-6);
    }
    for(std::size_t n=0;n<wave.size();++n)wave[n]=static_cast<float>(std::sin(2*std::numbers::pi*.375*n));
    auto high=asset(wave);p=rack(high.get());p.pads[0].tuneSemitones=12;DrumEngine e;configure(e,p);
    const auto alias=render(e,4352,{{0,on()}});double worst=0,re=0,im=0;
    for(int n=256;n<4352;++n){worst=std::max(worst,std::abs(double(alias[0][n])));re+=alias[0][n]*std::cos(2*std::numbers::pi*.25*n);im+=alias[0][n]*std::sin(2*std::numbers::pi*.25*n);}
    const double amplitude=2*std::hypot(re,im)/4096;std::cout<<"PITCH_UP_ALIAS amplitude="<<amplitude<<" max_identity_error="<<worst<<'\n';
    CHECK(worst<=.001);CHECK(amplitude<=.001);
    auto peak=asset(std::vector<float>(100,16));p=rack(peak.get());p.pads[0].gainDb=12;configure(e,p);std::vector<Event> hits;
    for(int n=0;n<32;++n)hits.push_back({0,on()});const auto sum=render(e,2,hits);CHECK(near(sum[0][0],32*16*std::pow(10.,.6),.001));CHECK(std::isfinite(sum[0][0]));
}

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false;thread_local int allocations=0,frees=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int)
{if(tracking&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC))++allocations;if(tracking&&kind==_HOOK_FREE)++frees;return 1;}
#endif
void realtimeHeap()
{
    section("prepared paths and full internal geometry allocate/free zero");
#if defined(_MSC_VER) && defined(_DEBUG)
    auto a=asset(std::vector<float>(4000,.25f)),b=asset(std::vector<float>(4000,.5f));
    auto p=rack(a.get());DrumEngine e;configure(e,p);juce::AudioBuffer<float> audio(2,512);
    juce::MidiBuffer midi,empty,stop,cc; midi.ensureSize(4096);
    for(int n=0;n<40;++n)midi.addEvent(on(36,1+n%16),n);stop.addEvent(juce::MidiMessage::midiStop(),0);
    cc.addEvent(juce::MidiMessage::controllerEvent(1,120,0),0);cc.addEvent(juce::MidiMessage::controllerEvent(2,123,0),1);
    // Allocate every full-geometry fixture before the callback guard.
    auto geometryAsset=asset(std::vector<float>(24000,1));auto geometrySettings=rack(geometryAsset.get());geometrySettings.pads[0].attackSeconds=.125f;
    DrumEngine geometryEngine;configure(geometryEngine,geometrySettings,768000,131072);
    juce::AudioBuffer<float> geometry(2,131072);juce::MidiBuffer geometryOn;geometryOn.addEvent(on(),0);
    const auto ownerCount=a.use_count();bool good=true,geometryGood=true;auto* old=_CrtSetAllocHook(allocationHook);allocations=0;frees=0;tracking=true;
    e.render(audio,midi);good&=e.activeHitCount()==32;p.pads[0].sample=b.get();p.pads[0].chokeGroup=1;p.pads[0].gainDb=-6;p.pads[0].pan=.5f;good&=e.setSettings(p);
    e.render(audio,midi);e.render(audio,empty);good&=!e.references(a.get());e.render(audio,cc);e.render(audio,stop);
    e.reset();for(double rate:{44100.,48000.,96000.,768000.}){good&=e.prepare(rate,512);e.render(audio,midi);e.reset();}
    p.pads[0].sample=nullptr;good&=e.setSettings(p);good&=!e.references(b.get());
    // At768k, canonical24,000 frames last384,000 physical frames; attack lasts96,000.
    geometryEngine.render(geometry,geometryOn);geometryGood&=geometryEngine.activeHitCount()==1;
    geometryGood&=geometry.getSample(0,0)==0&&geometry.getSample(0,131071)==1;
    for(int n=0;n<131072;++n)for(int c=0;c<2;++c){const auto value=geometry.getSample(c,n);
        geometryGood&=std::isfinite(value)&&near(value,std::min(1.,n/96000.));}
    geometryEngine.render(geometry,empty);geometryGood&=geometryEngine.activeHitCount()==1;
    geometryGood&=geometry.getSample(0,0)==1&&geometry.getSample(0,131071)==1;
    for(int n=0;n<131072;++n)for(int c=0;c<2;++c){const auto value=geometry.getSample(c,n);geometryGood&=std::isfinite(value)&&value==1;}
    geometryEngine.render(geometry,empty);geometryGood&=geometryEngine.activeHitCount()==0;
    geometryGood&=geometry.getSample(0,0)==1&&geometry.getSample(0,121855)==1&&geometry.getSample(0,121856)==0&&geometry.getSample(0,131071)==0;
    for(int n=0;n<131072;++n)for(int c=0;c<2;++c){const auto value=geometry.getSample(c,n);geometryGood&=std::isfinite(value)&&value==(n<121856?1.f:0.f);}
    tracking=false;_CrtSetAllocHook(old);CHECK(good);CHECK(geometryGood);CHECK(allocations==0&&frees==0);CHECK(a.use_count()==ownerCount);
    std::cout<<"DRUM_CALLBACK allocations="<<allocations<<" frees="<<frees<<'\n';
    std::cout<<"DRUM_FULL_GEOMETRY rate=768000 block=131072 accepted_blocks=3 natural_end=384000 functional_only="<<geometryGood<<'\n';
#else
    CHECK(false); // This acceptance target requires the MSVC Debug CRT allocation guard.
#endif
}
}
namespace {
void exactRangeAuthority(){
    section("exact pad range authority and selected duplicate group");auto a=asset(std::vector<float>(1024,.001f));auto b=asset(std::vector<float>(1024,-.002f));auto settings=rack(a.get());settings.pads[1].sample=b.get();settings.pads[1].midiNote=36;settings.pads[0].chokeGroup=settings.pads[1].chokeGroup=1;
    DrumEngine engine;configure(engine,settings);juce::AudioBuffer<float> audio(2,64);std::array<std::uint64_t,16> tokens{};tokens.fill(101);bool unavailable=false;
    const std::array<RenderEvent,2> events{{{101,17,RenderEventKind::exactPad,0,1,127,1},{0,33,RenderEventKind::stopChannel,0,0,0,1}}};
    CHECK(DrumEngineTestAccess::beginExact(engine,audio,events));CHECK(DrumEngineTestAccess::rangeExact(engine,0,16,tokens,unavailable));CHECK(DrumEngineTestAccess::rangeExact(engine,16,1,tokens,unavailable));CHECK(DrumEngineTestAccess::rangeExact(engine,17,47,tokens,unavailable));DrumEngineTestAccess::end(engine);
    for(int ch=0;ch<2;++ch)for(int n=0;n<64;++n)CHECK(audio.getSample(ch,n)==(n>=17&&n<33?-.002f:0.f));CHECK(!unavailable&&engine.activeHitCount()==0);
    auto stale=events;stale[0].token=100;CHECK(DrumEngineTestAccess::beginExact(engine,audio,stale));CHECK(DrumEngineTestAccess::rangeExact(engine,0,64,tokens,unavailable));DrumEngineTestAccess::end(engine);CHECK(unavailable&&audio.getMagnitude(0,64)==0);
}
}
int main()
{
    validation();literalPlayback();rateTuneEnvelope();liveSettingsAndLifetime();overlapChokeSteal();eventsAndInvalidOutput();partitionAndSpectrum();realtimeHeap();rangeSessions();
    exactRangeAuthority();
    std::cout<<"checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures==0?0:1;
}
