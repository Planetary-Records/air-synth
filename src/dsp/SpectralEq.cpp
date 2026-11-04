#include "SpectralEq.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <numeric>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace airack {
namespace {
using Complex=std::complex<float>;
bool supported(const DynamicEqSettings& s) noexcept {
 if(!validateDynamicEq(s))return false;
 for(const auto& b:s.bands)if(b.spectralEnabled&&(b.shape>DynamicEqShape::highShelf
   ||b.detectorSource!=DynamicEqDetectorSource::internalBand||b.autoThreshold||b.adaptiveTiming))return false;
 return true;
}
}
struct SpectralEq::State {
 DynamicEqSettings settings;
 double rate=0;
 std::size_t size=0,hop=0,position=0,untilHop=0,firstHop=0;
 float amplitudeScale=0;
 std::uint64_t transforms=0;
 std::vector<float> window,logFrequency,levels,medianScratch,positive,negative;
 std::vector<DynamicEq::Frame> input,output;
 std::vector<Complex> twiddles;
 std::vector<std::size_t> permutation;
 std::array<std::vector<Complex>,2> bins;
 struct Band {std::vector<float> weight,envelope;float attack=0,release=0;bool curveReady=false;float frequency=0,q=0;DynamicEqShape shape=DynamicEqShape::bell;std::size_t weighted=0;};
 std::array<Band,8> bands;
 std::array<float,8> movement{};
 enum class Phase { idle,capture,forwardLeft,forwardRight,clear,band,curve,levels,narrow,
                    medianClear,medianCount,medianScan,apply,mirrorLeft,inverseLeft,mirrorRight,inverseRight,overlap };
 SpectralEqWork work=SpectralEqWork::immediate;
 SpectralEqWorkStats stats;
 Phase phase=Phase::idle;
 DynamicEqSettings frameSettings;
 std::array<float,8> frameMovement{};
 std::array<std::uint32_t,256> histogram{};
 std::array<double,6> coefficients{};
 std::size_t frameInput=0,frameOutput=0,cursor=0,bandIndex=0,used=0,narrowBegin=0,narrowEnd=0;
 std::size_t fftLength=1,fftCursor=0,medianRank=0,medianBucket=0;
 std::uint32_t medianPrefix=0;
 int medianByte=3;
 bool medianFound=false;
 float gate=0,bandCenter=0;
 double curveLow=0,curveHigh=0,curveFrequency=0;
 std::uint64_t frameWork=0,workRemainder=0;

 bool sameCurve(std::size_t index,const DynamicEqBand& b) const noexcept {
  const auto& cached=bands[index];
  return cached.curveReady&&cached.frequency==b.frequencyHz&&cached.q==b.q&&cached.shape==b.shape;
 }
 void beginWork() noexcept {
  if(phase!=Phase::idle)++stats.deadlineMisses;
  frameSettings=settings;frameMovement={};frameInput=(position+input.size()-size)%input.size();
  frameOutput=(position+hop)%output.size();cursor=0;phase=Phase::capture;fftLength=1;fftCursor=0;
  const auto count=size/2+1;const auto order=std::bit_width(size)-1;
  frameWork=9*size+2*size*order+count-2;
  for(std::size_t i=0;i<8;++i){const auto& b=frameSettings.bands[i];if(!b.active||b.bypassed||!b.spectralEnabled)continue;
   const bool cached=sameCurve(i,b);const auto selected=cached?bands[i].weighted:count;
   frameWork+=2*count+(cached?0:count)+9;
   if(selected)frameWork+=4*(512+std::max<std::size_t>(9,selected));
  }
  workRemainder=0;++stats.framesStarted;
 }
 bool advanceFFT(std::vector<Complex>& data,bool inverse,std::uint64_t& budget) noexcept {
  while(budget){
   if(fftLength==1){const auto end=fftCursor+std::min<std::size_t>(size-fftCursor,std::size_t(budget));
    for(auto i=fftCursor;i<end;++i)if(i<permutation[i])std::swap(data[i],data[permutation[i]]);
    budget-=end-fftCursor;fftCursor=end;if(fftCursor<size)return false;fftCursor=0;fftLength=2;
   }else if(fftLength<=size){const auto half=fftLength/2,stride=size/fftLength;
    auto start=(fftCursor/half)*fftLength,k=fftCursor%half;
    while(budget&&fftCursor<size/2){
     const auto amount=std::min<std::size_t>(half-k,std::size_t(budget));
     auto j=k;
#if defined(_M_X64) || defined(__SSE2__)
     const auto conjugate=_mm_set1_ps(inverse?-0.f:0.f),subtractReal=_mm_set_ps(0.f,-0.f,0.f,-0.f);
     for(;j+1<k+amount;j+=2){
      const auto a=twiddles[j*stride],b=twiddles[(j+1)*stride];
      const auto wr=_mm_set_ps(b.real(),b.real(),a.real(),a.real());
      const auto wi=_mm_xor_ps(_mm_set_ps(b.imag(),b.imag(),a.imag(),a.imag()),conjugate);
      const auto even=_mm_loadu_ps(reinterpret_cast<const float*>(data.data()+start+j));
      const auto odd=_mm_loadu_ps(reinterpret_cast<const float*>(data.data()+start+j+half));
      const auto cross=_mm_mul_ps(_mm_shuffle_ps(odd,odd,_MM_SHUFFLE(2,3,0,1)),wi);
      const auto product=_mm_add_ps(_mm_mul_ps(odd,wr),_mm_xor_ps(cross,subtractReal));
      _mm_storeu_ps(reinterpret_cast<float*>(data.data()+start+j),_mm_add_ps(even,product));
      _mm_storeu_ps(reinterpret_cast<float*>(data.data()+start+j+half),_mm_sub_ps(even,product));
     }
#endif
     for(;j<k+amount;++j){const auto w=inverse?std::conj(twiddles[j*stride]):twiddles[j*stride];
      const auto even=data[start+j],odd=data[start+j+half]*w;data[start+j]=even+odd;data[start+j+half]=even-odd;}
     fftCursor+=amount;budget-=amount;k+=amount;if(k==half){k=0;start+=fftLength;}
    }
    if(fftCursor<size/2)return false;fftCursor=0;fftLength*=2;
    if(fftLength>size&&!inverse){++transforms;fftLength=1;return true;}
   }else{const auto end=fftCursor+std::min<std::size_t>(size-fftCursor,std::size_t(budget));
    for(auto i=fftCursor;i<end;++i)data[i]/=float(size);
    budget-=end-fftCursor;fftCursor=end;if(fftCursor<size)return false;
    ++transforms;fftCursor=0;fftLength=1;return true;
   }
  }
  return false;
 }
 float workLevel(std::size_t k,const DynamicEqBand& b) const noexcept {
  const auto l=bins[0][k],r=bins[1][k];
  const float power=b.placement==DynamicEqPlacement::stereo?(std::norm(l)+std::norm(r))*.5f:
   b.placement==DynamicEqPlacement::left?std::norm(l):b.placement==DynamicEqPlacement::right?std::norm(r):
   std::norm((b.placement==DynamicEqPlacement::mid?l+r:l-r)*.5f);
  const float amplitude=std::sqrt(power)*amplitudeScale*((k==0||k==size/2)?.5f:1.f);
  return 20*std::log10(std::max(amplitude,1.e-12f));
 }
 static std::uint32_t orderedFloat(float value) noexcept {
  const auto bits=std::bit_cast<std::uint32_t>(value);return bits&0x80000000u?~bits:bits^0x80000000u;
 }
 void beginMedian() noexcept {
  if(!used){gate=0;cursor=0;phase=Phase::apply;return;}
  medianPrefix=0;medianByte=3;medianRank=used/2;cursor=0;phase=Phase::medianClear;
 }
 void applyWorkBin(std::size_t k) noexcept {
  const auto& b=frameSettings.bands[bandIndex];auto& prepared=bands[bandIndex];
  if(prepared.weight[k]==0&&prepared.envelope[k]==0)return;
  const float threshold=b.thresholdDb+b.spectralTiltDbOct*(logFrequency[k]-bandCenter);
  const float target=prepared.weight[k]>0&&levels[k]>gate?dynamicEqTargetGainDb(levels[k],threshold,b.rangeDb):0;
  auto& envelope=prepared.envelope[k];const auto timing=std::abs(target)>std::abs(envelope)?prepared.attack:prepared.release;
  envelope=target+timing*(envelope-target);frameMovement[bandIndex]=std::max(frameMovement[bandIndex],std::abs(envelope));
  if(prepared.weight[k]==0)return;
  const float base=std::clamp(b.gainDb*frameSettings.gainScale,-24.f,24.f);
  const float db=prepared.weight[k]*std::clamp(base+envelope,-24.f,24.f);
  const float bounded=std::clamp(db,-24.f-negative[k],24.f-positive[k]);
  if(bounded>0)positive[k]+=bounded;else negative[k]+=bounded;
  const float gain=std::pow(10.f,bounded/20);auto& l=bins[0][k];auto& r=bins[1][k];
  switch(b.placement){
   case DynamicEqPlacement::stereo:l*=gain;r*=gain;break;
   case DynamicEqPlacement::left:l*=gain;break;
   case DynamicEqPlacement::right:r*=gain;break;
   case DynamicEqPlacement::mid:{const auto delta=(l+r)*(.5f*(gain-1));l+=delta;r+=delta;break;}
   case DynamicEqPlacement::side:{const auto delta=(l-r)*(.5f*(gain-1));l+=delta;r-=delta;break;}
  }
 }
 void advanceWork(std::uint64_t& budget) noexcept {
  const auto count=size/2+1;
  const auto endAt=[&](std::size_t limit){return cursor+std::min<std::size_t>(limit-cursor,std::size_t(budget));};
  const auto consume=[&](std::size_t end){budget-=end-cursor;cursor=end;};
  while(budget&&phase!=Phase::idle){
   switch(phase){
    case Phase::capture:{const auto end=endAt(size);for(auto j=cursor;j<end;++j)for(int ch=0;ch<2;++ch)bins[ch][j]={float(input[(frameInput+j)%input.size()][ch])*window[j],0};consume(end);if(cursor==size){cursor=0;phase=Phase::forwardLeft;}break;}
    case Phase::forwardLeft:if(advanceFFT(bins[0],false,budget))phase=Phase::forwardRight;break;
    case Phase::forwardRight:if(advanceFFT(bins[1],false,budget)){cursor=0;phase=Phase::clear;}break;
    case Phase::clear:{const auto end=endAt(count);for(auto j=cursor;j<end;++j)positive[j]=negative[j]=0;consume(end);if(cursor==count){bandIndex=0;phase=Phase::band;}break;}
    case Phase::band:{
     if(bandIndex==8){cursor=1;phase=Phase::mirrorLeft;break;}
     const auto& b=frameSettings.bands[bandIndex];if(!b.active||b.bypassed||!b.spectralEnabled){++bandIndex;break;}
     auto& prepared=bands[bandIndex];prepared.attack=float(std::exp(-double(hop)/(rate*b.attackMs*.001)));prepared.release=float(std::exp(-double(hop)/(rate*b.releaseMs*.001)));
     bandCenter=std::log2(b.frequencyHz);used=0;cursor=0;
     if(sameCurve(bandIndex,b)){phase=Phase::levels;break;}
     using Factory=juce::dsp::IIR::ArrayCoefficients<double>;curveFrequency=std::min(double(b.frequencyHz),.45*rate);
     coefficients=b.shape==DynamicEqShape::bell?Factory::makeBandPass(rate,curveFrequency,b.q):b.shape==DynamicEqShape::lowShelf?Factory::makeLowPass(rate,curveFrequency,std::sqrt(.5)):Factory::makeHighPass(rate,curveFrequency,std::sqrt(.5));
     const double half=1./(2*b.q);curveLow=curveFrequency*(std::sqrt(1+half*half)-half);curveHigh=curveFrequency*(std::sqrt(1+half*half)+half);
     prepared.curveReady=false;prepared.weighted=0;phase=Phase::curve;break;
    }
    case Phase::curve:{const auto end=endAt(count);auto& prepared=bands[bandIndex];const auto& b=frameSettings.bands[bandIndex];const auto& c=coefficients;
     for(auto k=cursor;k<end;++k){const double f=double(k)*rate/size;const bool inside=b.shape==DynamicEqShape::bell?f>=curveLow&&f<=curveHigh:b.shape==DynamicEqShape::lowShelf?f<=curveFrequency:f>=curveFrequency;
      if(!inside){prepared.weight[k]=0;continue;}const auto z=std::polar(1.,-2*std::numbers::pi*double(k)/size);
      prepared.weight[k]=float(std::clamp(std::abs((c[0]+c[1]*z+c[2]*z*z)/(c[3]+c[4]*z+c[5]*z*z)),0.,1.));if(prepared.weight[k]>0)++prepared.weighted;
     }
     consume(end);if(cursor==count){prepared.frequency=b.frequencyHz;prepared.q=b.q;prepared.shape=b.shape;prepared.curveReady=true;cursor=0;phase=Phase::levels;}break;
    }
    case Phase::levels:{const auto end=endAt(count);const auto& b=frameSettings.bands[bandIndex];const auto& prepared=bands[bandIndex];
     for(auto k=cursor;k<end;++k)if(prepared.weight[k]!=0){levels[k]=workLevel(k,b);medianScratch[used++]=levels[k];}
     consume(end);if(cursor==count){if(used>0&&used<9){const auto center=std::min(count-1,std::size_t(std::llround(b.frequencyHz*size/rate)));narrowBegin=center>4?center-4:0;narrowEnd=std::min(count,narrowBegin+9);used=0;cursor=narrowBegin;phase=Phase::narrow;}else beginMedian();}break;
    }
    case Phase::narrow:{const auto end=endAt(narrowEnd);for(auto k=cursor;k<end;++k)medianScratch[used++]=workLevel(k,frameSettings.bands[bandIndex]);consume(end);if(cursor==narrowEnd)beginMedian();break;}
    case Phase::medianClear:{const auto end=endAt(256);for(auto j=cursor;j<end;++j)histogram[j]=0;consume(end);if(cursor==256){cursor=0;phase=Phase::medianCount;}break;}
    case Phase::medianCount:{const auto end=endAt(used);const auto shift=8*medianByte;const auto mask=medianByte==3?0u:0xffffffffu<<(shift+8);
     for(auto j=cursor;j<end;++j){const auto key=orderedFloat(medianScratch[j]);if((key&mask)==medianPrefix)++histogram[(key>>shift)&255u];}
     consume(end);if(cursor==used){cursor=0;medianFound=false;phase=Phase::medianScan;}break;
    }
    case Phase::medianScan:{const auto end=endAt(256);for(auto j=cursor;j<end;++j)if(!medianFound){if(medianRank<histogram[j]){medianBucket=j;medianFound=true;}else medianRank-=histogram[j];}consume(end);
     if(cursor==256){medianPrefix|=std::uint32_t(medianBucket)<<(8*medianByte);cursor=0;if(medianByte--){phase=Phase::medianClear;}else{const auto bits=medianPrefix&0x80000000u?medianPrefix^0x80000000u:~medianPrefix;gate=std::bit_cast<float>(bits)+(1-frameSettings.bands[bandIndex].spectralDensity)*24;phase=Phase::apply;}}break;
    }
    case Phase::apply:{const auto end=endAt(count);for(auto k=cursor;k<end;++k)applyWorkBin(k);consume(end);if(cursor==count){++bandIndex;phase=Phase::band;}break;}
    case Phase::mirrorLeft:case Phase::mirrorRight:{const int ch=phase==Phase::mirrorLeft?0:1;const auto end=endAt(size/2);for(auto k=cursor;k<end;++k)bins[ch][size-k]=std::conj(bins[ch][k]);consume(end);if(cursor==size/2)phase=ch==0?Phase::inverseLeft:Phase::inverseRight;break;}
    case Phase::inverseLeft:if(advanceFFT(bins[0],true,budget)){cursor=1;phase=Phase::mirrorRight;}break;
    case Phase::inverseRight:if(advanceFFT(bins[1],true,budget)){cursor=0;phase=Phase::overlap;}break;
    case Phase::overlap:{const auto end=endAt(size);for(auto j=cursor;j<end;++j)for(int ch=0;ch<2;++ch)output[(frameOutput+j)%output.size()][ch]+=double(bins[ch][j].real())*window[j]*.5;consume(end);if(cursor==size){movement=frameMovement;++stats.framesCompleted;phase=Phase::idle;}break;}
    case Phase::idle:break;
   }
  }
 }
 void workSlice(std::size_t samples) noexcept {
  if(phase==Phase::idle)return;
  workRemainder+=frameWork*samples;const auto allowance=workRemainder/hop;workRemainder%=hop;auto remaining=allowance;
  advanceWork(remaining);stats.maximumSliceWork=std::max(stats.maximumSliceWork,allowance-remaining);
  stats.maximumSliceAllowance=std::max(stats.maximumSliceAllowance,allowance);
 }


 // JUCE's Windows fallback takes processLock. Prepared radix-2 butterflies
 // keep this prototype's transforms allocation-free and lock-free instead.
 void fft(std::vector<Complex>& data,bool inverse) noexcept {
  for(std::size_t i=0;i<size;++i)if(i<permutation[i])std::swap(data[i],data[permutation[i]]);
  for(std::size_t length=2;length<=size;length*=2){const auto half=length/2,stride=size/length;
   for(std::size_t start=0;start<size;start+=length)for(std::size_t k=0;k<half;++k){
    const auto w=inverse?std::conj(twiddles[k*stride]):twiddles[k*stride];
    const auto even=data[start+k],odd=data[start+k+half]*w;
    data[start+k]=even+odd;data[start+k+half]=even-odd;
   }
  }
  if(inverse)for(auto& value:data)value/=float(size);
  ++transforms;
 }
 void configure(const DynamicEqSettings& next) noexcept {
  if(work==SpectralEqWork::bounded){settings=next;return;}
  for(std::size_t index=0;index<8;++index){const auto& b=next.bands[index];auto& prepared=bands[index];
   prepared.attack=float(std::exp(-double(hop)/(rate*b.attackMs*.001)));
   prepared.release=float(std::exp(-double(hop)/(rate*b.releaseMs*.001)));
   const auto& old=settings.bands[index];
   if(rate&&b.spectralEnabled&&(prepared.weight.empty()||b.frequencyHz!=old.frequencyHz||b.q!=old.q||b.shape!=old.shape||!old.spectralEnabled)){
    using Factory=juce::dsp::IIR::ArrayCoefficients<double>;
    const double fc=std::min(double(b.frequencyHz),.45*rate);
    const auto c=b.shape==DynamicEqShape::bell?Factory::makeBandPass(rate,fc,b.q):
     b.shape==DynamicEqShape::lowShelf?Factory::makeLowPass(rate,fc,std::sqrt(.5)):Factory::makeHighPass(rate,fc,std::sqrt(.5));
    const double half=1./(2*b.q),lo=fc*(std::sqrt(1+half*half)-half),hi=fc*(std::sqrt(1+half*half)+half);
    for(std::size_t k=0;k<prepared.weight.size();++k){const double f=double(k)*rate/size;
     const bool inside=b.shape==DynamicEqShape::bell?f>=lo&&f<=hi:b.shape==DynamicEqShape::lowShelf?f<=fc:f>=fc;
     if(!inside){prepared.weight[k]=0;continue;}
     const auto z=std::polar(1.,-2*std::numbers::pi*double(k)/size);
     prepared.weight[k]=float(std::clamp(std::abs((c[0]+c[1]*z+c[2]*z*z)/(c[3]+c[4]*z+c[5]*z*z)),0.,1.));
    }
   }
  }
  settings=next;
 }
 void dynamics() noexcept {
  std::fill(positive.begin(),positive.end(),0.f);std::fill(negative.begin(),negative.end(),0.f);movement={};
  // Coherent-bin amplitude scale is prepared; DC/Nyquist use half.
  const float scale=amplitudeScale;
  for(std::size_t index=0;index<8;++index){const auto& b=settings.bands[index];auto& prepared=bands[index];
   if(!b.active||b.bypassed||!b.spectralEnabled)continue;
   std::size_t used=0;
   const auto levelAt=[&](std::size_t k) noexcept {const auto l=bins[0][k],r=bins[1][k];
    const float power=b.placement==DynamicEqPlacement::stereo?(std::norm(l)+std::norm(r))*.5f:
     b.placement==DynamicEqPlacement::left?std::norm(l):b.placement==DynamicEqPlacement::right?std::norm(r):
     std::norm((b.placement==DynamicEqPlacement::mid?l+r:l-r)*.5f);
    const float amplitude=std::sqrt(power)*scale*((k==0||k==size/2)?.5f:1.f);
    return 20*std::log10(std::max(amplitude,1.e-12f));
   };
   for(std::size_t k=0;k<levels.size();++k){if(prepared.weight[k]==0)continue;levels[k]=levelAt(k);medianScratch[used++]=levels[k];
   }
   // Narrow peaks need neighbouring bins to estimate a robust noise floor.
   if(used>0&&used<9){const auto center=std::min(levels.size()-1,std::size_t(std::llround(b.frequencyHz*size/rate)));const auto begin=center>4?center-4:0,end=std::min(levels.size(),begin+9);used=0;for(auto k=begin;k<end;++k)medianScratch[used++]=levelAt(k);}
   float gate=0;if(used){auto middle=medianScratch.begin()+std::ptrdiff_t(used/2);std::nth_element(medianScratch.begin(),middle,medianScratch.begin()+std::ptrdiff_t(used));gate=*middle+(1-b.spectralDensity)*24;}
   const float center=std::log2(b.frequencyHz);
   for(std::size_t k=0;k<levels.size();++k){
    const float threshold=b.thresholdDb+b.spectralTiltDbOct*(logFrequency[k]-center);
    const float target=prepared.weight[k]>0&&levels[k]>gate?dynamicEqTargetGainDb(levels[k],threshold,b.rangeDb):0;
    auto& envelope=prepared.envelope[k];const auto timing=std::abs(target)>std::abs(envelope)?prepared.attack:prepared.release;
    envelope=target+timing*(envelope-target);
    movement[index]=std::max(movement[index],std::abs(envelope));
    if(prepared.weight[k]==0)continue;
    const float base=std::clamp(b.gainDb*settings.gainScale,-24.f,24.f);
    const float db=prepared.weight[k]*std::clamp(base+envelope,-24.f,24.f);
    const float bounded=std::clamp(db,-24.f-negative[k],24.f-positive[k]);
    if(bounded>0)positive[k]+=bounded;else negative[k]+=bounded;
    const float gain=std::pow(10.f,bounded/20);
    auto& l=bins[0][k];auto& r=bins[1][k];
    switch(b.placement){
     case DynamicEqPlacement::stereo:l*=gain;r*=gain;break;
     case DynamicEqPlacement::left:l*=gain;break;
     case DynamicEqPlacement::right:r*=gain;break;
     case DynamicEqPlacement::mid:{const auto delta=(l+r)*(.5f*(gain-1));l+=delta;r+=delta;break;}
     case DynamicEqPlacement::side:{const auto delta=(l-r)*(.5f*(gain-1));l+=delta;r-=delta;break;}
    }
   }
  }
 }
 void frame() noexcept {
  for(int ch=0;ch<2;++ch){for(std::size_t j=0;j<size;++j)bins[ch][j]={float(input[(position+j)%size][ch])*window[j],0};fft(bins[ch],false);}
  dynamics();
  for(int ch=0;ch<2;++ch){for(std::size_t k=1;k<size/2;++k)bins[ch][size-k]=std::conj(bins[ch][k]);fft(bins[ch],true);
   for(std::size_t j=0;j<size;++j)output[(position+j)%size][ch]+=double(bins[ch][j].real())*window[j]*.5;
  }
 }
};
SpectralEq::SpectralEq():state(std::make_unique<State>()){}
SpectralEq::~SpectralEq()=default;
bool SpectralEq::prepare(double rate,int maximumBlockSize,int referenceWindow,int hopOffset,SpectralEqWork work){
 if(work!=SpectralEqWork::immediate&&work!=SpectralEqWork::bounded)return false;
 if(!std::isfinite(rate)||rate<8000||rate>192000||maximumBlockSize<1||maximumBlockSize>65536)return false;
 if(referenceWindow!=512&&referenceWindow!=1024&&referenceWindow!=2048&&referenceWindow!=4096)return false;
 const auto size=std::max<std::size_t>(std::size_t(referenceWindow),std::bit_ceil(std::size_t(std::ceil(referenceWindow*rate/48000))));
 if(hopOffset<0||std::size_t(hopOffset)>=size/4)return false;
 auto next=std::make_unique<State>();next->work=work;next->rate=rate;next->size=size;next->hop=size/4;next->firstHop=hopOffset?std::size_t(hopOffset):next->hop;next->untilHop=next->firstHop;
 const auto count=size/2+1;const auto delay=size+(work==SpectralEqWork::bounded?size/4:0);next->input.resize(delay);next->output.resize(delay);next->window.resize(size);next->twiddles.resize(size/2);next->permutation.resize(size);
 for(std::size_t i=0;i<size;++i){next->window[i]=float(std::sqrt(.5-.5*std::cos(2*std::numbers::pi*double(i)/size)));std::size_t reversed=0;for(auto j=i,bit=size/2;bit;bit>>=1,j>>=1)if(j&1)reversed|=bit;next->permutation[i]=reversed;}
 for(std::size_t i=0;i<size/2;++i)next->twiddles[i]=std::polar(1.f,float(-2*std::numbers::pi*double(i)/size));
 next->amplitudeScale=2.f/std::accumulate(next->window.begin(),next->window.end(),0.f);
 for(auto& data:next->bins)data.resize(size);
 for(auto* data:{&next->logFrequency,&next->levels,&next->medianScratch,&next->positive,&next->negative})data->resize(count);
 for(std::size_t k=0;k<count;++k)next->logFrequency[k]=float(std::log2(std::max(20.,double(k)*rate/size)));
 for(auto& b:next->bands){b.weight.resize(count);b.envelope.resize(count);}
 next->configure(state->settings);state=std::move(next);return true;
}
bool SpectralEq::setSettings(const DynamicEqSettings& settings) noexcept {if(!supported(settings))return false;if(state->size)state->configure(settings);else state->settings=settings;return true;}
void SpectralEq::processBlock(std::span<DynamicEq::Frame> audio) noexcept {
 if(!state->size)return;
 if(state->work==SpectralEqWork::bounded){
  std::size_t offset=0;
  while(offset<audio.size()){
   const auto count=std::min({std::size_t(16),state->untilHop,audio.size()-offset});
   for(std::size_t i=0;i<count;++i){auto& value=audio[offset+i];state->input[state->position]=value;value=state->output[state->position];state->output[state->position]={};state->position=(state->position+1)%state->input.size();}
   state->workSlice(count);state->untilHop-=count;offset+=count;
   if(!state->untilHop){state->beginWork();state->untilHop=state->hop;}
  }
  return;
 }
 for(auto& value:audio){state->input[state->position]=value;value=state->output[state->position];state->output[state->position]={};state->position=(state->position+1)%state->size;if(--state->untilHop==0){state->frame();state->untilHop=state->hop;}}
}
void SpectralEq::reset() noexcept {std::fill(state->input.begin(),state->input.end(),DynamicEq::Frame{});std::fill(state->output.begin(),state->output.end(),DynamicEq::Frame{});for(auto& b:state->bands){std::fill(b.envelope.begin(),b.envelope.end(),0.f);b.curveReady=false;b.weighted=0;}state->position=0;state->untilHop=state->firstHop;state->transforms=0;state->movement={};state->phase=State::Phase::idle;state->stats={};}
int SpectralEq::latencySamples() const noexcept{return int(state->output.size());}
int SpectralEq::transformSize() const noexcept{return int(state->size);}
SpectralEqWorkStats SpectralEq::workStats() const noexcept{return state->stats;}
std::uint64_t SpectralEq::transformsPerformed() const noexcept{return state->transforms;}
std::array<float,8> SpectralEq::dynamicGainsDb() const noexcept{return state->movement;}
std::size_t SpectralEq::weightedBinCount(std::size_t band) const noexcept {if(band>=8||!state->settings.bands[band].spectralEnabled)return 0;const auto& weights=state->bands[band].weight;return std::size_t(std::count_if(weights.begin(),weights.end(),[](float w){return w>0;}));}
std::size_t SpectralEq::preparedBytes() const noexcept {std::size_t bytes=sizeof(State);for(const auto* v:{&state->window,&state->logFrequency,&state->levels,&state->medianScratch,&state->positive,&state->negative})bytes+=v->capacity()*sizeof(float);for(const auto& b:state->bands)bytes+=(b.weight.capacity()+b.envelope.capacity())*sizeof(float);bytes+=(state->input.capacity()+state->output.capacity())*sizeof(DynamicEq::Frame);bytes+=state->twiddles.capacity()*sizeof(Complex)+state->permutation.capacity()*sizeof(std::size_t);for(const auto& v:state->bins)bytes+=v.capacity()*sizeof(Complex);return bytes;}
}
