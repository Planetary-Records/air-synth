#include "ModalTank.h"
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<50)std::cerr<<"DIRECT_MODAL_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
#include "DirectModalCases.h"
#include "ModalKernel.h"
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string_view>
namespace {
using namespace airack::space;using namespace airack::space::probe;
constexpr std::array<double,7> rates{8000,44100,48000,96000,192000,384000,768000};
constexpr std::array<double,3> sizes{0,.5,1};
struct FileHeader {std::uint64_t magic;std::uint32_t version,count,start,rows;};
struct RowHeader {std::uint32_t row,count;double rate,size,pickup,weighted;};
struct ModeRecord {double frequency,rc,rs,br,bi,target,real,imag;};
static_assert(sizeof(FileHeader)==24&&sizeof(RowHeader)==40&&sizeof(ModeRecord)==64&&std::endian::native==std::endian::little&&std::numeric_limits<double>::is_iec559);
template<class T> void write(std::ofstream& out,const T& value){out.write(reinterpret_cast<const char*>(&value),sizeof(value));}
bool prime(std::size_t n){if(n<2)return false;for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;}
double legacySum(double rate,double size){double sum=0;for(double seconds:{.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719}){auto n=std::size_t(std::llround(seconds*(.5+1.5*size)*rate));while(!prime(n))++n;sum+=double(n)/rate;}return sum;}
void settings(std::size_t index,const DecayCurve& c){
    std::cout<<"MODAL_RANGE_SETTINGS curve="<<index<<" decay="<<c.decaySeconds<<" brightness="<<c.brightness<<'\n';
    for(std::size_t k=0;k<6;++k){const auto& b=c.bands[k];std::cout<<"MODAL_RANGE_BAND curve="<<index<<" band="<<k<<" active="<<b.active<<" bypass="<<b.bypassed<<" shape="<<int(b.shape)<<" frequency="<<b.frequencyHz<<" multiplier="<<b.multiplier<<" q="<<b.q<<'\n';}
}
bool statesEqual(const ModalTank& a,const ModalTank& b,std::size_t count){for(std::size_t k=0;k<count;++k)if(std::bit_cast<std::array<std::uint64_t,2>>(a.modeState(k))!=std::bit_cast<std::array<std::uint64_t,2>>(b.modeState(k)))return false;return true;}
}
int main(int argc,char** argv){
    if(argc!=4||std::string_view(argv[1])!="--range")return 2;std::uint32_t count=0;const std::string_view value=argv[2];const auto parsed=std::from_chars(value.data(),value.data()+value.size(),count);
    if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||(count!=128&&count!=192&&count!=256&&count!=512&&count!=1024&&count!=2048)||std::filesystem::exists(argv[3]))return 2;
    const auto cases=directModalCurves();if(cases.size()!=70)return 2;std::ofstream output(argv[3],std::ios::binary);if(!output)return 2;
    write(output,FileHeader{0x31454c4f504d5244ULL,1,count,0,1470});std::size_t bytes=24,failed=0,totalModes=0,totalRegions=0;
    std::cout<<std::setprecision(17);for(std::size_t k=0;k<cases.size();++k)settings(k,cases[k].curve);
    auto p=std::make_unique<PreparedModal>();auto records=std::make_unique<std::array<ModeRecord,maximumDirectModes>>();
    std::array<double,maximumDirectModes> hz{},times{},errors{},residuals{},realParts{};
    std::array<ModalFrame,64> stimulus{};for(std::size_t n=0;n<stimulus.size();++n)stimulus[n]={n==0?.5:double(int(n*37%127)-63)/32768.,n==0?.25:double(int(n*53%127)-63)/65536.};
    for(std::size_t row=0;row<1470;++row){
        const auto& curve=cases[row/21].curve;const double rate=rates[row%21/3],size=sizes[row%3],upper=std::min(20000.,.45*rate),delaySum=legacySum(rate,size);
        *p=PreparedModal{};const bool prepared=prepareModal(curve,size,rate,count,*p);ModalTank scalar,automatic;
        bool pass=prepared&&scalar.prepareStorage(rate)&&automatic.prepareStorage(rate,ModalBackend::automatic)&&scalar.configure(*p)&&automatic.configure(*p);
        auto a=stimulus,b=stimulus;allocations=frees=0;tracking=true;
        const bool rendered=pass&&scalar.processBlock(a)&&automatic.processBlock(b);const bool bitAudio=std::memcmp(a.data(),b.data(),sizeof(a))==0;
        const bool bitState=statesEqual(scalar,automatic,p->count);tracking=false;
        pass=pass&&rendered&&bitAudio&&bitState&&allocations==0&&frees==0&&automatic.storageBytes()<=16*1024*1024;
        double maximumError=0,maximumResidual=0,weighted=0;std::size_t factors=0;
        for(std::size_t k=0;k<p->count;++k){
            const double rc=p->rc[k],rs=p->rs[k],radius=std::hypot(rc,rs);hz[k]=std::atan2(rs,rc)*rate/(2*std::numbers::pi);
            realParts[k]=rate*std::log(radius);times[k]=-std::log(1000.)/realParts[k];const auto expected=evaluate(curve,std::clamp(hz[k],20.,upper),std::min(rate,384000.));
            errors[k]=expected?std::abs(times[k]/expected->seconds-1):std::numeric_limits<double>::infinity();residuals[k]=0;
            for(const auto root:{std::complex<double>{rc,rs},std::complex<double>{rc,-rs}}){residuals[k]=std::max(residuals[k],std::abs(root*root-2*rc*root+(rc*rc+rs*rs)));++factors;}
            weighted=std::max(weighted,radius/std::exp(-std::log(1000.)/(rate*46)));maximumError=std::max(maximumError,errors[k]);maximumResidual=std::max(maximumResidual,residuals[k]);
            pass=pass&&std::isfinite(times[k])&&radius>0&&radius<1&&std::abs(hz[k]-p->frequency[k])<=1e-8&&errors[k]<=1e-6&&residuals[k]<=1e-9;
            const auto state=automatic.modeState(k);(*records)[k]={p->frequency[k],rc,rs,p->bReal[k],p->bImag[k],p->target[k],state.real,state.imag};
        }
        pass=pass&&factors==2*count&&weighted>0&&weighted<1;
        std::vector<double> centers{20,125,250,500,1000,2000,4000,std::min(8000.,upper),upper};for(const auto& band:curve.bands)if(band.active&&!band.bypassed)centers.push_back(std::min(band.frequencyHz,upper));
        for(auto& f:centers)f=std::min(f,upper);std::sort(centers.begin(),centers.end());centers.erase(std::unique(centers.begin(),centers.end()),centers.end());
        for(double center:centers){
            const double half=std::max(2/delaySum,center/24),low=std::max(20.,center-half),high=std::min(upper,center+half);double minimum=40;
            for(unsigned j=0;j<=128;++j)minimum=std::min(minimum,evaluate(curve,low+(high-low)*j/128.,std::min(rate,384000.))->seconds);
            const double left=-2*std::log(1000.)/minimum;std::size_t admitted=0;double error=0,residual=0;
            for(std::size_t k=0;k<p->count;++k)if(hz[k]>=low-1e-8&&hz[k]<=high+1e-8&&realParts[k]>=left&&realParts[k]<=1e-6){++admitted;error=std::max(error,errors[k]);residual=std::max(residual,residuals[k]);}
            const bool covered=admitted>0&&error<=.15&&residual<=1e-9;pass=pass&&covered;++totalRegions;
            std::cout<<"MODAL_RANGE_REGION row="<<row<<" center="<<center<<" low="<<low<<" high="<<high<<" left="<<left<<" positive_factors="<<admitted<<" error="<<error<<" residual="<<residual<<" pass="<<covered<<'\n';
        }
        write(output,RowHeader{std::uint32_t(row),std::uint32_t(p->count),rate,size,p->pickup,p->weightedRadius});output.write(reinterpret_cast<const char*>(records->data()),std::streamsize(p->count*sizeof(ModeRecord)));output.flush();
        bytes+=40+p->count*sizeof(ModeRecord);totalModes+=p->count;
        if(!output)return 2;if(!pass)++failed;
        std::cout<<"MODAL_RANGE_ROW row="<<row<<" curve="<<row/21<<" rate="<<rate<<" size="<<size<<" modes="<<p->count<<" factors="<<factors<<" error="<<maximumError<<" residual="<<maximumResidual<<" weighted="<<weighted<<" bytes="<<automatic.storageBytes()<<" bit_audio="<<bitAudio<<" bit_state="<<bitState<<" allocations="<<allocations<<" frees="<<frees<<" backend="<<automatic.backendName()<<" pass="<<pass<<'\n'<<std::flush;
    }
    output.close();if(output.fail())return 2;
    std::cout<<"MODAL_RANGE_FOOTER count="<<count<<" rows=1470 failed="<<failed<<" modes="<<totalModes<<" factors="<<2*totalModes<<" regions="<<totalRegions<<" sidecar_bytes="<<bytes<<" complete=1\n";return failed?1:0;
}
