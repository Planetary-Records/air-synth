#include "ModalField.h"
#include <bit>
#include <cstdint>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_SPARSE_FIELD_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
std::size_t testModes=128;
using Frame=ModalField::Frame;
constexpr std::array<double,8> diffusion{.00071,.00113,.00173,.00231,.00317,.00419,.00527,.00661};
constexpr std::array<std::array<double,3>,2> early{{{.0073,.0111,.0179},{.0089,.0137,.0193}}};
bool run(ModalField& f,std::vector<Frame>& a,std::size_t block){bool pass=true;for(std::size_t at=0;at<a.size();at+=block)pass=f.processBlock(std::span(a).subspan(at,std::min(block,a.size()-at)))&&pass;return pass;}
bool bitEqual(const std::vector<Frame>& a,const std::vector<Frame>& b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(Frame))==0;}
struct Tape {std::vector<double> data;double read(double delay,double now) const {const auto n=std::size_t(std::floor(delay));const double fraction=delay-n;const auto age=[&](std::size_t k){return k==0?now:k<=data.size()?data[data.size()-k]:0;};return fraction==0?age(n):(1-fraction)*age(n)+fraction*age(n+1);}void write(double v){data.push_back(v);}};
std::vector<Frame> reference(const Settings& s,double rate,const PreparedModal& bank,const std::vector<Frame>& input){
    ModalTank tank;CHECK(tank.prepareStorage(rate));CHECK(tank.configure(bank));PreparedPostEq postBank;PreparedDuck duckBank;CHECK(preparePostEq(s,rate,postBank));CHECK(prepareDuck(s,rate,duckBank));PostEq post;DuckDetector duck;CHECK(post.prepare(postBank));CHECK(duck.prepare(duckBank));
    std::array<Tape,2> pre,reflections,chorus;std::array<Tape,8> scatter;std::vector<Frame> output(input.size());
    for(std::size_t n=0;n<input.size();++n){const auto in=input[n];const double gain=duck.tick(in);Frame x{},e{};
        for(unsigned c=0;c<2;++c){x[c]=pre[c].read(s.predelayMs*.001*rate,in[c]);pre[c].write(in[c]);}
        for(unsigned c=0;c<2;++c)for(unsigned j=0;j<3;++j){const unsigned source=j==1?1-c:c;e[c]+=std::array<double,3>{.42,.25,.18}[j]*reflections[source].read(early[c][j]*(.75+.5*s.size)*rate,x[source]);}
        for(unsigned c=0;c<2;++c)reflections[c].write(x[c]);
        for(unsigned j=0;j<8;++j){const double delayed=scatter[j].read(diffusion[j]*(.5+1.5*s.size)*rate,x[1]);scatter[j].write(x[1]);const double left=x[0];x={.6*left+.8*delayed,-.8*left+.6*delayed};}
        auto wet=tank.tick(x);
        for(unsigned c=0;c<2;++c){const double phase=2*std::numbers::pi*(c?.31:.23)*double(n)/rate+(c?.37*std::numbers::pi:0);const double delay=(c?.008+.005*std::sin(phase):.006+.004*std::sin(phase))*rate;const double moved=chorus[c].read(delay,wet[c]);chorus[c].write(wet[c]);if(s.character!=0)wet[c]=(1-s.character)*wet[c]+s.character*moved;}
        if(s.distance!=1)for(unsigned c=0;c<2;++c)wet[c]=.8*(1-s.distance)*e[c]+(.2+.8*s.distance)*wet[c];wet=post.tick(wet);
        if(s.width!=1){const double m=(wet[0]+wet[1])*.5,side=(wet[0]-wet[1])*.5;wet={m+s.width*side,m-s.width*side};}
        if(gain!=1)for(auto& v:wet)v*=gain;if(s.mix==0)wet=in;else if(s.mix!=1)for(unsigned c=0;c<2;++c)wet[c]=(1-s.mix)*in[c]+s.mix*wet[c];output[n]=wet;
    }return output;
}
void invalid(){
    auto p=std::make_unique<PreparedModalField>(),saved=std::make_unique<PreparedModalField>();Settings s;
    CHECK(prepareModalField(s,48000,testModes,*p));*saved=*p;
    s.post[5].bellQ=13;CHECK(!prepareModalField(s,48000,testModes,*p));CHECK(std::memcmp(p.get(),saved.get(),sizeof(*p))==0);
    CHECK(!prepareModalField({},7999,testModes,*p));CHECK(!prepareModalField({},768001,testModes,*p));CHECK(!prepareModalField({},48000,1000,*p));
    unsigned calls=0;const auto cancelled=[](void* v) noexcept {return ++*static_cast<unsigned*>(v)>2;};CHECK(!prepareModalField({},48000,testModes,*p,{cancelled,&calls}));CHECK(std::memcmp(p.get(),saved.get(),sizeof(*p))==0);
    ModalField empty;std::vector<Frame> input(513);input[0]={.5,-.25};auto original=input;CHECK(!empty.processBlock(input));CHECK(bitEqual(input,original));CHECK(!empty.configure(*p));CHECK(!empty.prepareStorage(0));empty.reset();
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(64));::operator delete(known,std::align_val_t(64));tracking=false;CHECK(allocations==1&&frees==1);
}
void fixed(){
    auto p=std::make_unique<PreparedModalField>();Settings s;s.mix=1;s.character=0;s.predelayMs=3.25;
    for(double rate:{8000.,44100.,48000.,96000.,768000.})for(auto count:{testModes}){
        CHECK(prepareModalField(s,rate,count,*p));CHECK(p->modal.count==count&&p->modal.sampleRate==rate&&p->settings==s);
        bool geometry=true;for(unsigned j=0;j<8;++j)geometry=geometry&&p->diffusionSamples[j]==diffusion[j]*(.5+1.5*s.size)*rate;
        for(unsigned c=0;c<2;++c)for(unsigned j=0;j<3;++j)geometry=geometry&&p->earlySamples[c][j]==early[c][j]*(.75+.5*s.size)*rate;CHECK(geometry);
        CHECK(p->tail.passed&&p->tail.seconds>0&&p->tail.timeBound>0&&p->tail.timeBound<=46&&p->tail.referenceInputPeak==1);
        ModalField field;CHECK(field.prepareStorage(rate));CHECK(field.configure(*p));CHECK(field.storageBytes()<=16*1024*1024);
    }
    for(double rate:{44100.,48000.,96000.}){
        s.predelayMs=2.37;CHECK(prepareModalField(s,rate,testModes,*p));std::vector<Frame> input(4096);input[0]={.5,.5};for(std::size_t n=1;n<128;++n)input[n]={double(int(n%17)-8)/32768.,double(int(n%23)-11)/65536.};
        const auto expected=reference(s,rate,p->modal,input);ModalField field;CHECK(field.prepareStorage(rate));CHECK(field.configure(*p));auto actual=input;
        allocations=frees=0;tracking=true;const bool rendered=run(field,actual,512);tracking=false;CHECK(rendered);CHECK(allocations==0&&frees==0);
        double error=0,peak=0,energy=0;bool stereo=false;for(std::size_t n=0;n<actual.size();++n)for(unsigned c=0;c<2;++c){error=std::max(error,std::abs(actual[n][c]-expected[n][c]));peak=std::max(peak,std::abs(actual[n][c]));energy+=actual[n][c]*actual[n][c];stereo=stereo||actual[n][0]!=actual[n][1];}CHECK(error<=1e-12);CHECK(peak>0&&peak<=4&&energy>0&&stereo);
        for(std::size_t partition:{1U,17U,64U,512U}){field.reset();auto b=input;allocations=frees=0;tracking=true;const bool ok=run(field,b,partition);tracking=false;CHECK(ok);CHECK(bitEqual(actual,b));CHECK(allocations==0&&frees==0);}
        auto before=field.modeState(0);auto bad=*p;bad.settings.post[0].shelfQ=5;CHECK(!field.configure(bad));CHECK(field.modeState(0)==before);CHECK(!field.prepareStorage(0));CHECK(field.modeState(0)==before);
        field.reset();ModalField fresh;CHECK(fresh.prepareStorage(rate));CHECK(fresh.configure(*p));auto a=input,b=input;CHECK(run(field,a,64));CHECK(run(fresh,b,512));CHECK(bitEqual(a,b));
    }
}
void controls(){
    auto p=std::make_unique<PreparedModalField>();Settings s;s.character=0;s.predelayMs=250;s.mix=0;s.duckRangeDb=0;
    CHECK(prepareModalField(s,48000,testModes,*p));ModalField dry;CHECK(dry.prepareStorage(48000));CHECK(dry.configure(*p));std::vector<Frame> input(4096);input[0]={.5,-.25};input[1]={-0.,0.};for(std::size_t n=2;n<input.size();++n)input[n]={.1,-.2};auto output=input;
    allocations=frees=0;tracking=true;const bool ok=run(dry,output,64);tracking=false;CHECK(ok);CHECK(bitEqual(input,output));CHECK(dry.duckEnvelope()>.19&&dry.duckEnvelope()<.201);CHECK(allocations==0&&frees==0);
    s.predelayMs=0;s.mix=1;s.distance=1;CHECK(prepareModalField(s,48000,testModes,*p));ModalField wet;CHECK(wet.prepareStorage(48000));CHECK(wet.configure(*p));auto baseline=input;CHECK(run(wet,baseline,512));
    s.mix=0;CHECK(prepareModalField(s,48000,testModes,*p));CHECK(dry.configure(*p));output=input;CHECK(run(dry,output,17));CHECK(bitEqual(input,output));bool sameState=true;for(std::size_t k=0;k<testModes;++k)sameState=sameState&&dry.modeState(k)==wet.modeState(k);CHECK(sameState);
    s.mix=1;s.width=0;CHECK(prepareModalField(s,48000,testModes,*p));CHECK(dry.configure(*p));output=input;CHECK(run(dry,output,64));bool mono=true;for(auto v:output)mono=mono&&v[0]==v[1];CHECK(mono);
    s.width=2;CHECK(prepareModalField(s,48000,testModes,*p));CHECK(dry.configure(*p));output=input;CHECK(run(dry,output,64));double widthError=0;for(std::size_t n=0;n<output.size();++n){const double m=(baseline[n][0]+baseline[n][1])*.5,side=(baseline[n][0]-baseline[n][1])*.5;widthError=std::max({widthError,std::abs(output[n][0]-(m+2*side)),std::abs(output[n][1]-(m-2*side))});}CHECK(widthError<=1e-15);
    s.width=1;s.post[0]={true,false,PostShape::bell,1000,12,2,1,CutSlope::db12};CHECK(prepareModalField(s,48000,testModes,*p));CHECK(dry.configure(*p));output=input;CHECK(run(dry,output,64));PostEq referencePost;CHECK(referencePost.prepare(p->post));bool postExact=true;for(std::size_t n=0;n<output.size();++n)postExact=postExact&&output[n]==referencePost.tick(baseline[n]);CHECK(postExact);
    s.post={};s.predelayMs=250;s.duckRangeDb=24;s.duckAttackMs=10;s.duckReleaseMs=250;CHECK(prepareModalField(s,48000,testModes,*p));CHECK(dry.configure(*p));std::vector<Frame> steady(480,Frame{0,.5});CHECK(run(dry,steady,64));CHECK(std::abs(dry.duckEnvelope()-.5*(1-std::exp(-1.)))<=1e-12);
}
void character(){
    auto p=std::make_unique<PreparedModalField>();Settings s;s.mix=1;s.distance=1;s.predelayMs=0;s.character=1;
    CHECK(prepareModalField(s,48000,testModes,*p));std::vector<Frame> input(8192);input[0]={.5,.25};const auto expected=reference(s,48000,p->modal,input);
    ModalField field;CHECK(field.prepareStorage(48000));CHECK(field.configure(*p));auto actual=input;CHECK(run(field,actual,512));double error=0;for(std::size_t n=0;n<actual.size();++n)for(unsigned c=0;c<2;++c)error=std::max(error,std::abs(actual[n][c]-expected[n][c]));CHECK(error<=1e-11);
    for(std::size_t block:{1U,17U,64U,512U}){field.reset();auto b=input;allocations=frees=0;tracking=true;const bool ok=run(field,b,block);tracking=false;CHECK(ok);CHECK(bitEqual(actual,b));CHECK(allocations==0&&frees==0);}
    s.character=0;CHECK(prepareModalField(s,48000,testModes,*p));CHECK(field.configure(*p));auto stationary=input;CHECK(run(field,stationary,512));double difference=0;for(std::size_t n=2048;n<input.size();++n)for(unsigned c=0;c<2;++c)difference+=std::abs(actual[n][c]-stationary[n][c]);CHECK(difference>1e-5);
}
void tails(){
    for(const auto& c:{PostCoefficients{},PostCoefficients{0,1,0,-1,.25},PostCoefficients{.2,.1,.05,-1.6*std::cos(.4),.64},PostCoefficients{1,-.2,.03,-.8,.12}}){
        constexpr double rho=.95;const auto bound=boundModalPostSection(c,rho);CHECK(bound.passed&&std::isfinite(bound.logL1)&&bound.maximumPole<rho);
        double z1=0,z2=0,sum=0;for(unsigned n=0;n<20000;++n){const double x=n==0?1:0,y=c.b0*x+z1;z1=c.b1/rho*x-c.a1/rho*y+z2;z2=c.b2/(rho*rho)*x-c.a2/(rho*rho)*y;sum+=std::abs(y);}CHECK(sum>0&&sum<=std::exp(bound.logL1)*(1+1e-10));
    }
    CHECK(!boundModalPostSection({1,0,0,-2,1},.99).passed);CHECK(!boundModalPostSection({},0).passed);
    auto p=std::make_unique<PreparedModalField>();Settings s;s.decay.decaySeconds=.1;s.decay.brightness=1;s.mix=1;s.character=1;s.predelayMs=0;
    CHECK(prepareModalField(s,8000,testModes,*p));ModalField f;CHECK(f.prepareStorage(8000));CHECK(f.configure(*p));if(p->tail.passed&&p->tail.seconds<10){std::vector<Frame> impulse(std::size_t(std::ceil((p->tail.seconds+.01)*8000)));impulse[0]={.5,.25};CHECK(run(f,impulse,512));double last=0;for(std::size_t n=std::size_t(std::ceil(p->tail.seconds*8000));n<impulse.size();++n)for(double v:impulse[n])last=std::max(last,std::abs(v));CHECK(last<=1e-6);}
    s.decay.decaySeconds=20;s.predelayMs=250;for(unsigned j=0;j<6;++j)s.post[j]={true,false,static_cast<PostShape>(j%3),20*std::pow(2.,double(j)),18,.25,.25,CutSlope::db24};CHECK(prepareModalField(s,768000,testModes,*p));CHECK(p->tail.passed&&std::isfinite(p->tail.seconds));CHECK(f.prepareStorage(768000));CHECK(f.configure(*p));CHECK(f.storageBytes()<=16*1024*1024);
    s=Settings{};s.decay.brightness=1;for(auto& band:s.decay.bands)band={true,false,DecayShape::bell,1000,4,4};
    for(double rate:{8000.,44100.,96000.,768000.}){CHECK(prepareModalField(s,rate,testModes,*p));CHECK(p->tail.passed&&p->tail.timeBound==46&&p->tail.radius>std::exp(-std::log(1000.)/(rate*40)));}
}
}
int main(){for(std::size_t modes:{128U,192U}){testModes=modes;invalid();fixed();controls();character();tails();std::cout<<"MODAL_SPARSE_FIELD modes="<<modes<<" complete=1\n";}std::cout<<"SpaceReverbModalSparseField checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
