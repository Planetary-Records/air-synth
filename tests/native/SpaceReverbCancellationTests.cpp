#include "SpaceReverbTank.h"
#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include "SpaceReverbField.h"
#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>
#include <string_view>

using namespace airack::space;
using namespace airack::space::detail;
namespace {
struct Countdown {
    std::size_t calls=0,stop=1;
    static bool poll(void* context) noexcept {
        auto& state=*static_cast<Countdown*>(context);
        return ++state.calls>=state.stop;
    }
    PreparationControl control(){return {poll,this};}
};
template<class Work> bool cancelled(Work&& work){
    try {work();}catch(const PreparationCancelled&){return true;}
    return false;
}
bool sameBank(const LossBank& a,const LossBank& b){
    if(a.scalar!=b.scalar||a.stages.size()!=b.stages.size())return false;
    for(std::size_t i=0;i<a.stages.size();++i){const auto& x=a.stages[i];const auto& y=b.stages[i];
        if(x.b0!=y.b0||x.b1!=y.b1||x.b2!=y.b2||x.a1!=y.a1||x.a2!=y.a2||x.frequency!=y.frequency||x.q!=y.q||x.cutDb!=y.cutDb||x.shape!=y.shape)return false;}
    return true;
}
}
int main(){
    int checks=0,failures=0;const auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';std::cerr<<"ENDPOINT_FAIL cancellation "<<label<<std::endl;}};
    DecayCurve curve;curve.bands[0]={true,false,DecayShape::bell,1000,.5,2};
    const auto baseline=fitLoss(curve,48000,.03,FitGeometry::compact,FitDomain::poleContour);
    check(baseline.converged,"baseline");
    for(const auto stop:{std::size_t(1),std::size_t(20),std::size_t(100)}){
        Countdown state{0,stop};
        check(cancelled([&]{fitLoss(curve,48000,.03,FitGeometry::compact,FitDomain::poleContour,FitVariant::original,state.control());}),"cold_cancelled");
        check(state.calls==stop,"cold_exact_progress");
    }
    Countdown edge;
    check(cancelled([&]{fitLoss(curve,48000,.03,FitGeometry::compact,FitDomain::poleContour,FitVariant::edgeBandwidth,edge.control());}),"edge_cancelled");
    Countdown warm;
    check(cancelled([&]{fitLossWarm(curve,48000,.04,.03,baseline.bank,warm.control());}),"warm_cancelled");
    Countdown observer{0,std::size_t(-1)};
    const auto observed=fitLoss(curve,48000,.03,FitGeometry::compact,FitDomain::poleContour,FitVariant::original,observer.control());
    check(observer.calls>100,"progress_observed");check(sameBank(baseline.bank,observed.bank),"observer_exact_coefficients");
    check(baseline.maxRelativeError==observed.maxRelativeError&&baseline.iterations==observed.iterations,"observer_exact_result");
    double maximum=0;std::size_t intervals=0;
    for(const auto stop:{std::size_t(1),std::size_t(10)}){
        Countdown loss{0,stop};
        check(cancelled([&]{certifyLoss(baseline.bank,48000,maximum,intervals,nullptr,loss.control());}),"loss_certificate_cancelled");
        check(loss.calls==stop,"loss_certificate_exact_progress");
        Countdown tail{0,stop};LossNetwork network;network.sampleRate=48000;network.lines.push_back({.03,&baseline.bank});network.mixing.push_back(1);network.control=tail.control();
        check(cancelled([&]{certifyTail(network,46);}),"tail_certificate_cancelled");check(tail.calls==stop,"tail_certificate_exact_progress");
    }
    PreparedTank prepared;check(prepareTank(DecayCurve{},.5,48000,prepared),"prepared_baseline");const auto before=prepared;
    for(const auto stop:{std::size_t(1),std::size_t(100)}){
        Countdown tank{0,stop};TankPreparationReport report;
        check(!prepareTank(curve,.5,48000,prepared,&report,tank.control()),"tank_cancelled");
        check(report.failure==TankPreparationFailure::cancelled,"tank_reason");check(prepared==before,"tank_preserved");check(tank.calls==stop,"tank_exact_progress");
    }
    struct Epoch {
        std::atomic<unsigned> value{0},checkpoints{0};
        std::atomic<bool> release{false};
        static bool poll(void* p) noexcept {auto& state=*static_cast<Epoch*>(p);state.checkpoints.fetch_add(1,std::memory_order_release);
            while(!state.release.load(std::memory_order_acquire))std::this_thread::yield();
            return state.value.load(std::memory_order_acquire)!=0;}
    } epoch;
    bool stopped=false;
    // RED can finish without polling; GREEN pauses inside an actual checkpoint
    // until the owner changes epoch, establishing a deterministic in-flight race.
    std::atomic<bool> finished{false};
    std::thread worker([&]{stopped=cancelled([&]{fitLoss(curve,48000,.03,FitGeometry::compact,FitDomain::poleContour,FitVariant::original,{Epoch::poll,&epoch});});finished.store(true,std::memory_order_release);});
    while(epoch.checkpoints.load(std::memory_order_acquire)==0&&!finished.load(std::memory_order_acquire))std::this_thread::yield();
    epoch.value.store(1,std::memory_order_release);epoch.release.store(true,std::memory_order_release);worker.join();
    check(stopped,"epoch_worker_joined");check(epoch.checkpoints.load()>0,"epoch_observed");
    PreparedField field;const Settings settings;
    check(prepareField(settings,48000,field),"field_baseline");const auto fieldBefore=field;
    for(const auto stop:{std::size_t(1),std::size_t(100)}){
        Countdown state{0,stop};FieldPreparationReport report;
        check(!prepareField(settings,48000,field,&report,state.control()),"field_cancelled");
        check(std::string_view(report.reason)=="cancelled","field_cancel_reason");
        check(field==fieldBefore,"field_preserved");check(state.calls==stop,"field_exact_progress");
    }
    Countdown tankObserver{0,std::size_t(-1)},fieldObserver{0,std::size_t(-1)};
    PreparedTank tankObserved;PreparedField fieldObserved;
    check(prepareTank(settings.decay,settings.size,48000,tankObserved,nullptr,tankObserver.control()),"field_tank_observed");
    check(prepareField(settings,48000,fieldObserved,nullptr,fieldObserver.control()),"field_observed");
    check(fieldObserved==fieldBefore,"field_observer_exact_descriptor");
    check(fieldObserver.calls>tankObserver.calls+10,"field_tail_checkpoints_observed");
    const auto tailStop=tankObserver.calls+(fieldObserver.calls>tankObserver.calls?(fieldObserver.calls-tankObserver.calls)/2:1);
    Countdown fieldTail{0,tailStop};FieldPreparationReport tailReport;
    check(!prepareField(settings,48000,field,&tailReport,fieldTail.control()),"field_tail_cancelled");
    check(std::string_view(tailReport.reason)=="cancelled","field_tail_reason");
    check(field==fieldBefore,"field_tail_preserved");check(fieldTail.calls==tailStop,"field_tail_exact_progress");
    std::cout<<"SpaceReverbCancellation checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
