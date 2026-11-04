#include "SaturationCandidate.h"
#include "SaturationAnalysis.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace allocationWitness
{
thread_local bool enabled=false;
thread_local std::size_t allocations=0,frees=0;
void allocated() noexcept { if(enabled) ++allocations; }
void freed(void* pointer) noexcept { if(enabled&&pointer) ++frees; }
}
void* operator new(std::size_t size) { if(auto* p=std::malloc(size?size:1)){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { allocationWitness::freed(p);std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
#if defined(_WIN32)
void* operator new(std::size_t size,std::align_val_t alignment) { if(auto* p=_aligned_malloc(size?size:1,static_cast<std::size_t>(alignment))){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size,std::align_val_t alignment) { return ::operator new(size,alignment); }
void operator delete(void* p,std::align_val_t) noexcept { allocationWitness::freed(p);_aligned_free(p); }
void operator delete[](void* p,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete(void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
#endif

namespace
{
using namespace airack;
using namespace airack::probe;
using namespace airack::probe::analysis;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
constexpr double R=1./16;
constexpr std::size_t recordFrames=65536;
constexpr std::array<double,3> rates{44100,48000,96000};
constexpr std::array<double,3> levels{R/16,R/4,R};
constexpr std::array<const char*,3> styles{"soft","glow","dense"};
std::string quoted(const std::string& text)
{
    std::string result="\"";
    for(unsigned char c:text) switch(c){case '\\':result+="\\\\";break;case '"':result+="\\\"";break;case '\n':result+="\\n";break;case '\r':result+="\\r";break;case '\t':result+="\\t";break;default:if(c<32) result+='?';else result+=static_cast<char>(c);}
    return result+'"';
}
std::ofstream output(const fs::path& path,bool binary=false)
{
    std::ofstream stream;stream.exceptions(std::ios::failbit|std::ios::badbit);
    stream.open(path,binary?std::ios::out|std::ios::binary:std::ios::out);
    stream<<std::setprecision(17);return stream;
}
std::string read(const fs::path& path)
{
    std::ifstream stream(path,std::ios::binary);
    if(!stream) throw std::runtime_error("cannot hash source "+path.string());
    std::ostringstream bytes;bytes<<stream.rdbuf();return bytes.str();
}
std::string hash(std::span<const char> bytes) { return juce::SHA256(bytes.data(),bytes.size()).toHexString().toStdString(); }
std::string hash(const std::string& bytes) { return hash(std::span<const char>(bytes)); }
std::string hash(std::span<const double> values) { return hash(std::span<const char>(reinterpret_cast<const char*>(values.data()),values.size_bytes())); }
std::string id(double rate,int mode,int style,double drive,double level)
{
    std::ostringstream name;name<<static_cast<int>(rate)<<'-'<<mode<<'-'<<styles[static_cast<std::size_t>(style)]<<'-'<<drive<<'-'<<level;return name.str();
}
SaturationSettings settings(int mode,int style,double drive,bool compensation=false)
{
    SaturationSettings value;value.mode=static_cast<SaturationMode>(mode);value.driveCompensation=compensation;
    for(auto& lane:value.lanes){lane.character=static_cast<SaturationCharacter>(style);lane.driveDb=static_cast<float>(drive);}
    return value;
}
void prepare(Candidate& candidate,double rate,unsigned factor,const SaturationSettings& value)
{
    if(!candidate.prepare(rate,factor)||!candidate.setSettings(value)) throw std::runtime_error("candidate rejected frozen settings");
    candidate.reset();
}
struct Gate { std::string name;bool pass=false,invalid=false;std::string detail; };
std::string cohortSources()
{
    const auto source=fs::path(__FILE__).parent_path();std::ostringstream result;result<<'{';bool comma=false;
    for(const auto* name:{"SaturationCandidate.h","SaturationCandidate.cpp","SaturationCalibration.h","SaturationAnalysis.h","SaturationAnalysis.cpp","SaturationAnalysisTests.cpp","SaturationProbeTests.cpp","SaturationFeasibility.cpp","SaturationWideNetwork.cpp","SaturationWideNetwork.h","SaturationMath.h","SaturationGain.h","contracts/SaturationNumericalWitness.h","SaturationProductState.h","SaturationGainRuntime.h","contracts/SaturationOptimizationWitness.h","../../CMakeLists.txt"})
    {
        const auto bytes=read(source/name);if(comma) result<<',';comma=true;
        const auto relative=std::string(name)=="../../CMakeLists.txt"?"CMakeLists.txt":"probes/saturation/"+std::string(name);
        result<<quoted(relative)<<":{\"bytes\":"<<bytes.size()<<",\"sha256\":"<<quoted(hash(bytes))<<'}';
    }
    result<<'}';return result.str();
}
struct Evidence
{
    fs::path directory;unsigned factor;std::vector<Gate> gates;
    std::ofstream recordings,observations;std::string cohort,frozenSources;std::vector<std::string> memberKeys;
    Evidence(fs::path path,unsigned f,std::string selection={}):directory(std::move(path)),factor(f),recordings(output(directory/"recordings.csv")),cohort(std::move(selection))
    {
        recordings<<"file,sha256,sample_rate,frames,channels,encoding,warmup_frames,input,settings\n";
        if(!cohort.empty())
        {
            frozenSources=cohortSources();observations=output(directory/"quality-observations.csv");
            observations<<"name,summary_valid,spectrum_valid,result_valid,rms,unwanted_dbc,worst_db\n";
        }
    }
    void gate(std::string name,bool pass,std::string detail={},bool invalid=false)
    {
        std::cout<<name<<": "<<(invalid?"INVALID":pass?"PASS":"FAIL")<<' '<<detail<<std::endl;
        gates.push_back({std::move(name),pass,invalid,std::move(detail)});
    }
    void raw(const std::string& name,std::span<const double> samples,double rate,std::size_t warm,const std::string& inputDescription,const std::string& settingDescription)
    {
        const auto filename=name+".f64";auto stream=output(directory/filename,true);
        stream.write(reinterpret_cast<const char*>(samples.data()),static_cast<std::streamsize>(samples.size_bytes()));stream.close();
        recordings<<quoted(filename)<<','<<hash(samples)<<','<<rate<<','<<samples.size()<<",1,IEEE754-f64-little-endian,"<<warm<<','<<quoted(inputDescription)<<','<<quoted(settingDescription)<<'\n';
    }
};
std::vector<double> render(Candidate& candidate,double rate,double frequency,double peak,std::size_t frames,double warmSeconds)
{
    const auto warm=static_cast<std::size_t>(std::ceil(warmSeconds*rate));
    std::vector<double> samples(frames);
    for(std::size_t n=0;n<warm+frames;++n)
    {
        const double x=peak*std::sin(2*std::numbers::pi*frequency*static_cast<double>(n)/rate);
        const auto y=candidate.process({x,x});
        if(n>=warm) samples[n-warm]=y[0];
    }
    return samples;
}
double db(double power,double denominator) { return power==0?-std::numeric_limits<double>::infinity():10*std::log10(power/denominator); }
void freeze(Evidence& evidence)
{
    auto manifest=output(evidence.directory/"frozen-manifest.json");
    manifest<<"{\n\"schema\":1,\"factor\":"<<evidence.factor<<",\"record_frames\":"<<recordFrames<<",\"window\":\"rectangular\",\"normalization\":\"sum one-sided squared DFT/N; dBc denominator candidate fundamental power\",\n\"sources\":[";
    bool comma=false;const fs::path source=fs::path(__FILE__).parent_path();
    for(const auto* filename:{"SaturationCandidate.h","SaturationCandidate.cpp","SaturationCalibration.h","SaturationAnalysis.h","SaturationAnalysis.cpp","SaturationAnalysisTests.cpp","SaturationProbeTests.cpp","SaturationFeasibility.cpp"})
    {
        if(comma) manifest<<',';comma=true;const auto bytes=read(source/filename);
        manifest<<"{\"file\":"<<quoted(filename)<<",\"sha256\":"<<quoted(hash(bytes))<<'}';
    }
    const auto coefficients=saturationCoefficientManifest();
    if(coefficients.empty()) throw std::runtime_error("cannot freeze empty coefficient manifest");
    manifest<<"],\n\"coefficients\":[";comma=false;
    for(const auto& coefficient:coefficients)
    {
        if(!coefficient.name||!coefficient.name[0]||coefficient.values.empty()||!std::all_of(coefficient.values.begin(),coefficient.values.end(),[](double value){return std::isfinite(value);}))
            throw std::runtime_error("cannot freeze empty/nonfinite coefficient values");
        if(comma) manifest<<',';comma=true;
        manifest<<"{\"name\":"<<quoted(coefficient.name)<<",\"sha256\":"<<quoted(hash(coefficient.values))<<",\"values\":[";
        for(std::size_t n=0;n<coefficient.values.size();++n){if(n) manifest<<',';manifest<<coefficient.values[n];}manifest<<"]}";
    }
    manifest<<"],\n\"compensation_tables\":[";
    for(int style=0;style<3;++style)
    {
        if(style) manifest<<',';const auto table=saturationCompensationTable(static_cast<SaturationCharacter>(style));
        if(table.size()!=37||!std::all_of(table.begin(),table.end(),[](double value){return std::isfinite(value);}))
            throw std::runtime_error("cannot freeze compensation table: expected37 finite values");
        manifest<<"{\"style\":"<<quoted(styles[static_cast<std::size_t>(style)])<<",\"sha256\":"<<quoted(hash(table))<<",\"values\":[";
        for(std::size_t n=0;n<table.size();++n){if(n) manifest<<',';manifest<<table[n];}manifest<<"]}";
    }
    manifest<<"],\n\"coherent_bins\":[";comma=false;
    for(double rate:rates) for(double requested:{1760.,3520.,7040.})
    {
        const auto bin=nearestOddCoherentBin(requested,rate,recordFrames);if(!bin) throw std::runtime_error("coherent-bin calibration invalid");
        if(comma) manifest<<',';comma=true;
        manifest<<"{\"sample_rate\":"<<rate<<",\"requested_hz\":"<<requested<<",\"bin\":"<<*bin<<",\"exact_hz\":"<<rate*static_cast<double>(*bin)/recordFrames<<'}';
    }
    const std::string inputs="R=1/16;alias modes=full,three;styles=soft,glow,dense;drive=36;peak=R/4;compOff;tilt0;trim0;mix1;cutsOff;crossovers180,3500;2s warm;N65536;ref64 independent FIR;1x negative;mask reference unaliased harmonic amplitude>-120dBc;ref<=-100dBc;candidate<=-50dBc and10dB below1x;character bin301,N65536,2s warm,drive18;levels=R/16,R/4,R;comp220Hz48k peakR/4,1s discard1s measure73points0:.5:36;CPU8serialthreebandmixed24lanes8eachStyle;fixed24dB,mix.75,compOn,cuts20/18000;moving20ms triangle,Hzlog,dBlinear,mixlinear;A global(-3,+3,.4),xover80/1800,cuts40/9000,lane6/-6/.35/-3;B global(+3,-3,1),xover800/6500,cuts150/18000,lane30/+6/1/+3;callback-start targets,setSettings timed;stereo .01sin220+.005sin1760,rightphasepi/3both;block64/512;warm>=2s bothaudio/wall;measure>=10s bothaudio/wall;nearest-rankp99;0/0 C++heapalloc/free;neutral impulseN65536 rawinput response;corners180/3500,2000/500,40/16000";
    auto inputVectors=output(evidence.directory/"frozen-input-vectors.csv");
    inputVectors<<"name,sample_rate,frequency,peak,warmup_frames,measured_frames,sha256_all_native_input_frames\n";
    const auto freezeTone=[&](const std::string& name,double rate,double hz,double peak,std::size_t warm,std::size_t measured)
    {
        std::vector<double> data(warm+measured);
        for(std::size_t n=0;n<data.size();++n) data[n]=peak*std::sin(2*std::numbers::pi*hz*static_cast<double>(n)/rate);
        inputVectors<<name<<','<<rate<<','<<hz<<','<<peak<<','<<warm<<','<<measured<<','<<hash(std::span<const double>(data))<<'\n';
    };
    for(double rate:rates)
    {
        for(double requested:{1760.,3520.,7040.}){const auto bin=*nearestOddCoherentBin(requested,rate,recordFrames);freezeTone("alias-all-modes-styles",rate,rate*bin/recordFrames,R/4,static_cast<std::size_t>(2*rate),recordFrames);}
        for(double level:levels){freezeTone("character-all-styles",rate,rate*301/recordFrames,level,static_cast<std::size_t>(2*rate),recordFrames);freezeTone("operating-grid-all-settings",rate,rate*31/4096,level,static_cast<std::size_t>(std::ceil(.25*rate)),4096);}
    }
    freezeTone("compensation-all-219-points",48000,220,R/4,48000,48000);
    inputVectors.close();
    manifest<<"],\n\"input_manifest\":"<<quoted(inputs)<<",\"input_sha256\":"<<quoted(hash(inputs))<<",\"frozen_input_vectors_sha256\":"<<quoted(hash(read(evidence.directory/"frozen-input-vectors.csv")))<<",\"other_input_generators\":\"neutral impulse with2s zero prehistory;neutral null220+7040Hz eachR/8;Dense history prior2s220HzR then1.1sR/4;link L220R/8,R713R/2;burst220Hz segments seconds2/.1/1/2/1/.1/1 peaks0/level/0/R/0/level/0 with phase reset eachsegment;CPU deterministic generator plus actual rendered extent in timing-summary\",\"cpu_output_integrity\":\"store each serial8-unit stereo frame inside callback;outside timing finite and positive phase energy;FNV1a64 offset14695981039346656037 prime1099511628211 modulo2^64 over every outputdouble as8little-endian bytes in frame/channel order;separate warm/measured checksums\",\"operating_grid_warm_seconds\":0.25,\"operating_grid_frames\":4096,\"neutral_frequency_grid\":\"161 log points30..min18k/.4Fs plus exact requested corners and inner/outer quality limits;group-delay central +/-1Hz\",\"endian\":\"little\",\"hash_algorithm\":\"SHA256\"\n}\n";
    manifest.close();
}

void calibration(Evidence& evidence)
{
    std::vector<double> sine(4096),candidate(4096);
    for(std::size_t n=0;n<sine.size();++n){sine[n]=.4*std::sin(2*std::numbers::pi*101*static_cast<double>(n)/sine.size());candidate[n]=sine[n]+.04*std::sin(2*std::numbers::pi*317*static_cast<double>(n)/sine.size());}
    const auto summary=summarizeSignal(sine);
    const auto spectrum=rectangularPowerSpectrum(sine);
    const auto result=measureAlias(rectangularPowerSpectrum(candidate),spectrum,48000,101);
    const bool valid=summary.valid&&spectrum.valid&&result.valid&&std::abs(summary.rms-.4/std::sqrt(2.))<1e-12&&std::abs(result.unwantedDbc+20)<1e-6;
    if(!evidence.cohort.empty()) evidence.observations<<"calibration,"<<summary.valid<<','<<spectrum.valid<<','<<result.valid<<','<<summary.rms<<','<<result.unwantedDbc<<",\n";
    evidence.gate("analysis_calibration",valid,"known coherent sine power and injected -20 dBc",!valid);
    if(!valid) throw std::runtime_error("analysis calibration invalid");
}

void aliases(Evidence& evidence,double selectedRate=0,int selectedStyle=-1)
{
    auto rows=output(evidence.directory/"aliases.csv");
    rows<<"sample_rate,mode,style,requested_hz,bin,exact_hz,reference_unwanted_dbc,candidate_unwanted_dbc,one_x_unwanted_dbc,improvement_db,valid,pass,legitimate_bins,mismatched_dry_dbc,mismatched_control_failed\n";
    std::ofstream observations;
    if(!evidence.cohort.empty())
    {
        observations=output(evidence.directory/"alias-observations.csv");
        observations<<"sample_rate,mode,style,requested_hz,result_valid,negative_valid,mismatch_valid,eligible_dry_mask,candidate_power,candidate_fundamental,one_x_power,one_x_fundamental\n";
    }
    bool pass=true,valid=true,negativePass=true;std::size_t mismatchCases=0;
    std::array<std::array<bool,3>,2> oneXFailures{};
    for(double rate:rates) if(!selectedRate||rate==selectedRate)
        for(int mode=0;mode<2;++mode)
            for(int style=0;style<3;++style) if(selectedStyle<0||style==selectedStyle)
                for(double requested:{1760.,3520.,7040.})
    {
        const auto bin=*nearestOddCoherentBin(requested,rate,recordFrames);const auto hz=rate*static_cast<double>(bin)/recordFrames;
        const auto s=settings(mode,style,36);Candidate measured,reference,oneX,dry;
        prepare(measured,rate,evidence.factor,s);prepare(reference,rate,64,s);prepare(oneX,rate,1,s);
        auto drySettings=s;drySettings.mix=0;prepare(dry,rate,64,drySettings);
        const auto y=render(measured,rate,hz,R/4,recordFrames,2),ref=render(reference,rate,hz,R/4,recordFrames,2),control=render(oneX,rate,hz,R/4,recordFrames,2),dryRef=render(dry,rate,hz,R/4,recordFrames,2);
        const auto refSpectrum=rectangularPowerSpectrum(ref);
        const auto result=measureAlias(rectangularPowerSpectrum(y),refSpectrum,rate,bin);
        const auto negative=measureAlias(rectangularPowerSpectrum(control),refSpectrum,rate,bin);
        const auto mismatch=measureAlias(rectangularPowerSpectrum(y),rectangularPowerSpectrum(dryRef),rate,bin);
        const bool accepted=passesAliasGate(result,negative);
        const bool mismatchedRejected=mismatch.valid&&mismatch.unwantedDbc>-50;
        // A mismatched dry reference is meaningful only where legitimate harmonics lie inside the alias band.
        bool inBandHarmonic=false;for(auto harmonic:result.harmonicBins) if(harmonic!=bin&&harmonic*rate/recordFrames<std::min(16000.,.4*rate)) inBandHarmonic=true;
        if(inBandHarmonic){negativePass=negativePass&&mismatchedRejected;++mismatchCases;}
        if(negative.valid&&negative.unwantedDbc>-50) oneXFailures[static_cast<std::size_t>(mode)][static_cast<std::size_t>(style)]=true;
        valid=valid&&result.valid&&negative.valid;pass=pass&&accepted;
        std::ostringstream mask;for(auto harmonic:result.harmonicBins) mask<<harmonic<<';';
        rows<<rate<<','<<mode<<','<<styles[static_cast<std::size_t>(style)]<<','<<requested<<','<<bin<<','<<hz<<','<<result.referenceUnwantedDbc<<','<<result.unwantedDbc<<','<<negative.unwantedDbc<<','<<negative.unwantedDbc-result.unwantedDbc<<','<<result.valid<<','<<accepted<<','<<quoted(mask.str())<<','<<mismatch.unwantedDbc<<','<<mismatchedRejected<<'\n';
        if(!evidence.cohort.empty())
        {
            observations<<rate<<','<<mode<<','<<styles[static_cast<std::size_t>(style)]<<','<<requested<<','<<result.valid<<','<<negative.valid<<','<<mismatch.valid<<','<<inBandHarmonic<<','<<result.unwantedPower<<','<<result.fundamentalPower<<','<<negative.unwantedPower<<','<<negative.fundamentalPower<<'\n';
            evidence.memberKeys.push_back("["+std::to_string(static_cast<int>(rate))+','+std::to_string(mode)+','+quoted(styles[static_cast<std::size_t>(style)])+','+std::to_string(static_cast<int>(requested))+"]");
        }
        const auto name="alias-"+id(rate,mode,style,36,R/4)+'-'+std::to_string(bin);
        const auto input="sin;peak=R/4;bin="+std::to_string(bin)+";N=65536";
        evidence.raw(name+"-candidate",y,rate,static_cast<std::size_t>(2*rate),input,"factor="+std::to_string(evidence.factor));
        evidence.raw(name+"-reference64",ref,rate,static_cast<std::size_t>(2*rate),input,"independent FIR64;drive36;compOff");
        evidence.raw(name+"-negative1x",control,rate,static_cast<std::size_t>(2*rate),input,"unoversampled1x;drive36;compOff");
        evidence.raw(name+"-mismatched-dry64",dryRef,rate,static_cast<std::size_t>(2*rate),input,"mix0 mismatched dry reference");
        std::cout<<"alias "<<rate<<' '<<mode<<' '<<style<<' '<<requested<<": "<<result.unwantedDbc<<" dBc, ref "<<result.referenceUnwantedDbc<<std::endl;
    }
    rows.close();if(observations.is_open()) observations.close();
    evidence.gate("alias_reference_qualification",valid,"all 54 independent64x cases must be <=-100 dBc",!valid);
    evidence.gate("alias_candidate_and_improvement",pass&&valid,"all 54 cases <=-50 dBc and >=10 dB below same1x",!valid);
    bool oneXFailed=true;
    for(const auto& mode:oneXFailures)
        for(int style=0;style<3;++style) if(selectedStyle<0||style==selectedStyle)
            oneXFailed=oneXFailed&&mode[static_cast<std::size_t>(style)];
    evidence.gate("unoversampled_1x_negative_control",oneXFailed,"each mode/style must have at least one deliberately1x case above -50dBc");
    evidence.gate("mismatched_dry_reference_control",negativePass&&mismatchCases>0,"eligible cases with legitimate in-band harmonics must reject dry mask; cases="+std::to_string(mismatchCases));
}

void compensation(Evidence& evidence)
{
    auto rows=output(evidence.directory/"compensation.csv");
    rows<<"style,drive_db,baseline_rms,uncompensated_rms,compensated_rms,table_db,error_db,clamp_contact,pass\n";
    bool pass=true,valid=true;std::size_t contacts=0;
    for(int style=0;style<3;++style)
    {
        Candidate baseline;prepare(baseline,48000,evidence.factor,settings(0,style,0));
        const auto reference=summarizeSignal(render(baseline,48000,220,R/4,48000,1));
        valid=valid&&reference.valid&&reference.rms>0;
        for(int halfStep=0;halfStep<=72;++halfStep)
        {
            const double drive=halfStep*.5;Candidate raw,corrected;
            prepare(raw,48000,evidence.factor,settings(0,style,drive));
            prepare(corrected,48000,evidence.factor,settings(0,style,drive,true));
            const auto rawRms=summarizeSignal(render(raw,48000,220,R/4,48000,1));
            const auto correctedRms=summarizeSignal(render(corrected,48000,220,R/4,48000,1));
            const auto table=saturationDriveCompensationDb(static_cast<SaturationCharacter>(style),static_cast<float>(drive));
            const double error=20*std::log10(correctedRms.rms/reference.rms);
            const bool contact=table<=-18||table>=6;contacts+=contact?1:0;
            const bool accepted=rawRms.valid&&correctedRms.valid&&std::isfinite(error)&&std::abs(error)<=.5;
            valid=valid&&rawRms.valid&&correctedRms.valid&&std::isfinite(error);pass=pass&&accepted;
            rows<<styles[static_cast<std::size_t>(style)]<<','<<drive<<','<<reference.rms<<','<<rawRms.rms<<','<<correctedRms.rms<<','<<table<<','<<error<<','<<contact<<','<<accepted<<'\n';
        }
    }
    rows.close();
    evidence.gate("compensation_all_219_integer_half_steps",pass&&valid,"73 points/style; clamped accuracy retained; clamp contacts="+std::to_string(contacts),!valid);
}

void characters(Evidence& evidence)
{
    auto rows=output(evidence.directory/"character-harmonics.csv");
    rows<<"sample_rate,style,peak,fundamental_bin,mean,rms,even_h2_h4_dbc,harmonic_order,harmonic_dbc\n";
    std::array<double,3> requiredEven{};double requiredGlowMean=0;bool valid=true;
    for(double rate:rates) for(double level:levels) for(int style=0;style<3;++style)
    {
        Candidate candidate;prepare(candidate,rate,evidence.factor,settings(0,style,18));
        const auto samples=render(candidate,rate,rate*301/recordFrames,level,recordFrames,2);
        const auto summary=summarizeSignal(samples);const auto spectrum=rectangularPowerSpectrum(samples);
        const bool usable=spectrum.valid&&summary.valid&&spectrum.oneSidedPower[301]>0;
        valid=valid&&usable;if(!usable) continue;
        const auto fundamental=spectrum.oneSidedPower[301];
        const auto even=db(spectrum.oneSidedPower[602]+spectrum.oneSidedPower[1204],fundamental);
        for(std::size_t harmonic=1;harmonic*301<recordFrames/2;++harmonic)
            rows<<rate<<','<<styles[static_cast<std::size_t>(style)]<<','<<level<<",301,"<<summary.mean<<','<<summary.rms<<','<<even<<','<<harmonic<<','<<db(spectrum.oneSidedPower[harmonic*301],fundamental)<<'\n';
        if(rate==48000&&level==R/4){requiredEven[static_cast<std::size_t>(style)]=even;if(style==1) requiredGlowMean=std::abs(summary.mean);}
        evidence.raw("character-"+id(rate,0,style,18,level),samples,rate,static_cast<std::size_t>(2*rate),"coherent bin301;N65536;initial state zero","Full;Drive18;compOff;neutral;mix1");
    }
    rows.close();
    evidence.gate("character_spectrum_valid",valid,"all rates and R/16,R/4,R retained",!valid);
    evidence.gate("soft_symmetry",valid&&requiredEven[0]<=-100,"H2+H4 dBc="+std::to_string(requiredEven[0]),!valid);
    evidence.gate("glow_asymmetry",valid&&requiredEven[1]>=-60&&requiredEven[1]-requiredEven[0]>=20,"Glow/Soft even dBc="+std::to_string(requiredEven[1])+"/"+std::to_string(requiredEven[0]),!valid);
    evidence.gate("glow_dc",valid&&requiredGlowMean<=1e-5*R,"abs mean="+std::to_string(requiredGlowMean),!valid);
}

std::complex<double> response(std::span<const double> impulse,double frequency,double rate)
{
    std::complex<double> sum=0,phase=1;const auto step=std::polar(1.,-2*std::numbers::pi*frequency/rate);
    for(double sample:impulse){sum+=sample*phase;phase*=step;}return sum;
}
std::vector<double> impulse(double rate,unsigned factor,const SaturationSettings& value)
{
    Candidate candidate;prepare(candidate,rate,factor,value);
    for(std::size_t n=0;n<static_cast<std::size_t>(2*rate);++n) candidate.process({0,0});
    std::vector<double> result(recordFrames);
    for(std::size_t n=0;n<result.size();++n) result[n]=candidate.process({n==0?R:0,n==0?R:0})[0]/R;
    return result;
}
struct Biquad
{
    double b0=0,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    Biquad(double rate,double frequency,bool high)
    {
        const double k=std::tan(std::numbers::pi*frequency/rate),norm=1/(1+std::sqrt(2.)*k+k*k);
        b0=(high?1:k*k)*norm;b1=(high?-2:2)*b0;b2=b0;a1=2*(k*k-1)*norm;a2=(1-std::sqrt(2.)*k+k*k)*norm;
    }
    double process(double x){const auto y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
};
void neutral(Evidence& evidence)
{
    auto rows=output(evidence.directory/"neutral-frequency-group-delay.csv");
    rows<<"sample_rate,low_requested,high_requested,frequency_hz,full_raw_input_db,three_raw_input_db,crossover_only_db,full_phase_rad,three_phase_rad,full_group_delay_frames,three_group_delay_frames\n";
    auto nulls=output(evidence.directory/"neutral-null-layer-controls.csv");nulls<<"sample_rate,mode,low_requested,high_requested,max_wet_dry_null,max_identical_layer_error,max_mismatched_raw_dry_error\n";
    bool crossoverPass=true,combinedPass=true,nullPass=true,layerPass=true,mismatchPass=true,valid=true;
    for(double rate:rates) for(auto corners:{std::array<float,2>{180,3500},std::array<float,2>{2000,500},std::array<float,2>{40,16000}})
    {
        auto full=settings(0,0,0),three=settings(1,0,0);full.lowCrossoverHz=three.lowCrossoverHz=corners[0];full.highCrossoverHz=three.highCrossoverHz=corners[1];
        const auto fullImpulse=impulse(rate,evidence.factor,full),threeImpulse=impulse(rate,evidence.factor,three);
        evidence.raw("impulse-full-"+std::to_string(static_cast<int>(rate))+'-'+std::to_string(static_cast<int>(corners[0]))+'-'+std::to_string(static_cast<int>(corners[1])),fullImpulse,rate,static_cast<std::size_t>(2*rate),"unit impulse normalized from peakR","drive0;compOff;neutral");
        evidence.raw("impulse-three-"+std::to_string(static_cast<int>(rate))+'-'+std::to_string(static_cast<int>(corners[0]))+'-'+std::to_string(static_cast<int>(corners[1])),threeImpulse,rate,static_cast<std::size_t>(2*rate),"unit impulse normalized from peakR","drive0;compOff;neutral");
        const double top=std::min(18000.,.4*rate),inner=std::min(15000.,.35*rate);
        std::vector<double> frequencies{30,static_cast<double>(corners[0]),static_cast<double>(corners[1]),inner,top};
        for(int i=0;i<=160;++i) frequencies.push_back(30*std::pow(top/30,static_cast<double>(i)/160));
        for(double frequency:frequencies)
        {
            if(frequency<30||frequency>top) continue;
            const auto f=response(fullImpulse,frequency,rate),t=response(threeImpulse,frequency,rate);
            const auto fullDb=20*std::log10(std::abs(f)),threeDb=20*std::log10(std::abs(t)),crossDb=threeDb-fullDb;
            const auto fullDelay=-std::arg(response(fullImpulse,frequency+1,rate)/response(fullImpulse,frequency-1,rate))*rate/(4*std::numbers::pi);
            const auto threeDelay=-std::arg(response(threeImpulse,frequency+1,rate)/response(threeImpulse,frequency-1,rate))*rate/(4*std::numbers::pi);
            valid=valid&&std::isfinite(fullDb)&&std::isfinite(threeDb)&&std::isfinite(fullDelay)&&std::isfinite(threeDelay);
            crossoverPass=crossoverPass&&std::abs(crossDb)<=.1;
            const double bound=frequency<=inner+1e-8?.5:1.;combinedPass=combinedPass&&std::abs(fullDb)<=bound&&std::abs(threeDb)<=bound;
            rows<<rate<<','<<corners[0]<<','<<corners[1]<<','<<frequency<<','<<fullDb<<','<<threeDb<<','<<crossDb<<','<<std::arg(f)<<','<<std::arg(t)<<','<<fullDelay<<','<<threeDelay<<'\n';
        }
        for(int mode=0;mode<2;++mode)
        {
            auto wet=mode?three:full,dry=wet;dry.mix=0;
            Candidate a,b,reference;prepare(a,rate,evidence.factor,wet);prepare(b,rate,evidence.factor,wet);prepare(reference,rate,evidence.factor,dry);
            double maxNull=0,maxLayer=0,maxMismatch=0;
            const auto warm=static_cast<std::size_t>(2*rate);
            std::vector<double> witness(recordFrames);
            for(std::size_t n=0;n<warm+recordFrames;++n)
            {
                const double x=R/8*(std::sin(2*std::numbers::pi*220*n/rate)+std::sin(2*std::numbers::pi*7040*n/rate));
                const auto ya=a.process({x,x})[0],yb=b.process({x,x})[0],yd=reference.process({x,x})[0];
                if(n>=warm){maxNull=std::max(maxNull,std::abs(ya-yd));maxLayer=std::max(maxLayer,std::abs(ya+yb-2*yd));maxMismatch=std::max(maxMismatch,std::abs(ya-x));witness[n-warm]=ya+yb;}
            }
            nullPass=nullPass&&maxNull<=1e-10;layerPass=layerPass&&maxLayer<=2e-10;mismatchPass=mismatchPass&&maxMismatch>1e-7*R;
            nulls<<rate<<','<<mode<<','<<corners[0]<<','<<corners[1]<<','<<maxNull<<','<<maxLayer<<','<<maxMismatch<<'\n';
            evidence.raw("two-layer-"+std::to_string(static_cast<int>(rate))+'-'+std::to_string(mode)+'-'+std::to_string(static_cast<int>(corners[0])),witness,rate,warm,"220+7040Hz each peakR/8","two independently prepared identical neutral layers");
        }
    }
    // Independent uncompensated LR4 split: its missing upper allpass must fail unity.
    Biquad ll1(48000,400,false),ll2=ll1,lh1(48000,400,true),lh2=lh1,ml1(48000,500,false),ml2=ml1,hh1(48000,500,true),hh2=hh1;
    std::vector<double> naive(recordFrames);
    for(std::size_t n=0;n<naive.size();++n){const double x=n==0?1:0;const auto low=ll2.process(ll1.process(x)),upper=lh2.process(lh1.process(x));naive[n]=low+ml2.process(ml1.process(upper))+hh2.process(hh1.process(upper));}
    double worst=0;for(int i=0;i<=300;++i){const double hz=30*std::pow(18000./30,static_cast<double>(i)/300);worst=std::max(worst,std::abs(20*std::log10(std::abs(response(naive,hz,48000)))));}
    evidence.raw("negative-naive-uncompensated-LR4",naive,48000,0,"unit impulse","raw2000/500;effective400/500;low branch intentionally lacks upper allpass");
    rows.close();nulls.close();
    if(!evidence.cohort.empty()) evidence.observations<<"naive_split,,,,,,"<<worst<<'\n';
    evidence.gate("neutral_response_valid",valid,"raw input normalization; all AA and ADAA attenuation retained",!valid);
    evidence.gate("crossover_only_unity_0_1db",valid&&crossoverPass,"three/full ratio isolates crossover magnitude",!valid);
    evidence.gate("combined_neutral_raw_input_envelope",valid&&combinedPass,".5dB through min15k/.35Fs;1dB through min18k/.4Fs",!valid);
    evidence.gate("neutral_wet_reference_null",nullPass,"max<=1e-10 after equal settled preparation");
    evidence.gate("two_identical_layer_sum",layerPass,"sum vs twice independently prepared reference max<=2e-10");
    evidence.gate("mismatched_raw_dry_control",mismatchPass,"unmatched raw dry must not null");
    evidence.gate("naive_uncompensated_split_control",worst>.1,"raw2000/500;effective400/500;worst absolute dB="+std::to_string(worst));
}

void denseStateAndBursts(Evidence& evidence)
{
    auto recurrence=output(evidence.directory/"dense-recurrence.csv");
    recurrence<<"sample_rate,factor,phase,samples,retained_envelope,closed_form_envelope,worst_relative_error,min_sag_db,max_sag_db\n";
    bool recurrencePass=true,rangePass=true,historyPass=true,swapPass=true;
    for(double rate:rates) for(unsigned factor:{1u,2u,4u,8u,64u})
    {
        const double highRate=rate*factor;double envelope=0,worst=0,minDb=0,maxDb=-6;
        const auto attack=static_cast<std::size_t>(std::ceil(.01*highRate));
        for(std::size_t n=1;n<=attack;++n)
        {
            const auto gain=advanceSag(2,highRate,envelope);const auto gainDb=20*std::log10(gain);
            const auto expected=2*(1-std::exp(-static_cast<double>(n)/(highRate*.002)));
            worst=std::max(worst,std::abs(envelope-expected)/std::max(1.,expected));minDb=std::min(minDb,gainDb);maxDb=std::max(maxDb,gainDb);
        }
        recurrence<<rate<<','<<factor<<",attack,"<<attack<<','<<envelope<<','<<2*(1-std::exp(-static_cast<double>(attack)/(highRate*.002)))<<','<<worst<<','<<minDb<<','<<maxDb<<'\n';
        const double retained=envelope;const auto release=static_cast<std::size_t>(std::ceil(.4*highRate));
        for(std::size_t n=1;n<=release;++n)
        {
            const auto gain=advanceSag(0,highRate,envelope);const auto gainDb=20*std::log10(gain);
            const auto expected=retained*std::exp(-static_cast<double>(n)/(highRate*.080));
            worst=std::max(worst,std::abs(envelope-expected)/std::max(1.,expected));minDb=std::min(minDb,gainDb);maxDb=std::max(maxDb,gainDb);
        }
        recurrence<<rate<<','<<factor<<",release,"<<release<<','<<envelope<<','<<retained*std::exp(-static_cast<double>(release)/(highRate*.080))<<','<<worst<<','<<minDb<<','<<maxDb<<'\n';
        recurrencePass=recurrencePass&&worst<=1e-10;rangePass=rangePass&&minDb>=-6-1e-12&&maxDb<=1e-12;
    }
    auto histories=output(evidence.directory/"dense-history-and-link.csv");histories<<"sample_rate,first_20ms_max_difference,after_1s_max_difference,swap_max_error\n";
    for(double rate:rates)
    {
        Candidate hot,fresh,leftRight,rightLeft;const auto s=settings(0,2,36);
        prepare(hot,rate,evidence.factor,s);prepare(fresh,rate,evidence.factor,s);prepare(leftRight,rate,evidence.factor,s);prepare(rightLeft,rate,evidence.factor,s);
        for(std::size_t n=0;n<static_cast<std::size_t>(2*rate);++n){const double x=R*std::sin(2*std::numbers::pi*220*n/rate);hot.process({x,x});}
        const auto count=static_cast<std::size_t>(std::ceil(1.1*rate));std::vector<double> hotAudio(count),freshAudio(count),difference(count),linkedLeft(count),linkedRight(count);
        double first=0,last=0,swap=0;
        for(std::size_t n=0;n<count;++n)
        {
            const auto x=R/4*std::sin(2*std::numbers::pi*220*n/rate);hotAudio[n]=hot.process({x,x})[0];freshAudio[n]=fresh.process({x,x})[0];difference[n]=hotAudio[n]-freshAudio[n];
            if(n<static_cast<std::size_t>(.02*rate)) first=std::max(first,std::abs(difference[n]));
            if(n>=static_cast<std::size_t>(rate)) last=std::max(last,std::abs(difference[n]));
            const double l=R/8*std::sin(2*std::numbers::pi*220*n/rate),r=R/2*std::sin(2*std::numbers::pi*713*n/rate);
            const auto a=leftRight.process({l,r}),b=rightLeft.process({r,l});swap=std::max({swap,std::abs(a[0]-b[1]),std::abs(a[1]-b[0])});linkedLeft[n]=a[0];linkedRight[n]=a[1];
        }
        historyPass=historyPass&&first>1e-7*R&&last<=1e-5*R;swapPass=swapPass&&swap<=1e-12;
        histories<<rate<<','<<first<<','<<last<<','<<swap<<'\n';
        const auto suffix=std::to_string(static_cast<int>(rate));
        evidence.raw("dense-preconditioned-"+suffix,hotAudio,rate,static_cast<std::size_t>(2*rate),"prior2s220Hz peakR,then220Hz peakR/4","Dense;Drive36;compOff;initial state zero before precondition");
        evidence.raw("dense-reset-"+suffix,freshAudio,rate,0,"220Hz peakR/4","Dense;Drive36;compOff;reset");
        evidence.raw("dense-history-difference-"+suffix,difference,rate,0,"preconditioned minus reset","Dense;controlled history witness");
        evidence.raw("dense-linked-left-"+suffix,linkedLeft,rate,0,"L220Hz R/8;R713Hz R/2","Dense;Drive36;max stereo linking");
        evidence.raw("dense-linked-right-"+suffix,linkedRight,rate,0,"L220Hz R/8;R713Hz R/2","Dense;Drive36;max stereo linking");
    }
    auto burstRows=output(evidence.directory/"burst-segments.csv");burstRows<<"recording,sample_rate,style,level,segment,start_frame,frames,input_peak\n";
    for(double rate:rates) for(int style=0;style<3;++style) for(double level:levels)
    {
        Candidate candidate;prepare(candidate,rate,evidence.factor,settings(0,style,18));
        const std::array<double,7> seconds{2,.1,1,2,1,.1,1},peaks{0,level,0,R,0,level,0};
        const std::array<const char*,7> labels{"initial_silence","fresh_equal_burst","independent_recovery","sustained_drive","recovery_after_prior_drive","repeated_equal_burst","final_recovery"};
        const auto name="burst-"+id(rate,0,style,18,level);std::vector<double> audio;std::size_t frame=0;
        for(std::size_t segment=0;segment<seconds.size();++segment)
        {
            const auto count=static_cast<std::size_t>(std::llround(seconds[segment]*rate));burstRows<<quoted(name+".f64")<<','<<rate<<','<<styles[static_cast<std::size_t>(style)]<<','<<level<<','<<labels[segment]<<','<<frame<<','<<count<<','<<peaks[segment]<<'\n';
            for(std::size_t n=0;n<count;++n,++frame){const auto x=peaks[segment]*std::sin(2*std::numbers::pi*220*n/rate);audio.push_back(candidate.process({x,x})[0]);}
        }
        evidence.raw(name,audio,rate,0,"220Hz;segment manifest in burst-segments.csv;2s independent initial silence","Full;Drive18;compOff;zero initial state");
    }
    recurrence.close();histories.close();burstRows.close();
    evidence.gate("dense_1e_attack_release",recurrencePass,"2ms/80ms closed-form recurrence; all rates and factors1/2/4/8/64");
    evidence.gate("dense_sag_range",rangePass,"gain range [-6,0] dB");
    evidence.gate("dense_history_audio",historyPass,"first20ms >1e-7R; after1s <=1e-5R; retained audio");
    evidence.gate("dense_stereo_swap",swapPass,"max channel-swap error <=1e-12");
}

void operatingGrid(Evidence& evidence)
{
    auto rows=output(evidence.directory/"operating-grid.csv");
    rows<<"sample_rate,mode,style,drive,peak,corner_case,cuts_enabled,low_requested,high_requested,low_effective,high_effective,pre_cut_hz,post_cut_hz,warmup_frames,measured_frames,rms,mean,finite\n";
    bool valid=true;std::size_t cases=0;
    for(double rate:rates) for(int mode=0;mode<2;++mode) for(int style=0;style<3;++style) for(double drive:{0.,6.,18.,36.}) for(double level:levels) for(int corner=0;corner<3;++corner) for(int cuts=0;cuts<2;++cuts)
    {
        auto s=settings(mode,style,drive);
        constexpr std::array<std::array<float,2>,3> corners{{{180,3500},{2000,500},{40,16000}}};
        s.lowCrossoverHz=corners[static_cast<std::size_t>(corner)][0];s.highCrossoverHz=corners[static_cast<std::size_t>(corner)][1];
        s.preLowCutEnabled=s.postHighCutEnabled=cuts!=0;s.preLowCutHz=corner==2?2000.f:40.f;s.postHighCutHz=corner==2?1000.f:18000.f;
        Candidate candidate;prepare(candidate,rate,evidence.factor,s);const auto effective=effectiveSaturationCrossovers(s,rate);
        const auto samples=render(candidate,rate,rate*31/4096,level,4096,.25);const auto summary=summarizeSignal(samples);valid=valid&&summary.valid;
        rows<<rate<<','<<mode<<','<<styles[static_cast<std::size_t>(style)]<<','<<drive<<','<<level<<','<<corner<<','<<cuts<<','<<s.lowCrossoverHz<<','<<s.highCrossoverHz<<','<<effective.lowHz<<','<<effective.highHz<<','<<s.preLowCutHz<<','<<s.postHighCutHz<<','<<static_cast<std::size_t>(.25*rate)<<",4096,"<<summary.rms<<','<<summary.mean<<','<<summary.valid<<'\n';
        evidence.raw("grid-"+std::to_string(cases++),samples,rate,static_cast<std::size_t>(std::ceil(.25*rate)),"coherent bin31/N4096;peak="+std::to_string(level),id(rate,mode,style,drive,level)+";corner="+std::to_string(corner)+";cuts="+std::to_string(cuts));
    }
    rows.close();
    evidence.gate("operating_grid_finite",valid,"1296 raw records; .25s warm; measurement coverage, not a perceptual/loudness promise",!valid);
}

#if !defined(RACKSYNTH_SATURATION_QUALITY_ONLY)
SaturationSettings cpuSettings(std::size_t unit,int endpoint)
{
    SaturationSettings s;s.mode=SaturationMode::threeBand;s.driveCompensation=true;s.preLowCutEnabled=s.postHighCutEnabled=true;
    s.preLowCutHz=endpoint<0?20.f:endpoint==0?40.f:150.f;s.postHighCutHz=endpoint==0?9000.f:18000.f;
    s.inputTrimDb=endpoint<0?0.f:endpoint==0?-3.f:3.f;s.outputTrimDb=-s.inputTrimDb;s.mix=endpoint<0?1.f:endpoint==0?.4f:1.f;
    s.lowCrossoverHz=endpoint<0?180.f:endpoint==0?80.f:800.f;s.highCrossoverHz=endpoint<0?3500.f:endpoint==0?1800.f:6500.f;
    for(std::size_t lane=0;lane<3;++lane)
    {
        auto& value=s.lanes[lane+1];value.character=static_cast<SaturationCharacter>((unit+lane)%3);
        value.driveDb=endpoint<0?24.f:endpoint==0?6.f:30.f;value.preTiltDb=endpoint<0?0.f:endpoint==0?-6.f:6.f;
        value.mix=endpoint<0?.75f:endpoint==0?.35f:1.f;value.trimDb=endpoint<0?0.f:endpoint==0?-3.f:3.f;
    }
    return s;
}
float interpolate(float a,float b,double position) { return static_cast<float>(a+(b-a)*position); }
float interpolateHz(float a,float b,double position) { return static_cast<float>(std::exp(std::log(static_cast<double>(a))+(std::log(static_cast<double>(b))-std::log(static_cast<double>(a)))*position)); }
SaturationSettings cpuMoving(std::size_t unit,double elapsed)
{
    auto a=cpuSettings(unit,0);const auto b=cpuSettings(unit,1);const auto phase=std::fmod(elapsed/.020,2.);const auto t=phase<=1?phase:2-phase;
    a.inputTrimDb=interpolate(a.inputTrimDb,b.inputTrimDb,t);a.outputTrimDb=interpolate(a.outputTrimDb,b.outputTrimDb,t);a.mix=interpolate(a.mix,b.mix,t);
    a.lowCrossoverHz=interpolateHz(a.lowCrossoverHz,b.lowCrossoverHz,t);a.highCrossoverHz=interpolateHz(a.highCrossoverHz,b.highCrossoverHz,t);a.preLowCutHz=interpolateHz(a.preLowCutHz,b.preLowCutHz,t);a.postHighCutHz=interpolateHz(a.postHighCutHz,b.postHighCutHz,t);
    for(std::size_t lane=1;lane<4;++lane){auto& value=a.lanes[lane];const auto& end=b.lanes[lane];value.driveDb=interpolate(value.driveDb,end.driveDb,t);value.preTiltDb=interpolate(value.preTiltDb,end.preTiltDb,t);value.mix=interpolate(value.mix,end.mix,t);value.trimDb=interpolate(value.trimDb,end.trimDb,t);}
    return a;
}
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
struct CpuRecord
{
    std::string suite;double rate=0;std::size_t block=0;bool moving=false,valid=true,accepted=true,finite=true;
    std::array<double,2> audio{},wall{},energy{};std::array<std::size_t,2> callbacks{},samples{};
    std::array<std::uint64_t,2> checksum{};double mean=0,p99=0,maximum=0;std::size_t allocations=0,frees=0;
};
struct CpuOptions { bool describe=false;unsigned factor=0;fs::path directory; };
CpuOptions cpuParse(const std::vector<std::string>& args)
{
    CpuOptions result;bool mode=false,factor=false,outputFlag=false;
    for(std::size_t i=0;i<args.size();++i)
    {
        const auto& arg=args[i];
        if((arg=="--describe"||arg=="--measure")&&!mode){mode=true;result.describe=arg=="--describe";}
        else if(arg=="--factor"&&!factor&&i+1<args.size())
        {
            factor=true;const auto& value=args[++i];
            if(value!="2"&&value!="4"&&value!="8") throw std::runtime_error("factor must be 2,4,8");
            result.factor=static_cast<unsigned>(std::stoul(value));
        }
        else if(arg=="--output"&&!outputFlag&&i+1<args.size()){outputFlag=true;result.directory=args[++i];}
        else throw std::runtime_error("explicit --describe OR --measure --factor 2|4|8; --output FRESH_ABSOLUTE_DIRECTORY");
    }
    if(!mode||!outputFlag||result.directory.empty()||!result.directory.is_absolute()||fs::exists(result.directory)||
       (result.describe?factor:!factor)) throw std::runtime_error("invalid mode/factor or output is not a fresh absolute directory");
    return result;
}
template<class Close> void cpuComplete(std::ostream& stream,Close close)
{
    stream.flush();if(!stream) throw std::runtime_error("CPU manifest write failure");
    close();if(!stream) throw std::runtime_error("CPU manifest close failure");
}
void cpuWrite(const fs::path& path,const std::string& bytes)
{
    auto stream=output(path,true);stream<<bytes;cpuComplete(stream,[&]{stream.close();});
}
SaturationSettings cpuTarget(std::size_t unit,bool moving,std::size_t rendered,double rate,bool overlap)
{
    const double elapsed=static_cast<double>(rendered)/rate;
    auto value=moving?cpuMoving(unit,elapsed):cpuSettings(unit,-1);
    if(overlap)
    {
        const auto framesPer20ms=static_cast<std::size_t>(rate)/50;
        const auto rotation=(rendered/framesPer20ms)%3;
        for(std::size_t lane=1;lane<4;++lane) value.lanes[lane].character=static_cast<SaturationCharacter>((unit+lane-1+rotation)%3);
    }
    return value;
}
bool cpuScheduleCheck()
{
    for(std::size_t frame:{27776u,27840u,27904u})
    {
        const auto expected=frame==27776?28u:29u;
        if(static_cast<unsigned>(cpuTarget(0,false,frame,48000,true).lanes[1].character)!=expected%3) return false;
    }
    for(std::size_t framesPer20ms:{882u,960u,1920u}) for(std::size_t block:{64u,512u}) for(bool moving:{false,true})
    {
        const double rate=static_cast<double>(framesPer20ms*50);
        for(std::size_t boundary=1;boundary<=600;++boundary)
        {
            const auto nominal=boundary*framesPer20ms,delivery=((nominal+block-1)/block)*block;
            if(delivery<nominal||delivery-nominal>=block) return false;
            for(std::size_t frame:{delivery-block,delivery}) for(std::size_t unit=0;unit<8;++unit)
            {
                const auto expected=frame==delivery?boundary:boundary-1;
                const double elapsed=static_cast<double>(frame)/rate;
                const auto base=moving?cpuMoving(unit,elapsed):cpuSettings(unit,-1),overlap=cpuTarget(unit,moving,frame,rate,true);auto restored=overlap;
                for(std::size_t lane=1;lane<4;++lane)
                {
                    if(static_cast<unsigned>(overlap.lanes[lane].character)!=(unit+lane-1+expected)%3) return false;
                    restored.lanes[lane].character=base.lanes[lane].character;
                }
                if(restored!=base) return false;
            }
        }
    }
    return true;
}
int cpuOutcome(const std::vector<CpuRecord>& records)
{
    bool invalid=records.size()!=24,pass=true;
    for(const char* suite:{"base","structural_overlap"}) for(double rate:rates) for(std::size_t block:{64u,512u}) for(bool moving:{false,true})
        invalid=invalid||std::count_if(records.begin(),records.end(),[&](const auto& row){return row.suite==suite&&row.rate==rate&&row.block==block&&row.moving==moving;})!=1;
    for(const auto& row:records)
    {
        const double period=static_cast<double>(row.block)/row.rate;
        invalid=invalid||!row.valid||!row.accepted||!row.finite||!std::isfinite(row.mean)||!std::isfinite(row.p99)||!std::isfinite(row.maximum)||
            row.mean<=0||row.p99<=0||row.maximum<=0||row.mean>row.maximum||row.p99>row.maximum;
        for(std::size_t phase=0;phase<2;++phase)
        {
            const double minimum=phase?10:2;
            invalid=invalid||!std::isfinite(row.audio[phase])||!std::isfinite(row.wall[phase])||!std::isfinite(row.energy[phase])||
                row.audio[phase]<minimum||row.wall[phase]<minimum||row.energy[phase]<=0||!row.callbacks[phase]||
                row.samples[phase]!=row.callbacks[phase]*row.block*2||std::abs(row.audio[phase]-static_cast<double>(row.callbacks[phase]*row.block)/row.rate)>1e-10;
        }
        pass=pass&&row.mean<=.35*period&&row.p99<=.5*period&&row.allocations==0&&row.frees==0;
    }
    return invalid?2:pass?0:1;
}
std::string cpuDecision(const std::vector<CpuRecord>& records,bool synthetic,bool described,unsigned factor=0,const std::string& manifestHash={})
{
    const int code=described?0:cpuOutcome(records);std::ostringstream receipt;
    receipt<<"{\"schema\":1,\"scope\":\"cpu_preparation\",\"synthetic\":"<<(synthetic?"true":"false")
        <<",\"actual_measurement\":"<<quoted(synthetic||described?"NOT_RUN":"EXECUTED")
        <<",\"cpu_validation\":"<<quoted(synthetic||described?"DEFERRED":"CORE_ONLY")
        <<",\"status\":"<<quoted(described?"prepared":code==2?"invalid":code==1?"rejected":synthetic?"synthetic_pass_no_actual_measurement":"core_pass_provisional")
        <<",\"workload_sha256\":"<<(manifestHash.empty()?"null":quoted(manifestHash))<<",\"diagnostic_factor\":"<<(factor?std::to_string(factor):"null")<<",\"exit_code\":"<<code<<",\"selected_factor\":null,\"adoptable\":false,\"production\":\"FORCED_BYPASS\",\"R4_R7\":\"OPEN\",";
    if(synthetic||described) receipt<<"\"not_run\":{\"core_cpu_mean_and_p99\":\"NOT_RUN\",\"callback_cpp_heap\":\"NOT_RUN\",\"core_cpu_output_integrity\":\"NOT_RUN\",\"base12\":\"NOT_RUN\",\"structural_overlap12\":\"NOT_RUN\"},";
    receipt<<"\"required_members\":24,\"observed_members\":"<<records.size()
        <<",\"limits\":[\"both base12 and structural_overlap12 required for future core qualification\",\"maximum product contention and product .75/1 period gates remain unproved\",\"quality adoption remains independently blocked; no selected factor\",\"global C++ new/delete witness does not intercept direct C malloc/free\"]}\n";
    return receipt.str();
}
// CPU description begins
void cpuSettingJson(std::ostream& manifest,std::size_t unit,int endpoint)
{
    const auto value=cpuSettings(unit,endpoint);
    manifest<<"{\"unit\":"<<unit<<",\"endpoint\":"<<quoted(endpoint<0?"fixed":endpoint==0?"A":"B")
        <<",\"mode\":\"threeBand\",\"drive_compensation\":true,\"pre_low_cut_enabled\":true,\"post_high_cut_enabled\":true"
        <<",\"input_trim_db\":"<<value.inputTrimDb<<",\"output_trim_db\":"<<value.outputTrimDb<<",\"mix\":"<<value.mix
        <<",\"low_crossover_hz\":"<<value.lowCrossoverHz<<",\"high_crossover_hz\":"<<value.highCrossoverHz
        <<",\"pre_low_cut_hz\":"<<value.preLowCutHz<<",\"post_high_cut_hz\":"<<value.postHighCutHz<<",\"lanes\":[";
    for(std::size_t lane=0;lane<4;++lane)
    {
        if(lane) manifest<<',';const auto& s=value.lanes[lane];
        manifest<<"{\"lane\":"<<lane<<",\"active\":"<<(lane?"true":"false")<<",\"bypassed\":"<<(s.bypassed?"true":"false")
            <<",\"character\":"<<quoted(styles[static_cast<std::size_t>(s.character)])<<",\"drive_db\":"<<s.driveDb
            <<",\"pre_tilt_db\":"<<s.preTiltDb<<",\"mix\":"<<s.mix<<",\"trim_db\":"<<s.trimDb<<'}';
    }
    manifest<<"]}";
}
std::string cpuManifest(const fs::path& image)
{
    std::ostringstream manifest;manifest<<std::setprecision(17);
    manifest<<"{\"schema\":1,\"scope\":\"transition_cpu_preparation\",\"supported_factors\":[2,4,8],\"cpu_validation\":\"DEFERRED\",\"selected_factor\":null,\"adoptable\":false,"
        <<"\"image\":{\"file\":"<<quoted(image.filename().string())<<",\"sha256\":"<<quoted(hash(read(image)))<<"},"
        <<"\"units\":8,\"serial\":true,\"channels\":2,\"active_lanes\":24,\"styles\":[\"soft\",\"glow\",\"dense\"],\"each_style_lanes\":8,"
        <<"\"warm_min_audio_seconds\":2,\"warm_min_wall_seconds\":2,\"measure_min_audio_seconds\":10,\"measure_min_wall_seconds\":10,"
        <<"\"statistics\":\"measured callback arithmetic mean; nearest-rank p99; maximum; period=block/rate\","
        <<"\"gates\":{\"core_mean_period\":0.35,\"core_p99_period\":0.5,\"allocations\":0,\"frees\":0,\"future_product_mean_period\":0.75,\"future_product_p99_period\":1},"
        <<"\"trajectory\":\"phase=fmod(elapsed/.020,2);t=phase<=1?phase:2-phase;A+(B-A)*t for dB/mix;exp(log(A)+(log(B)-log(A))*t) for Hz;float cast; elapsed=rendered/rate, continuous across warm/measure\","
        <<"\"input\":\"native stereo .01*sin(2*pi*220*time+offset)+.005*sin(2*pi*1760*time+offset);time=(rendered+n)/rate;offset=L0/Rpi/3; continuous across warm/measure\","
        <<"\"overlap\":\"additive suite only: all Low/Mid/High characters in all eight units rotate together by (rendered/framesPer20ms)%3 using integer frames; framesPer20ms=882/960/1920 at44100/48000/96000Hz; original fixed and A/B continuous controls retained; setSettings every callback; nominal 20ms events quantized to first callback start at/after boundary (delay < block/rate); blocks below20ms at all declared rates\","
        <<"\"callback_timer\":\"starts before all setSettings; includes both transition endpoints and every serial8-unit stereo output write; ends after processing; input generation and output integrity scan outside timer\","
        <<"\"output_integrity\":\"every stereo double stored in preallocated block; warm/measured finite positive energy; samples=callbacks*block*2; separate FNV1a64 offset14695981039346656037 prime1099511628211 modulo2^64 little-endian8bytes/frame/channel\","
        <<"\"heap_scope\":\"all global C++ new/delete arrays/aligned inside callback; direct C malloc/free requires source audit\","
        <<"\"schedule_self_check\":{\"pass\":"<<(cpuScheduleCheck()?"true":"false")<<",\"slots\":8,\"active_lanes_per_slot\":3,\"observed_boundaries_per_case\":600,\"cases\":12,\"callback_observations_per_boundary\":2,\"focused_regression_callbacks\":[27776,27840,27904],\"expectation\":\"delivery=ceil(event_index*framesPer20ms/block)*block; previous callback expects event_index-1; delivery callback expects event_index; non-character controls exact\"},\"cases\":[";
    bool comma=false;
    for(const char* suite:{"base","structural_overlap"}) for(double rate:rates) for(std::size_t block:{64u,512u}) for(bool moving:{false,true})
    {
        if(comma) manifest<<',';comma=true;
        manifest<<"{\"suite\":"<<quoted(suite)<<",\"rate\":"<<rate<<",\"block\":"<<block<<",\"moving\":"<<(moving?"true":"false")
            <<",\"timing\":\"NOT_RUN\",\"heap\":\"NOT_RUN\",\"output_integrity\":\"NOT_RUN\"}";
    }
    manifest<<"],\"settings\":[";comma=false;
    for(std::size_t unit=0;unit<8;++unit) for(int endpoint:{-1,0,1}){if(comma) manifest<<',';comma=true;cpuSettingJson(manifest,unit,endpoint);}
    manifest<<"],\"sources\":[";comma=false;const fs::path source=fs::path(__FILE__).parent_path();
    for(const auto* name:{"SaturationFeasibility.cpp","SaturationCandidate.cpp","SaturationCandidate.h","SaturationCalibration.h","SaturationMath.h","SaturationGain.h","SaturationWideNetwork.cpp","SaturationWideNetwork.h","SaturationAnalysis.cpp","SaturationAnalysis.h"})
    {
        if(comma) manifest<<',';comma=true;manifest<<"{\"file\":"<<quoted(name)<<",\"sha256\":"<<quoted(hash(read(source/name)))<<'}';
    }
    manifest<<",{\"file\":\"CMakeLists.txt\",\"sha256\":"<<quoted(hash(read(source.parent_path().parent_path()/"CMakeLists.txt")))<<"}],\"coefficients\":[";comma=false;
    const auto coefficients=saturationCoefficientManifest();if(coefficients.empty()) throw std::runtime_error("empty CPU coefficient manifest");
    for(const auto& coefficient:coefficients)
    {
        if(!coefficient.name||!coefficient.name[0]||coefficient.values.empty()||!std::all_of(coefficient.values.begin(),coefficient.values.end(),[](double v){return std::isfinite(v);})) throw std::runtime_error("invalid CPU coefficient manifest");
        if(comma) manifest<<',';comma=true;manifest<<"{\"name\":"<<quoted(coefficient.name)<<",\"sha256\":"<<quoted(hash(coefficient.values))<<",\"values\":[";
        for(std::size_t n=0;n<coefficient.values.size();++n){if(n) manifest<<',';manifest<<coefficient.values[n];}manifest<<"]}";
    }
    manifest<<"],\"compensation_tables\":[";
    for(std::size_t style=0;style<3;++style)
    {
        const auto table=saturationCompensationTable(static_cast<SaturationCharacter>(style));
        if(table.size()!=37||!std::all_of(table.begin(),table.end(),[](double v){return std::isfinite(v);})) throw std::runtime_error("invalid compensation table");
        if(style) manifest<<',';manifest<<"{\"style\":"<<quoted(styles[style])<<",\"sha256\":"<<quoted(hash(table))<<",\"values\":[";
        for(std::size_t n=0;n<table.size();++n){if(n) manifest<<',';manifest<<table[n];}manifest<<"]}";
    }
    manifest<<"]}\n";return manifest.str();
}
void cpuDescribe(const fs::path& directory,const fs::path& image)
{
    const auto manifest=cpuManifest(image);if(!cpuScheduleCheck()) throw std::runtime_error("CPU schedule self-check failed");
    fs::create_directories(directory);cpuWrite(directory/"workload.json",manifest);cpuWrite(directory/"decision.json",cpuDecision({},false,true,0,hash(manifest)));
}
// CPU description ends
#endif
void settingRow(std::ostream& rows,const char* label,std::size_t unit,const SaturationSettings& s)
{
    for(std::size_t lane=1;lane<4;++lane) rows<<label<<','<<unit<<','<<lane<<','<<static_cast<int>(s.lanes[lane].character)<<','<<s.inputTrimDb<<','<<s.outputTrimDb<<','<<s.mix<<','<<s.lowCrossoverHz<<','<<s.highCrossoverHz<<','<<s.preLowCutHz<<','<<s.postHighCutHz<<','<<s.lanes[lane].driveDb<<','<<s.lanes[lane].preTiltDb<<','<<s.lanes[lane].mix<<','<<s.lanes[lane].trimDb<<'\n';
}
struct Callback { double seconds;std::size_t allocations,frees; };
volatile double timingSink=0;
void checksumSample(std::uint64_t& checksum,double sample) noexcept
{
    auto bits=std::bit_cast<std::uint64_t>(sample);
    for(int byte=0;byte<8;++byte){checksum^=bits&255;checksum*=1099511628211ULL;bits>>=8;}
}
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
void timings(Evidence& evidence,bool overlap,std::vector<CpuRecord>& records)
#else
void timings(Evidence& evidence)
#endif
{
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
    const std::string suite=overlap?"structural_overlap":"base";
    auto rows=output(evidence.directory/(suite+"-timing-summary.csv"));
#else
    auto rows=output(evidence.directory/"timing-summary.csv");
#endif
    rows<<"sample_rate,block,moving,warm_audio_seconds,warm_wall_seconds,measured_audio_seconds,measured_wall_seconds,callbacks,mean_seconds,p99_seconds,max_seconds,period_seconds,mean_fraction,p99_fraction,allocations,frees,warm_output_energy,measured_output_energy,output_finite,output_nonzero,warm_output_fnv1a64,measured_output_fnv1a64,pass\n";
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
    auto settingRows=output(evidence.directory/(suite+"-timing-settings.csv"));settingRows<<"label,unit,lane,character,input_db,output_db,global_mix,low_hz,high_hz,precut_hz,postcut_hz,drive_db,tilt_db,lane_mix,lane_trim_db\n";
#else
    auto settingRows=output(evidence.directory/"timing-settings.csv");settingRows<<"label,unit,lane,character,input_db,output_db,global_mix,low_hz,high_hz,precut_hz,postcut_hz,drive_db,tilt_db,lane_mix,lane_trim_db\n";
#endif
    for(std::size_t unit=0;unit<8;++unit){settingRow(settingRows,"fixed",unit,cpuSettings(unit,-1));settingRow(settingRows,"A",unit,cpuSettings(unit,0));settingRow(settingRows,"B",unit,cpuSettings(unit,1));}
    bool allPass=true,zeroHeap=true,allValid=true,allOutputFinite=true,allOutputNonzero=true;
    for(double rate:rates) for(std::size_t block:{64u,512u}) for(bool moving:{false,true})
    {
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
        const auto name=suite+"-timings-"+std::to_string(static_cast<int>(rate))+'-'+std::to_string(block)+'-'+std::to_string(moving);
#else
        const auto name="timings-"+std::to_string(static_cast<int>(rate))+'-'+std::to_string(block)+'-'+std::to_string(moving);
#endif
        std::array<std::unique_ptr<Candidate>,8> units;
        for(std::size_t unit=0;unit<units.size();++unit){units[unit]=std::make_unique<Candidate>();prepare(*units[unit],rate,evidence.factor,cpuSettings(unit,moving?0:-1));}
        std::vector<Candidate::Frame> input(block),actualOutput(block);std::array<std::vector<Callback>,2> callbacks;std::array<double,2> audioSeconds{},wallSeconds{};std::array<long double,2> outputEnergy{};std::array<bool,2> outputFinite{true,true};std::array<std::uint64_t,2> checksums{14695981039346656037ULL,14695981039346656037ULL};std::size_t rendered=0;bool acceptedSettings=true;
        for(int phase=0;phase<2;++phase)
        {
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
            for(std::size_t unit=0;unit<units.size();++unit) settingRow(settingRows,(name+(phase?"-measure-first-target":"-warm-prepared-state")).c_str(),unit,cpuTarget(unit,moving,rendered,rate,overlap));
#else
            for(std::size_t unit=0;unit<units.size();++unit) settingRow(settingRows,(name+(phase?"-measure-first-target":"-warm-prepared-state")).c_str(),unit,moving?cpuMoving(unit,static_cast<double>(rendered)/rate):cpuSettings(unit,-1));
#endif
            callbacks[static_cast<std::size_t>(phase)].reserve(static_cast<std::size_t>(rate*(phase?10:2)/static_cast<double>(block))*4+1);
            const auto started=Clock::now();const auto firstFrame=rendered;const double minimum=phase?10:2;
            do
            {
                for(std::size_t n=0;n<block;++n) for(std::size_t channel=0;channel<2;++channel)
                {
                    const double time=static_cast<double>(rendered+n)/rate,offset=channel?std::numbers::pi/3:0;
                    input[n][channel]=.01*std::sin(2*std::numbers::pi*220*time+offset)+.005*std::sin(2*std::numbers::pi*1760*time+offset);
                }
                allocationWitness::allocations=allocationWitness::frees=0;
                const auto before=Clock::now();allocationWitness::enabled=true;
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
                if(overlap) for(std::size_t unit=0;unit<units.size();++unit) acceptedSettings=units[unit]->setSettings(cpuTarget(unit,moving,rendered,rate,true))&&acceptedSettings;
                else if(moving) for(std::size_t unit=0;unit<units.size();++unit) acceptedSettings=units[unit]->setSettings(cpuMoving(unit,static_cast<double>(rendered)/rate))&&acceptedSettings;
#else
                if(moving) for(std::size_t unit=0;unit<units.size();++unit) acceptedSettings=units[unit]->setSettings(cpuMoving(unit,static_cast<double>(rendered)/rate))&&acceptedSettings;
#endif
                std::size_t outputIndex=0;
                for(auto value:input){for(auto& unit:units) value=unit->process(value);actualOutput[outputIndex++]=value;}
                allocationWitness::enabled=false;const auto after=Clock::now();timingSink=actualOutput.back()[0]+actualOutput.back()[1];
                for(const auto& frame:actualOutput) for(double sample:frame)
                {
                    checksumSample(checksums[static_cast<std::size_t>(phase)],sample);
                    if(std::isfinite(sample)) outputEnergy[static_cast<std::size_t>(phase)]+=static_cast<long double>(sample)*sample;
                    else outputFinite[static_cast<std::size_t>(phase)]=false;
                }
                callbacks[static_cast<std::size_t>(phase)].push_back({std::chrono::duration<double>(after-before).count(),allocationWitness::allocations,allocationWitness::frees});rendered+=block;
                audioSeconds[static_cast<std::size_t>(phase)]=static_cast<double>(rendered-firstFrame)/rate;wallSeconds[static_cast<std::size_t>(phase)]=std::chrono::duration<double>(Clock::now()-started).count();
            } while(audioSeconds[static_cast<std::size_t>(phase)]<minimum||wallSeconds[static_cast<std::size_t>(phase)]<minimum);
        }
        auto raw=output(evidence.directory/(name+".csv"));raw<<"phase,index,duration_seconds,allocations,frees\n";std::vector<double> measured;std::size_t allocations=0,frees=0;
        for(std::size_t phase=0;phase<2;++phase) for(std::size_t i=0;i<callbacks[phase].size();++i)
        {
            const auto& c=callbacks[phase][i];raw<<(phase?"measure":"warm")<<','<<i<<','<<c.seconds<<','<<c.allocations<<','<<c.frees<<'\n';allocations+=c.allocations;frees+=c.frees;if(phase) measured.push_back(c.seconds);
        }
        const auto summary=summarizeTiming(measured);const double period=static_cast<double>(block)/rate;
        const bool finite=outputFinite[0]&&outputFinite[1]&&std::isfinite(outputEnergy[0])&&std::isfinite(outputEnergy[1]),nonzero=outputEnergy[0]>0&&outputEnergy[1]>0;
        const bool pass=summary.valid&&acceptedSettings&&finite&&nonzero&&summary.meanSeconds<=.35*period&&summary.p99Seconds<=.5*period&&allocations==0&&frees==0;
        allPass=allPass&&pass;allValid=allValid&&summary.valid&&acceptedSettings&&finite;zeroHeap=zeroHeap&&allocations==0&&frees==0;allOutputFinite=allOutputFinite&&finite;allOutputNonzero=allOutputNonzero&&nonzero;
        rows<<rate<<','<<block<<','<<moving<<','<<audioSeconds[0]<<','<<wallSeconds[0]<<','<<audioSeconds[1]<<','<<wallSeconds[1]<<','<<summary.count<<','<<summary.meanSeconds<<','<<summary.p99Seconds<<','<<summary.maximumSeconds<<','<<period<<','<<summary.meanSeconds/period<<','<<summary.p99Seconds/period<<','<<allocations<<','<<frees<<','<<outputEnergy[0]<<','<<outputEnergy[1]<<','<<finite<<','<<nonzero<<','<<checksums[0]<<','<<checksums[1]<<','<<pass<<'\n';
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
        for(std::size_t unit=0;unit<units.size();++unit) settingRow(settingRows,(name+"-final-target").c_str(),unit,cpuTarget(unit,moving,rendered-block,rate,overlap));
        CpuRecord record;record.suite=suite;record.rate=rate;record.block=block;record.moving=moving;record.valid=summary.valid;record.accepted=acceptedSettings;record.finite=finite;
        record.audio=audioSeconds;record.wall=wallSeconds;record.mean=summary.meanSeconds;record.p99=summary.p99Seconds;record.maximum=summary.maximumSeconds;record.allocations=allocations;record.frees=frees;record.checksum=checksums;
        for(std::size_t phase=0;phase<2;++phase){record.energy[phase]=static_cast<double>(outputEnergy[phase]);record.callbacks[phase]=callbacks[phase].size();record.samples[phase]=callbacks[phase].size()*block*2;}records.push_back(record);raw.close();
#else
        for(std::size_t unit=0;unit<units.size();++unit) settingRow(settingRows,(name+"-final-target").c_str(),unit,moving?cpuMoving(unit,static_cast<double>(rendered-block)/rate):cpuSettings(unit,-1));
#endif
        std::cout<<name<<": mean="<<summary.meanSeconds/period<<" p99="<<summary.p99Seconds/period<<" period, heap="<<allocations<<'/'<<frees<<std::endl;
    }
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
    evidence.gate(suite+"_core_cpu_mean_and_p99",allPass,"8serial stereo three-band mixed styles; mean<=.35period;p99<=.5period;both audio/wall minima",!allValid);
    evidence.gate(suite+"_callback_cpp_heap",zeroHeap,"all global C++ new/delete incl arrays/aligned; C allocation freedom also requires core source audit");
    evidence.gate(suite+"_core_cpu_output_integrity",allOutputFinite&&allOutputNonzero,"preallocated output block; finite and positive energy in warm/measured phases; exact FNV1a64 over every stereo double in frame/channel order",!allOutputFinite);
    rows.close();settingRows.close();
#else
    evidence.gate("core_cpu_mean_and_p99",allPass,"8serial stereo three-band mixed styles; mean<=.35period;p99<=.5period;both audio/wall minima",!allValid);
    evidence.gate("callback_cpp_heap",zeroHeap,"all global C++ new/delete incl arrays/aligned; C allocation freedom also requires core source audit");
    evidence.gate("core_cpu_output_integrity",allOutputFinite&&allOutputNonzero,"preallocated output block; finite and positive energy in warm/measured phases; exact FNV1a64 over every stereo double in frame/channel order",!allOutputFinite);
#endif
}

void finish(Evidence& evidence)
{
    bool pass=true,invalid=false;for(const auto& gate:evidence.gates){pass=pass&&gate.pass;invalid=invalid||gate.invalid;}
    evidence.recordings.flush();
    auto receipt=output(evidence.directory/"decision.json");
    receipt<<"{\n\"schema\":1,\"factor\":"<<evidence.factor<<",\"status\":"<<quoted(invalid?"invalid":pass?"passing_candidate":"rejected")<<",\"all_gates_pass\":"<<(pass?"true":"false")<<",\"frozen_manifest_sha256\":"<<quoted(hash(read(evidence.directory/"frozen-manifest.json")))<<",\"selection\":\"parent must select the lowest passing factor after all factor receipts; provisional on Task2 structural overlap and Task7 whole-product contention\",\"routing\":\"inherited parent model; no overrides\",\"gates\":[\n";
    for(std::size_t i=0;i<evidence.gates.size();++i){const auto& gate=evidence.gates[i];if(i) receipt<<",\n";receipt<<"{\"name\":"<<quoted(gate.name)<<",\"pass\":"<<(gate.pass?"true":"false")<<",\"invalid\":"<<(gate.invalid?"true":"false")<<",\"detail\":"<<quoted(gate.detail)<<'}';}
    receipt<<"],\"limits\":[\"core timing is the frozen representative mixed-style workload, not maximum production overlap\",\"global C++ allocation instrumentation does not intercept direct malloc/free; audit candidate source\",\"operating-grid RMS is measured at declared .25s warmup, not promised perceptual equivalence\",\"no DAW or listening acceptance claimed\"]}\n";
    receipt.close();
    auto hashes=output(evidence.directory/"artifact-hashes.csv");hashes<<"file,sha256,bytes\n";
    std::vector<fs::path> paths;for(const auto& entry:fs::directory_iterator(evidence.directory)) if(entry.is_regular_file()&&entry.path().filename()!="artifact-hashes.csv") paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());for(const auto& path:paths){const auto bytes=read(path);hashes<<quoted(path.filename().string())<<','<<hash(bytes)<<','<<bytes.size()<<'\n';}
    std::cout<<"factor "<<evidence.factor<<" decision: "<<(invalid?"INVALID":pass?"PASS":"REJECT")<<std::endl;
}
#else
int finishCohort(Evidence& evidence)
{
    evidence.recordings.close();evidence.observations.close();
    if(evidence.frozenSources!=cohortSources()) throw std::runtime_error("source closure changed during cohort capture");
    const fs::path image=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toWideCharPointer();
    const auto imageBytes=read(image);auto receipt=output(evidence.directory/"cohort.json");
    receipt<<"{\n\"schema\":\"saturation-quality-cohort-v1\",\"status\":\"INCOMPLETE_QUALITY\",\"cohort\":"<<quoted(evidence.cohort)
        <<",\"consumer\":"<<quoted(image.stem().string())<<",\"configuration\":"<<quoted(CMAKE_INTDIR)
        <<",\"diagnostic_factor\":"<<evidence.factor<<",\"image\":{\"bytes\":"<<imageBytes.size()<<",\"sha256\":"<<quoted(hash(imageBytes))
        <<"},\"sources\":"<<evidence.frozenSources<<",\"frozen_manifest_sha256\":"<<quoted(hash(read(evidence.directory/"frozen-manifest.json")))
        <<",\"selected_factor\":null,\"adoptable\":false,\"production\":\"FORCED_BYPASS\",\"cpu_validation\":\"DEFERRED\",\"member_keys\":[";
    if(evidence.cohort=="nonalias") receipt<<"\"calibration\",\"neutral\",\"compensation\",\"characters\",\"denseStateAndBursts\",\"operatingGrid\"";
    else for(std::size_t i=0;i<evidence.memberKeys.size();++i){if(i) receipt<<',';receipt<<evidence.memberKeys[i];}
    receipt<<"],\"gates\":[";
    for(std::size_t i=0;i<evidence.gates.size();++i)
    {
        const auto& gate=evidence.gates[i];if(i) receipt<<',';
        receipt<<"{\"name\":"<<quoted(gate.name)<<",\"pass\":"<<(gate.pass?"true":"false")<<",\"invalid\":"<<(gate.invalid?"true":"false")<<",\"detail\":"<<quoted(gate.detail)<<'}';
    }
    receipt<<"],\"limits\":[\"cohort gates are local observations; exact four- or six-cohort finalization is required\",\"CPU DEFERRED; no factor selection, adoption, DAW or listening acceptance\"]}\n";receipt.close();
    auto hashes=output(evidence.directory/"artifact-hashes.csv");hashes<<"file,sha256,bytes\n";
    std::vector<fs::path> paths;for(const auto& entry:fs::directory_iterator(evidence.directory)) if(entry.is_regular_file()&&entry.path().filename()!="artifact-hashes.csv") paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());for(const auto& path:paths){const auto bytes=read(path);hashes<<quoted(path.filename().string())<<','<<hash(bytes)<<','<<bytes.size()<<'\n';}
    hashes.close();std::cout<<"cohort "<<evidence.cohort<<": INCOMPLETE_QUALITY; CPU DEFERRED; production FORCED_BYPASS\n";return 0;
}
int finish(Evidence& evidence)
{
    constexpr std::array<const char*,22> expected{
        "analysis_calibration","neutral_response_valid","crossover_only_unity_0_1db",
        "combined_neutral_raw_input_envelope","neutral_wet_reference_null","two_identical_layer_sum",
        "mismatched_raw_dry_control","naive_uncompensated_split_control","compensation_all_219_integer_half_steps",
        "character_spectrum_valid","soft_symmetry","glow_asymmetry","glow_dc","dense_1e_attack_release",
        "dense_sag_range","dense_history_audio","dense_stereo_swap","operating_grid_finite",
        "alias_reference_qualification","alias_candidate_and_improvement","unoversampled_1x_negative_control",
        "mismatched_dry_reference_control"};
    bool pass=true,invalid=evidence.gates.size()!=expected.size();
    for(const auto* name:expected)
        invalid=invalid||std::count_if(evidence.gates.begin(),evidence.gates.end(),[&](const auto& gate){return gate.name==name;})!=1;
    for(const auto& gate:evidence.gates){pass=pass&&gate.pass;invalid=invalid||gate.invalid;}
    evidence.recordings.close();
    auto receipt=output(evidence.directory/"decision.json");
    receipt<<"{\n\"schema\":1,\"scope\":\"quality_only\",\"diagnostic_factor\":"<<evidence.factor
        <<",\"status\":"<<quoted(invalid?"invalid":pass?"quality_pass":"quality_rejected")
        <<",\"quality_gates_pass\":"<<(!invalid&&pass?"true":"false")
        <<",\"expected_quality_gates\":22,\"selected_factor\":null,\"adoptable\":false,\"production\":\"FORCED_BYPASS\",\"cpu_validation\":\"DEFERRED\","
        <<"\"not_run\":{\"core_cpu_mean_and_p99\":\"NOT_RUN\",\"callback_cpp_heap\":\"NOT_RUN\",\"core_cpu_output_integrity\":\"NOT_RUN\"},"
        <<"\"frozen_manifest_sha256\":"<<quoted(hash(read(evidence.directory/"frozen-manifest.json")))
        <<",\"routing\":\"inherited parent model; no overrides\",\"gates\":[\n";
    for(std::size_t i=0;i<evidence.gates.size();++i){const auto& gate=evidence.gates[i];if(i) receipt<<",\n";receipt<<"{\"name\":"<<quoted(gate.name)<<",\"pass\":"<<(gate.pass?"true":"false")<<",\"invalid\":"<<(gate.invalid?"true":"false")<<",\"detail\":"<<quoted(gate.detail)<<'}';}
    receipt<<"],\"limits\":[\"diagnostic factor is unadopted; quality alone cannot select a production factor\",\"frozen manifest describes the original whole workload; its CPU work was not executed\",\"no callback timing, callback heap or CPU output-integrity acceptance\",\"no DAW or listening acceptance claimed\"]}\n";
    receipt.close();
    auto hashes=output(evidence.directory/"artifact-hashes.csv");hashes<<"file,sha256,bytes\n";
    std::vector<fs::path> paths;for(const auto& entry:fs::directory_iterator(evidence.directory)) if(entry.is_regular_file()&&entry.path().filename()!="artifact-hashes.csv") paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());for(const auto& path:paths){const auto bytes=read(path);hashes<<quoted(path.filename().string())<<','<<hash(bytes)<<','<<bytes.size()<<'\n';}
    hashes.close();
    std::cout<<"diagnostic factor "<<evidence.factor<<" quality decision: "<<(invalid?"INVALID":pass?"PASS":"REJECT")<<"; CPU DEFERRED; production FORCED_BYPASS"<<std::endl;
    return invalid?2:pass?0:1;
}
#endif
}

#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
#if defined(RACKSYNTH_SATURATION_CPU_POLICY)
int saturationCpuPolicyEntry(const char* outputDirectory)
{
    try
    {
        const fs::path directory(outputDirectory);if(!directory.is_absolute()||fs::exists(directory)) return 2;
        fs::create_directories(directory);unsigned checks=0;
        const auto require=[&](bool pass,const char* message){++checks;if(!pass) throw std::runtime_error(message);};
        std::vector<CpuRecord> original;
        for(const char* suite:{"base","structural_overlap"}) for(double rate:rates) for(std::size_t block:{64u,512u}) for(bool moving:{false,true})
        {
            CpuRecord r;r.suite=suite;r.rate=rate;r.block=block;r.moving=moving;r.mean=.2*block/rate;r.p99=.3*block/rate;r.maximum=.4*block/rate;
            for(std::size_t phase=0;phase<2;++phase)
            {
                r.callbacks[phase]=static_cast<std::size_t>(std::ceil((phase?10.:2.)*rate/block));
                r.samples[phase]=r.callbacks[phase]*block*2;r.audio[phase]=r.wall[phase]=static_cast<double>(r.callbacks[phase]*block)/rate;
                r.energy[phase]=1;r.checksum[phase]=14695981039346656037ULL;
            }
            original.push_back(r);
        }
        const auto outcome=[&](const char* name,const std::vector<CpuRecord>& rows,int expected)
        {
            require(cpuOutcome(rows)==expected,name);cpuWrite(directory/(std::string(name)+".json"),cpuDecision(rows,true,false));
        };
        outcome("pass",original,0);
        auto rows=original;rows[0].mean=.36*rows[0].block/rows[0].rate;outcome("reject",rows,1);
        rows=original;rows[0].valid=false;outcome("invalid",rows,2);
        rows=original;rows.pop_back();outcome("missing",rows,2);
        rows=original;rows.back()=rows.front();outcome("duplicate",rows,2);
        rows=original;rows[0].suite="unknown";outcome("unknown",rows,2);
        rows=original;rows[0].mean=std::numeric_limits<double>::quiet_NaN();outcome("nonfinite",rows,2);
        rows=original;rows[0].p99=.51*rows[0].block/rows[0].rate;rows[0].maximum=rows[0].p99;outcome("p99_reject",rows,1);
        rows=original;rows[0].allocations=1;outcome("allocation_reject",rows,1);
        rows=original;rows[0].frees=1;outcome("free_reject",rows,1);
        rows=original;rows[0].energy[1]=0;outcome("zero_energy",rows,2);
        rows=original;rows[0].energy[0]=std::numeric_limits<double>::infinity();outcome("nonfinite_energy",rows,2);
        rows=original;rows[0].finite=false;outcome("nonfinite_output",rows,2);
        rows=original;rows[0].audio[0]=1;outcome("short_warm",rows,2);
        rows=original;rows[0].wall[1]=9;outcome("short_wall",rows,2);
        rows=original;--rows[0].samples[1];outcome("incomplete_integrity",rows,2);
        rows=original;rows[0].accepted=false;outcome("rejected_settings",rows,2);
        rows=original;rows.push_back(rows.front());outcome("extra_member",rows,2);
        require(cpuScheduleCheck(),"schedule");
        const auto fresh=(directory/"uncreated").string();
        for(const char* factor:{"2","4","8"})
        {
            const auto parsed=cpuParse({"--measure","--factor",factor,"--output",fresh});
            require(!parsed.describe&&parsed.factor==static_cast<unsigned>(std::stoul(factor)),"synthetic measure parser accepted");
            require(!fs::exists(fresh),"parse must not mutate");
        }
        require(cpuParse({"--describe","--output",fresh}).describe,"describe parser");
        const std::vector<std::vector<std::string>> bad{
            {},{"--measure","--output",fresh},{"--measure","--factor","64","--output",fresh},
            {"--measure","--factor","4","--factor","4","--output",fresh},
            {"--measure","--factor","4","--output",fresh,"--output",fresh},
            {"--measure","--factor","4","--output","relative"},
            {"--measure","--factor","4","--output",directory.string()},
            {"--measure","--factor","4","--output",fresh,"--describe"},
            {"--measure","--measure","--factor","4","--output",fresh},
            {"--measure","--factor"},{"--measure","--factor","04","--output",fresh},
            {"--measure","--factor","4","--output",fresh,"--unknown"},
            {"--describe","--factor","4","--output",fresh}};
        for(const auto& args:bad)
        {
            bool refused=false;try{cpuParse(args);}catch(const std::exception&){refused=true;}
            require(refused&&!fs::exists(fresh),"synthetic parser refusal");
        }
        fs::create_directory(directory/"write-error");bool failed=false;
        try{cpuWrite(directory/"write-error","synthetic no actual measurement");}catch(const std::exception&){failed=true;}
        require(failed,"real writer open error propagation");
        struct FailedFlush:std::stringbuf { int sync() override {return -1;} } buffer;
        std::ostream failedStream(&buffer);failed=false;
        try{cpuComplete(failedStream,[]{});}catch(const std::exception&){failed=true;}
        require(failed,"real stream write/flush error propagation");
        std::ostringstream closed;failed=false;
        try{cpuComplete(closed,[&]{closed.setstate(std::ios::failbit);});}catch(const std::exception&){failed=true;}
        require(failed,"real stream close error propagation");
        cpuWrite(directory/"self-check.json","{\"synthetic\":true,\"actual_measurement\":\"NOT_RUN\",\"synthetic_checks\":"+std::to_string(checks)+"}\n");
        std::cout<<checks<<" synthetic policy/parser/schedule/IO checks; no Candidate construction/audio/timer/measurement\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"CPU synthetic policy failure: "<<error.what()<<'\n';return 2;}
}
#endif
#endif
#if defined(RACKSYNTH_SATURATION_CPU_ONLY)
#if defined(RACKSYNTH_SATURATION_CPU_POLICY)
// The separate policy translation unit owns main and calls only this private entry.
int saturationCpuPolicyEntry(const char* directory);
#else
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::string> args;for(int i=1;i<argc;++i) args.emplace_back(argv[i]);
        const auto options=cpuParse(args);
        if(options.describe){cpuDescribe(options.directory,fs::absolute(argv[0]));std::cout<<"CPU DEFERRED; all actual CPU gates NOT_RUN; no selected factor\n";return 0;}
        if(!cpuScheduleCheck()) throw std::runtime_error("CPU schedule self-check failed");
        const auto manifest=cpuManifest(fs::absolute(argv[0]));
        fs::create_directories(options.directory);cpuWrite(options.directory/"workload.json",manifest);
        Evidence evidence(options.directory,options.factor);std::vector<CpuRecord> records;
        timings(evidence,false,records);timings(evidence,true,records);evidence.recordings.close();
        cpuWrite(options.directory/"decision.json",cpuDecision(records,false,false,options.factor,hash(manifest)));return cpuOutcome(records);
    }
    catch(const std::exception& error){allocationWitness::enabled=false;std::cerr<<"INVALID/incomplete CPU preparation: "<<error.what()<<'\n';return 2;}
}
#endif
#else
#if defined(RACKSYNTH_SATURATION_QUALITY_ONLY)
int main(int argc,char** argv)
{
    try
    {
        unsigned factor=0;fs::path directory;std::string cohort;
        for(int i=1;i<argc;++i)
        {
            const std::string option=argv[i];
            if(option=="--factor"&&!factor&&i+1<argc){const std::string value=argv[++i];if(value=="2"||value=="4"||value=="8") factor=static_cast<unsigned>(std::stoul(value));else throw std::runtime_error("factor must be 2,4,8");}
            else if(option=="--output"&&directory.empty()&&i+1<argc) directory=fs::path(argv[++i]);
            else if(option=="--cohort"&&cohort.empty()&&i+1<argc)
            {
                cohort=argv[++i];if(cohort!="nonalias"&&cohort!="alias-44100"&&cohort!="alias-48000"&&cohort!="alias-96000"&&cohort!="alias-96000-soft"&&cohort!="alias-96000-glow"&&cohort!="alias-96000-dense") throw std::runtime_error("unknown quality cohort");
            }
            else throw std::runtime_error("usage: --factor 2|4|8 --output FRESH_ABSOLUTE_DIRECTORY (quality only)");
        }
        if(!factor||directory.empty()||!directory.is_absolute()||fs::exists(directory)) throw std::runtime_error("output must be a fresh absolute directory and factor must be 2,4,8");
        fs::create_directories(directory);Evidence evidence(directory,factor,cohort);
        if(!cohort.empty())
        {
            freeze(evidence);calibration(evidence);
            if(cohort=="nonalias"){neutral(evidence);compensation(evidence);characters(evidence);denseStateAndBursts(evidence);operatingGrid(evidence);}
            else aliases(evidence,std::stod(cohort.substr(6)),
                cohort=="alias-96000-soft"?0:cohort=="alias-96000-glow"?1:cohort=="alias-96000-dense"?2:-1);
            return finishCohort(evidence);
        }
        freeze(evidence);calibration(evidence);neutral(evidence);compensation(evidence);characters(evidence);denseStateAndBursts(evidence);operatingGrid(evidence);aliases(evidence);
        return finish(evidence);
    }
    catch(const std::exception& error){std::cerr<<"INVALID/incomplete saturation quality measurement: "<<error.what()<<'\n';return 2;}
}
#else
int main(int argc,char** argv)
{
    try
    {
        unsigned factor=0;fs::path directory;
        for(int i=1;i<argc;++i)
        {
            const std::string option=argv[i];
            if(option=="--factor"&&i+1<argc){const std::string value=argv[++i];if(value=="2"||value=="4"||value=="8") factor=static_cast<unsigned>(std::stoul(value));else throw std::runtime_error("factor must be 2,4,8");}
            else if(option=="--output"&&i+1<argc) directory=fs::path(argv[++i]);
            else throw std::runtime_error("usage: --factor 2|4|8 --output FRESH_ABSOLUTE_DIRECTORY");
        }
        if(!factor||directory.empty()||!directory.is_absolute()||fs::exists(directory)) throw std::runtime_error("output must be a fresh absolute directory and factor must be 2,4,8");
        fs::create_directories(directory);Evidence evidence(directory,factor);
        freeze(evidence);calibration(evidence);neutral(evidence);compensation(evidence);characters(evidence);denseStateAndBursts(evidence);operatingGrid(evidence);aliases(evidence);timings(evidence);finish(evidence);
        return 0;
    }
    catch(const std::exception& error){allocationWitness::enabled=false;std::cerr<<"INVALID/incomplete saturation measurement: "<<error.what()<<'\n';return 2;}
}
#endif
#endif
