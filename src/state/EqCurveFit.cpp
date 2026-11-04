#include "EqCurveFit.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace airack
{
namespace
{
constexpr std::uint32_t maximumEvaluations=4096;
constexpr double minimumImprovement=.05, regularization=.02;
using Samples=std::array<double,129>;
double db(std::complex<double> value)
{ return 20.*std::log10(std::max(std::abs(value),1e-12)); }
auto order(const DynamicEqBand& band)
{ return std::tuple(band.shape,band.frequencyHz,band.q,band.gainDb); }

struct Evaluation {
    DynamicEqBand band;
    Samples bandDb{},totalDb{};
    double rms=0,penalty=0,objective=std::numeric_limits<double>::infinity();
    bool valid=false;
};
struct Search {
    const EqCurve& target;
    double rate;
    const std::atomic_bool* cancel;
    DynamicEqSettings settings;
    Samples frequencies{},currentDb{};
    std::array<Samples,8> addedDb{};
    std::bitset<8> added;
    std::size_t samples=0;
    std::uint32_t evaluations=0;
    double rms=0,penalty=0;
    bool interrupted=false;

    bool cancelled() {
        if(cancel&&cancel->load(std::memory_order_relaxed))interrupted=true;
        return interrupted;
    }
    double error(const Samples& values) const {
        double squared=0;
        for(std::size_t i=0;i<values.size();++i)if(frequencies[i]>0){
            const auto residual=double(target.db[i])-values[i];
            squared+=residual*residual;
        }
        return std::sqrt(squared/double(samples));
    }
    bool prepare() {
        if(cancelled()||!validateDynamicEq(settings))return false;
        for(const auto value:target.db)if(!std::isfinite(value))return false;
        // Ask the shared response API to validate the rate, including its
        // lower/upper supported limits; never invent another DSP rate domain.
        if(!dynamicEqResponse(settings,rate,20.))return false;
        const bool mixed=std::any_of(settings.bands.begin(),settings.bands.end(),[](const auto& b){
            return b.active&&!b.bypassed&&b.placement!=DynamicEqPlacement::stereo;
        });
        for(std::size_t i=0;i<frequencies.size();++i){
            const double frequency=20.*std::pow(1000.,double(i)/128.);
            if(frequency>rate*.5)continue;
            frequencies[i]=frequency;++samples;
            if(mixed){
                // Every addition is Stereo (a scalar times identity), so its
                // magnitude multiplies the entire ordered base matrix.
                const auto energy=dynamicEqEnergyResponse(settings,rate,frequency);
                if(!energy)return false;
                currentDb[i]=20.*std::log10(std::max(*energy,1e-12));
            } else {
                const auto response=dynamicEqResponse(settings,rate,frequency);
                if(!response)return false;
                currentDb[i]=db(*response);
            }
        }
        ++evaluations;
        rms=error(currentDb);
        return std::isfinite(rms)&&!cancelled();
    }
    bool evaluate(std::size_t slot,const DynamicEqBand& band,Evaluation& out) {
        if(cancelled()||evaluations>=maximumEvaluations)return false;
        ++evaluations;
        auto candidate=settings;candidate.bands[slot]=band;
        const double compensation=double(dynamicEqAutoGainDb(candidate))-dynamicEqAutoGainDb(settings);
        if(!std::isfinite(compensation))return false;
        out.band=band;
        out.penalty=penalty+regularization*(std::abs(double(band.gainDb))
            -(added[slot]?std::abs(double(settings.bands[slot].gainDb)):0.));
        for(std::size_t i=0;i<frequencies.size();++i){
            if((i%16)==0&&cancelled())return false;
            if(frequencies[i]==0)continue;
            const auto response=dynamicEqBandResponse(band,rate,frequencies[i],0,settings.gainScale);
            if(!response)return false;
            out.bandDb[i]=db(*response);
            out.totalDb[i]=currentDb[i]-addedDb[slot][i]+out.bandDb[i]+compensation;
        }
        out.rms=error(out.totalDb);out.objective=out.rms+out.penalty;
        out.valid=std::isfinite(out.objective);
        return out.valid;
    }
    static bool better(const Evaluation& candidate,const Evaluation& best) {
        if(!best.valid)return true;
        if(candidate.objective<best.objective-1e-10)return true;
        return std::abs(candidate.objective-best.objective)<=1e-10&&order(candidate.band)<order(best.band);
    }
    void consider(std::size_t slot,const DynamicEqBand& band,Evaluation& best) {
        Evaluation trial;
        if(evaluate(slot,band,trial)&&better(trial,best))best=std::move(trial);
    }
    void refine(std::size_t slot,Evaluation& best,double frequencyStep,double gainStep,double qStep) {
        for(int dimension=0;dimension<3&&!cancelled();++dimension)for(const int direction:{-1,1}){
            auto band=best.band;
            if(dimension==0)band.frequencyHz=float(std::clamp(double(band.frequencyHz)*std::exp(direction*frequencyStep),20.,std::min(20000.,rate*.45)));
            if(dimension==1)band.gainDb=float(std::clamp(double(band.gainDb)+direction*gainStep,-12.,12.));
            if(dimension==2)band.q=float(std::clamp(double(band.q)*std::exp(direction*qStep),.25,band.shape==DynamicEqShape::bell?18.:4.));
            if(band==best.band)continue;
            consider(slot,band,best);
        }
    }
    void accept(std::size_t slot,const Evaluation& fit) {
        settings.bands[slot]=fit.band;added.set(slot);addedDb[slot]=fit.bandDb;
        currentDb=fit.totalDb;rms=fit.rms;penalty=fit.penalty;
    }
    void fit(std::bitset<8> available) {
        const double span=std::log(std::min(20000.,rate*.45)/20.);
        for(std::size_t slot=0;slot<8&&!cancelled();++slot){
            if(!available[slot]||settings.bands[slot].active)continue;
            Evaluation best;
            // Hierarchical grid search keeps all eight destinations inside
            // the same complete-candidate budget, rather than multiplying
            // frequency, Q and gain into an unbounded Cartesian product.
            for(int shape=0;shape<3&&!cancelled();++shape)for(int f=0;f<17&&!cancelled();++f)
                for(const float gain:{-12.f,-9.f,-6.f,-3.f,3.f,6.f,9.f,12.f}){
                    DynamicEqBand band;band.active=true;band.shape=DynamicEqShape(shape);
                    band.frequencyHz=float(20.*std::exp(span*f/16.));band.q=.7071f;band.gainDb=gain;
                    consider(slot,band,best);
                }
            if(!best.valid)break;
            for(const float q:{.5f,.7071f,1.f,2.f,4.f,8.f}){
                auto band=best.band;
                if(q>(band.shape==DynamicEqShape::bell?18.f:4.f))continue;
                band.q=q;consider(slot,band,best);
            }
            double step=.5;
            for(int pass=0;pass<6&&!cancelled();++pass,step*=.5)
                refine(slot,best,span/16.*step,3.*step,std::log(2.)*step);
            if(rms-best.rms<minimumImprovement||best.objective>=rms+penalty-1e-10)break;
            accept(slot,best);
            if(rms<minimumImprovement)break;
        }
        // Refine only additions, preserving every occupied/raw band. A small
        // fixed pass bound and the shared candidate counter bound both loops.
        double step=.25;
        for(int pass=0;pass<16&&evaluations<maximumEvaluations&&!cancelled();++pass,step*=.5){
            const double previous=rms;
            for(std::size_t slot=0;slot<8&&!cancelled();++slot)if(added[slot]){
                Evaluation best;best.band=settings.bands[slot];best.bandDb=addedDb[slot];
                best.totalDb=currentDb;best.rms=rms;best.penalty=penalty;best.objective=rms+penalty;best.valid=true;
                refine(slot,best,span/16.*step,3.*step,std::log(2.)*step);
                if(best.objective<rms+penalty-1e-10)accept(slot,best);
            }
            if(previous-rms<minimumImprovement)break;
        }
    }
};
}
EqFitResult fitEqCurve(const EqCurve& target,const DynamicEqSettings& base,
    std::bitset<8> available,double rate,const std::atomic_bool* cancel)
{
    EqFitResult result;
    result.settings=base;
    Search search{target,rate,cancel,base};
    if(!search.prepare()){result.evaluations=search.evaluations;return result;}
    result.rmsDb=float(std::min(search.rms,double(std::numeric_limits<float>::max())));
    result.evaluations=search.evaluations;
    if(search.rms<minimumImprovement){result.ready=!search.cancelled();return result;}
    for(std::size_t i=0;i<8;++i)if(base.bands[i].active)available.reset(i);
    if(available.none()||base.gainScale==0)return result;
    search.fit(available);
    result.evaluations=search.evaluations;
    if(search.cancelled())return result;
    result.settings=search.settings;
    result.rmsDb=float(std::min(search.rms,double(std::numeric_limits<float>::max())));
    result.ready=true;
    return result;
}
}
