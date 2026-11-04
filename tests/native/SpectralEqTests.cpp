#include "SpectralEq.h"
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <limits>
#include <numbers>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
thread_local bool counting=false;
thread_local int allocations=0,frees=0;
thread_local std::size_t allocatedBytes=0;
int heapHook(int operation,void*,size_t bytes,int,long,const unsigned char*,int){if(counting){if(operation==_HOOK_ALLOC||operation==_HOOK_REALLOC){++allocations;allocatedBytes+=bytes;}if(operation==_HOOK_FREE||operation==_HOOK_REALLOC)++frees;}return 1;}
#endif

namespace {
using namespace airack;
int checks=0,failures=0;
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;std::cout<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void boundedWorkContract(){
 std::cout<<"SECTION bounded_work_retains_resolution_and_reports_scheduling_delay\n";
 SpectralEq eq;CHECK(eq.prepare(48000,64,4096,0,SpectralEqWork::bounded));
 CHECK(eq.transformSize()==4096);CHECK(eq.latencySamples()==5120);
 std::vector<DynamicEq::Frame> audio(16384);audio[0]={.25,-.25};eq.processBlock(audio);
 CHECK(std::abs(audio[5120][0]-.25)<2.e-7);CHECK(std::abs(audio[5120][1]+.25)<2.e-7);
 const auto stats=eq.workStats();CHECK(stats.deadlineMisses==0);CHECK(stats.maximumSliceWork>0);
 CHECK(stats.maximumSliceWork<=stats.maximumSliceAllowance);CHECK(stats.framesCompleted>0);
}
void smallerPreparedGeometry(){
 std::cout<<"SECTION explicit_lower_latency_geometry\n";
 SpectralEq eq;CHECK(eq.prepare(48000,64,512,31));CHECK(eq.latencySamples()==512);
 std::vector<DynamicEq::Frame> audio(1536);audio[0]={.25,-.25};eq.processBlock(audio);
 CHECK(std::abs(audio[512][0]-.25)<2.e-7);CHECK(std::abs(audio[512][1]+.25)<2.e-7);
 CHECK(!eq.prepare(48000,64,513));CHECK(!eq.prepare(48000,64,512,128));CHECK(!eq.prepare(48000,64,512,-1));CHECK(eq.latencySamples()==512);
 eq.reset();std::vector<DynamicEq::Frame> replay(1536);replay[0]={.25,-.25};eq.processBlock(replay);CHECK(replay==audio);
}
void flatSpectralKernelReconstructsDelayedAudio(){
 SpectralEq eq;CHECK(eq.prepare(48000,64));DynamicEqSettings flat;CHECK(eq.setSettings(flat));CHECK(eq.latencySamples()==4096);
 std::array<DynamicEq::Frame,12288> audio{};audio[0]={.25,-.25};eq.processBlock(audio);
 for(std::size_t i=0;i<audio.size();++i){const double expected=i==4096?.25:0.;CHECK(std::abs(audio[i][0]-expected)<2.e-7);CHECK(std::abs(audio[i][1]+expected)<2.e-7);}
}
void reconstruction(){
 std::cout<<"SECTION all_rates_all_hop_phases_noise_chirp_dc_nyquist_partitions_reset\n";
 for(double rate:{8000.,44100.,48000.,88200.,96000.,176400.,192000.}){
  SpectralEq eq;CHECK(eq.prepare(rate,512));const auto n=std::size_t(eq.latencySamples()),hop=n/4;
  CHECK(n>=4096&&n<=16384);CHECK(8*eq.preparedBytes()<=64*1024*1024);
  for(int signal=0;signal<4;++signal){std::vector<DynamicEq::Frame> source(4*n),audio(6*n);std::uint32_t random=17;
   for(std::size_t i=0;i<source.size();++i){random=random*1664525+1013904223;
    double v=signal==0?(double(random)/4294967295.-.5)*.4:signal==1?.2*std::sin(double(i*i)*.00001):signal==2?.2:((i&1)?-.2:.2);
    // One independently coded pulse in every hop phase, including startup.
    if(signal==0&&i<hop)v=(i&1)?.25:-.125;
    source[i]={v,-v*.75};audio[i]=source[i];
   }
   eq.reset();std::size_t offset=0,step=0;constexpr std::array<std::size_t,4> partitions{1,7,64,512};
   while(offset<audio.size()){const auto count=std::min(partitions[step++%4],audio.size()-offset);eq.processBlock(std::span(audio).subspan(offset,count));offset+=count;}
   double error=0;for(std::size_t i=0;i<audio.size();++i)for(int ch=0;ch<2;++ch){const auto expected=i>=n&&i-n<source.size()?source[i-n][ch]:0.;error=std::max(error,std::abs(audio[i][ch]-expected));}
   CHECK(error<=2.e-6);CHECK(eq.transformsPerformed()==4*(audio.size()/hop));
   std::cout<<"RECON rate="<<rate<<" size="<<n<<" signal="<<signal<<" max_error="<<error<<" bytes="<<eq.preparedBytes()<<'\n';
  }
  eq.reset();std::array<DynamicEq::Frame,71> silence{};eq.processBlock(silence);CHECK(std::all_of(silence.begin(),silence.end(),[](const auto& f){return f==DynamicEq::Frame{};}));
 }
}
double toneAmplitude(const std::vector<DynamicEq::Frame>& audio,int ch,double hz,std::size_t start,double rate=48000){std::complex<double> sum{};for(std::size_t n=start;n<audio.size();++n)sum+=audio[n][ch]*std::polar(1.,-2*std::numbers::pi*hz*double(n)/rate);return 2*std::abs(sum)/double(audio.size()-start);}
std::vector<DynamicEq::Frame> render(DynamicEqSettings settings,int inputMode=0){
 SpectralEq eq;CHECK(eq.prepare(48000,64));CHECK(eq.setSettings(settings));std::vector<DynamicEq::Frame> audio(48000);
 for(std::size_t n=0;n<audio.size();++n){const double v=.4*std::sin(2*std::numbers::pi*1500*n/48000)+.01*std::sin(2*std::numbers::pi*6000*n/48000);audio[n]={v,inputMode==1?-v:inputMode==2?0:v};}
 for(std::size_t n=0;n<audio.size();n+=64)eq.processBlock(std::span(audio).subspan(n,std::min<std::size_t>(64,audio.size()-n)));return audio;
}
DynamicEqSettings selective(){DynamicEqSettings s;auto& b=s.bands[0];b.active=b.spectralEnabled=true;b.frequencyHz=1500;b.q=.25f;b.rangeDb=-6;b.thresholdDb=-24;b.attackMs=.1f;b.releaseMs=10;return s;}
void staticEndpoints(){
 std::cout<<"SECTION static_gain_at_endpoint_leaves_no_further_dynamic_headroom\n";
 for(auto shape:{DynamicEqShape::bell,DynamicEqShape::lowShelf,DynamicEqShape::highShelf})for(int mode:{0,1,2}){
  auto s=selective();auto& b=s.bands[0];b.shape=shape;b.thresholdDb=-60;b.gainDb=mode==0?24.f:mode==1?-24.f:12.f;s.gainScale=mode==2?2.f:1.f;b.rangeDb=0;
  const auto reference=render(s);b.rangeDb=mode==0?24.f:mode==1?-24.f:6.f;const auto ranged=render(s);CHECK(reference==ranged);
 }
}
void preparedBandCoverage(){SpectralEq eq;CHECK(eq.prepare(48000,64,512));CHECK(eq.setSettings(selective()));CHECK(eq.weightedBinCount(0)>0);CHECK(eq.weightedBinCount(8)==0);}
void selectivity(){
 std::cout<<"SECTION independent_two_tone_dynamic_gain_and_component_placement\n";
 const auto base=render({});const double reference=toneAmplitude(base,0,1500,32768),other=toneAmplitude(base,0,6000,32768);
 for(float range:{-6.f,6.f}){auto s=selective();s.bands[0].rangeDb=range;auto audio=render(s);
  const auto movement=20*std::log10(toneAmplitude(audio,0,1500,32768)/reference),untouched=20*std::log10(toneAmplitude(audio,0,6000,32768)/other);
  std::cout<<"SELECTIVITY range="<<range<<" target_db="<<movement<<" untriggered_db="<<untouched<<'\n';CHECK(range<0?movement<=-3:movement>=3);CHECK(std::abs(untouched)<=.25);
 }
 for(const auto placement:{DynamicEqPlacement::left,DynamicEqPlacement::right,DynamicEqPlacement::mid,DynamicEqPlacement::side})for(int inputMode:{0,1,2}){
  auto s=selective();s.bands[0].placement=placement;auto audio=render(s,inputMode);const auto l=toneAmplitude(audio,0,1500,32768),r=toneAmplitude(audio,1,1500,32768);
  if(placement==DynamicEqPlacement::left){CHECK(l<reference*.71);CHECK(std::abs(r-(inputMode==2?0:reference))<reference*.001);}
  if(placement==DynamicEqPlacement::right){CHECK(std::abs(l-reference)<reference*.001);if(inputMode!=2)CHECK(r<reference*.71);else CHECK(r<1.e-7);}
  if(placement==DynamicEqPlacement::mid&&inputMode==0)CHECK(l<reference*.71&&r<reference*.71);
  if(placement==DynamicEqPlacement::mid&&inputMode==1)CHECK(std::abs(l-reference)<reference*.001&&std::abs(r-reference)<reference*.001);
  if(placement==DynamicEqPlacement::side&&inputMode==1)CHECK(l<reference*.71&&r<reference*.71);
  if(placement==DynamicEqPlacement::side&&inputMode==0)CHECK(std::abs(l-reference)<reference*.001&&std::abs(r-reference)<reference*.001);
 }
 auto s=selective();s.bands[0].spectralTiltDbOct=6;auto tilted=render(s);CHECK(toneAmplitude(tilted,0,1500,32768)<reference*.71);
 s.bands[0].spectralDensity=0;CHECK(validateDynamicEq(s));s.bands[0].spectralDensity=1;CHECK(validateDynamicEq(s));
}
std::vector<DynamicEq::Frame> process(DynamicEqSettings settings,std::vector<DynamicEq::Frame> audio,std::size_t partition=64,int window=4096){
 SpectralEq eq;CHECK(eq.prepare(48000,int(partition),window));CHECK(eq.setSettings(settings));
 for(std::size_t n=0;n<audio.size();n+=partition)eq.processBlock(std::span(audio).subspan(n,std::min(partition,audio.size()-n)));return audio;
}
void narrowBandNoiseFloor(){
 std::cout<<"SECTION narrow_band_median_cannot_be_its_own_peak\n";
 for(auto fixture:{std::array<int,4>{4096,100,125,4},std::array<int,4>{2048,1000,1500,18}}){std::vector<DynamicEq::Frame> audio(96000);for(std::size_t n=0;n<audio.size();++n){const double v=.4*(std::sin(2*std::numbers::pi*fixture[1]*n/48000)+std::sin(2*std::numbers::pi*fixture[2]*n/48000));audio[n]={v,v};}
  auto s=selective();s.bands[0].frequencyHz=float(fixture[1]);s.bands[0].q=float(fixture[3]);s.bands[0].spectralDensity=1;const auto output=process(s,audio,64,fixture[0]);const auto target=20*std::log10(toneAmplitude(output,0,fixture[1],48000)/.4),other=20*std::log10(toneAmplitude(output,0,fixture[2],48000)/.4);CHECK(target<=-3);CHECK(std::abs(other)<=.25);
 }
}
void densityTiltMotionAndBounds(){
 std::cout<<"SECTION measured_density_tilt_moving_partials_order_bounds_and_partition_identity\n";
 std::vector<DynamicEq::Frame> tones(96000);for(std::size_t n=0;n<tones.size();++n){const double v=.03*(std::sin(2*std::numbers::pi*750*n/48000)+std::sin(2*std::numbers::pi*3000*n/48000));tones[n]={v,v};}
 auto s=selective();s.bands[0].thresholdDb=-34;const auto base=process({},tones);
 for(float tilt:{-6.f,6.f}){s.bands[0].spectralTiltDbOct=tilt;const auto audio=process(s,tones);const auto low=20*std::log10(toneAmplitude(audio,0,750,65536)/toneAmplitude(base,0,750,65536)),high=20*std::log10(toneAmplitude(audio,0,3000,65536)/toneAmplitude(base,0,3000,65536));std::cout<<"TILT db_oct="<<tilt<<" low_db="<<low<<" high_db="<<high<<'\n';CHECK(tilt>0?low<high-2:high<low-2);}
 std::uint32_t random=17;for(std::size_t n=0;n<tones.size();++n){random=random*1664525+1013904223;const double v=(double(random)/4294967295.-.5)*.04+.003*std::sin(2*std::numbers::pi*3000*n/48000)+.05*std::sin(2*std::numbers::pi*1500*n/48000);tones[n]={v,v};}
 s=selective();s.bands[0].thresholdDb=-60;s.bands[0].spectralDensity=0;const auto sparse=process(s,tones);s.bands[0].spectralDensity=1;const auto dense=process(s,tones);const auto densityDifference=20*std::log10(toneAmplitude(dense,0,3000,65536)/toneAmplitude(sparse,0,3000,65536));std::cout<<"DENSITY high_vs_low_db="<<densityDifference<<'\n';CHECK(densityDifference<-2);
 const auto a=process(s,tones,7),b=process(s,tones,512);CHECK(a==b);
 // Partial jumps inside one wide band; both occupied frequencies must move.
 for(std::size_t n=0;n<tones.size();++n){const auto hz=n<48000?1500.:3000.;const double v=.4*std::sin(2*std::numbers::pi*hz*n/48000);tones[n]={v,v};}
 s=selective();auto moving=process(s,tones);auto movingBase=process({},tones);CHECK(toneAmplitude(moving,0,3000,65536)<toneAmplitude(movingBase,0,3000,65536)*.71);
 // Six identical full-range bands cannot exceed the combined 24 dB cap.
 for(std::size_t n=0;n<tones.size();++n){const double v=.4*std::sin(2*std::numbers::pi*1500*n/48000);tones[n]={v,v};}
 for(float range:{-24.f,24.f}){s=selective();s.bands.fill(s.bands[0]);for(auto& band:s.bands){band.rangeDb=range;band.thresholdDb=-60;}const auto audio=process(s,tones);const auto movement=20*std::log10(toneAmplitude(audio,0,1500,65536)/.4);std::cout<<"COMBINED range="<<range<<" movement_db="<<movement<<'\n';CHECK(std::abs(movement)<=24.01);CHECK(std::abs(movement)>20);}
 // Opposite order in different component bases changes the result, as specified.
 s=selective();s.bands[0].placement=DynamicEqPlacement::left;s.bands[1]=s.bands[0];s.bands[1].placement=DynamicEqPlacement::mid;s.bands[1].rangeDb=6;auto first=process(s,tones);std::swap(s.bands[0],s.bands[1]);auto second=process(s,tones);CHECK(std::abs(toneAmplitude(first,1,1500,65536)-toneAmplitude(second,1,1500,65536))>.001);
}
void validationAndHeap(){
 std::cout<<"SECTION validation_atomic_rejection_prepared_callback_heap\n";
 SpectralEq eq;CHECK(!eq.prepare(384000,64));CHECK(!eq.prepare(48000,0));CHECK(!eq.prepare(std::numeric_limits<double>::quiet_NaN(),64));CHECK(eq.prepare(192000,64));
 auto s=selective();CHECK(eq.setSettings(s));for(int field=0;field<5;++field){auto bad=s;auto& b=bad.bands[0];if(field==0)b.spectralDensity=1.01f;if(field==1)b.spectralTiltDbOct=std::numeric_limits<float>::quiet_NaN();if(field==2)b.detectorSource=DynamicEqDetectorSource::externalBand;if(field==3)b.autoThreshold=true;if(field==4)b.shape=DynamicEqShape::notch;CHECK(!eq.setSettings(bad));}
 std::array<DynamicEq::Frame,512> block{};
#if defined(_MSC_VER) && defined(_DEBUG)
 const auto previous=_CrtSetAllocHook(heapHook);allocations=frees=0;counting=true;
#endif
 for(int k=0;k<200;++k){s.bands[0].frequencyHz=1500+float(k%11);eq.setSettings(s);block[0]={.25,-.25};eq.processBlock(block);}
 eq.reset();
#if defined(_MSC_VER) && defined(_DEBUG)
 counting=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);std::cout<<"HEAP allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
 CHECK(eq.transformsPerformed()==0);
#if defined(_MSC_VER) && defined(_DEBUG)
 allocatedBytes=0;counting=true;const auto hook=_CrtSetAllocHook(heapHook);const bool prepared=eq.prepare(192000,512);counting=false;_CrtSetAllocHook(hook);CHECK(prepared);CHECK(8*allocatedBytes<64*1024*1024);std::cout<<"PREPARED_CRT cumulative_requested_bytes="<<allocatedBytes<<" eight_racks="<<8*allocatedBytes<<'\n';
#endif
}
std::vector<DynamicEq::Frame> workRender(double rate,int phase,std::size_t partition,DynamicEqSettings settings,
                                       const std::vector<DynamicEq::Frame>& input,SpectralEqWork mode,bool edits,
                                       SpectralEqWorkStats& stats){
 SpectralEq eq;CHECK(eq.prepare(rate,int(partition),4096,phase,mode));CHECK(eq.setSettings(settings));
 auto audio=input;const auto n=std::size_t(eq.transformSize()),hop=n/4;
 const std::array<std::size_t,4> events{hop/3,hop+hop/2,2*hop+17,4*hop+hop/3};std::size_t event=0,offset=0;
 while(offset<audio.size()){
  if(edits&&event<events.size()&&offset==events[event]){auto& b=settings.bands[0];
   if(event==0){b.frequencyHz=1697;b.q=18;settings.gainScale=1.25f;}
   if(event==1)b.bypassed=true;
   if(event==2){b.bypassed=false;b.rangeDb=-b.rangeDb;b.thresholdDb=-31;}
   if(event==3){b.shape=DynamicEqShape::highShelf;b.frequencyHz=3000;b.spectralDensity=.37f;b.spectralTiltDbOct=-3;}
   CHECK(eq.setSettings(settings));++event;
  }
  auto count=std::min(partition,audio.size()-offset);if(edits&&event<events.size())count=std::min(count,events[event]-offset);
  eq.processBlock(std::span(audio).subspan(offset,count));offset+=count;
 }
 stats=eq.workStats();if(mode==SpectralEqWork::bounded){
  CHECK(eq.latencySamples()==int(n+hop));CHECK(stats.deadlineMisses==0);CHECK(stats.framesCompleted>0);
  CHECK(stats.framesStarted>=stats.framesCompleted&&stats.framesStarted-stats.framesCompleted<=1);
  const auto c=n/2+1,order=std::size_t(std::bit_width(n)-1);
  const auto upper=9*n+2*n*order+c-2+8*(3*c+9+4*(512+c));
  CHECK(stats.maximumSliceAllowance<=16*((upper+hop-1)/hop));CHECK(stats.maximumSliceWork<=stats.maximumSliceAllowance);
 }
 eq.reset();std::vector<DynamicEq::Frame> replay=input;
 if(!edits){for(std::size_t p=0;p<replay.size();p+=73)eq.processBlock(std::span(replay).subspan(p,std::min<std::size_t>(73,replay.size()-p)));CHECK(replay==audio);}
 return audio;
}
void boundedReferenceEquivalence(){
 std::cout<<"SECTION bounded_exact_reference_after_one_hop_including_live_edits\n";
 for(double rate:{44100.,48000.,96000.,192000.}){
  SpectralEq geometry;CHECK(geometry.prepare(rate,512));const auto n=std::size_t(geometry.transformSize()),hop=n/4;
  std::vector<DynamicEq::Frame> input(6*n);std::uint32_t random=17;
  for(std::size_t i=0;i<3*n;++i){random=random*1664525+1013904223;const double noise=(double(random)/4294967295.-.5)*.03;
   const auto a=.2*std::sin(2*std::numbers::pi*1500*i/rate)+noise,b=.17*std::sin(2*std::numbers::pi*750*i/rate)-noise;input[i]={a,b};}
  for(int variant=0;variant<6;++variant){auto settings=selective();auto& band=settings.bands[0];band.shape=DynamicEqShape(variant%3);band.placement=DynamicEqPlacement(variant%5);band.rangeDb=variant%2?6.f:-6.f;band.spectralDensity=float(variant%3)*.5f;band.spectralTiltDbOct=float(variant%3-1)*6;
   if(variant==5){settings.bands.fill(band);for(std::size_t i=0;i<8;++i){settings.bands[i].placement=DynamicEqPlacement(i%5);settings.bands[i].shape=DynamicEqShape(i%3);settings.bands[i].frequencyHz=400+float(i)*311;settings.bands[i].q=i%2?18.f:.25f;settings.bands[i].rangeDb=i%2?6.f:-6.f;}}
   for(int phase:{0,1,int(hop)-1}){const std::size_t partitions[]{1,7,64,512};const auto partition=partitions[variant%4];SpectralEqWorkStats oldStats,newStats;const bool edits=variant==5;
    const auto reference=workRender(rate,phase,64,settings,input,SpectralEqWork::immediate,edits,oldStats);
    const auto bounded=workRender(rate,phase,partition,settings,input,SpectralEqWork::bounded,edits,newStats);
    double error=0;bool exact=true;for(std::size_t i=0;i<bounded.size();++i)for(int ch=0;ch<2;++ch){const double expected=i>=hop?reference[i-hop][ch]:0;error=std::max(error,std::abs(bounded[i][ch]-expected));exact=exact&&std::bit_cast<std::uint64_t>(bounded[i][ch])==std::bit_cast<std::uint64_t>(expected);}
    CHECK(exact);std::cout<<"BOUNDED_EQUIVALENCE rate="<<rate<<" variant="<<variant<<" phase="<<phase<<" partition="<<partition<<" exact="<<exact<<" max_error="<<error<<" misses="<<newStats.deadlineMisses<<" maximum_slice_work="<<newStats.maximumSliceWork<<'\n';
   }
  }
 }
}
void boundedHeapAndReset(){
 std::cout<<"SECTION bounded_active_jobs_edits_reset_and_preparation_heap\n";
 SpectralEq eq;CHECK(eq.prepare(192000,512,4096,1,SpectralEqWork::bounded));auto s=selective();s.bands.fill(s.bands[0]);
 for(std::size_t i=0;i<8;++i){s.bands[i].shape=DynamicEqShape(i%3);s.bands[i].placement=DynamicEqPlacement(i%5);s.bands[i].frequencyHz=1000+float(i)*311;s.bands[i].q=i%2?18.f:.25f;}
 CHECK(eq.setSettings(s));std::array<DynamicEq::Frame,512> block{};
#if defined(_MSC_VER) && defined(_DEBUG)
 const auto previous=_CrtSetAllocHook(heapHook);allocations=frees=0;counting=true;
#endif
 for(int k=0;k<160;++k){for(std::size_t i=0;i<block.size();++i){const double t=double(k*512+int(i))/192000;block[i]={.2*std::sin(2*std::numbers::pi*1500*t),-.17*std::cos(2*std::numbers::pi*1700*t)};}s.bands[0].frequencyHz=1000+float(k%7);eq.setSettings(s);eq.processBlock(block);if(k==39||k==111)eq.reset();}
#if defined(_MSC_VER) && defined(_DEBUG)
 counting=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);std::cout<<"BOUNDED_HEAP allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
 CHECK(eq.workStats().deadlineMisses==0);CHECK(eq.workStats().framesCompleted>0);eq.reset();CHECK(eq.workStats().framesStarted==0&&eq.transformsPerformed()==0);
 std::array<DynamicEq::Frame,73> silence{};eq.processBlock(silence);CHECK(std::all_of(silence.begin(),silence.end(),[](auto f){return f==DynamicEq::Frame{};}));
 CHECK(!eq.prepare(192000,64,4096,0,static_cast<SpectralEqWork>(2)));CHECK(eq.transformSize()==16384&&eq.latencySamples()==20480);
#if defined(_MSC_VER) && defined(_DEBUG)
 allocatedBytes=0;counting=true;const auto hook=_CrtSetAllocHook(heapHook);const bool prepared=eq.prepare(192000,512,4096,0,SpectralEqWork::bounded);counting=false;_CrtSetAllocHook(hook);CHECK(prepared);CHECK(8*allocatedBytes<64*1024*1024);std::cout<<"BOUNDED_PREPARED_CRT cumulative_requested_bytes="<<allocatedBytes<<" eight_racks="<<8*allocatedBytes<<'\n';
#endif
 CHECK(8*eq.preparedBytes()<64*1024*1024);
}
void boundedPhysicalSignals(){
 std::cout<<"SECTION bounded_physical_impulse_random_chirp_dc_nyquist_reconstruction\n";
 for(double rate:{8000.,44100.,48000.,88200.,96000.,176400.,192000.})for(int signal=0;signal<5;++signal){
  SpectralEq eq;CHECK(eq.prepare(rate,512,4096,1,SpectralEqWork::bounded));const auto n=std::size_t(eq.transformSize()),delay=std::size_t(eq.latencySamples());std::vector<DynamicEq::Frame> source(2*n),audio(4*n);std::uint32_t random=17;
  for(std::size_t i=0;i<source.size();++i){random=random*1664525+1013904223;const double v=signal==0?(i==0?.25:0):signal==1?(double(random)/4294967295.-.5)*.4:signal==2?.2:signal==3?((i&1)?-.2:.2):.2*std::sin(double(i*i)*.00001);source[i]={v,-v*.75};audio[i]=source[i];}
  std::size_t offset=0,step=0;constexpr std::array<std::size_t,4> partitions{1,7,64,512};while(offset<audio.size()){const auto count=std::min(partitions[step++%4],audio.size()-offset);eq.processBlock(std::span(audio).subspan(offset,count));offset+=count;}
  double error=0;for(std::size_t i=0;i<audio.size();++i)for(int ch=0;ch<2;++ch){const double expected=i>=delay&&i-delay<source.size()?source[i-delay][ch]:0.;error=std::isfinite(audio[i][ch])?std::max(error,std::abs(audio[i][ch]-expected)):std::numeric_limits<double>::infinity();}
  CHECK(error<=(signal==0?2.e-7:2.e-6));CHECK(eq.workStats().deadlineMisses==0);std::cout<<"BOUNDED_RECON rate="<<rate<<" N="<<n<<" delay="<<delay<<" signal="<<signal<<" max_error="<<error<<'\n';
 }
}
void smallerReconstruction(int window,double rate,int phase,int signal,std::ostream* out=nullptr){
 SpectralEq eq;CHECK(eq.prepare(rate,512,window,phase));const auto n=std::size_t(eq.latencySamples());std::vector<DynamicEq::Frame> source(2*n),audio(3*n);std::uint32_t random=17;
 for(std::size_t i=0;i<source.size();++i){random=random*1664525+1013904223;const double v=signal==0?(i==0?.25:0):signal==1?(double(random)/4294967295.-.5)*.4:signal==2?.2:signal==3?((i&1)?-.2:.2):.2*std::sin(double(i*i)*.00001);source[i]={v,-v};audio[i]=source[i];}
 std::size_t offset=0,step=0;constexpr std::array<std::size_t,4> partitions{1,7,64,512};while(offset<audio.size()){const auto count=std::min(partitions[step++%4],audio.size()-offset);eq.processBlock(std::span(audio).subspan(offset,count));offset+=count;}
 double error=0;for(std::size_t i=0;i<audio.size();++i)for(int ch=0;ch<2;++ch){const auto expected=i>=n&&i-n<source.size()?source[i-n][ch]:0.;error=std::max(error,std::abs(audio[i][ch]-expected));}
 const auto limit=signal==0?2.e-7:2.e-6;CHECK(error<=limit);CHECK(eq.transformsPerformed()==48);CHECK(8*eq.preparedBytes()<64*1024*1024);
 eq.reset();std::array<DynamicEq::Frame,71> silence{};eq.processBlock(silence);CHECK(std::all_of(silence.begin(),silence.end(),[](auto x){return x==DynamicEq::Frame{};}));
 if(out)*out<<window<<','<<rate<<','<<n<<','<<phase<<','<<signal<<','<<error<<','<<eq.preparedBytes()<<','<<(error<=limit)<<'\n';
}
void smallerRegression(std::ostream* out=nullptr){
 for(int window:{512,1024,2048})for(double rate:{44100.,48000.,192000.}){SpectralEq probe;CHECK(probe.prepare(rate,512,window));const int hop=probe.latencySamples()/4;for(int phase:{0,1,hop/3,hop-1})for(int signal=0;signal<5;++signal)smallerReconstruction(window,rate,phase,signal,out);}
}
void lowerWindowHeap(){
#if defined(_MSC_VER) && defined(_DEBUG)
 std::cout<<"SECTION lower_windows_all_bands_narrow_floor_and_components_callback_heap\n";
 for(int window:{512,1024,2048,4096}){SpectralEq eq;CHECK(eq.prepare(192000,512,window,31));auto s=selective();s.bands.fill(s.bands[0]);for(std::size_t i=0;i<8;++i){s.bands[i].frequencyHz=1000;s.bands[i].q=18;s.bands[i].placement=DynamicEqPlacement(i%5);}CHECK(eq.setSettings(s));std::array<DynamicEq::Frame,512> audio{};const auto previous=_CrtSetAllocHook(heapHook);allocations=frees=0;counting=true;for(int k=0;k<80;++k){s.bands[0].frequencyHz=1000+float(k%3);eq.setSettings(s);audio[0]={.25,-.25};eq.processBlock(audio);}eq.reset();counting=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);std::cout<<"LOWER_HEAP window="<<window<<" allocations="<<allocations<<" frees="<<frees<<'\n';}
#endif
}
std::ofstream studyFile(const std::filesystem::path& root,const char* name,const char* columns){std::ofstream out(root/name);if(!out)throw std::runtime_error("cannot create study file");out<<std::setprecision(17)<<columns<<'\n';return out;}
void studyDirectory(const std::filesystem::path& root){if(!std::filesystem::create_directory(root))throw std::runtime_error("study requires a fresh directory");}
int windowStudy(const std::filesystem::path& root){
 studyDirectory(root);auto curve=studyFile(root,"frequency.csv","window,rate,N,delay_ms,target_hz,other_hz,q,weighted_bins,target_db,other_db,passes");auto coverage=studyFile(root,"coverage.csv","window,rate,frequency_hz,q,weighted_bins");auto recon=studyFile(root,"reconstruction.csv","window,rate,N,hop_offset,signal,max_error,bytes,passes");smallerRegression(&recon);
 for(int window:{512,1024,2048,4096})for(double rate:{44100.,48000.}){
  for(auto pair:{std::array<double,2>{1500,6000},std::array<double,2>{500,700},std::array<double,2>{100,125}}){auto s=selective();auto& b=s.bands[0];b.frequencyHz=float(pair[0]);b.q=4;b.spectralDensity=1;
   SpectralEq eq;CHECK(eq.prepare(rate,512,window));CHECK(eq.setSettings(s));std::vector<DynamicEq::Frame> audio(std::size_t(2*rate));for(std::size_t i=0;i<audio.size();++i){const double v=.4*(std::sin(2*std::numbers::pi*pair[0]*i/rate)+std::sin(2*std::numbers::pi*pair[1]*i/rate));audio[i]={v,v};}
   for(std::size_t i=0;i<audio.size();i+=64)eq.processBlock(std::span(audio).subspan(i,std::min<std::size_t>(64,audio.size()-i)));
   const double target=20*std::log10(toneAmplitude(audio,0,pair[0],std::size_t(rate),rate)/.4),other=20*std::log10(toneAmplitude(audio,0,pair[1],std::size_t(rate),rate)/.4);const bool pass=target<=-3&&std::abs(other)<=.25;
   curve<<window<<','<<rate<<','<<eq.latencySamples()<<','<<1000.*eq.latencySamples()/rate<<','<<pair[0]<<','<<pair[1]<<",4,"<<eq.weightedBinCount(0)<<','<<target<<','<<other<<','<<pass<<'\n';
  }
  for(float hz:{100.f,500.f,1000.f,5000.f})for(float q:{1.f,4.f,18.f}){auto s=selective();s.bands[0].frequencyHz=hz;s.bands[0].q=q;SpectralEq eq;CHECK(eq.prepare(rate,512,window));CHECK(eq.setSettings(s));coverage<<window<<','<<rate<<','<<hz<<','<<q<<','<<eq.weightedBinCount(0)<<'\n';}
 }
 std::cout<<"Window study: "<<checks<<" checks, "<<failures<<" failures; frequency passes are separate CSV columns\n";return failures?1:0;
}
int allPhases(const std::filesystem::path& root,int window,double rate){
 studyDirectory(root);auto out=studyFile(root,"impulses.csv","window,rate,N,hop_offset,input_phase,max_error,passes");SpectralEq eq;CHECK(eq.prepare(rate,512,window));const auto n=std::size_t(eq.latencySamples()),hop=n/4;if(!n)return 2;std::vector<DynamicEq::Frame> audio(3*n);
 for(int offset:{0,int(hop/3)}){CHECK(eq.prepare(rate,512,window,offset));for(std::size_t phase=0;phase<hop;++phase){eq.reset();std::fill(audio.begin(),audio.end(),DynamicEq::Frame{});audio[phase]={.25,-.25};for(std::size_t p=0;p<audio.size();p+=73)eq.processBlock(std::span(audio).subspan(p,std::min<std::size_t>(73,audio.size()-p)));double error=0;for(std::size_t i=0;i<audio.size();++i){const double expected=i==n+phase?.25:0.;error=std::max({error,std::abs(audio[i][0]-expected),std::abs(audio[i][1]+expected)});}CHECK(error<=2.e-7);out<<window<<','<<rate<<','<<n<<','<<offset<<','<<phase<<','<<error<<','<<(error<=2.e-7)<<'\n';}}
 std::cout<<"All phases: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
int boundedPhases(const std::filesystem::path& root,double rate,std::size_t first,std::size_t count){
 SpectralEq eq;CHECK(eq.prepare(rate,512,4096,0,SpectralEqWork::bounded));const auto n=std::size_t(eq.transformSize()),hop=n/4,delay=std::size_t(eq.latencySamples());
 if(!count||first>=hop||count>hop-first)return 2;studyDirectory(root);auto out=studyFile(root,"impulses.csv","rate,N,delay,hop_offset,input_phase,max_error,deadline_misses,passes");std::vector<DynamicEq::Frame> audio(3*n);
 for(int offset:{0,int(hop/3)}){CHECK(eq.prepare(rate,512,4096,offset,SpectralEqWork::bounded));for(std::size_t phase=first;phase<first+count;++phase){eq.reset();std::fill(audio.begin(),audio.end(),DynamicEq::Frame{});audio[phase]={.25,-.25};for(std::size_t p=0;p<audio.size();p+=73)eq.processBlock(std::span(audio).subspan(p,std::min<std::size_t>(73,audio.size()-p)));double error=0;
   for(std::size_t i=0;i<audio.size();++i){const double expected=i==delay+phase?.25:0.;error=std::isfinite(audio[i][0])&&std::isfinite(audio[i][1])?std::max({error,std::abs(audio[i][0]-expected),std::abs(audio[i][1]+expected)}):std::numeric_limits<double>::infinity();}const bool pass=error<=2.e-7&&eq.workStats().deadlineMisses==0;CHECK(pass);out<<rate<<','<<n<<','<<delay<<','<<offset<<','<<phase<<','<<error<<','<<eq.workStats().deadlineMisses<<','<<pass<<'\n';}}
 std::cout<<"Bounded independent phases: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
int boundedFrequency(const std::filesystem::path& root){
 studyDirectory(root);auto out=studyFile(root,"frequency.csv","rate,N,delay,target_hz,other_hz,q,range_db,weighted_bins,target_db,other_db,deadline_misses,passes");
 for(double rate:{44100.,48000.,96000.,192000.})for(auto pair:{std::array<double,2>{100,125},std::array<double,2>{500,700},std::array<double,2>{1500,6000}})for(float range:{-6.f,6.f}){
  SpectralEq eq;CHECK(eq.prepare(rate,512,4096,0,SpectralEqWork::bounded));auto s=selective();s.bands[0].frequencyHz=float(pair[0]);s.bands[0].q=4;s.bands[0].rangeDb=range;s.bands[0].spectralDensity=1;CHECK(eq.setSettings(s));std::vector<DynamicEq::Frame> audio(std::size_t(2*rate));
  for(std::size_t i=0;i<audio.size();++i){const double v=.4*(std::sin(2*std::numbers::pi*pair[0]*i/rate)+std::sin(2*std::numbers::pi*pair[1]*i/rate));audio[i]={v,v};}
  for(std::size_t p=0;p<audio.size();p+=64)eq.processBlock(std::span(audio).subspan(p,std::min<std::size_t>(64,audio.size()-p)));
  const double target=20*std::log10(toneAmplitude(audio,0,pair[0],std::size_t(rate),rate)/.4),other=20*std::log10(toneAmplitude(audio,0,pair[1],std::size_t(rate),rate)/.4);const bool pass=(range<0?target<=-3:target>=3)&&std::abs(other)<=.25&&eq.workStats().deadlineMisses==0;CHECK(pass);
  out<<rate<<','<<eq.transformSize()<<','<<eq.latencySamples()<<','<<pair[0]<<','<<pair[1]<<",4,"<<range<<','<<eq.weightedBinCount(0)<<','<<target<<','<<other<<','<<eq.workStats().deadlineMisses<<','<<pass<<'\n';
 }
 std::cout<<"Bounded frequency: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
}
int main(int argc,char** argv){try{if(argc==6&&std::string_view(argv[1])=="--bounded-impulse-phases")return boundedPhases(argv[2],std::stod(argv[3]),std::stoull(argv[4]),std::stoull(argv[5]));if(argc==3&&std::string_view(argv[1])=="--bounded-frequency")return boundedFrequency(argv[2]);if(argc==3&&std::string_view(argv[1])=="--window-study")return windowStudy(argv[2]);if(argc==5&&std::string_view(argv[1])=="--impulse-phases")return allPhases(argv[2],std::stoi(argv[3]),std::stod(argv[4]));if(argc!=1)return 2;boundedWorkContract();boundedReferenceEquivalence();boundedHeapAndReset();boundedPhysicalSignals();lowerWindowHeap();narrowBandNoiseFloor();preparedBandCoverage();smallerPreparedGeometry();smallerRegression();staticEndpoints();flatSpectralKernelReconstructsDelayedAudio();reconstruction();selectivity();densityTiltMotionAndBounds();validationAndHeap();std::cout<<"Spectral EQ: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
