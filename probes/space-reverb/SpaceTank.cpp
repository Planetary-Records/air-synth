#include "SpaceTank.h"
#include <algorithm>
#include <cmath>
#include <utility>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space::probe
{
namespace
{
bool prime(std::size_t n) noexcept
{
    if(n<2)return false;
    for(std::size_t divisor=2;divisor*divisor<=n;++divisor)if(n%divisor==0)return false;
    return true;
}
std::size_t nextPrime(std::size_t n) noexcept {while(!prime(n))++n;return n;}
constexpr std::array<double,8> lineSeconds{.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719};
constexpr std::array<std::array<double,4>,2> diffusionSeconds{{{.0037,.0053,.0071,.0109},{.0043,.0061,.0083,.0127}}};
constexpr double normalization=.3535533905932737622;
}
bool SpaceTank::prepare(double rate,int maxBlock,const DecayCurve& curve,double size,FitGeometry geometry,bool inputDiffusion,bool packed,TankGeometry tank,FitDomain domain,TankBackend backend)
{
    if(!valid(curve)||!std::isfinite(rate)||rate<8000||rate>384000
        ||maxBlock<1||maxBlock>4096||!std::isfinite(size)||size<0||size>1||int(backend)<0||int(backend)>2)return false;
    SpaceTank candidate;
    if(packed&&backend!=TankBackend::sse2)candidate.accelerated_=availableAvx2Kernel();
    if(backend==TankBackend::avx2&&!candidate.accelerated_)return false;
    candidate.lines_=tank==TankGeometry::sixteen?16:8;
    double shortestTime=40;
    const double upper=std::min(20000.,.45*rate);
    for(int i=0;i<193;++i)shortestTime=std::min(shortestTime,evaluate(curve,20*std::pow(upper/20,i/192.),rate)->seconds);
    for(const auto& b:curve.bands)if(b.active&&!b.bypassed)shortestTime=std::min(shortestTime,evaluate(curve,b.frequencyHz,rate)->seconds);
    for(std::size_t i=0;i<candidate.lines_;++i){
        const double scale=candidate.lines_==16?.5*(i<8?.97:1.03):1;
        const auto length=nextPrime(std::size_t(std::llround(lineSeconds[i%8]*scale*(.5+1.5*size)*rate)));
        auto fit=fitLoss(curve,rate,double(length)/rate,geometry,domain);
        if(!fit.converged)return false;
        candidate.maximumFitError_=std::max(candidate.maximumFitError_,fit.maxRelativeError);
        candidate.maximumNominalError_=std::max(candidate.maximumNominalError_,fit.maxNominalRelativeError);
        candidate.losses_[i]=std::move(fit.bank);
        candidate.delays_[i].data.resize(length);
    }
    for(std::size_t line=0;line<candidate.lines_;++line){
        candidate.stageCount_=std::max(candidate.stageCount_,candidate.losses_[line].stages.size());
        for(std::size_t stage=0;stage<candidate.losses_[line].stages.size();++stage){
            const auto& source=candidate.losses_[line].stages[stage];auto& dest=candidate.packedStages_[stage];
            dest.b0[line]=source.b0;dest.b1[line]=source.b1;dest.b2[line]=source.b2;dest.a1[line]=source.a1;dest.a2[line]=source.a2;
        }
    }
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t stage=0;stage<4;++stage){
        auto& ring=candidate.diffusion_[ch][stage];
        const auto length=std::size_t(std::llround(diffusionSeconds[ch][stage]*(.75+.5*size)*rate));
        ring.data.resize(std::max(std::size_t(1),length));
        ring.attenuation=std::pow(10.,-3*double(ring.data.size())/(rate*shortestTime));
    }
    candidate.rate_=rate;candidate.packed_=packed;candidate.diffuse_=inputDiffusion;candidate.ready_=true;*this=std::move(candidate);return true;
}
void SpaceTank::reset() noexcept
{
    for(auto& ring:delays_){std::fill(ring.data.begin(),ring.data.end(),0);ring.cursor=0;}
    for(auto& channel:diffusion_)for(auto& ring:channel){std::fill(ring.data.begin(),ring.data.end(),0);ring.cursor=0;}
    for(auto& loss:losses_)for(auto& s:loss.stages)s.z1=s.z2=0;
    for(auto& stage:packedStages_){stage.z1.fill(0);stage.z2.fill(0);}
}
std::array<double,2> SpaceTank::tick(std::array<double,2> excitation) noexcept
{
    if(!ready_)return {};
    if(diffuse_)for(std::size_t ch=0;ch<2;++ch)for(auto& ring:diffusion_[ch]){
        const double delayed=ring.attenuation*ring.data[ring.cursor];
        const double next=delayed-.6*excitation[ch];
        ring.data[ring.cursor]=excitation[ch]+.6*next;
        if(++ring.cursor==ring.data.size())ring.cursor=0;
        excitation[ch]=next;
    }
    std::array<double,16> values{};
    for(std::size_t i=0;i<lines_;++i)values[i]=delays_[i].data[delays_[i].cursor];
#if defined(_M_X64) || defined(__SSE2__)
    if(packed_){
      if(accelerated_)accelerated_(packedStages_.data(),stageCount_,values.data(),lines_);
      else {
      for(std::size_t group=0;group<lines_;group+=8){
        auto x0=_mm_loadu_pd(values.data()+group),x1=_mm_loadu_pd(values.data()+group+2),x2=_mm_loadu_pd(values.data()+group+4),x3=_mm_loadu_pd(values.data()+group+6);
        for(std::size_t index=0;index<stageCount_;++index){auto& s=packedStages_[index];
            auto tickPair=[&](const __m128d x,std::size_t lane){
                const auto y=_mm_add_pd(_mm_mul_pd(_mm_load_pd(s.b0.data()+lane),x),_mm_load_pd(s.z1.data()+lane));
                _mm_store_pd(s.z1.data()+lane,_mm_add_pd(_mm_sub_pd(_mm_mul_pd(_mm_load_pd(s.b1.data()+lane),x),_mm_mul_pd(_mm_load_pd(s.a1.data()+lane),y)),_mm_load_pd(s.z2.data()+lane)));
                _mm_store_pd(s.z2.data()+lane,_mm_sub_pd(_mm_mul_pd(_mm_load_pd(s.b2.data()+lane),x),_mm_mul_pd(_mm_load_pd(s.a2.data()+lane),y)));
                return y;
            };
            x0=tickPair(x0,group);x1=tickPair(x1,group+2);x2=tickPair(x2,group+4);x3=tickPair(x3,group+6);
        }
        _mm_storeu_pd(values.data()+group,x0);_mm_storeu_pd(values.data()+group+2,x1);_mm_storeu_pd(values.data()+group+4,x2);_mm_storeu_pd(values.data()+group+6,x3);
      }}
        for(std::size_t i=0;i<lines_;++i)values[i]*=losses_[i].scalar;
    }else
#endif
    for(std::size_t i=0;i<lines_;++i)values[i]=losses_[i].tick(values[i]);
    auto mixed=values;
    for(std::size_t stride=1;stride<lines_;stride*=2)for(std::size_t start=0;start<lines_;start+=2*stride)
        for(std::size_t j=0;j<stride;++j){const double a=mixed[start+j],b=mixed[start+j+stride];mixed[start+j]=a+b;mixed[start+j+stride]=a-b;}
    std::array<double,2> wet{};
    const double norm=lines_==8?normalization:.25;
    const double injection=lines_==8?.25:normalization*.5;
    for(std::size_t i=0;i<lines_;++i){
        const double sign=i%2?-1.:1.;
        auto& ring=delays_[i];ring.data[ring.cursor]=injection*(excitation[0]+sign*excitation[1])+norm*mixed[i];
        if(++ring.cursor==ring.data.size())ring.cursor=0;
        wet[0]+=norm*values[i];wet[1]+=norm*sign*values[i];
    }
    return wet;
}
std::size_t SpaceTank::preparedBytes() const noexcept
{
    std::size_t bytes=sizeof(*this);
    for(const auto& ring:delays_)bytes+=ring.data.capacity()*sizeof(double);
    for(const auto& channel:diffusion_)for(const auto& ring:channel)bytes+=ring.data.capacity()*sizeof(double);
    for(const auto& bank:losses_)bytes+=bank.stages.capacity()*sizeof(Biquad);
    return bytes;
}
SpaceTank::LineView SpaceTank::inspectLine(std::size_t index) const noexcept
{return ready_&&index<lines_?LineView{double(delays_[index].data.size())/rate_,&losses_[index]}:LineView{};}
}

