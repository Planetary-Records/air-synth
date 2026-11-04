#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include "Saturation.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <system_error>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <emmintrin.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#if __has_include("DspPipeline.h")
#include "DspPipeline.h"
#define AIRACK_HAS_DSP_PIPELINE 1
#endif

namespace {
int checks=0,failures=0;
void check(bool ok,const char* what,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<line<<' '<<what<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
#if defined(AIRACK_HAS_DSP_PIPELINE)
using airack::detail::DspPipeline;
template<class Pool> unsigned finishUnstarted(Pool& pool){
 if constexpr(requires{pool.finishUnstarted();})return pool.finishUnstarted();
 else {pool.finish();return 0;}
}
template<class Pool> constexpr bool supportsUnstartedCancellation=requires(Pool& pool){pool.finishUnstarted();};
struct Job {
 DspPipeline* pool=nullptr;
 unsigned count=0,epoch=0;
 std::array<std::uint64_t,257> first{},second{},mixed{},output{};
 std::array<bool,3> marked{};
#if defined(_WIN32)
 std::array<PROCESSOR_NUMBER,3> ideals{};std::array<bool,3> idealKnown{};
 std::array<GROUP_AFFINITY,3> affinities{};std::array<bool,3> affinityKnown{};
 std::array<std::uint32_t,3> taskIds{};
#endif
 static void work(void* pointer,unsigned worker) noexcept {
  auto& job=*static_cast<Job*>(pointer);job.marked[worker]=DspPipeline::renderingOnThisThread();
#if defined(_WIN32)
  job.idealKnown[worker]=GetThreadIdealProcessorEx(GetCurrentThread(),&job.ideals[worker])!=FALSE;
  job.affinityKnown[worker]=GetThreadGroupAffinity(GetCurrentThread(),&job.affinities[worker])!=FALSE;
  job.taskIds[worker]=DspPipeline::schedulingTaskOnThisThread();
#endif
  for(unsigned offset=0,packet=1;offset<job.count;offset+=32,++packet){
   const auto end=std::min(offset+32,job.count);
   if(worker==2){job.pool->await(0,packet);job.pool->await(1,packet);}
   for(unsigned n=offset;n<end;++n){if(worker==0)job.first[n]=(std::uint64_t(job.epoch)<<32)^n;
    else if(worker==1)job.second[n]=n*3+job.epoch;else job.mixed[n]=job.first[n]+job.second[n];}
   job.pool->publish(worker,packet);
  }
 }
 void render(){pool->begin();for(unsigned offset=0,packet=1;offset<count;offset+=32,++packet){pool->await(2,packet);for(unsigned n=offset;n<std::min(offset+32,count);++n)output[n]=mixed[n]*7+13;}pool->finish();}
};
std::atomic<int> live{0},exited{0};
#if defined(_WIN32)
std::atomic<unsigned> affinityRestoreFailures{0};
#endif
unsigned launches=0,failAt=0;
std::thread failingLaunch(DspPipeline::Entry entry,DspPipeline* pool,unsigned worker){
 if(launches++==failAt)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
 return std::thread([=]{++live;
#if defined(_WIN32)
  GROUP_AFFINITY before{},after{};const auto known=GetThreadGroupAffinity(GetCurrentThread(),&before)!=FALSE;const auto priorityBefore=GetThreadPriority(GetCurrentThread());
#endif
  entry(pool,worker);
#if defined(_WIN32)
  if(!known||!GetThreadGroupAffinity(GetCurrentThread(),&after)||before.Group!=after.Group||before.Mask!=after.Mask||priorityBefore!=GetThreadPriority(GetCurrentThread()))++affinityRestoreFailures;
#endif
  --live;++exited;});
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool callbackCounting=false;
std::atomic<unsigned> allocations{0},frees{0};
int __cdecl heapHook(int kind,void*,size_t,int,long,const unsigned char*,int){if(callbackCounting||DspPipeline::renderingOnThisThread()){
 if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)allocations.fetch_add(1,std::memory_order_relaxed);if(kind==_HOOK_FREE)frees.fetch_add(1,std::memory_order_relaxed);}return 1;}
#endif
void publicationAndRepeatedJobs(){
#if defined(_WIN32)
 GROUP_AFFINITY parentBefore{};CHECK(GetThreadGroupAffinity(GetCurrentThread(),&parentBefore));
 const auto priorityBefore=GetThreadPriority(GetCurrentThread());
#endif
 DspPipeline pool;Job job;job.pool=&pool;CHECK(!pool.available()&&pool.workerCount()==0);CHECK(!pool.prepare(&job,nullptr));CHECK(pool.prepare(&job,Job::work));CHECK(pool.available()&&pool.workerCount()==3);CHECK(!pool.prepare(&job,Job::work));
#if defined(_MSC_VER) && defined(_DEBUG)
 allocations=0;frees=0;const auto previous=_CrtSetAllocHook(heapHook);
#endif
 for(unsigned epoch=1;epoch<=256;++epoch){job.count=std::array<unsigned,6>{1,31,32,64,128,257}[epoch%6];job.epoch=epoch;job.marked.fill(false);
#if defined(_MSC_VER) && defined(_DEBUG)
  callbackCounting=true;
#endif
  job.render();
#if defined(_MSC_VER) && defined(_DEBUG)
  callbackCounting=false;
#endif
  CHECK(job.marked[0]&&job.marked[1]&&job.marked[2]);CHECK(!DspPipeline::renderingOnThisThread());CHECK(DspPipeline::schedulingTaskOnThisThread()==0);
#if defined(_WIN32)
  CHECK(job.taskIds[0]!=0&&job.taskIds[0]==job.taskIds[1]&&job.taskIds[0]==job.taskIds[2]);
  for(unsigned stage=0;stage<3;++stage){CHECK(job.idealKnown[stage]);if(parentBefore.Group==job.ideals[stage].Group&&job.ideals[stage].Number<64)CHECK((parentBefore.Mask&(KAFFINITY(1)<<job.ideals[stage].Number))!=0);
   CHECK(job.affinityKnown[stage]);if(GetActiveProcessorGroupCount()==1&&job.ideals[0].Number!=job.ideals[1].Number&&job.ideals[0].Number!=job.ideals[2].Number&&job.ideals[1].Number!=job.ideals[2].Number){const auto unionMask=job.affinities[0].Mask|job.affinities[1].Mask|job.affinities[2].Mask;
    const bool placed=job.affinities[0].Mask!=parentBefore.Mask||job.affinities[1].Mask!=parentBefore.Mask||job.affinities[2].Mask!=parentBefore.Mask;
    if(placed&&std::popcount(unionMask)>=6){CHECK(std::popcount(job.affinities[stage].Mask)>=2);CHECK((job.affinities[stage].Mask&job.affinities[(stage+1)%3].Mask)==0);}
    else {CHECK(std::popcount(job.affinities[stage].Mask)>=3);CHECK(job.affinities[stage].Mask==job.affinities[0].Mask);}}}
#endif
  for(unsigned n=0;n<job.count;++n)CHECK(job.output[n]==(((std::uint64_t(epoch)<<32)^n)+n*3+epoch)*7+13);
 }
#if defined(_WIN32)
 GROUP_AFFINITY parentAfter{};CHECK(GetThreadGroupAffinity(GetCurrentThread(),&parentAfter));
 CHECK(parentBefore.Group==parentAfter.Group&&parentBefore.Mask==parentAfter.Mask);CHECK(GetThreadPriority(GetCurrentThread())==priorityBefore);
 std::cout<<"DSP_WORKER_IDEALS "<<unsigned(job.ideals[0].Number)<<','<<unsigned(job.ideals[1].Number)<<','<<unsigned(job.ideals[2].Number)<<" parent_affinity_priority_unchanged=1\n";
 std::cout<<"DSP_WORKER_MASKS "<<std::hex<<job.affinities[0].Mask<<','<<job.affinities[1].Mask<<','<<job.affinities[2].Mask<<std::dec<<'\n';
 std::cout<<"DSP_WORKER_TASKS "<<job.taskIds[0]<<','<<job.taskIds[1]<<','<<job.taskIds[2]<<" pipeline_bytes="<<sizeof(DspPipeline)<<'\n';
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
 _CrtSetAllocHook(previous);CHECK(allocations.load()==0&&frees.load()==0);std::cout<<"DSP_JOBS_CALLBACK allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
}
void saturationWorkerOwnership(){
 struct SaturationJob {
  std::array<airack::Saturation,3> processors;
  std::array<std::array<airack::Saturation::Frame,257>,3> frames{};
  airack::SaturationSettings target;bool reset=false,monitoring=false;unsigned epoch=0;std::array<bool,3> admitted{},marked{},auditioned{};
  static bool monitor(airack::Saturation& processor,std::span<airack::Saturation::Frame> frames,unsigned stage,unsigned epoch,bool threeBand) noexcept {
   const auto lane=[&](unsigned offset)->std::optional<airack::SaturationLane>{return threeBand?std::optional<airack::SaturationLane>(airack::SaturationLane(1+(stage+epoch+offset)%3)):std::nullopt;};
   bool ok=processor.setAudition(lane(0));processor.processBlock(frames.first(17));
   ok=processor.setAudition(lane(1))&&ok;processor.processBlock(frames.subspan(17,64));
   processor.cancelAudition();processor.processBlock(frames.subspan(81,64));
   ok=processor.setAudition(lane(2))&&ok;processor.processBlock(frames.subspan(145));return ok;
  }
  static void work(void* pointer,unsigned stage) noexcept {
   auto& job=*static_cast<SaturationJob*>(pointer);auto& processor=job.processors[stage];
   job.marked[stage]=DspPipeline::renderingOnThisThread();job.admitted[stage]=processor.setSettings(job.target);
   if(job.reset)processor.reset();
   if(job.monitoring)job.admitted[stage]=monitor(processor,job.frames[stage],stage,job.epoch,job.target.mode==airack::SaturationMode::threeBand)&&job.admitted[stage];
   else processor.processBlock(job.frames[stage]);
   job.auditioned[stage]=processor.auditioning();
  }
 } job;
 for(double rate:{8000.,48000.,768000.})for(bool monitoring:{false,true}){
  job.monitoring=monitoring;
  std::array<airack::Saturation,3> reference;
  for(unsigned stage=0;stage<3;++stage){CHECK(job.processors[stage].setSettings({}));CHECK(reference[stage].setSettings({}));CHECK(job.processors[stage].prepare(rate));CHECK(reference[stage].prepare(rate));}
  DspPipeline pool;CHECK(pool.prepare(&job,SaturationJob::work));CHECK(pool.workerCount()==3);
#if defined(_MSC_VER) && defined(_DEBUG)
  allocations=0;frees=0;const auto previous=_CrtSetAllocHook(heapHook);
#endif
  for(unsigned epoch=0;epoch<18;++epoch){
   job.epoch=epoch;
   job.target.mode=epoch%2?airack::SaturationMode::threeBand:airack::SaturationMode::fullBand;
   job.target.driveCompensation=epoch%2==0;job.target.preLowCutEnabled=epoch%3==0;job.target.postHighCutEnabled=epoch%3==1;
   for(unsigned lane=0;lane<4;++lane){auto& l=job.target.lanes[lane];l.character=airack::SaturationCharacter((epoch+lane)%3);l.driveDb=float(epoch)*2;l.bypassed=(epoch+lane)%4==0;l.preTiltDb=float(int(epoch)%7)-3;}
   job.reset=epoch==9;
   for(unsigned stage=0;stage<3;++stage)for(unsigned n=0;n<257;++n)job.frames[stage][n]={.35*std::sin((n+epoch*257)*(stage+1)*.031),-.27*std::cos((n+epoch*257)*(stage+1)*.019)};
   auto expected=job.frames;
   for(unsigned stage=0;stage<3;++stage){CHECK(reference[stage].setSettings(job.target));if(job.reset)reference[stage].reset();
    if(monitoring)CHECK(SaturationJob::monitor(reference[stage],expected[stage],stage,epoch,job.target.mode==airack::SaturationMode::threeBand));
    else reference[stage].processBlock(expected[stage]);}
#if defined(_MSC_VER) && defined(_DEBUG)
   callbackCounting=true;
#endif
   pool.begin();pool.finish();
#if defined(_MSC_VER) && defined(_DEBUG)
   callbackCounting=false;
#endif
   for(unsigned stage=0;stage<3;++stage){CHECK(job.marked[stage]&&job.admitted[stage]);double maximum=0;unsigned unequal=0;
    if(monitoring&&job.target.mode==airack::SaturationMode::threeBand)CHECK(job.auditioned[stage]);
    for(unsigned n=0;n<257;++n)for(unsigned ch=0;ch<2;++ch){maximum=std::max(maximum,std::abs(job.frames[stage][n][ch]-expected[stage][n][ch]));if(std::bit_cast<std::uint64_t>(job.frames[stage][n][ch])!=std::bit_cast<std::uint64_t>(expected[stage][n][ch]))++unequal;}
    CHECK(unequal==0);if(unequal)std::cout<<"WORKER_DIAGNOSTIC rate="<<rate<<" epoch="<<epoch<<" stage="<<stage<<" unequal="<<unequal<<" maximum="<<maximum<<'\n';}
  }
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(previous);CHECK(allocations.load()==0&&frees.load()==0);
  std::cout<<"SATURATION_WORKERS rate="<<rate<<" owners=3 epochs=18 cohort="<<(monitoring?"audition_retarget_cancel_repress":"original_normal")<<" marked_worker=1 allocations="<<allocations<<" frees="<<frees<<'\n';
#else
  std::cout<<"SATURATION_WORKERS rate="<<rate<<" owners=3 epochs=18 cohort="<<(monitoring?"audition_retarget_cancel_repress":"original_normal")<<" marked_worker=1 exact_serial_equal=1\n";
#endif
 }
}
void partialStartupAndIdleRetirement(){
 for(failAt=0;failAt<3;++failAt){launches=0;exited=0;live=0;DspPipeline pool;Job job;job.pool=&pool;CHECK(!pool.prepare(&job,Job::work,failingLaunch));CHECK(!pool.available()&&pool.workerCount()==0);CHECK(live.load()==0&&exited.load()==int(failAt));CHECK(pool.prepare(&job,Job::work));job.count=257;job.epoch=17;job.render();CHECK(job.output[256]==(((std::uint64_t(17)<<32)^256)+256*3+17)*7+13);}
 for(int n=0;n<12;++n){DspPipeline pool;Job job;job.pool=&pool;CHECK(pool.prepare(&job,Job::work));}
 CHECK(live.load()==0);
#if defined(_WIN32)
 CHECK(affinityRestoreFailures.load()==0);
#endif
}
void callbackFloatingPointMode(){
#if defined(_WIN32)
 struct Arithmetic {
  volatile double first=1,halfUlp=std::numeric_limits<double>::epsilon()*.5,divisor=3;
  std::array<double,3> result{};
  static void work(void* pointer,unsigned stage) noexcept {auto& a=*static_cast<Arithmetic*>(pointer);a.result[stage]=(a.first+a.halfUlp)/a.divisor;}
 } arithmetic;
 DspPipeline pool;CHECK(pool.prepare(&arithmetic,Arithmetic::work));
 const auto original=_mm_getcsr();std::uint64_t nearest=0;bool sensitive=false;
 for(auto rounding:{_MM_ROUND_NEAREST,_MM_ROUND_UP,_MM_ROUND_DOWN,_MM_ROUND_TOWARD_ZERO}){
  _mm_setcsr((original&~_MM_ROUND_MASK)|rounding);
  const double expected=(arithmetic.first+arithmetic.halfUlp)/arithmetic.divisor;
  const auto bits=std::bit_cast<std::uint64_t>(expected);
  if(rounding==_MM_ROUND_NEAREST)nearest=bits;else sensitive=sensitive||bits!=nearest;
  pool.begin();pool.finish();for(auto value:arithmetic.result)CHECK(std::bit_cast<std::uint64_t>(value)==bits);
  CHECK((_mm_getcsr()&_MM_ROUND_MASK)==unsigned(rounding));
 }
 _mm_setcsr(original);CHECK(sensitive);
#endif
}
void finishWaitsForEveryOwner(){
 struct Held {
  std::atomic<unsigned> arrived{0};std::atomic<bool> release{false};
  std::array<unsigned,3> result{};unsigned epoch=0;
  static void work(void* pointer,unsigned stage) noexcept {
   auto& job=*static_cast<Held*>(pointer);job.arrived.fetch_add(1,std::memory_order_release);
   while(!job.release.load(std::memory_order_acquire))std::this_thread::yield();
   job.result[stage]=job.epoch*7+stage;
  }
 } job;
 DspPipeline pool;const auto prepared=pool.prepare(&job,Held::work);CHECK(prepared);if(!prepared)return;
#if defined(_MSC_VER) && defined(_DEBUG)
 allocations=0;frees=0;const auto previous=_CrtSetAllocHook(heapHook);
#endif
 for(unsigned epoch=1;epoch<=16;++epoch){
  job.epoch=epoch;job.arrived=0;job.release=false;job.result.fill(0);pool.begin();
  while(job.arrived.load(std::memory_order_acquire)!=3)std::this_thread::yield();
  std::thread release([&]{std::this_thread::sleep_for(std::chrono::milliseconds(2));job.release.store(true,std::memory_order_release);});
#if defined(_MSC_VER) && defined(_DEBUG)
  callbackCounting=true;
#endif
  if(epoch&1)pool.finish();else finishUnstarted(pool);
#if defined(_MSC_VER) && defined(_DEBUG)
  callbackCounting=false;
#endif
  CHECK(job.release.load(std::memory_order_acquire));for(unsigned stage=0;stage<3;++stage)CHECK(job.result[stage]==epoch*7+stage);
  release.join();
 }
#if defined(_MSC_VER) && defined(_DEBUG)
 _CrtSetAllocHook(previous);CHECK(allocations.load()==0&&frees.load()==0);
#endif
}
void ownedWorkerScheduling(){
#if defined(_WIN32)
 struct Scheduling {
  std::array<bool,3> read{},highQos{},audioPriority{};
  static void work(void* pointer,unsigned stage) noexcept {
   auto& s=*static_cast<Scheduling*>(pointer);THREAD_POWER_THROTTLING_STATE power{};power.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
   s.read[stage]=GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power))!=FALSE;
   s.highQos[stage]=(power.ControlMask&THREAD_POWER_THROTTLING_EXECUTION_SPEED)!=0&&(power.StateMask&THREAD_POWER_THROTTLING_EXECUTION_SPEED)==0;
   s.audioPriority[stage]=GetThreadPriority(GetCurrentThread())>=THREAD_PRIORITY_HIGHEST;
  }
 } scheduling;
 THREAD_POWER_THROTTLING_STATE before{};before.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
 CHECK(GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&before,sizeof(before))!=FALSE);
 const auto priority=GetThreadPriority(GetCurrentThread());
 {DspPipeline pool;CHECK(pool.prepare(&scheduling,Scheduling::work));pool.begin();pool.finish();
  for(unsigned i=0;i<3;++i){CHECK(scheduling.read[i]);CHECK(scheduling.highQos[i]);CHECK(scheduling.audioPriority[i]);}}
 THREAD_POWER_THROTTLING_STATE after{};after.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
 CHECK(GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&after,sizeof(after))!=FALSE);
 CHECK(before.ControlMask==after.ControlMask&&before.StateMask==after.StateMask);CHECK(priority==GetThreadPriority(GetCurrentThread()));
#endif
}
void unstartedEpochCancellation(){
 CHECK(supportsUnstartedCancellation<DspPipeline>);
 struct Optional {
  unsigned epoch=0;std::array<unsigned,3> written{};
  static void work(void* pointer,unsigned stage) noexcept {auto& job=*static_cast<Optional*>(pointer);job.written[stage]=job.epoch;}
 } job;
 DspPipeline pool;CHECK(pool.prepare(&job,Optional::work));
 for(unsigned epoch=1;epoch<=20000;++epoch){
  job.epoch=epoch;job.written.fill(0);pool.begin();
  if(epoch%16==0){pool.finish();for(auto value:job.written)CHECK(value==epoch);}
  else {const auto cancelled=finishUnstarted(pool);for(unsigned stage=0;stage<3;++stage)CHECK(job.written[stage]==((cancelled&(1u<<stage))?0:epoch));}
 }
}
#if defined(_WIN32)
std::array<HANDLE,3> capturedWorkers{};
std::thread captureWorker(DspPipeline::Entry entry,DspPipeline* pool,unsigned stage){
 std::thread thread(entry,pool,stage);capturedWorkers[stage]=thread.native_handle();return thread;
}
void suspendedUnstartedWorker(){
 struct Optional {std::array<unsigned,3> written{};static void work(void* pointer,unsigned stage) noexcept {++static_cast<Optional*>(pointer)->written[stage];}} job;
 DspPipeline pool;const auto prepared=pool.prepare(&job,Optional::work,captureWorker);CHECK(prepared);if(!prepared)return;
 // Suspend only this test's idle worker; the watchdog resumes it even if the fence is wrong.
 const auto suspended=SuspendThread(capturedWorkers[1])!=DWORD(-1);CHECK(suspended);if(!suspended)return;
 std::atomic<bool> resumed{false},watchdogUsed{false};
 std::thread watchdog([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));if(!resumed.exchange(true)){watchdogUsed=true;ResumeThread(capturedWorkers[1]);}});
 pool.begin();const auto cancelled=finishUnstarted(pool);
 CHECK((cancelled&2)!=0);CHECK(!watchdogUsed.load());CHECK(job.written[1]==0);
 if(!resumed.exchange(true))CHECK(ResumeThread(capturedWorkers[1])!=DWORD(-1));watchdog.join();
 pool.begin();pool.finish();CHECK(job.written[1]==1);
}
void startupRetainsNoKernelHandles(){
 // Earlier tests warm the Windows audio-task client before measuring owned resources.
 DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
 for(unsigned cycle=0;cycle<32;++cycle)for(failAt=0;failAt<3;++failAt){
  launches=0;exited=0;live=0;DspPipeline pool;Job job;job.pool=&pool;
  CHECK(!pool.prepare(&job,Job::work,failingLaunch));CHECK(live.load()==0);
  CHECK(pool.prepare(&job,Job::work));job.count=64;job.epoch=cycle;job.render();
 }
 CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));CHECK(after<=before);
 std::cout<<"DSP_KERNEL_HANDLES before="<<before<<" after="<<after<<'\n';
}
#endif
#endif
}
int main(){
#if defined(AIRACK_HAS_DSP_PIPELINE)
 publicationAndRepeatedJobs();saturationWorkerOwnership();partialStartupAndIdleRetirement();callbackFloatingPointMode();finishWaitsForEveryOwner();ownedWorkerScheduling();unstartedEpochCancellation();
#if defined(_WIN32)
 suspendedUnstartedWorker();
 startupRetainsNoKernelHandles();
#endif
#else
 CHECK(false); // Required prepared worker behavior is not implemented yet.
#endif
 std::cout<<"DSP pipeline: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
