#include "ModalOuter.h"
#include "ModalField.h"
#include <bit>
#include <cstdint>
#include <cfenv>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <type_traits>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;unsigned checks=0,failures=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){if(tracking)++allocations;
#if defined(_MSC_VER)
    auto* p=_aligned_malloc(n?n:1,std::size_t(a));
#else
    auto* p=std::aligned_alloc(std::size_t(a),((n?n:1)+std::size_t(a)-1)/std::size_t(a)*std::size_t(a));
#endif
    if(p)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept {if(tracking&&p)++frees;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"DIRECT_MODAL_OUTER_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;using Frame=ModalRoom::Frame;
std::vector<Frame> input(std::size_t n){std::vector<Frame> a(n);for(std::size_t k=0;k<n;++k)a[k]={k==0?.5:k<512?double(int(k%37)-18)/8192.:0,k==0?.25:k<512?double(int(k%31)-15)/16384.:0};return a;}
bool bits(const std::vector<Frame>& a,const std::vector<Frame>& b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(Frame))==0;}
struct Rig {
    ModalRoom room;ModalWetOuter wet;ModalFinalOuter final;
    bool prepare(const Settings& s,double rate){auto r=std::make_unique<PreparedModalRoom>();PreparedModalWet w;PreparedModalFinal f;return prepareModalRoom(s.decay,s.size,rate,1024,*r)&&prepareModalWet(s,rate,w)&&prepareModalFinal(s,rate,f)&&room.prepareStorage(rate)&&wet.prepareStorage(rate)&&final.prepareStorage(rate)&&room.configure(*r)&&wet.configure(w)&&final.configure(f);}
    bool run(std::vector<Frame>& audio,std::size_t block){bool ok=true;for(std::size_t at=0;at<audio.size();at+=block){auto span=std::span(audio).subspan(at,std::min(block,audio.size()-at));ok=final.capture(span)&&ok;ok=wet.excite(span)&&ok;ok=room.processBlock(span,wet.characters())&&ok;ok=wet.mixRooms(span)&&ok;ok=wet.post(span)&&ok;ok=final.finish(span)&&ok;}return ok;}
    void reset(){room.reset();wet.reset();final.reset();}
};
void invalid(){
    PreparedModalWet w,saved;PreparedModalFinal f,old;CHECK(prepareModalWet({},48000,w));CHECK(prepareModalFinal({},48000,f));saved=w;old=f;
    auto s=Settings{};s.post[5].bellQ=13;CHECK(!prepareModalWet(s,48000,w));CHECK(w==saved);CHECK(!prepareModalFinal(s,48000,f));CHECK(f==old);
    CHECK(!prepareModalWet({},7999,w));CHECK(!prepareModalFinal({},768001,f));PreparedModalEarly e;CHECK(!prepareModalEarly(-1,48000,e));
    ModalWetOuter wet;ModalFinalOuter final;auto a=input(1),b=a;CHECK(!wet.excite(a));CHECK(bits(a,b));CHECK(!final.capture(a));CHECK(!final.finish(a));CHECK(bits(a,b));CHECK(!wet.configure(w));CHECK(!final.configure(f));CHECK(!wet.prepareStorage(0));CHECK(!final.prepareStorage(0));wet.reset();final.reset();
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(64));::operator delete(known,std::align_val_t(64));tracking=false;CHECK(allocations==1&&frees==1);
}
void equivalent(){
    auto field=std::make_unique<PreparedModalField>();unsigned configurations=0;std::uint64_t frames=0;
    for(double rate:{44100.,48000.,96000.})for(double mix:{0.,.35,1.})for(double width:{0.,1.,2.})for(double duck:{0.,12.})for(double character:{0.,.25}){
        Settings s;s.predelayMs=3.75;s.mix=mix;s.width=width;s.character=character;s.duckRangeDb=duck;s.post[0]={true,false,PostShape::bell,1000,6,2,1,CutSlope::db12};
        CHECK(prepareModalField(s,rate,1024,*field));ModalField reference;CHECK(reference.prepareStorage(rate));CHECK(reference.configure(*field));Rig rig;CHECK(rig.prepare(s,rate));
        auto expected=input(4096),actual=expected;bool ok=true;for(std::size_t n=0;n<expected.size();n+=512)ok=reference.processBlock(std::span(expected).subspan(n,std::min<std::size_t>(512,expected.size()-n)))&&ok;
        allocations=frees=0;tracking=true;ok=rig.run(actual,64)&&ok;tracking=false;double error=0;for(std::size_t n=0;n<actual.size();++n)for(unsigned c=0;c<2;++c)error=std::max(error,std::abs(actual[n][c]-expected[n][c]));
        CHECK(ok);CHECK(duck==0?bits(actual,expected):error<=1e-12);CHECK(allocations==0&&frees==0);++configurations;frames+=actual.size();
        if(rate==48000&&width==1&&mix==1&&duck==12&&character==.25)for(std::size_t block:{1U,17U,512U}){auto b=input(4096);rig.reset();allocations=frees=0;tracking=true;const bool rendered=rig.run(b,block);tracking=false;CHECK(rendered);CHECK(bits(actual,b));CHECK(allocations==0&&frees==0);}
    }
    std::cout<<"MODAL_OUTER_REFERENCE configurations="<<configurations<<" frames="<<frames<<'\n';
}
void duckLaw(){
    double maximumError=0,maximumEnvelopeError=0;unsigned configurations=0;
    for(double rate:{8000.,48000.,768000.})for(double attack:{.1,10.,200.})for(double release:{10.,250.,2000.})for(double threshold:{-60.,-24.,0.})for(double range:{0.,12.,24.}){
        Settings s;s.duckAttackMs=attack;s.duckReleaseMs=release;s.duckThresholdDb=threshold;s.duckRangeDb=range;s.mix=1;PreparedModalFinal p;CHECK(prepareModalFinal(s,rate,p));
        PreparedDuck baseline;CHECK(prepareDuck(s,rate,baseline));DuckDetector reference;CHECK(reference.prepare(baseline));ModalFinalOuter final;CHECK(final.prepareStorage(rate));CHECK(final.configure(p));
        bool ok=true;allocations=frees=0;tracking=true;for(unsigned n=0;n<1000;++n){const std::array<Frame,1> raw{{{n<500?.5:0,n<500?-.7:0}}};const double gain=reference.tick(raw[0]);std::array<Frame,1> wet{{{1,1}}};ok=final.capture(raw)&&final.finish(wet)&&ok;maximumError=std::max(maximumError,std::abs(wet[0][0]-gain));maximumEnvelopeError=std::max(maximumEnvelopeError,std::abs(final.duckEnvelope()-reference.envelope()));if(range==0)ok=ok&&wet[0][0]==1&&wet[0][1]==1;}tracking=false;
        CHECK(ok);CHECK(allocations==0&&frees==0);++configurations;
    }
    CHECK(maximumError<=1e-12);CHECK(maximumEnvelopeError<=1e-12);
    Settings s;s.mix=1;s.duckRangeDb=24;PreparedModalFinal p;CHECK(prepareModalFinal(s,48000,p));ModalFinalOuter final;CHECK(final.prepareStorage(48000));CHECK(final.configure(p));
    std::array<Frame,480> raw;raw.fill({0,.5});auto wet=raw;CHECK(final.capture(raw));CHECK(final.finish(wet));CHECK(std::abs(final.duckEnvelope()-.5*(1-std::exp(-1.)))<=1e-12);
    const double held=final.duckEnvelope();s.duckAttackMs=.1;s.duckReleaseMs=2000;s.duckRangeDb=0;PreparedModalDuck next;CHECK(prepareModalDuck(s,48000,next));CHECK(final.adoptDuck(next));CHECK(final.duckEnvelope()==held);
    double expected=held;bool agrees=true;for(unsigned n=0;n<=480;++n){const double alpha=std::min(1.,double(n)/240),a=(1-alpha)*p.duck.coefficients.attack+alpha*next.coefficients.attack;expected=a*expected+(1-a)*.7;std::array<Frame,1> r{{{.7,0}}},v{{{1,1}}};CHECK(final.capture(r));CHECK(final.finish(v));agrees=agrees&&std::abs(expected-final.duckEnvelope())<=1e-12;if(n>=240)agrees=agrees&&v[0][0]==1&&v[0][1]==1;}CHECK(agrees);CHECK(final.observedPeak()==1);
    std::array<Frame,1> loud{{{2,-3}}},v{{{1,1}}};CHECK(final.capture(loud));CHECK(final.finish(v));CHECK(final.observedPeak()==3);final.reset();CHECK(final.observedPeak()==1&&final.duckEnvelope()==0);
    std::cout<<"MODAL_OUTER_DUCK configurations="<<configurations<<" gain_error="<<maximumError<<" envelope_error="<<maximumEnvelopeError<<'\n';
}
void ramps(){
    Settings s;s.mix=0;PreparedModalFinal p;CHECK(prepareModalFinal(s,48000,p));ModalFinalOuter final,control;CHECK(final.prepareStorage(48000));CHECK(control.prepareStorage(48000));CHECK(final.configure(p));CHECK(control.configure(p));CHECK(final.setFastTargets(2,1));CHECK(control.setFastTargets(2,1));
    bool equal=true,endpoints=true;allocations=frees=0;tracking=true;
    for(unsigned n=0;n<=480;++n){if(n==100)equal=final.setFastTargets(2,1)&&equal;const double alpha=std::min(1.,double(n)/240),width=1+alpha,mix=alpha;std::array<Frame,1> raw{{{0,-0.}}},a{{{1,-.5}}},b=a;
        equal=final.capture(raw)&&control.capture(raw)&&final.finish(a)&&control.finish(b)&&equal;equal=equal&&a==b;endpoints=endpoints&&std::abs(a[0][0]-mix*(.25+.75*width))<=1e-12&&std::abs(a[0][1]-mix*(.25-.75*width))<=1e-12;if(n==0)endpoints=endpoints&&std::bit_cast<std::array<std::uint64_t,2>>(a[0])==std::bit_cast<std::array<std::uint64_t,2>>(raw[0]);}
    tracking=false;CHECK(equal);CHECK(endpoints);CHECK(allocations==0&&frees==0);CHECK(final.setFastTargets(0,.3));
    for(unsigned n=0;n<=240;++n){const double a=double(n)/240,w=2*(1-a),m=1-.7*a;std::array<Frame,1> raw{{{0,0}}},v{{{1,-.5}}};CHECK(final.capture(raw));CHECK(final.finish(v));CHECK(std::abs(v[0][0]-m*(.25+.75*w))<=1e-12);}
    const auto before=final.duckEnvelope();CHECK(!final.setFastTargets(-1,.5));CHECK(final.duckEnvelope()==before);
}
struct Tape {std::vector<double> values;double read(double delay,double now) const {const auto n=std::size_t(std::floor(delay));const double f=delay-n;auto age=[&](std::size_t a){return a==0?now:a<=values.size()?values[values.size()-a]:0;};return f==0?age(n):(1-f)*age(n)+f*age(n+1);}};
void predelay(){
    Settings s;s.predelayMs=0;s.character=0;s.distance=1;PreparedModalWet p;CHECK(prepareModalWet(s,48000,p));ModalWetOuter wet;CHECK(wet.prepareStorage(48000));CHECK(wet.configure(p));CHECK(wet.setFastTargets({10,0,1}));
    Tape tape;double current=0,from=0,to=0,pending=480;unsigned phase=0;bool fading=false;double maximumError=0;
    for(unsigned n=0;n<1000;++n){if(n==50)CHECK(wet.setFastTargets({10,0,1}));if(n==100){CHECK(wet.setFastTargets({2.5,0,1}));pending=120;}
        if(!fading&&pending!=current){from=current;to=pending;phase=0;fading=true;}const double raw=double(n+1)/2048;double expected=tape.read(current,raw);
        if(fading){const double a=double(phase)/240;expected=phase==0?tape.read(from,raw):phase>=240?tape.read(to,raw):(1-a)*tape.read(from,raw)+a*tape.read(to,raw);}
        std::array<Frame,1> v{{{raw,-raw}}};CHECK(wet.excite(v));maximumError=std::max({maximumError,std::abs(v[0][0]-expected),std::abs(v[0][1]+expected)});CHECK(wet.characters().size()==1&&wet.characters()[0]==0);CHECK(wet.mixRooms(v));CHECK(wet.post(v));tape.values.push_back(raw);
        if(fading){if(phase>=240){current=to;fading=false;}else ++phase;}
    }CHECK(maximumError<=1e-12);
}
void postFadeAndErrors(){
    Settings s;s.predelayMs=0;s.distance=1;s.character=0;PreparedModalWet p;CHECK(prepareModalWet(s,48000,p));ModalWetOuter wet;CHECK(wet.prepareStorage(48000));CHECK(wet.configure(p));
    s.post[0]={true,false,PostShape::bell,1000,12,2,1,CutSlope::db12};PreparedModalPost next;CHECK(prepareModalPost(s,48000,next));CHECK(next.commonLogL1>=0);PostEq oldPost,newPost;CHECK(oldPost.prepare(p.post.coefficients));CHECK(newPost.prepare(next.coefficients));CHECK(wet.adoptPost(next));CHECK(wet.postPending());CHECK(!wet.adoptPost(next));CHECK(!wet.commitPost());CHECK(wet.setFastTargets({0,.5,1}));
    auto raw=input(241),signal=raw;std::vector<double> alpha(signal.size());for(std::size_t n=0;n<alpha.size();++n)alpha[n]=double(n)/240;CHECK(wet.excite(raw));CHECK(wet.mixRooms(signal));auto bad=alpha;bad.back()=2;auto unchanged=signal;CHECK(!wet.post(signal,bad));CHECK(bits(signal,unchanged));CHECK(wet.post(signal,alpha));CHECK(wet.commitPost());CHECK(!wet.postPending());CHECK(wet.activePost()==next);
    double error=0;for(std::size_t n=0;n<signal.size();++n){const auto a=oldPost.tick(unchanged[n]),b=newPost.tick(unchanged[n]);for(unsigned c=0;c<2;++c){const double expected=n==0?a[c]:n==240?b[c]:(1-alpha[n])*a[c]+alpha[n]*b[c];error=std::max(error,std::abs(signal[n][c]-expected));}}CHECK(error<=1e-15);
    raw=input(64);signal=raw;CHECK(wet.excite(raw));CHECK(!wet.excite(raw));CHECK(!wet.setEarlyGeometries(p.early,p.early));CHECK(!wet.post(signal));CHECK(wet.mixRooms(signal));CHECK(!wet.mixRooms(signal));CHECK(wet.post(signal));CHECK(!wet.post(signal));
    auto broken=p;broken.post.coefficients.sampleRate=96000;CHECK(!wet.configure(broken));CHECK(wet.activePost()==next);CHECK(!wet.setFastTargets({251,0,1}));wet.reset();CHECK(!wet.postPending());
    PreparedModalFinal f;CHECK(prepareModalFinal({},48000,f));ModalFinalOuter final;CHECK(final.prepareStorage(48000));CHECK(final.configure(f));raw=input(64);signal=raw;CHECK(final.capture(raw));const double envelope=final.duckEnvelope();CHECK(!final.capture(raw));CHECK(!final.finish(std::span(signal).first(63)));CHECK(final.duckEnvelope()==envelope);CHECK(final.finish(signal));CHECK(!final.finish(signal));
    raw[0][0]=std::numeric_limits<double>::quiet_NaN();CHECK(!final.capture(raw));CHECK(final.duckEnvelope()==envelope);auto badDuck=f.duck;badDuck.minimumGain=0;CHECK(!final.adoptDuck(badDuck));CHECK(final.duckEnvelope()==envelope);
    std::array<Frame,1025> oversized{};CHECK(!final.capture(oversized));std::array<Frame,513> wetLarge{};CHECK(!wet.excite(wetLarge));CHECK(wet.excite({}));CHECK(final.capture({}));CHECK(final.finish({}));
}
void dualRooms(){
    Settings s;s.size=0;s.predelayMs=0;s.character=.25;s.distance=.4;PreparedModalWet p;CHECK(prepareModalWet(s,48000,p));PreparedModalEarly next;CHECK(prepareModalEarly(1,48000,next));ModalWetOuter wet;CHECK(wet.prepareStorage(48000));CHECK(wet.configure(p));std::array<Tape,2> tape;
    const auto rawAt=[](std::size_t n){return Frame{double(int(n%37)-18)/1024.,double(int(n%31)-15)/2048.};};
    for(std::size_t n=0;n<1024;++n){const auto raw=rawAt(n);std::array<Frame,1> a{{raw}},late{};CHECK(wet.excite(a));CHECK(wet.mixRooms(late));CHECK(wet.post(late));for(unsigned c=0;c<2;++c)tape[c].values.push_back(raw[c]);}
    CHECK(wet.setEarlyGeometries(p.early,next));CHECK(wet.setFastTargets({0,.7,.7}));double error=0,charError=0;
    constexpr std::array<std::array<double,3>,2> seconds{{{.0073,.0111,.0179},{.0089,.0137,.0193}}};constexpr std::array<double,3> gain{.42,.25,.18};
    for(std::size_t base=0;base<2048;base+=64){std::vector<Frame> raw(64),a(64),b(64),expected(64);std::vector<double> alpha(64);
        for(std::size_t n=0;n<64;++n){const auto frame=base+n;raw[n]=rawAt(1024+frame);a[n]={.01*std::cos(frame*.07),.02*std::sin(frame*.09)};b[n]={.03*std::cos(frame*.11),-.04*std::sin(frame*.13)};alpha[n]=std::min(1.,double(frame)/240);const double d=frame>=240?.7:.4+(.7-.4)*double(frame)/240;Frame oldEarly{},newEarly{};
            for(unsigned c=0;c<2;++c)for(unsigned j=0;j<3;++j){const auto source=j==1?1-c:c;oldEarly[c]+=gain[j]*tape[source].read(seconds[c][j]*.75*48000,raw[n][source]);newEarly[c]+=gain[j]*tape[source].read(seconds[c][j]*1.25*48000,raw[n][source]);}
            for(unsigned c=0;c<2;++c){const double old=.8*(1-d)*oldEarly[c]+(.2+.8*d)*a[n][c],nextValue=.8*(1-d)*newEarly[c]+(.2+.8*d)*b[n][c];expected[n][c]=alpha[n]==0?old:alpha[n]==1?nextValue:(1-alpha[n])*old+alpha[n]*nextValue;tape[c].values.push_back(raw[n][c]);}}
        CHECK(wet.excite(raw));auto bad=alpha;bad.back()=std::numeric_limits<double>::quiet_NaN();const auto unchanged=a;CHECK(!wet.mixRooms(a,b,bad));CHECK(bits(a,unchanged));CHECK(wet.mixRooms(a,b,alpha));CHECK(wet.post(a));
        const auto chars=wet.characters();CHECK(chars.size()==64);for(std::size_t n=0;n<64;++n){const double c=base+n>=240?.7:.25+(.7-.25)*double(base+n)/240;charError=std::max(charError,std::abs(chars[n]-c));for(unsigned ch=0;ch<2;++ch)error=std::max(error,std::abs(a[n][ch]-expected[n][ch]));}
    }CHECK(error<=1e-12);CHECK(charError<=1e-12);
    s.mix=0;s.duckRangeDb=24;PreparedModalFinal f;CHECK(prepareModalFinal(s,48000,f));ModalFinalOuter final;CHECK(final.prepareStorage(48000));CHECK(final.configure(f));std::vector<Frame> raw(1024,Frame{.1,-.2}),late(1024,Frame{.5,-.75});raw[0]={1.5,-1};raw[1]={-0.,0.};
    allocations=frees=0;tracking=true;const bool rendered=final.capture(raw)&&final.finish(late);tracking=false;CHECK(rendered);CHECK(bits(raw,late));CHECK(final.duckEnvelope()>0&&final.observedPeak()==1.5);CHECK(allocations==0&&frees==0);
}
void identicalEarlyBits(){
    // An unbounded history oracle covers the shared-geometry path, including
    // room fades: reuse must preserve the original per-channel summation bits.
    const int originalRounding=std::fegetround();
    constexpr std::array<double,3> gains{.42,.25,.18};
    for(double rate:{44100.,48000.,96000.})for(double size:{0.,.5,1.})for(int partition:{17,512}){
        Settings settings;settings.size=size;settings.predelayMs=0;settings.character=0;settings.distance=.6;
        PreparedModalWet prepared;CHECK(prepareModalWet(settings,rate,prepared));
        for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
            ModalWetOuter wet;CHECK(wet.prepareStorage(rate)&&wet.configure(prepared));
            CHECK(wet.setEarlyGeometries(prepared.early,prepared.early));
            std::array<Tape,2> history;bool equal=true;
            CHECK(std::fesetround(rounding)==0);
            for(std::size_t first=0;first<4096;first+=std::size_t(partition)){
                const auto count=std::min(std::size_t(partition),4096-first);
                std::vector<Frame> input(count),old(count),next(count),expected(count);std::vector<double> alpha(count);
                for(std::size_t n=0;n<count;++n){
                    const auto frame=first+n;
                    input[n]={double(int(frame%37)-18)/1024.,double(int(frame%31)-15)/2048.};
                    old[n]={double(int(frame%23)-11)/4096.,double(int(frame%19)-9)/8192.};
                    next[n]={-old[n][1],old[n][0]};alpha[n]=double(frame%241)/240.;
                    Frame early{};
                    for(unsigned c=0;c<2;++c)for(unsigned j=0;j<3;++j){
                        const auto source=j==1?1-c:c;
                        const auto p=prepared.early.positions[c][j];
                        // Read by the prepared integral/fractional position rather
                        // than re-deriving a delay under a different rounding mode.
                        const auto age=[&](std::size_t k){return k==0?input[n][source]:k<=history[source].values.size()?history[source].values[history[source].values.size()-k]:0.;};
                        const double a=age(p.whole),value=p.fraction==0?a:(1-p.fraction)*a+p.fraction*age(p.whole+1);
                        early[c]+=gains[j]*value;
                    }
                    for(unsigned c=0;c<2;++c){
                        const double a=.8*(1-settings.distance)*early[c]+(.2+.8*settings.distance)*old[n][c];
                        const double b=.8*(1-settings.distance)*early[c]+(.2+.8*settings.distance)*next[n][c];
                        expected[n][c]=alpha[n]==0?a:alpha[n]==1?b:(1-alpha[n])*a+alpha[n]*b;
                        history[c].values.push_back(input[n][c]);
                    }
                }
                equal=wet.excite(input)&&wet.mixRooms(old,next,alpha)&&wet.post(old)&&bits(old,expected)&&equal;
            }
            CHECK(std::fesetround(originalRounding)==0);CHECK(equal);
        }
    }
}
void resources(){
    for(double rate:{8000.,768000.}){PreparedModalWet w;PreparedModalFinal f;CHECK(prepareModalWet({},rate,w));CHECK(prepareModalFinal({},rate,f));ModalWetOuter wet;ModalFinalOuter final;CHECK(wet.prepareStorage(rate));CHECK(final.prepareStorage(rate));CHECK(wet.configure(w));CHECK(final.configure(f));CHECK(wet.storageBytes()+final.storageBytes()<=16*1024*1024);std::cout<<"MODAL_OUTER_RESOURCE rate="<<rate<<" wet_bytes="<<wet.storageBytes()<<" final_bytes="<<final.storageBytes()<<'\n';}
}
void preparedOperations(){
    Settings s;PreparedModalWet w;PreparedModalFinal f;PreparedModalEarly a,b;PreparedModalPost post;PreparedModalDuck duck;
    CHECK(prepareModalWet(s,48000,w));CHECK(prepareModalFinal(s,48000,f));CHECK(prepareModalEarly(0,48000,a));CHECK(prepareModalEarly(1,48000,b));s.post[0]={true,false,PostShape::highShelf,4000,6,1,.5,CutSlope::db12};s.duckRangeDb=24;s.duckAttackMs=.1;CHECK(prepareModalPost(s,48000,post));CHECK(prepareModalDuck(s,48000,duck));
    ModalWetOuter wet;ModalFinalOuter final;CHECK(wet.prepareStorage(48000));CHECK(final.prepareStorage(48000));allocations=frees=0;tracking=true;
    const bool configured=wet.configure(w)&&final.configure(f),geometry=wet.setEarlyGeometries(a,b),fast=wet.setFastTargets({250,1,.75})&&final.setFastTargets(2,0),postAdopted=wet.adoptPost(post),duckAdopted=final.adoptDuck(duck);wet.reset();final.reset();tracking=false;
    CHECK(configured&&geometry&&fast&&postAdopted&&duckAdopted);CHECK(allocations==0&&frees==0);CHECK(!wet.postPending()&&final.duckEnvelope()==0&&final.observedPeak()==1);
    Settings first,second;first.mix=1;second=first;second.post[0]={true,false,PostShape::bell,1000,12,2,1,CutSlope::db12};Rig one,two;CHECK(one.prepare(first,48000));CHECK(two.prepare(second,48000));auto x=input(2048),y=x;CHECK(one.run(x,64));CHECK(two.run(y,512));bool same=true;for(std::size_t k=0;k<1024;++k)same=same&&one.room.modeState(k)==two.room.modeState(k);CHECK(same);CHECK(!bits(x,y));
}
}
int main(){invalid();equivalent();duckLaw();ramps();predelay();postFadeAndErrors();dualRooms();identicalEarlyBits();resources();preparedOperations();std::cout<<"SpaceReverbDirectModalOuter checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
