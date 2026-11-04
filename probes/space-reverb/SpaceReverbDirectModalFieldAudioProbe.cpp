#include "ModalField.h"
#include "DirectModalCases.h"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <string_view>
#if defined(_MSC_VER)
#include <malloc.h>
#include <xmmintrin.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;}
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
namespace {
using namespace airack::space;using namespace airack::space::probe;
constexpr std::array<double,7> rates{8000,44100,48000,96000,192000,384000,768000};
constexpr std::array<double,3> sizes{0,.5,1};
constexpr std::array<double,7> frequencies{125,250,500,1000,2000,4000,8000};
constexpr std::array<unsigned,24> corners{0,20,42,62,168,188,1365,1372,1385,1412,1414,1415,1418,1421,1424,1427,1428,1448,1449,1452,1455,1461,1462,1469};
struct Denormals {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned old=_mm_getcsr();Denormals(){_mm_setcsr(old|0x8040);}~Denormals(){_mm_setcsr(old);}
#endif
};
struct Biquad {
    double b0=0,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    double tick(double x) noexcept {const double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
    std::complex<double> response(double f,double rate) const {const auto z=std::polar(1.,-2*std::numbers::pi*f/rate);return (b0+b1*z+b2*z*z)/(1.+a1*z+a2*z*z);}
};
Biquad bandpass(double f,double rate){const double w=2*std::numbers::pi*f/rate,a=std::sin(w)/6,d=1+a;return {a/d,0,-a/d,-2*std::cos(w)/d,(1-a)/d};}
struct Meter {
    std::array<std::array<std::array<Biquad,4>,2>,7> filters{};
    std::array<double,7> early{},late{};
    bool prepare(double rate){bool valid=true;for(std::size_t b=0;b<7;++b){const auto p=bandpass(frequencies[b],rate);
        valid=valid&&std::abs(std::abs(p.response(frequencies[b],rate))-1)<=1e-10&&std::abs(p.response(0,rate))<=1e-15&&std::abs(p.response(frequencies[b]/2,rate))<.5;
        for(auto& channel:filters[b])for(auto& filter:channel)filter=p;}return valid;}
    void tick(const ModalField::Frame& wet,std::size_t frame,double rate) noexcept {
        for(std::size_t b=0;b<7;++b)for(std::size_t c=0;c<2;++c){double y=wet[c];for(auto& f:filters[b][c])y=f.tick(y);
            if(double(frame)>=.5*rate&&double(frame)<.75*rate)early[b]+=y*y;
            if(double(frame)>=1.5*rate&&double(frame)<1.75*rate)late[b]+=y*y;}}
};
struct RowHeader {std::uint32_t row,count;double rate,size,pickup,weighted;};
static_assert(sizeof(RowHeader)==40&&std::endian::native==std::endian::little&&std::numeric_limits<double>::is_iec559);
void hashBytes(std::uint64_t& hash,const void* p,std::size_t n){const auto* b=static_cast<const unsigned char*>(p);for(std::size_t i=0;i<n;++i){hash^=b[i];hash*=1099511628211ULL;}}
std::uint64_t fingerprint(const PreparedModal& p,unsigned row){std::uint64_t hash=14695981039346656037ULL;const RowHeader h{row,std::uint32_t(p.count),p.sampleRate,p.size,p.pickup,p.weightedRadius};hashBytes(hash,&h,sizeof(h));
    for(std::size_t k=0;k<p.count;++k){const std::array<double,6> v{p.frequency[k],p.rc[k],p.rs[k],p.bReal[k],p.bImag[k],p.target[k]};hashBytes(hash,v.data(),sizeof(v));}return hash;}
bool parse(std::string_view value,unsigned& result){const auto p=std::from_chars(value.data(),value.data()+value.size(),result);return p.ec==std::errc{}&&p.ptr==value.data()+value.size();}
}
int main(int argc,char** argv){
    if(argc!=4)return 2;const std::string_view mode=argv[1];unsigned count=0,ordinal=0;
    if(!parse(argv[2],count)||(count!=128&&count!=192&&count!=256&&count!=512&&count!=1024&&count!=2048)||!parse(argv[3],ordinal))return 2;
    const bool calibration=mode=="--calibrate",corpusRow=mode=="--row",corner=mode=="--corner";
    if((!calibration&&!corpusRow&&!corner)||(calibration&&ordinal!=49)||(corpusRow&&ordinal>=333)||(corner&&ordinal>=24))return 2;
    const unsigned row=corner?corners[ordinal]:(ordinal/9)*21+((ordinal%9)/3+1)*3+ordinal%3;
    const auto cases=directModalCurves();const auto& curve=cases[row/21].curve;const double rate=rates[row%21/3],size=sizes[row%3];
    const char* label=calibration?"calibrate":corner?"corner":"row";auto field=std::make_unique<PreparedModalField>();auto* p=&field->modal;ModalField tank;Settings settings;settings.decay=curve;settings.size=size;settings.mix=1;settings.character=0;
    const bool prepared=prepareModalField(settings,rate,count,*field)&&tank.prepareStorage(rate,ModalBackend::automatic)&&tank.configure(*field);
    std::cout<<std::setprecision(17)<<"MODAL_FIELD_AUDIO_PREP mode="<<label<<" ordinal="<<ordinal<<" row="<<row<<" count="<<count<<" rate="<<rate<<" size="<<size<<" decay="<<curve.decaySeconds<<" brightness="<<curve.brightness<<" prepared="<<prepared;
    if(prepared)std::cout<<" bank="<<fingerprint(*p,row)<<" character="<<settings.character<<" predelay_ms="<<settings.predelayMs<<" distance="<<settings.distance<<" width="<<settings.width<<" mix="<<settings.mix<<" duck_range="<<settings.duckRangeDb<<" post_stages="<<field->post.count<<" tail_seconds="<<field->tail.seconds<<" finite_support="<<field->tail.finiteSupportFrames<<" tail_radius="<<field->tail.radius;std::cout<<'\n'<<std::flush;
    if(!prepared){std::cout<<"MODAL_FIELD_AUDIO_FOOTER mode="<<label<<" ordinal="<<ordinal<<" row="<<row<<" failed=1 complete=1\n";return 1;}
    const double upper=std::min(20000.,.45*rate),evaluationRate=std::min(rate,384000.);double longest=.1;
    for(unsigned k=0;k<=768;++k)longest=std::max(longest,evaluate(curve,20*std::pow(upper/20,k/768.),evaluationRate)->seconds);
    for(const auto& b:curve.bands)if(b.active&&!b.bypassed)longest=std::max(longest,evaluate(curve,std::min(b.frequencyHz,upper),evaluationRate)->seconds);
    for(std::size_t k=0;k<p->count;++k)longest=std::max(longest,p->target[k]);
    const auto frames=std::size_t(std::ceil(std::max(2.,1.25*longest+.3)*rate)),tailStart=frames-std::min(frames,std::size_t(std::ceil(.25*rate)));
    Meter meter,twice;const bool meters=corner||(meter.prepare(rate)&&(!calibration||twice.prepare(rate)));bool finite=true,rendered=true;
    double peak=0,energy=0,tail=0,twiceEnergy=0;allocations=frees=0;std::array<ModalField::Frame,512> block{};
    {Denormals guard;for(std::size_t base=0;base<frames;base+=block.size()){
        const auto n=std::min(block.size(),frames-base);std::fill(block.begin(),block.end(),ModalField::Frame{});if(base==0)block[0]={.5,.25};
        tracking=true;rendered=tank.processBlock(std::span(block.data(),n))&&rendered;tracking=false;
        for(std::size_t i=0;i<n;++i){const auto& wet=block[i];const auto frame=base+i;finite=finite&&std::isfinite(wet[0])&&std::isfinite(wet[1]);
            peak=std::max({peak,std::abs(wet[0]),std::abs(wet[1])});const double e=wet[0]*wet[0]+wet[1]*wet[1];energy+=e;if(frame>=tailStart)tail+=e;
            if(!corner)meter.tick(wet,frame,rate);if(calibration){const ModalField::Frame doubled{2*wet[0],2*wet[1]};twice.tick(doubled,frame,rate);twiceEnergy+=doubled[0]*doubled[0]+doubled[1]*doubled[1];}}
    }}
    const double fraction=energy>0?tail/energy:std::numeric_limits<double>::infinity();bool pass=rendered&&meters&&finite&&energy>0&&std::isfinite(energy)&&peak>0&&peak<=4&&fraction<=1e-6&&tank.storageBytes()<=16*1024*1024&&allocations==0&&frees==0;
    std::cout<<"MODAL_FIELD_AUDIO_RAW mode="<<label<<" ordinal="<<ordinal<<" row="<<row<<" frames="<<frames<<" longest="<<longest<<" peak="<<peak<<" energy="<<energy<<" tail_energy="<<tail<<" tail_fraction="<<fraction<<" bytes="<<tank.storageBytes()<<" finite="<<finite<<" rendered="<<rendered<<" meters="<<meters<<" allocations="<<allocations<<" frees="<<frees<<" pass="<<pass<<'\n';
    if(!corner)for(std::size_t b=0;b<7;++b){const double ratio=meter.early[b]>0?meter.late[b]/meter.early[b]:std::numeric_limits<double>::infinity();
        bool meterPass=meter.early[b]>0&&std::isfinite(meter.early[b])&&std::isfinite(meter.late[b])&&std::isfinite(ratio);
        std::cout<<"MODAL_FIELD_AUDIO_METER mode="<<label<<" ordinal="<<ordinal<<" row="<<row<<" frequency="<<frequencies[b]<<" early="<<meter.early[b]<<" late="<<meter.late[b]<<" ratio="<<ratio;
        if(calibration){const double scaled=twice.early[b]>0?twice.late[b]/twice.early[b]:std::numeric_limits<double>::infinity();const double error=std::abs(scaled/ratio-1);meterPass=meterPass&&std::isfinite(error)&&error<=1e-12;
            std::cout<<" twice_early="<<twice.early[b]<<" twice_late="<<twice.late[b]<<" twice_ratio="<<scaled<<" ratio_error="<<error;}
        pass=pass&&meterPass;std::cout<<" pass="<<meterPass<<'\n';}
    if(calibration){const double error=std::abs(twiceEnergy/(4*energy)-1);const bool valid=std::isfinite(error)&&error<=1e-12;pass=pass&&valid;std::cout<<"MODAL_FIELD_AUDIO_CALIBRATION energy="<<energy<<" twice_energy="<<twiceEnergy<<" error="<<error<<" pass="<<valid<<'\n';}
    std::cout<<"MODAL_FIELD_AUDIO_FOOTER mode="<<label<<" ordinal="<<ordinal<<" row="<<row<<" failed="<<!pass<<" complete=1\n";return pass?0:1;
}
