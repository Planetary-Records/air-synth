#include "DrumEngine.h"
#include "RoutedSynthEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <thread>
#include <system_error>
#include <limits>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <optional>
#include <vector>

using namespace airack::drums;
#ifndef NDEBUG
namespace airack::drums {
struct DrumQualityTestAccess {
    static const auto* table(const DrumEngine& e){return e.readTable_;}
    static auto hits(const DrumEngine& e){return e.hits_;}
    static auto state(const DrumEngine& e){std::array<double,32*19> result{};std::size_t n=0;for(const auto& h:e.hits_){
        for(double value:{h.step,h.readScale,h.readRadius,double(h.elapsed),double(h.naturalFrames),double(h.begin),double(h.end),double(h.rank),double(h.release),double(h.channel),double(h.stealIntervals),double(h.stealRemaining),double(h.hasRenderedContribution),h.lastContribution[0],h.lastContribution[1],h.stealFrom[0],h.stealFrom[1],double(h.attack),double(h.decay)})result[n++]=value;}return result;}
    struct MemoSnapshot {
        bool available=false,valid=false;std::size_t bytes=0;
        double readScale=0,readRadius=0,phase=0,normalization=0;int relativeFirst=0,relativeLast=0;
        std::array<double,1153> values{};
    };
    template<class E>static MemoSnapshot memo(const E& e){MemoSnapshot s;
        if constexpr(requires{e.readWeights_;}){const auto& c=e.readWeights_;s.available=true;s.valid=c.valid;s.bytes=sizeof(c);
            s.readScale=c.readScale;s.readRadius=c.readRadius;s.phase=c.phase;s.normalization=c.normalization;
            s.relativeFirst=c.relativeFirst;s.relativeLast=c.relativeLast;s.values=c.values;}return s;}
    template<class E>static void invalidateMemo(E& e){if constexpr(requires{e.readWeights_;})e.readWeights_.valid=false;}
    static std::array<double,2> query(DrumEngine& e,std::size_t slot){const auto& h=e.hits_[slot];
        return e.reconstructed(h,std::min(double(h.begin)+double(h.elapsed)*h.step,double(h.end-1)));}
    static std::size_t hitBytes(){return sizeof(DrumEngine::Hit);}
    static bool begin(DrumEngine& e,juce::AudioBuffer<float>& b,const juce::MidiBuffer& m,int factor=1){return e.beginRender(b,m.cbegin(),m.cend(),factor);}
    static bool range(DrumEngine& e,int first,int count){return e.renderRange(first,count);}
    static bool beginExact(DrumEngine& e,juce::AudioBuffer<float>& b,std::span<const RenderEvent> events){return e.beginRender(b,events,0,1);}
    static bool rangeExact(DrumEngine& e,int first,int count,const std::array<std::uint64_t,16>& tokens,bool& unavailable){return e.renderRange(first,count,true,true,tokens,unavailable);}
    static void end(DrumEngine& e){e.endRender();}
};
}
namespace airack {
struct RoutedDrumTestAccess {
    static bool prepareWithLaunch(RoutedSynthEngine& e,double r,int b,RenderQuality q,detail::DspPipeline::Launch launch){return e.prepareWithPipelineLaunch(r,b,q,{},launch);}
    static auto snapshot(const RoutedSynthEngine& e){return e.drumTestSnapshot();}
    static void hook(RoutedSynthEngine& e,void* context,void (*fn)(void*,unsigned)){e.setDrumSourceBorrowTestHook(context,fn);}
};
}
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label,int line) {
    ++checks;if(!ok){++failures;std::cerr<<"FAIL "<<line<<" "<<label<<'\n';}
}

#define CHECK(x) check(bool(x),#x,__LINE__)
using Audio=std::array<std::vector<float>,2>;
struct Event{int offset;juce::MidiMessage message;};
juce::MidiMessage on(int note=36) {
    return juce::MidiMessage::noteOn(1,note,juce::uint8(127));
}
AssetPtr asset(std::vector<float> data,std::uint32_t channels=1) {
    AssetPtr result;AssetError error=AssetError::none;
    CHECK(makeSampleAsset(data,channels,result,error));
    CHECK(error==AssetError::none);return result;
}
RackSettings rack(const SampleAsset* sample) {
    RackSettings p;p.pads[0].sample=sample;return p;
}
void prepare(DrumEngine& e,const RackSettings& p,double rate=48000) {
    CHECK(e.setSettings(p));CHECK(e.prepare(rate,512));
}
Audio render(DrumEngine& e,int frames,const std::vector<Event>& events={},int block=512) {
    Audio result;for(auto& channel:result)channel.resize(std::size_t(frames));
    std::size_t eventCursor=0;
    for(int first=0;first<frames;first+=block){
        const int count=std::min(block,frames-first);
        juce::AudioBuffer<float> output(2,count);juce::MidiBuffer midi;
        while(eventCursor<events.size()&&events[eventCursor].offset<first+count){
            const auto& event=events[eventCursor++];if(event.offset>=first)midi.addEvent(event.message,event.offset-first);}
        e.render(output,midi);
        for(int c=0;c<2;++c)std::copy_n(output.getReadPointer(c),count,result[c].data()+first);
    }
    return result;
}
double amplitude(const std::vector<float>& y,int first,int count,double cycles) {
    double re=0,im=0;
    for(int n=0;n<count;++n){const double angle=2*std::numbers::pi*cycles*(first+n);
        re+=y[std::size_t(first+n)]*std::cos(angle);
        im+=y[std::size_t(first+n)]*std::sin(angle);}
    return 2*std::hypot(re,im)/count;
}
void aliasRed() {
    std::vector<float> samples(160000);
    for(std::size_t n=0;n<samples.size();++n)
        samples[n]=float(std::sin(2*std::numbers::pi*.375*double(n)));
    auto sample=asset(std::move(samples));auto p=rack(sample.get());
    p.pads[0].tuneSemitones=12;DrumEngine e;prepare(e,p);
    const auto y=render(e,4608,{{0,on()}});
    const double folded=amplitude(y[0],256,4096,.25);
    std::cout<<"QUALITY_ALIAS amplitude="<<folded<<'\n';CHECK(folded<=.001);
}
void stealRed() {
    auto a=asset(std::vector<float>(4000,.25f));
    auto b=asset(std::vector<float>(4000,.5f));
    auto p=rack(a.get());p.pads[1].sample=b.get();DrumEngine e;prepare(e,p);
    std::vector<Event> fill{{0,on()}};for(int i=0;i<31;++i)fill.push_back({0,on(37)});
    CHECK(render(e,1,fill)[0][0]==15.75f);p.pads[0].sample=nullptr;CHECK(e.setSettings(p));
    const auto y=render(e,241,{{0,on(37)}});
    CHECK(y[0][0]==15.75f);CHECK(y[0][240]==16.f);
    for(int n=0;n<=240;++n)CHECK(std::abs(double(y[0][n])-(15.75+.25*n/240.))<2e-6);
    CHECK(e.activeHitCount()==32);CHECK(!e.references(a.get()));
}
}


namespace {
using namespace airack;
constexpr int fftSize=4096;
constexpr double tau=2*std::numbers::pi;
bool near(double a,double b,double epsilon=2e-6){return std::abs(a-b)<=epsilon;}
int duration(const PadSettings& p,double rate){const auto frames=p.sample->frames();
    const auto begin=std::min(frames-1,std::uint32_t(std::floor(double(p.trimStart)*frames)));
    const auto end=std::clamp(std::uint32_t(std::ceil(double(p.trimEnd)*frames)),begin+1,frames);
    return int(std::ceil(double(end-begin)*rate/(48000*std::exp2(double(p.tuneSemitones)/12))));
}
int capturedBegin(const PadSettings& p){return int(std::floor(double(p.trimStart)*p.sample->frames()));}
double spectrum(const std::vector<double>& residual){
    std::array<float,2*fftSize> data{};double sum=0;
    for(int n=0;n<fftSize;++n){const double x=tau*n/(fftSize-1);
        const double w=.35875-.48829*std::cos(x)+.14128*std::cos(2*x)-.01168*std::cos(3*x);
        sum+=w;data[n]=float(residual[n]*w);}
    juce::dsp::FFT fft(12);fft.performFrequencyOnlyForwardTransform(data.data(),true);
    double peak=0;for(int k=0;k<=fftSize/2;++k)peak=std::max(peak,double(data[k])*(k==0||k==fftSize/2?1:2)/sum);
    return peak;
}
void waveform(const std::vector<float>& y,int first,int onset,double step,int begin,double cycles,double phase,bool stop,const char* route){
    std::vector<double> residual(fftSize);double worst=0,ss=0,cc=0,sc=0,ys=0,yc=0;
    for(int i=0;i<fftSize;++i){const int n=first+i;const double theta=phase+tau*cycles*(begin+(n-onset)*step);
        const double s=std::sin(theta),c=std::cos(theta),expected=stop?0:s;
        residual[i]=y[n]-expected;worst=std::max(worst,std::abs(residual[i]));ss+=s*s;cc+=c*c;sc+=s*c;ys+=y[n]*s;yc+=y[n]*c;}
    const double det=ss*cc-sc*sc,amplitude=std::hypot((ys*cc-yc*sc)/det,(yc*ss-ys*sc)/det);
    const double residualPeak=spectrum(residual);
    std::cout<<"QUALITY_WAVE route="<<route<<" step="<<step<<" cycles="<<cycles<<" phase="<<phase<<" stop="<<stop
        <<" max_error="<<worst<<" magnitude="<<amplitude<<" residual="<<residualPeak<<'\n';
    CHECK(worst<=.001);CHECK(residualPeak<=.001);if(!stop)CHECK(std::abs(20*std::log10(amplitude))<=.05);
}
AssetPtr sine(double cycles,double phase=0){std::vector<float> samples(160000);
    for(std::size_t n=0;n<samples.size();++n)samples[n]=float(std::sin(phase+tau*cycles*n));return asset(std::move(samples));}
void stationary(){
    struct Row{double rate;float tune;double cycles;bool stop=false;};
    const std::array rows{Row{44100,0,.05},Row{44100,0,.30},Row{96000,0,.05},Row{96000,0,.30},
        Row{48000,-7.25f,.025},Row{48000,-7.25f,.10},Row{44100,3,.025},Row{44100,3,.10},
        Row{96000,11.5f,.025},Row{96000,11.5f,.10},Row{48000,.00001f,.30},Row{48000,-.00001f,.30},
        Row{48000,12,.375,true},Row{8000,24,1125./48000,true},Row{8000,24,375./48000},
        Row{96000,0,14062.5/48000},Row{768000,-24,.05},Row{768000,24,.05}};
    for(const auto row:rows)for(double phase:{0.,.137,std::numbers::pi/2}){
        auto a=sine(row.cycles,phase);auto p=rack(a.get());p.pads[0].trimStart=1024.f/160000;p.pads[0].trimEnd=159000.f/160000;p.pads[0].tuneSemitones=row.tune;
        const double step=48000/row.rate*std::exp2(double(row.tune)/12);const int first=7+int(std::ceil(24*std::max(1.,step)/step))+64;
        DrumEngine e;prepare(e,p,row.rate);const auto y=render(e,first+fftSize,{{7,on()}});
        waveform(y[0],first,7,step,capturedBegin(p.pads[0]),row.cycles,phase,row.stop,"standalone");
        if(row.stop){const double folded=row.rate==8000?3500/row.rate:.25;CHECK(amplitude(y[0],first,fftSize,folded)<=.001);}
        if(row.rate==96000&&row.cycles==14062.5/48000)CHECK(amplitude(y[0],first,fftSize,33937.5/96000)<=.001);
    }
}
void boundaries(){
    for(double rate:{8000.,44100.,96000.,768000.})for(int length:{1,2,7,1024})for(int shape:{0,1,2,3,4,5,6,7}){
        const float tune=rate==8000?24.f:rate==768000?-24.f:0.f;const unsigned channels=shape==5?2:1;
        std::vector<float> clean(4096*channels),poison(clean.size());
        for(int n=0;n<4096;++n)for(unsigned c=0;c<channels;++c){float value=0;
            if(n>=1024&&n<1024+length){const int i=n-1024;
                if(shape==0)value=.25f;else if((shape==1&&i==0)||(shape==2&&i==length/2)||(shape==3&&i==length-1))value=.5f;
                else if((shape==4&&i==length/2)||(shape==6&&i==0)||(shape==7&&i==length-1))value=-.5f;else if(shape==5)value=(i%2?.25f:-.5f)*(c?-1:1);}
            clean[n*channels+c]=value;poison[n*channels+c]=n>=1024&&n<1024+length?value:(n%2?16.f:-16.f);}
        auto a=asset(std::move(clean),channels),b=asset(std::move(poison),channels);auto p=rack(a.get());
        p.pads[0].trimStart=.25f;p.pads[0].trimEnd=float(1024+length)/4096;p.pads[0].tuneSemitones=tune;
        const int natural=duration(p.pads[0],rate);DrumEngine e,reference;prepare(e,p,rate);auto q=p;q.pads[0].sample=b.get();prepare(reference,q,rate);
        const auto y=render(e,7+natural+2,{{7,on()}}),z=render(reference,7+natural+2,{{7,on()}});CHECK(y==z);
        bool legal=true;double peak=0;for(int c=0;c<2;++c)for(int n=0;n<int(y[c].size());++n){const float v=y[c][n];legal&=std::isfinite(v);peak=std::max(peak,std::abs(double(v)));
            if(n<7||n>=7+natural)legal&=v==0;else if(shape==0)legal&=near(v,.25);
            if(channels==1)legal&=v==y[0][n];else legal&=v==-y[1-c][n];}
        CHECK(legal);CHECK(e.activeHitCount()==0);p.pads[0].sample=nullptr;CHECK(e.setSettings(p));CHECK(!e.references(a.get()));
        if(length==1024)std::cout<<"QUALITY_TRANSIENT rate="<<rate<<" shape="<<shape<<" peak="<<peak<<" natural="<<natural<<'\n';
    }
    auto a=asset(std::vector<float>(160000,.25f));
    for(double rate:{8000.,44100.,48000.,96000.,768000.})for(float tune:{-24.f,0.f,24.f,-.00001f,.00001f}){
        auto p=rack(a.get());p.pads[0].tuneSemitones=tune;DrumEngine e;prepare(e,p,rate);const auto y=render(e,64,{{0,on()}});
        CHECK(std::all_of(y[0].begin(),y[0].end(),[](float x){return near(x,.25);}));CHECK(e.activeHitCount()==1);
    }
}
double analyticInteriorWeight(double u){const auto i0=[](double x){double term=1,sum=1;for(int k=1;k<60;++k){const double t=x/(2*k);term*=t*t;sum+=term;}return sum;};
    const double z=.9*u;return .9*(z==0?1:std::sin(std::numbers::pi*z)/(std::numbers::pi*z))*i0(7.5*std::sqrt(1-u*u/576))/i0(7.5);}
double analyticWeight(double u){return u>=24?0:analyticInteriorWeight(u);}
void tableBounds(){
    DrumEngine unprepared;CHECK(DrumQualityTestAccess::table(unprepared)==nullptr);CHECK(!unprepared.prepare(7999,512));CHECK(DrumQualityTestAccess::table(unprepared)==nullptr);
    std::array<DrumEngine,4> engines;for(auto& e:engines)CHECK(e.prepare(48000,512));const auto* table=DrumQualityTestAccess::table(engines[0]);CHECK(table);
    CHECK(sizeof(table->values)==196616);CHECK(table->values.front()==.9);
    const double terminal=analyticInteriorWeight(24);CHECK(std::abs(table->values.back()-terminal)<=1e-14);
    CHECK(table->at(24)==0&&table->at(std::nextafter(24.,std::numeric_limits<double>::infinity()))==0);
    CHECK(std::isfinite(table->at(std::nextafter(24.,0.))));CHECK(std::abs(table->at(std::nextafter(24.,0.))-terminal)<=1e-12);
    std::cout<<"QUALITY_TABLE_ENDPOINT stored="<<table->values.back()<<" analytic_left_limit="<<terminal<<" at24="<<table->at(24)<<" just_inside="<<table->at(std::nextafter(24.,0.))<<'\n';
    CHECK(std::all_of(table->values.begin(),table->values.end(),[](double x){return std::isfinite(x);}));
    for(auto& e:engines){CHECK(DrumQualityTestAccess::table(e)==table);CHECK(!e.prepare(7999,512));CHECK(DrumQualityTestAccess::table(e)==table);e.reset();CHECK(e.prepare(96000,512));CHECK(DrumQualityTestAccess::table(e)==table);}
    double minRaw=1e9,maxRaw=0,minScaled=1e9,maxScaled=0,maxL1=0,maxAnalytic=0;int maxVisits=0;
    for(double m:{1.,1.00001,48000./44100,1.5,2.,6.,24.})for(int p=0;p<=260;++p){
        const double fraction=p==260?0:p<257?(p+.137)/257:std::nextafter((p-256)/1024.,p==259?0.:1.);
        const double position=1024+fraction,scale=1/m;const auto first=std::int64_t(std::ceil(position-24*m)),last=std::int64_t(std::floor(position+24*m));
        CHECK(last-first+1<=1153);maxVisits=std::max(maxVisits,int(last-first+1));double sum=0,l1=0,exactSum=0;
        for(auto j=first;j<=last;++j){const double u=std::abs((j-position)*scale);if(u<24){const auto index=std::size_t(u*1024);CHECK(index<=24575&&index+1<=24576);}sum+=table->at(u);l1+=std::abs(table->at(u));exactSum+=analyticWeight(u);}
        double normalized=0,error=0;for(auto j=first;j<=last;++j){const double u=std::abs((j-position)*scale),w=table->at(u)/sum;CHECK(std::isfinite(w));normalized+=w;error+=std::abs(w-analyticWeight(u)/exactSum);}
        CHECK(std::abs(normalized-1)<=2e-12);CHECK(sum/m>=.99&&sum/m<=1.01);CHECK(l1/sum<=3);
        CHECK(error<=2e-5);
        std::cout<<"QUALITY_ANALYTIC_GRID m="<<m<<" p="<<p<<" fraction="<<fraction<<" normalized_l1_error="<<error<<" scaled_sum="<<sum/m<<" normalized_sum_error="<<std::abs(normalized-1)<<" l1="<<l1/sum<<" visits="<<last-first+1<<" status="<<(error<=2e-5?"PASS":"FAILED")<<'\n';
        if(p==64||p==128||p==192){std::cout<<"QUALITY_ANALYTIC_REQUIRED m="<<m<<" fraction="<<fraction<<" normalized_l1_error="<<error<<'\n';}
        if(p<3)std::cout<<"QUALITY_ANALYTIC_ORIGINAL status="<<(error<=2e-5?"PASS":"FAILED")<<" m="<<m<<" fraction="<<fraction<<" normalized_l1_error="<<error<<" limit=2e-5 lookup_at24_is_zero=1 stored_endpoint_is_analytic_left_limit=1"<<'\n';
        minRaw=std::min(minRaw,sum);maxRaw=std::max(maxRaw,sum);minScaled=std::min(minScaled,sum/m);maxScaled=std::max(maxScaled,sum/m);maxL1=std::max(maxL1,l1/sum);maxAnalytic=std::max(maxAnalytic,error);
    }
    std::cout<<"QUALITY_TABLE raw_min="<<minRaw<<" raw_max="<<maxRaw<<" scaled_min="<<minScaled<<" scaled_max="<<maxScaled<<" max_visits="<<maxVisits<<" l1="<<maxL1<<" analytic_l1_error="<<maxAnalytic<<" hit_bytes="<<DrumQualityTestAccess::hitBytes()<<'\n';
    auto a=asset(std::vector<float>(4000,.25f));for(const auto row:std::array<std::pair<double,float>,3>{{{8000.,24.f},{768000.,-24.f},{44100.,0.f}}}){
        auto p=rack(a.get());p.pads[0].tuneSemitones=row.second;DrumEngine e;prepare(e,p,row.first);(void)render(e,1,{{0,on()}});const auto hits=DrumQualityTestAccess::hits(e);
        CHECK(hits[0].step>=1./64&&hits[0].step<=24);CHECK(hits[0].readScale==1/std::max(1.,hits[0].step));CHECK(hits[0].readRadius==24*std::max(1.,hits[0].step));}
}
std::vector<Event> fullPool(int first=36,int rest=38){std::vector<Event> events{{0,on(first)}};for(int n=1;n<32;++n)events.push_back({0,on(rest)});return events;}
void stealLaws(){
    auto old=asset(std::vector<float>(16000,-.25f)),filler=asset(std::vector<float>(16000,.125f)),flat=asset(std::vector<float>(16000,.5f));
    for(double rate:{44100.,48000.,96000.}){
        auto p=rack(old.get());p.pads[1].sample=flat.get();p.pads[2].sample=filler.get();DrumEngine e;prepare(e,p,rate);
        const double base=31*.125;CHECK(render(e,1,fullPool())[0][0]==float(base-.25));p.pads[0].sample=nullptr;CHECK(e.setSettings(p));
        const int intervals=int(std::ceil(.005*rate));const auto y=render(e,intervals+2,{{0,on(37)}});
        bool correct=true;for(int n=0;n<int(y[0].size());++n)correct&=near(y[0][n],base+(-.25*(1-std::min(n,intervals)/double(intervals))+.5*std::min(n,intervals)/double(intervals)));
        CHECK(correct);CHECK(!e.references(old.get()));CHECK(e.activeHitCount()==32);
    }
    // The target advances normally during blending; no held or delayed read cursor.
    std::vector<float> ramp(16000);for(int n=0;n<16000;++n)ramp[n]=float(.01+.0001*n);auto moving=asset(ramp);
    for(int support:{1,2,240,241,4000})for(bool envelope:{false,true}){
        auto p=rack(old.get());p.pads[1].sample=moving.get();p.pads[2].sample=filler.get();
        if(envelope){p.pads[1].attackSeconds=1.f/48000;p.pads[1].decaySeconds=float(support)/48000;}
        else p.pads[1].trimEnd=std::nextafter(float(support)/16000,0.f);
        const auto attack=std::uint32_t(std::ceil(double(p.pads[1].attackSeconds)*48000)),decay=std::uint32_t(std::ceil(double(p.pads[1].decaySeconds)*48000));
        const int legal=envelope?int(attack+decay):duration(p.pads[1],48000);const int intervals=std::min(240,legal-1);
        if(!envelope)CHECK(legal==support);
        DrumEngine e;prepare(e,p);(void)render(e,1,fullPool());p.pads[0].sample=nullptr;CHECK(e.setSettings(p));const auto y=render(e,legal+1,{{0,on(37)}});
        bool correct=true;for(int n=0;n<=legal;++n){double expected=31*.125;
            if(n<legal){double env=attack&&n<int(attack)?double(n)/attack:1.;if(decay&&n>=int(attack))env*=1-double(n-attack)/decay;
                const double alpha=intervals?std::min(1.,double(n)/intervals):1;expected+=(1-alpha)*(-.25)+alpha*ramp[n]*env;}
            correct&=near(y[0][n],expected);}
        CHECK(correct);CHECK(!e.references(old.get()));CHECK(e.activeHitCount()==31);
        if(!envelope&&support==1)CHECK(near(y[0][0],31*.125+ramp[0])&&near(y[0][1],31*.125));
        if(!envelope&&support==2)CHECK(near(y[0][0],31*.125-.25)&&near(y[0][1],31*.125+ramp[1])&&near(y[0][2],31*.125));
        p.pads[1].sample=nullptr;CHECK(e.setSettings(p));CHECK(!e.references(moving.get()));
        if(!envelope)std::cout<<"QUALITY_STEAL_SUPPORT requested="<<support<<" actual="<<legal<<" intervals="<<intervals<<" first="<<y[0][0]<<" final_legal="<<y[0][legal-1]<<" next="<<y[0][legal]<<'\n';
    }
    // Initially inaudible same-offset replacements must not fade from zero.
    for(int count:{33,64,96}){auto p=rack(flat.get());DrumEngine e;prepare(e,p);std::vector<Event> events(count,{0,on()});CHECK(render(e,1,events)[0][0]==16.f);}
    // Even a one-frame unrendered replacement forwards the prior actual scalar.
    auto one=asset({.75f});auto p=rack(old.get());p.pads[1].sample=flat.get();p.pads[2].sample=one.get();DrumEngine repeated;prepare(repeated,p);
    std::vector<Event> first(32,{0,on()});CHECK(render(repeated,1,first)[0][0]==-8.f);
    std::vector<Event> replacements(32,{0,on(38)});for(int n=0;n<64;++n)replacements.push_back({0,on(37)});
    const auto y=render(repeated,242,replacements);CHECK(y[0][0]==-8.f);for(int n=0;n<=240;++n)CHECK(near(y[0][n],-8+24*n/240.));CHECK(y[0][241]==16.f);
}
void adversarialSteals(){
    std::vector<float> a(8000),b(8000);for(int n=0;n<4000;++n){a[2*n]=.25f;a[2*n+1]=-.25f;b[2*n]=-.5f;b[2*n+1]=.5f;}
    auto positive=asset(a,2),negative=asset(b,2);auto p=rack(positive.get());p.pads[1].sample=negative.get();DrumEngine e;prepare(e,p);
    std::vector<Event> fill;for(int n=0;n<32;++n)fill.push_back({0,on(36+n%2)});(void)render(e,1,fill);
    std::array<std::array<double,2>,32> held{};std::array<double,2> expected{};
    for(std::size_t slot=0;slot<held.size();++slot){held[slot]={slot%2?-.5:.25,slot%2?.5:-.25};for(int c=0;c<2;++c)expected[c]+=held[slot][c];}
    std::vector<Event> events;events.reserve(65536);for(int frame=0;frame<1024;++frame)for(int n=0;n<64;++n)events.push_back({frame,on(36+(frame+n)%2)});
    const auto y=render(e,1024,events);bool convex=true;for(int frame=0;frame<1024;++frame)for(int c=0;c<2;++c)convex&=near(y[c][frame],expected[c])&&std::abs(y[c][frame])<=16;
    // Each frame makes two complete oldest-first turns before any read: k=0,
    // so the independent convex recurrence retains each original held scalar.
    const auto hits=DrumQualityTestAccess::hits(e);for(std::size_t n=0;n<hits.size();++n)for(int c=0;c<2;++c)convex&=hits[n].lastContribution[c]==held[n][c]&&hits[n].elapsed==1;
    CHECK(convex);CHECK(e.activeHitCount()==32);std::array<const SampleAsset*,48> references{};e.borrowedReferences(references);CHECK(references.size()==48);
    for(int block:{1,7,64,511}){DrumEngine other;prepare(other,p);(void)render(other,1,fill);CHECK(render(other,1024,events,block)==y);}
}
void gainAndChoke(){
    auto a=asset(std::vector<float>(4000,.25f)),b=asset(std::vector<float>(4000,-.5f));auto p=rack(a.get());p.pads[0].gainDb=-6;p.pads[0].pan=-1;
    p.pads[1].sample=b.get();p.pads[1].gainDb=6;p.pads[1].pan=1;p.level=.5;p.pan=.25;DrumEngine e;prepare(e,p);std::vector<Event> fill(32,{0,on()});(void)render(e,1,fill);
    const double old=.25*std::pow(10.,-6./20),newGain=std::pow(10.,6./20);bool correct=true;
    for(int n=0;n<=240;++n){if(n==7){p.pads[1].gainDb=0;CHECK(e.setSettings(p));}if(n==13){p.level=.8f;CHECK(e.setSettings(p));}
        const auto y=render(e,1,n==0?std::vector<Event>{{0,on(37)}}:std::vector<Event>{});const double alpha=n/240.;
        const double g=n<7?newGain:newGain+(1-newGain)*(n-6)/240.;const double kit=n<13?.5:.5+(double(p.level)-.5)*(n-12)/240.;
        correct&=near(y[0][0],(31*old+(1-alpha)*old)*kit*.75)&&near(y[1][0],alpha*(-.5)*g*kit);}
    CHECK(correct);
    // A release attenuates both terms and never extends the preexisting boundary.
    auto shortFiller=asset({.125f,.125f});p=rack(a.get());p.pads[1].sample=b.get();p.pads[1].chokeGroup=1;p.pads[2].sample=b.get();p.pads[2].chokeGroup=1;p.pads[2].pan=1;p.pads[3].sample=shortFiller.get();
    prepare(e,p);(void)render(e,1,fullPool(36,39));const auto start=render(e,7,{{0,on(37)}});CHECK(start[0][0]==4.125f);
    const auto released=render(e,241,{{0,on(38)},{10,on(38)}});bool choke=true;
    for(int n=0;n<240;++n){const int k=n+7;const double alpha=std::min(1.,k/240.),release=(240-n)/240.;
        choke&=near(released[0][n],((1-alpha)*.25+alpha*(-.5))*release);}
    CHECK(choke);CHECK(released[0][240]==0);
    p=rack(a.get());p.pads[0].chokeGroup=1;p.pads[1].sample=b.get();prepare(e,p);(void)render(e,10,fill);
    const auto capturedRelease=render(e,1,{{0,on(37)}});
    CHECK(near(capturedRelease[0][0],30*.25*230/240.+.25+.25*231/240.));
}
void capturedAndPartitions(){
    auto a=sine(.025,.137),b=sine(.1);auto p=rack(a.get());p.pads[0].tuneSemitones=3;p.pads[0].trimStart=.25;p.pads[0].trimEnd=.75;
    const int natural=duration(p.pads[0],44100);DrumEngine edited,reference;prepare(edited,p,44100);prepare(reference,p,44100);
    CHECK(render(edited,73,{{0,on()}})==render(reference,73,{{0,on()}}));auto q=p;q.pads[0].sample=b.get();q.pads[0].tuneSemitones=-7.25;q.pads[0].trimStart=.1f;q.pads[0].trimEnd=.9f;q.pads[0].attackSeconds=.001f;q.pads[0].decaySeconds=.01f;
    CHECK(edited.setSettings(q));CHECK(edited.references(a.get()));CHECK(render(edited,natural-74)==render(reference,natural-74));CHECK(edited.references(a.get()));CHECK(render(edited,1)==render(reference,1));CHECK(!edited.references(a.get()));
    DrumEngine newReference;prepare(newReference,q,44100);CHECK(render(edited,1024,{{0,on()}})==render(newReference,1024,{{0,on()}}));
    const auto timeline=[&](int block,bool ranges){DrumEngine engine;prepare(engine,p,44100);Audio out;for(auto& c:out)c.resize(1536);
        std::vector<Event> events;for(int n:{0,7,17,63,64,73,511,512,700})events.push_back({n,on()});events.push_back({900,juce::MidiMessage::controllerEvent(1,123,0)});events.push_back({901,on()});events.push_back({1200,juce::MidiMessage::midiStop()});events.push_back({1201,on()});
        for(int first=0;first<1536;){if(first==73)CHECK(engine.setSettings(q));int count=std::min(block,1536-first);if(first<73)count=std::min(count,73-first);
            juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;for(const auto& event:events)if(event.offset>=first&&event.offset<first+count)midi.addEvent(event.message,event.offset-first);
            if(ranges){CHECK(DrumQualityTestAccess::begin(engine,audio,midi));CHECK(!DrumQualityTestAccess::range(engine,1,1));CHECK(DrumQualityTestAccess::range(engine,0,0));for(int n=0;n<count;){const int size=std::min(17,count-n);CHECK(DrumQualityTestAccess::range(engine,n,size));n+=size;}DrumQualityTestAccess::end(engine);}else engine.render(audio,midi);
            for(int c=0;c<2;++c)std::copy_n(audio.getReadPointer(c),count,out[c].data()+first);first+=count;}return std::pair{out,DrumQualityTestAccess::state(engine)};};
    const auto expected=timeline(512,false);for(int block:{1,7,64,511,512}){CHECK(timeline(block,false)==expected);CHECK(timeline(block,true)==expected);}
    // Invalid outputs cannot consume a stop or advance an ongoing steal.
    auto flat=asset(std::vector<float>(4000,.25f));auto p2=rack(flat.get());DrumEngine e,r;prepare(e,p2);prepare(r,p2);std::vector<Event> fill(32,{0,on()});CHECK(render(e,1,fill)==render(r,1,fill));CHECK(render(e,1,{{0,on()}})==render(r,1,{{0,on()}}));
    juce::MidiBuffer stop;stop.addEvent(juce::MidiMessage::midiStop(),0);juce::AudioBuffer<float> empty(2,0),mono(1,4);std::array<float,10> words{};float* ptr[]{words.data(),words.data()+1};juce::AudioBuffer<float> alias(ptr,2,4);e.render(empty,stop);e.render(mono,stop);e.render(alias,stop);CHECK(render(e,16)==render(r,16));
}
RoutedPatch routedKit(unsigned banks=1){RoutedPatch p;p.synth.masterLevel=1;p.outputGainDb=0;p.outputProtection=false;
    for(unsigned n=0;n<banks;++n){if(n)CHECK(editRackLayout(p.layout,{RackEditKind::addSynth,std::uint8_t(n),masterChain,std::uint8_t(n)},p.layout)==RackEditResult::applied);
        p.synth.layers[n].enabled=true;p.synth.layers[n].level=1;p.synth.layers[n].pan=0;p.drumSource[n]=true;}return p;}
Audio routedRender(RoutedSynthEngine& e,int frames,const std::vector<Event>& events={},int block=512){Audio result;for(auto& c:result)c.resize(frames);
    for(int first=0;first<frames;first+=block){const int count=std::min(block,frames-first);juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;
        for(const auto& event:events)if(event.offset>=first&&event.offset<first+count)midi.addEvent(event.message,event.offset-first);e.render(audio,midi);
        for(int c=0;c<2;++c)std::copy_n(audio.getReadPointer(c),count,result[c].data()+first);}return result;}
void routedQuality(){
    struct Row{float tune;double cycles;bool stop;};
    for(double rate:{44100.,48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        for(const auto row:std::array<Row,4>{{{0,.05,false},{0,.30,false},{12,.375,true},{12,.05,false}}}){
            const double hostCycles=row.cycles*48000/rate*std::exp2(double(row.tune)/12);if(!row.stop&&hostCycles>=.4)continue;
            // The pitch-up stop row is below Nyquist at 96k, so measure the desired tone there.
            const bool stop=row.stop&&hostCycles>=.5;auto sample=sine(row.cycles,.137);BorrowedDrumAssignments bindings{};bindings[0][0]=sample.get();
            auto p=routedKit();p.drumPads[0][0].tuneSemitones=row.tune;p.drumPads[0][0].trimStart=1024.f/160000;p.drumPads[0][0].trimEnd=159000.f/160000;
            RoutedSynthEngine e;CHECK(e.setPatch(p,bindings));CHECK(e.prepare(rate,512,quality));e.reset();CHECK(e.dspWorkerStats().prepared==3);
            const int latency=e.latencySamples(),first=17+latency+1024;const auto y=routedRender(e,first+fftSize,{{17,on()}});
            waveform(y[0],first,17+latency,48000/rate*std::exp2(double(row.tune)/12),int(std::floor(double(p.drumPads[0][0].trimStart)*160000)),row.cycles,.137,stop,quality==RenderQuality::higher?"RoutedHigher":"RoutedNormal");
        }
        // Real poison exclusion and onset geometry also pass through the outer Higher filter.
        std::vector<float> clean(4096),poison(4096);for(int n=0;n<4096;++n){clean[n]=n>=1024&&n<2048?.25f:0;poison[n]=n>=1024&&n<2048?.25f:n%2?16.f:-16.f;}
        auto a=asset(clean),b=asset(poison);auto p=routedKit();p.drumPads[0][0].trimStart=.25f;p.drumPads[0][0].trimEnd=.5f;p.drumPads[0][0].tuneSemitones=12;
        BorrowedDrumAssignments x{},z{};x[0][0]=a.get();z[0][0]=b.get();RoutedSynthEngine left,right;CHECK(left.setPatch(p,x)&&right.setPatch(p,z));CHECK(left.prepare(rate,512,quality)&&right.prepare(rate,512,quality));left.reset();right.reset();CHECK(routedRender(left,2048,{{17,on()}})==routedRender(right,2048,{{17,on()}}));
        // Lower-frequency unity/DC gain witness, with integer host latency retained.
        auto dc=asset(std::vector<float>(16000,.25f));x[0][0]=dc.get();p=routedKit();p.synth.layers[0].level=.5f;p.synth.masterLevel=.5f;
        RoutedSynthEngine gain;CHECK(gain.setPatch(p,x));CHECK(gain.prepare(rate,512,quality));gain.reset();const auto y=routedRender(gain,2048,{{17,on()}});CHECK(near(y[0][1024],.0625));
        if(quality==RenderQuality::realtime){CHECK(gain.latencySamples()==0);CHECK(y[0][16]==0&&near(y[0][17],.0625));}
        else CHECK(gain.latencySamples()>0);
    }
}
std::atomic<int> liveWorkers{0},exitedWorkers{0};unsigned launches=0,failAt=0;
std::thread failingLaunch(detail::DspPipeline::Entry entry,detail::DspPipeline* pipeline,unsigned worker){
    if(launches++==failAt)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return std::thread([=]{++liveWorkers;entry(pipeline,worker);--liveWorkers;++exitedWorkers;});}
void workerEffects(RoutedPatch& p){for(unsigned slot=0;slot<2;++slot)CHECK(editRackLayout(p.layout,{RackEditKind::addEffect,std::uint8_t(slot),masterChain,std::uint8_t(slot),EffectKind::dynamicEq},p.layout)==RackEditResult::applied);}
void routedWorkers(){auto sample=sine(.025);BorrowedDrumAssignments bindings{};auto p=routedKit(4);workerEffects(p);
    for(unsigned bank=0;bank<4;++bank){bindings[bank][0]=sample.get();p.drumPads[bank][0].tuneSemitones=3;p.drumPads[bank][0].midiNote=36+int(bank);}
    std::vector<Event> events;for(int n:{0,7,17,63,64,73,511,512,700})events.push_back({n,on(36+n%4)});
    for(auto q:{RenderQuality::realtime,RenderQuality::higher}){RoutedSynthEngine normal;CHECK(normal.setPatch(p,bindings));CHECK(normal.prepare(48000,512,q));normal.reset();
        const auto expected=routedRender(normal,1536,events);CHECK(normal.dspWorkerStats().prepared==3&&normal.dspWorkerStats().blocks>0);
        for(unsigned failure=0;failure<3;++failure){failAt=failure;launches=0;exitedWorkers=0;liveWorkers=0;RoutedSynthEngine serial;CHECK(serial.setPatch(p,bindings));CHECK(RoutedDrumTestAccess::prepareWithLaunch(serial,48000,512,q,failingLaunch));serial.reset();
            CHECK(serial.dspWorkerStats().prepared==0&&liveWorkers==0&&exitedWorkers==int(failure)&&launches==failure+1);CHECK(routedRender(serial,1536,events,7)==expected);}
        RoutedSynthEngine small;CHECK(small.setPatch(p,bindings));CHECK(small.prepare(48000,512,q));(void)routedRender(small,31,{{0,on()}},31);CHECK(small.dspWorkerStats().blocks==0);
    }
}
void clears(){auto a=asset(std::vector<float>(4000,.25f));auto b=asset(std::vector<float>(4000,-.5f));
    for(int operation=0;operation<7;++operation){auto p=rack(a.get());p.pads[1].sample=b.get();DrumEngine e;prepare(e,p);std::vector<Event> fill(32,{0,on()});(void)render(e,1,fill);(void)render(e,7,{{0,on(37)}});
        if(operation==0)e.reset();else if(operation==1){p.enabled=false;CHECK(e.setSettings(p));}else if(operation==2)CHECK(e.prepare(96000,512));else if(operation==3)e.clearBorrowedSamplesQuiescent();
        else (void)render(e,1,{{0,operation==4?juce::MidiMessage::midiStop():juce::MidiMessage::controllerEvent(1,operation==5?120:123,0)}});
        CHECK(e.activeHitCount()==0);bool clear=true;for(const auto& h:DrumQualityTestAccess::hits(e)){clear&=!h.sample&&!h.hasRenderedContribution&&!h.stealIntervals&&!h.stealRemaining;for(int c=0;c<2;++c)clear&=h.lastContribution[c]==0&&h.stealFrom[c]==0;}CHECK(clear);
        CHECK(render(e,7)[0]==std::vector<float>(7,0));}
    auto patch=routedKit();BorrowedDrumAssignments bindings{};bindings[0][0]=a.get();bindings[0][1]=b.get();RoutedSynthEngine routed;
    CHECK(routed.setPatch(patch,bindings));CHECK(routed.prepare(48000,512));routed.reset();std::vector<Event> fill(32,{0,on()});(void)routedRender(routed,1,fill);(void)routedRender(routed,7,{{0,on(37)}});
    patch.drumSource[0]=false;CHECK(routed.setPatch(patch));(void)routedRender(routed,512);CHECK(RoutedDrumTestAccess::snapshot(routed).hits[0]==0);
    patch.drumSource[0]=true;CHECK(routed.setPatch(patch));CHECK(routedRender(routed,512)[0]==std::vector<float>(512,0));CHECK(routedRender(routed,1,{{0,on()}})[0][0]==.25f);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool callbackGuard=false;std::array<std::atomic<unsigned>,2> heapAllocs{},heapFrees{},lifecycleAllocs{},lifecycleFrees{};std::atomic<unsigned> workerBorrows{0},lifecyclePhase{0};
int heapHook(int kind,void*,std::size_t,int,long,const unsigned char*,int){const auto phase=lifecyclePhase.load();const bool worker=detail::DspPipeline::renderingOnThisThread();if(phase){const auto slot=phase-1;if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++lifecycleAllocs[slot];if(kind==_HOOK_FREE)++lifecycleFrees[slot];}else if(callbackGuard||worker){const int slot=worker?1:0;if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++heapAllocs[slot];if(kind==_HOOK_FREE)++heapFrees[slot];}return 1;}
void borrowHook(void*,unsigned){if(detail::DspPipeline::renderingOnThisThread())++workerBorrows;}
#endif
void offAudioLifecycle(){
#if defined(_MSC_VER) && defined(_DEBUG)
    std::optional<DrumEngine> direct;std::optional<RoutedSynthEngine> routed;
    for(auto& count:lifecycleAllocs)count=0;for(auto& count:lifecycleFrees)count=0;
    auto* previous=_CrtSetAllocHook(heapHook);lifecyclePhase=1;direct.emplace();routed.emplace();lifecyclePhase=0;
    const bool prepared=direct->prepare(48000,512)&&routed->prepare(48000,512);
    lifecyclePhase=2;routed.reset();direct.reset();lifecyclePhase=0;_CrtSetAllocHook(previous);CHECK(prepared);
    std::cout<<"QUALITY_OFF_AUDIO constructor_alloc="<<lifecycleAllocs[0]<<" constructor_free="<<lifecycleFrees[0]<<" destructor_alloc="<<lifecycleAllocs[1]<<" destructor_free="<<lifecycleFrees[1]<<" prepare_excluded=1"<<'\n';
#endif
}
void realtimeGuard(){
#if defined(_MSC_VER) && defined(_DEBUG)
    auto a=asset(std::vector<float>(480000,.25f)),b=asset(std::vector<float>(960000,-.125f),2);auto p=rack(a.get());p.pads[0].tuneSemitones=24;DrumEngine direct;prepare(direct,p,8000);
    auto patch=routedKit(4);workerEffects(patch);BorrowedDrumAssignments bindings{};for(unsigned bank=0;bank<4;++bank){bindings[bank][0]=bank%2?a.get():b.get();patch.drumPads[bank][0].tuneSemitones=24;}
    RoutedSynthEngine routed;CHECK(routed.setPatch(patch,bindings));CHECK(routed.prepare(48000,512,RenderQuality::higher));routed.reset();RoutedDrumTestAccess::hook(routed,nullptr,borrowHook);
    juce::AudioBuffer<float> output(2,512);juce::MidiBuffer notes,empty,stop,cc;notes.ensureSize(2048);for(int n=0;n<64;++n)notes.addEvent(on(),n%17);stop.addEvent(juce::MidiMessage::midiStop(),17);cc.addEvent(juce::MidiMessage::controllerEvent(1,123,0),7);
    const std::array<RenderEvent,2> exact{{{1,17,RenderEventKind::exactPad,0,0,127,1},{0,73,RenderEventKind::stopAll}}};std::array<std::uint64_t,16> tokens{};tokens.fill(1);bool unavailable=false;
    const auto ownersA=a.use_count(),ownersB=b.use_count();bool ok=true;auto* previous=_CrtSetAllocHook(heapHook);for(auto& c:heapAllocs)c=0;for(auto& c:heapFrees)c=0;workerBorrows=0;callbackGuard=true;
    direct.render(output,notes);p.pads[0].sample=b.get();p.pads[0].chokeGroup=1;p.pads[0].gainDb=-6;p.pads[0].pan=.5f;ok&=direct.setSettings(p);direct.render(output,notes);direct.render(output,cc);direct.render(output,stop);direct.reset();
    ok&=DrumQualityTestAccess::begin(direct,output,notes);ok&=DrumQualityTestAccess::range(direct,0,17);ok&=DrumQualityTestAccess::range(direct,17,495);DrumQualityTestAccess::end(direct);
    ok&=DrumQualityTestAccess::beginExact(direct,output,exact);ok&=DrumQualityTestAccess::rangeExact(direct,0,64,tokens,unavailable);ok&=DrumQualityTestAccess::rangeExact(direct,64,448,tokens,unavailable);DrumQualityTestAccess::end(direct);ok&=!unavailable;
    for(int n=0;n<8;++n)routed.render(output,notes);routed.render(output,cc);routed.render(output,stop);routed.reset();
    callbackGuard=false;_CrtSetAllocHook(previous);CHECK(ok);CHECK(heapAllocs[0]==0&&heapFrees[0]==0);CHECK(heapAllocs[1]==0&&heapFrees[1]==0);CHECK(workerBorrows>0);CHECK(a.use_count()==ownersA&&b.use_count()==ownersB);
    CHECK(routed.dspWorkerStats().prepared==3&&routed.dspWorkerStats().blocks>0);
    std::cout<<"QUALITY_HEAP callback_alloc="<<heapAllocs[0]<<" callback_free="<<heapFrees[0]<<" worker_alloc="<<heapAllocs[1]<<" worker_free="<<heapFrees[1]<<" real_worker_borrows="<<workerBorrows<<'\n';
#else
    CHECK(false);
#endif
}
std::array<double,2> memoOracle(const PadSettings& p,double rate,int elapsed){
    const int frames=int(p.sample->frames()),channels=int(p.sample->channels());
    const int begin=std::min(frames-1,int(std::floor(double(p.trimStart)*frames)));
    const int end=std::clamp(int(std::ceil(double(p.trimEnd)*frames)),begin+1,frames);
    const double step=48000/rate*std::exp2(double(p.tuneSemitones)/12),position=std::min(begin+elapsed*step,double(end-1));
    const auto data=p.sample->samples();std::array<double,2> sum{};
    if(step==1){for(int c=0;c<2;++c)sum[c]=data[std::size_t(int(position)*channels+(channels==1?0:c))];return sum;}
    const double m=std::max(1.,step);double norm=0;
    for(int j=int(std::ceil(position-24*m));j<=int(std::floor(position+24*m));++j){const double w=analyticWeight(std::abs((j-position)*(1/m)));norm+=w;
        for(int c=0;c<2;++c)sum[c]+=w*data[std::size_t(std::clamp(j,begin,end-1)*channels+(channels==1?0:c))];}
    for(auto& v:sum)v/=norm;return sum;
}
struct MemoKey{double scale,radius,phase;int first,last;};
MemoKey memoKey(const PadSettings& p,double rate,int elapsed){
    const int frames=int(p.sample->frames()),begin=std::min(frames-1,int(std::floor(double(p.trimStart)*frames)));
    const int end=std::clamp(int(std::ceil(double(p.trimEnd)*frames)),begin+1,frames);
    const double step=48000/rate*std::exp2(double(p.tuneSemitones)/12),m=std::max(1.,step),position=std::min(begin+elapsed*step,double(end-1));const int base=int(position);
    return {1/m,24*m,position-base,int(std::ceil(position-24*m))-base,int(std::floor(position+24*m))-base};
}
bool memoMatches(const DrumQualityTestAccess::MemoSnapshot& s,const MemoKey& k){
    return s.available&&s.valid&&s.readScale==k.scale&&s.readRadius==k.radius&&s.phase==k.phase&&s.relativeFirst==k.first&&s.relativeLast==k.last;
}
void memoTrim(PadSettings& p,int begin,int length){const int frames=int(p.sample->frames());p.trimStart=float(begin)/frames;p.trimEnd=float(begin+length)/frames;
    for(int n=0;n<8&&int(std::floor(double(p.trimStart)*frames))!=begin;++n)p.trimStart=std::nextafter(p.trimStart,int(std::floor(double(p.trimStart)*frames))>begin?0.f:1.f);
    for(int n=0;n<8&&int(std::ceil(double(p.trimEnd)*frames))!=begin+length;++n)p.trimEnd=std::nextafter(p.trimEnd,int(std::ceil(double(p.trimEnd)*frames))>begin+length?0.f:1.f);
    CHECK(int(std::floor(double(p.trimStart)*frames))==begin);CHECK(int(std::ceil(double(p.trimEnd)*frames))==begin+length);
}
void memoLaws(){
    struct Case{double rate;float tune;int begin,length;double phaseTarget=-1;int phaseSide=0;};
    const std::array<Case,18> cases{{{96000,0,0,1},{8000,24,479998,2},{8000,0,478976,7},{32000,0,1024,1024},{24000,0,0,1024},
        {48000/1.00001,0,478976,1024},{44100,0,1,1024},{768000,-24,1024,7},{std::nextafter(48000.,0.),0,1,1024},
        {std::nextafter(48000.,96000.),0,1024,1024},{32000,0,478976,1024},
        {48000/(1+(64+.137)/257),0,0,1024,(64+.137)/257},
        {48000/(1+(128+.137)/257),0,0,1024,(128+.137)/257},
        {48000/(1+(192+.137)/257),0,0,1024,(192+.137)/257},
        {std::nextafter(48000/(513./1024),std::numeric_limits<double>::infinity()),0,0,1024,513./1024,-1},
        {std::nextafter(48000/(513./1024),0.),0,0,1024,513./1024,1},
        {std::nextafter(48000.,0.),0,0,1024,0,1},{std::nextafter(48000.,96000.),0,0,1024,1,-1}}};
    int fractionalWarm=0;
    DrumEngine e;int warm=0,replaced=0,maxVisits=0;CHECK(sizeof(DrumEngine)==15352);CHECK(DrumQualityTestAccess::hitBytes()==128);
    for(const auto& row:cases){CHECK(e.prepare(row.rate,512));AssetPtr retained;RackSettings p;
        for(int variant=0;variant<4;++variant){const unsigned channels=variant%2?2:1;std::vector<float> data(std::size_t(480000)*channels);
            for(int n=0;n<480000;++n)for(unsigned c=0;c<channels;++c){const bool selected=n>=row.begin&&n<row.begin+row.length;
                const double word=.0625*((n-row.begin)%7-3)+.03125*c;
                data[std::size_t(n)*channels+c]=selected?float(variant>=2?-.5*word+.03125:word):(variant>=2?(n%2?16.f:-16.f):0.f);}
            retained=asset(std::move(data),channels);p=rack(retained.get());p.pads[0].tuneSemitones=row.tune;memoTrim(p.pads[0],row.begin,row.length);
            e.reset();CHECK(e.setSettings(p));const auto key=memoKey(p.pads[0],row.rate,0);const auto prior=DrumQualityTestAccess::memo(e);
            if(variant){CHECK(memoMatches(prior,key));++warm;}else if(prior.available&&prior.valid&&!memoMatches(prior,key))++replaced;
            const auto y=render(e,1,{{0,on()}});const auto expected=memoOracle(p.pads[0],row.rate,0);
            for(int c=0;c<2;++c)CHECK(std::abs(y[c][0]-expected[c])<=2e-5);
            const auto after=DrumQualityTestAccess::memo(e);CHECK(after.available);CHECK(after.bytes==9272);CHECK(memoMatches(after,key));
            const int count=key.last-key.first+1;CHECK(count<=1153);maxVisits=std::max(maxVisits,count);
            if(after.available&&after.valid){double sum=0,exact=0,error=0;for(int n=0;n<count;++n){sum+=after.values[std::size_t(n)];exact+=analyticWeight(std::abs((key.first+n-key.phase)*key.scale));}
                CHECK(sum==after.normalization);for(int n=0;n<count;++n)error+=std::abs(after.values[std::size_t(n)]/sum-analyticWeight(std::abs((key.first+n-key.phase)*key.scale))/exact);CHECK(error<=2e-5);}
            p.pads[0].sample=nullptr;CHECK(e.setSettings(p));e.reset();CHECK(!e.references(retained.get()));
        }
        p.pads[0].sample=retained.get();CHECK(e.setSettings(p));(void)render(e,1,{{0,on()}});
        for(int elapsed=1;elapsed<=3;++elapsed){const bool active=elapsed<duration(p.pads[0],row.rate);const auto key=memoKey(p.pads[0],row.rate,elapsed);const auto prior=DrumQualityTestAccess::memo(e);
            if(active&&elapsed==1&&row.phaseTarget>=0){
                const auto live=DrumQualityTestAccess::hits(e)[0];CHECK(live.elapsed==std::uint32_t(elapsed));CHECK(live.step!=1);
                const double actualPosition=std::min(double(live.begin)+double(live.elapsed)*live.step,double(live.end-1)),actualPhase=actualPosition-int(actualPosition);
                CHECK(actualPhase==key.phase);CHECK(live.readScale==key.scale&&live.readRadius==key.radius);
                if(row.phaseSide<0)CHECK(actualPhase<row.phaseTarget);else if(row.phaseSide>0)CHECK(actualPhase>row.phaseTarget);
                CHECK(std::abs(actualPhase-row.phaseTarget)<=1e-12);
                std::cout<<"QUALITY_MEMO_PHASE target="<<row.phaseTarget<<" actual="<<actualPhase<<" cursor="<<actualPosition<<" captured_step="<<live.step<<" side="<<row.phaseSide<<'\n';
            }
            if(active&&!memoMatches(prior,key))++replaced;
            if(active&&row.phaseTarget>=0){
                const auto live=DrumQualityTestAccess::hits(e)[0];CHECK(live.elapsed==std::uint32_t(elapsed));
                const double cursor=std::min(double(live.begin)+double(live.elapsed)*live.step,double(live.end-1));CHECK(cursor-int(cursor)==key.phase);CHECK(key.phase!=0);
                DrumQualityTestAccess::invalidateMemo(e);CHECK(!DrumQualityTestAccess::memo(e).valid);
                const auto cold=DrumQualityTestAccess::query(e,0),coldExpected=memoOracle(p.pads[0],row.rate,elapsed);
                for(int c=0;c<2;++c)CHECK(std::abs(cold[c]-coldExpected[c])<=2e-5);
                CHECK(memoMatches(DrumQualityTestAccess::memo(e),key));++fractionalWarm;
                const auto sameCursorWarm=DrumQualityTestAccess::query(e,0);
                for(int c=0;c<2;++c)CHECK(std::bit_cast<std::uint64_t>(sameCursorWarm[c])==std::bit_cast<std::uint64_t>(cold[c]));
                std::cout<<"QUALITY_MEMO_FRACTION_WARM elapsed="<<elapsed<<" cursor="<<cursor<<" phase="<<key.phase<<" captured_step="<<live.step<<'\n';
            }
            const auto y=render(e,1);const auto expected=active?memoOracle(p.pads[0],row.rate,elapsed):std::array<double,2>{};
            for(int c=0;c<2;++c)CHECK(std::abs(y[c][0]-expected[c])<=2e-5);if(active){const auto current=DrumQualityTestAccess::memo(e);CHECK(memoMatches(current,key));
                double sum=0,exact=0,error=0;const int count=key.last-key.first+1;
                for(int n=0;n<count;++n){sum+=current.values[std::size_t(n)];exact+=analyticWeight(std::abs((key.first+n-key.phase)*key.scale));}
                CHECK(sum==current.normalization);for(int n=0;n<count;++n)error+=std::abs(current.values[std::size_t(n)]/sum-analyticWeight(std::abs((key.first+n-key.phase)*key.scale))/exact);CHECK(error<=2e-5);
            }}
        p.pads[0].sample=nullptr;CHECK(e.setSettings(p));e.reset();CHECK(!e.references(retained.get()));
    }
    CHECK(warm>=33);CHECK(replaced>=5);CHECK(maxVisits==1153);
    CHECK(fractionalWarm==21);std::cout<<"QUALITY_MEMO_FRACTION_WARM_TOTAL count="<<fractionalWarm<<'\n';
    std::cout<<"QUALITY_MEMO_LAWS warm_conditions="<<warm<<" replacements="<<replaced<<" max_visits="<<maxVisits<<" entry_bytes="<<DrumQualityTestAccess::memo(e).bytes<<" engine_bytes="<<sizeof(e)<<'\n';
}
void memoCosts(){
    auto a=asset(std::vector<float>(4000,.125f)),b=asset(std::vector<float>(8000,-.25f),2);auto p=rack(a.get());p.pads[0].tuneSemitones=12;p.pads[1].sample=b.get();p.pads[1].tuneSemitones=24;DrumEngine e;prepare(e,p);
    (void)render(e,1,{{0,on()},{0,on(37)}});double consumed=0;
    for(int mode=0;mode<3;++mode){(void)DrumQualityTestAccess::query(e,0);
        for(int n=0;n<32;++n){const int slot=mode==2?(n%2?0:1):0;const auto key=memoKey(p.pads[std::size_t(slot)],48000,1);
            if(mode==0)DrumQualityTestAccess::invalidateMemo(e);const auto before=DrumQualityTestAccess::memo(e);
            if(mode==1)CHECK(memoMatches(before,key));else CHECK(!memoMatches(before,key));
            const auto start=std::chrono::steady_clock::now();const auto value=DrumQualityTestAccess::query(e,std::size_t(slot));const auto finish=std::chrono::steady_clock::now();
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count();consumed+=value[0]+value[1];
            CHECK(memoMatches(DrumQualityTestAccess::memo(e),key));const auto expected=memoOracle(p.pads[std::size_t(slot)],48000,1);for(int c=0;c<2;++c)CHECK(std::abs(value[c]-expected[c])<=2e-5);
            std::cout<<"QUALITY_MEMO_COST mode="<<(mode==0?"cold":mode==1?"warm":"alternating")<<" n="<<n<<" slot="<<slot<<" visits="<<key.last-key.first+1<<" ns="<<ns<<" consumed="<<value[0]+value[1]<<'\n';}
    }
    CHECK(std::isfinite(consumed));std::cout<<"QUALITY_MEMO_COST_CONTEXT calls=96 clock=steady_clock_ns includes_clock_overhead=1 single_engine_private_reconstruction=1 no_callback_capacity_claim=1 consumed="<<consumed<<'\n';
    std::vector<float> warmData(8192);for(int n=0;n<4096;++n){warmData[std::size_t(n)*2]=float(.0625*(n%7-3));warmData[std::size_t(n)*2+1]=float(.03125*(n%11-5)+.015625);}
    auto warmAsset=asset(std::move(warmData),2);auto warmSettings=rack(warmAsset.get());warmSettings.pads[0].tuneSemitones=24;
    DrumEngine warmEngine;prepare(warmEngine,warmSettings,96000);(void)render(warmEngine,64,{{0,on()}});
    const auto live=DrumQualityTestAccess::hits(warmEngine)[0];CHECK(live.elapsed==64&&live.step==2);CHECK(live.sample==warmAsset.get()&&live.begin==0&&live.end==4096);
    const double cursor=std::min(double(live.begin)+double(live.elapsed)*live.step,double(live.end-1));CHECK(cursor==128);
    const auto warmKey=memoKey(warmSettings.pads[0],96000,64);CHECK(warmKey.phase==0&&warmKey.first==-48&&warmKey.last==48);CHECK(live.readScale==warmKey.scale&&live.readRadius==warmKey.radius);
    CHECK(int(std::ceil(cursor-live.readRadius))>=int(live.begin)&&int(std::floor(cursor+live.readRadius))<int(live.end));
    DrumQualityTestAccess::invalidateMemo(warmEngine);CHECK(!DrumQualityTestAccess::memo(warmEngine).valid);
    const auto scalarCold=DrumQualityTestAccess::query(warmEngine,0),warmExpected=memoOracle(warmSettings.pads[0],96000,64);
    for(int c=0;c<2;++c)CHECK(std::abs(scalarCold[c]-warmExpected[c])<=2e-5);
    double warmConsumed=0;for(int n=0;n<32;++n){CHECK(memoMatches(DrumQualityTestAccess::memo(warmEngine),warmKey));
        const auto start=std::chrono::steady_clock::now();const auto value=DrumQualityTestAccess::query(warmEngine,0);const auto finish=std::chrono::steady_clock::now();
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count();warmConsumed+=value[0]+value[1];
        for(int c=0;c<2;++c){CHECK(std::bit_cast<std::uint64_t>(value[c])==std::bit_cast<std::uint64_t>(scalarCold[c]));CHECK(std::abs(value[c]-warmExpected[c])<=2e-5);}
        std::cout<<"QUALITY_MEMO_STEREO_WARM_COST n="<<n<<" visits="<<warmKey.last-warmKey.first+1<<" ns="<<ns<<" consumed="<<value[0]+value[1]<<'\n';}
    CHECK(std::isfinite(warmConsumed));std::cout<<"QUALITY_MEMO_STEREO_WARM_COST_CONTEXT calls=32 clock=steady_clock_ns includes_clock_overhead=1 single_engine_private_reconstruction=1 no_callback_capacity_claim=1 cursor="<<cursor<<" captured_step="<<live.step<<" visits="<<warmKey.last-warmKey.first+1<<" scalar_bit_comparisons=64 consumed="<<warmConsumed<<'\n';
}
void memoRealtimeGuard(){
#if defined(_MSC_VER) && defined(_DEBUG)
    auto a=asset(std::vector<float>(480000,.125f)),b=asset(std::vector<float>(960000,-.0625f),2);auto p=rack(a.get());p.pads[0].tuneSemitones=24;p.pads[1].sample=b.get();p.pads[1].tuneSemitones=-24;DrumEngine direct;prepare(direct,p,8000);
    auto patch=routedKit(4);workerEffects(patch);BorrowedDrumAssignments bindings{};for(unsigned bank=0;bank<4;++bank){bindings[bank][0]=a.get();bindings[bank][1]=b.get();patch.drumPads[bank][0].tuneSemitones=24;patch.drumPads[bank][1].tuneSemitones=-24;}
    RoutedSynthEngine routed;CHECK(routed.setPatch(patch,bindings));CHECK(routed.prepare(48000,512,RenderQuality::higher));routed.reset();RoutedDrumTestAccess::hook(routed,nullptr,borrowHook);
    juce::AudioBuffer<float> output(2,512);juce::MidiBuffer notes;notes.ensureSize(4096);for(int n=0;n<64;++n)notes.addEvent(on(n%2?37:36),n%17);
    const auto ownersA=a.use_count(),ownersB=b.use_count();auto* previous=_CrtSetAllocHook(heapHook);for(auto& c:heapAllocs)c=0;for(auto& c:heapFrees)c=0;workerBorrows=0;callbackGuard=true;
    for(int n=0;n<3;++n){DrumQualityTestAccess::invalidateMemo(direct);direct.render(output,notes);direct.render(output,notes);}for(int n=0;n<8;++n)routed.render(output,notes);
    callbackGuard=false;_CrtSetAllocHook(previous);CHECK(heapAllocs[0]==0&&heapFrees[0]==0);CHECK(heapAllocs[1]==0&&heapFrees[1]==0);CHECK(workerBorrows>0);CHECK(a.use_count()==ownersA&&b.use_count()==ownersB);
    CHECK(routed.dspWorkerStats().prepared==3&&routed.dspWorkerStats().blocks>0);std::cout<<"QUALITY_MEMO_HEAP callback_alloc="<<heapAllocs[0]<<" callback_free="<<heapFrees[0]<<" worker_alloc="<<heapAllocs[1]<<" worker_free="<<heapFrees[1]<<" real_worker_borrows="<<workerBorrows<<'\n';
#endif
}

}
int main(){std::cout<<std::setprecision(17);aliasRed();stealRed();stationary();boundaries();tableBounds();stealLaws();adversarialSteals();gainAndChoke();capturedAndPartitions();routedQuality();routedWorkers();clears();offAudioLifecycle();realtimeGuard();memoLaws();memoCosts();memoRealtimeGuard();std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
#else
int main(){std::cerr<<"Drum playback quality acceptance requires a Debug build\n";return 2;}
#endif
