#define airack airack_reference
#include "SaturationCandidate.h"
#undef airack
#include "Saturation.h"
#include "SaturationAnalysis.h"
#include "RackExamples.h"
#include "ParameterRegistry.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <vector>

using ReferenceCandidate = airack_reference::probe::Candidate;
static_assert(std::is_trivially_copyable_v<airack::SaturationSettings>);
static_assert(std::is_trivially_copyable_v<airack_reference::SaturationSettings>);
static_assert(sizeof(airack::SaturationSettings) == sizeof(airack_reference::SaturationSettings));
static_assert(alignof(airack::SaturationSettings) == alignof(airack_reference::SaturationSettings));
static_assert(std::is_trivially_copyable_v<airack::SaturationLaneSettings>);
static_assert(std::is_trivially_copyable_v<airack_reference::SaturationLaneSettings>);
static_assert(sizeof(airack::SaturationLaneSettings) == sizeof(airack_reference::SaturationLaneSettings));
static_assert(alignof(airack::SaturationLaneSettings) == alignof(airack_reference::SaturationLaneSettings));
static_assert(unsigned(airack::SaturationMode::fullBand) == unsigned(airack_reference::SaturationMode::fullBand));
static_assert(unsigned(airack::SaturationMode::threeBand) == unsigned(airack_reference::SaturationMode::threeBand));
static_assert(unsigned(airack::SaturationCharacter::soft) == unsigned(airack_reference::SaturationCharacter::soft));
static_assert(unsigned(airack::SaturationCharacter::glow) == unsigned(airack_reference::SaturationCharacter::glow));
static_assert(unsigned(airack::SaturationCharacter::dense) == unsigned(airack_reference::SaturationCharacter::dense));
static_assert(unsigned(airack::SaturationLane::full) == unsigned(airack_reference::SaturationLane::full));
static_assert(unsigned(airack::SaturationLane::low) == unsigned(airack_reference::SaturationLane::low));
static_assert(unsigned(airack::SaturationLane::mid) == unsigned(airack_reference::SaturationLane::mid));
static_assert(unsigned(airack::SaturationLane::high) == unsigned(airack_reference::SaturationLane::high));
static_assert(offsetof(airack::SaturationSettings, mode) == offsetof(airack_reference::SaturationSettings, mode));
static_assert(offsetof(airack::SaturationSettings, inputTrimDb) == offsetof(airack_reference::SaturationSettings, inputTrimDb));
static_assert(offsetof(airack::SaturationSettings, outputTrimDb) == offsetof(airack_reference::SaturationSettings, outputTrimDb));
static_assert(offsetof(airack::SaturationSettings, mix) == offsetof(airack_reference::SaturationSettings, mix));
static_assert(offsetof(airack::SaturationSettings, driveCompensation) == offsetof(airack_reference::SaturationSettings, driveCompensation));
static_assert(offsetof(airack::SaturationSettings, lowCrossoverHz) == offsetof(airack_reference::SaturationSettings, lowCrossoverHz));
static_assert(offsetof(airack::SaturationSettings, highCrossoverHz) == offsetof(airack_reference::SaturationSettings, highCrossoverHz));
static_assert(offsetof(airack::SaturationSettings, preLowCutEnabled) == offsetof(airack_reference::SaturationSettings, preLowCutEnabled));
static_assert(offsetof(airack::SaturationSettings, preLowCutHz) == offsetof(airack_reference::SaturationSettings, preLowCutHz));
static_assert(offsetof(airack::SaturationSettings, postHighCutEnabled) == offsetof(airack_reference::SaturationSettings, postHighCutEnabled));
static_assert(offsetof(airack::SaturationSettings, postHighCutHz) == offsetof(airack_reference::SaturationSettings, postHighCutHz));
static_assert(offsetof(airack::SaturationSettings, lanes) == offsetof(airack_reference::SaturationSettings, lanes));
static_assert(offsetof(airack::SaturationLaneSettings, bypassed) == offsetof(airack_reference::SaturationLaneSettings, bypassed));
static_assert(offsetof(airack::SaturationLaneSettings, character) == offsetof(airack_reference::SaturationLaneSettings, character));
static_assert(offsetof(airack::SaturationLaneSettings, driveDb) == offsetof(airack_reference::SaturationLaneSettings, driveDb));
static_assert(offsetof(airack::SaturationLaneSettings, preTiltDb) == offsetof(airack_reference::SaturationLaneSettings, preTiltDb));
static_assert(offsetof(airack::SaturationLaneSettings, mix) == offsetof(airack_reference::SaturationLaneSettings, mix));
static_assert(offsetof(airack::SaturationLaneSettings, trimDb) == offsetof(airack_reference::SaturationLaneSettings, trimDb));
static_assert(std::tuple_size_v<decltype(airack::SaturationSettings::lanes)> == 4);
static_assert(std::tuple_size_v<decltype(airack_reference::SaturationSettings::lanes)> == 4);

namespace {
using Frame=ReferenceCandidate::Frame;
using Audio=std::vector<Frame>;
namespace fs=std::filesystem;
constexpr std::size_t frames=192000, active=144000, fftFrames=262144, gap=12000;
constexpr double rate=48000;
const double targetRms=std::pow(10.,-24./20.);
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
struct Summary { double rms=0,peak=0,crestDb=0,leftDc=0,rightDc=0; };
Summary summarize(const Audio& audio){
    require(!audio.empty(),"empty measurement extent");
    long double squares=0,left=0,right=0;Summary result;
    for(const auto& frame:audio){for(double v:frame){require(std::isfinite(v),"nonfinite sample");squares+=static_cast<long double>(v)*v;result.peak=std::max(result.peak,std::abs(v));}left+=frame[0];right+=frame[1];}
    result.rms=std::sqrt(double(squares/(2*audio.size())));require(std::isfinite(result.rms),"nonfinite RMS");
    result.crestDb=result.rms>0?20*std::log10(result.peak/result.rms):0;
    result.leftDc=double(left/audio.size());result.rightDc=double(right/audio.size());return result;
}
Audio matched(const Audio& audio,double& gain){
    const auto s=summarize(audio);require(s.rms>0,"zero normalization energy");gain=targetRms/s.rms;
    require(std::isfinite(gain)&&s.peak*gain<1,"normalization would clip");
    Audio result=audio;for(auto& frame:result)for(double& v:frame)v=double(float(v*gain));
    require(std::abs(summarize(result).rms-targetRms)<1e-9,"matched float RMS mismatch");return result;
}
void writeWave(const fs::path& path,const Audio& audio){
    require(!fs::exists(path),"existing WAV refused: "+path.string());require(summarize(audio).peak<1,"clipped WAV refused");
    require(audio.size()<(std::numeric_limits<std::uint32_t>::max()-48)/8,"WAV too long");
    juce::FileOutputStream f(juce::File(path.string()));require(f.openedOk(),"WAV open failed");
    const int bytes=int(audio.size()*8);f.write("RIFF",4);f.writeInt(bytes+48);f.write("WAVEfmt ",8);f.writeInt(16);f.writeShort(3);f.writeShort(2);
    f.writeInt(int(rate));f.writeInt(int(rate)*8);f.writeShort(8);f.writeShort(32);f.write("fact",4);f.writeInt(4);f.writeInt(int(audio.size()));f.write("data",4);f.writeInt(bytes);
    for(const auto& frame:audio)for(double v:frame)f.writeFloat(float(v));f.flush();require(f.getStatus().wasOk(),"WAV write failed");
}
int selfTest(const fs::path& root){
    require(!fs::exists(root),"self-test output exists");fs::create_directories(root);
    int failures=0,checks=0;
    const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    const auto rejects=[&](auto action,const char* name){bool threw=false;try{action();}catch(const std::exception&){threw=true;}check(threw,name);};
    Audio sine(4096);for(std::size_t i=0;i<sine.size();++i){const double phase=2*std::numbers::pi*32*double(i)/double(sine.size());sine[i]={.25*std::sin(phase),.125*std::cos(phase)};}
    const auto s=summarize(sine);const double rms=std::sqrt((.25*.25+.125*.125)/4);
    check(std::abs(s.rms-rms)<1e-14,"known stereo sine RMS");check(std::abs(s.peak-.25)<1e-14,"known sine peak");
    check(std::abs(s.crestDb-20*std::log10(.25/rms))<1e-12,"known sine crest");
    double gain=0;const auto m=matched(sine,gain);check(std::abs(gain-targetRms/rms)<1e-12,"one common stereo gain");
    long double squares=0;for(std::size_t i=0;i<m.size();++i)for(int ch=0;ch<2;++ch){squares+=m[i][ch]*m[i][ch];check(m[i][ch]==double(float(sine[i][ch]*gain)),"matched float samples retain stereo scalar");}
    check(std::abs(std::sqrt(double(squares/(2*m.size())))-targetRms)<1e-9,"matched RMS including full extent");
    Audio nan=sine;nan[10][1]=std::numeric_limits<double>::quiet_NaN();
    rejects([&]{double g;matched(nan,g);},"nonfinite normalization rejected");
    rejects([&]{double g;matched(Audio(20),g);},"zero energy normalization rejected");
    rejects([&]{double g;matched(Audio{},g);},"empty normalization rejected");
    Audio impulse(4096);impulse[0][0]=1;
    rejects([&]{double g;matched(impulse,g);},"clipping normalization rejected");
    rejects([&]{writeWave(root/"nonfinite.wav",nan);},"nonfinite WAV rejected");
    rejects([&]{writeWave(root/"empty.wav",{});},"empty WAV rejected");
    Audio clipped(2,Frame{1.01,0});rejects([&]{writeWave(root/"clipped.wav",clipped);},"clipped WAV rejected");
    const auto path=root/"known.wav";writeWave(path,m);
    check(fs::file_size(path)==56+8*m.size(),"float WAV exact extent");
    const auto before=juce::SHA256(juce::File(path.string())).toHexString();
    rejects([&]{writeWave(path,sine);},"existing output refused");
    check(before==juce::SHA256(juce::File(path.string())).toHexString(),"existing output preserved");
    std::ifstream file(path,std::ios::binary);std::vector<char> header(56);file.read(header.data(),56);
    check(std::string(header.data(),4)=="RIFF"&&std::string(header.data()+8,4)=="WAVE"&&std::string(header.data()+48,4)=="data","WAV RIFF and data headers");
    check(static_cast<unsigned char>(header[20])==3&&static_cast<unsigned char>(header[22])==2&&static_cast<unsigned char>(header[34])==32,"WAV stereo IEEE float32 format");
    std::cout<<"Factory exporter self-test "<<checks<<" checks / "<<failures<<" failures\n";return failures?1:0;
}
using juce::var;
var object(std::initializer_list<std::pair<const char*,var>> fields){auto* o=new juce::DynamicObject;for(const auto& [name,value]:fields)o->setProperty(name,value);return var(o);}
void put(var& o,const char* key,var value){o.getDynamicObject()->setProperty(key,value);}
void put(var& o,const char* key,const std::string& value){put(o,key,var(juce::String(value)));}
std::string text(const var& v){return v.toString().toStdString();}
juce::String hash(const fs::path& p){require(fs::is_regular_file(p),"missing file: "+p.string());return juce::SHA256(juce::File(p.string())).toHexString();}
var loadJson(const fs::path& p){var result;const auto error=juce::JSON::parse(juce::File(p.string()).loadFileAsString(),result);require(error.wasOk()&&result.isObject(),"invalid JSON: "+p.string());return result;}
double number(const var& v){require(v.isInt()||v.isInt64()||v.isDouble(),"numeric field required");const double n=double(v);require(std::isfinite(n),"nonfinite numeric field");return n;}
void writeJson(const fs::path& p,const var& value){require(!fs::exists(p),"existing JSON refused");require(juce::File(p.string()).replaceWithText(juce::JSON::toString(value,false,20)),"JSON write failed");}
void writeRaw(const fs::path& p,const Audio& a){
    static_assert(std::endian::native==std::endian::little&&sizeof(Frame)==16&&std::numeric_limits<double>::is_iec559);
    require(!fs::exists(p),"existing raw refused");summarize(a);std::ofstream f(p,std::ios::binary);require(bool(f),"raw open failed");
    f.write(reinterpret_cast<const char*>(a.data()),static_cast<std::streamsize>(a.size()*sizeof(Frame)));f.flush();require(bool(f),"raw write failed");
}
var basic(const Summary& s){return object({{"rms",s.rms},{"peak",s.peak},{"crest_db",s.crestDb},{"dc_left",s.leftDc},{"dc_right",s.rightDc}});}
var metrics(const Audio& a){
    require(a.size()==frames,"measurement requires complete frozen extent");var result=basic(summarize(a));juce::Array<var> windows;
    const auto window=[&](const std::string& name,std::size_t begin,std::size_t end){var w=basic(summarize(Audio(a.begin()+begin,a.begin()+end)));put(w,"name",name);put(w,"begin_frame",int(begin));put(w,"end_frame",int(end));windows.add(w);};
    for(unsigned note=0;note<6;++note){const std::size_t start=note*24000;const auto prefix="note_"+std::to_string(note)+"_";
        window(prefix+"first_20ms",start,start+960);window(prefix+"20_to_100ms",start+960,start+4800);window(prefix+"200_to_400ms",start+9600,start+19200);}
    window("tail_full",active,frames);window("tail_first_20ms",active,active+960);window("tail_last_100ms",frames-4800,frames);put(result,"windows",windows);
    std::array<double,4> bands{};double total=0,weighted=0;const std::array<double,3> edges{120,180,3500};
    for(unsigned channel=0;channel<2;++channel){std::vector<double> padded(fftFrames,0);for(std::size_t i=0;i<frames;++i)padded[i]=a[i][channel];
        const auto spectrum=airack::probe::analysis::rectangularPowerSpectrum(padded);require(spectrum.valid,"invalid diagnostic FFT");
        for(std::size_t bin=0;bin<spectrum.oneSidedPower.size();++bin){const double hz=double(bin)*rate/fftFrames,power=.5*spectrum.oneSidedPower[bin];
            const auto band=std::size_t(std::upper_bound(edges.begin(),edges.end(),hz)-edges.begin());bands[band]+=power;total+=power;weighted+=hz*power;}}
    juce::Array<var> powers;for(double p:bands)powers.add(p);put(result,"band_power",powers);put(result,"total_spectral_power",total);put(result,"spectral_centroid_hz",total?weighted/total:0);return result;
}
struct Factory {std::string name,slug;airack::SaturationSettings settings;var receipt;};
std::vector<Factory> factories(const var& expected){
    const auto all=airack::factoryRackDesigns();std::vector<Factory> result;const auto maps=expected["factories"];
    require(maps.isObject()&&maps.getDynamicObject()->getProperties().size()==2,"expected two frozen factories");
    require(number(expected["factory_slot_bypassed"])==1,"expected global bypass must stay one");
    for(const auto* name:{"Character Drive","Band Color"}){
        const auto it=std::find_if(all.begin(),all.end(),[&](const auto& r){return r.name==name;});require(it!=all.end(),"actual factory absent");
        require(it->units.size()==1&&it->units[0].module==airack::NativeModule::saturation,"unexpected actual factory unit");const auto& unit=it->units[0];
        require(unit.defaults.size()==36,"actual factory must have all 35 fields and slot bypass");const auto map=maps[name];require(map.isObject()&&map.getDynamicObject()->getProperties().size()==35,"expected 35 saturation controls");
        std::map<std::string,float> physical;juce::Array<var> fields;float bypass=-1;
        for(const auto& entry:unit.defaults){const auto key=entry.key.toStdString();require(entry.unit==unit.id,"factory control unit mismatch");
            const auto index=airack::rackParameterIndex(unit.module,0,entry.key);require(index.has_value(),"unknown actual factory key");const auto& d=airack::parameterRegistry()[*index];
            const juce::NormalisableRange<float> range(d.minimum,d.maximum,0,d.skew);require(std::isfinite(entry.normalized)&&entry.normalized>=0&&entry.normalized<=1,"invalid actual normalized default");
            const double literal=key=="bypassed"?number(expected["factory_slot_bypassed"]):number(map[juce::Identifier(entry.key)]);
            const float expectedNormalized=range.convertTo0to1(float(literal));require(entry.normalized==expectedNormalized,"actual normalized factory differs from frozen expectation: "+std::string(name)+"/"+key);
            const float decoded=range.convertFrom0to1(entry.normalized);require(physical.emplace(key,decoded).second,"duplicate factory control");
            if(key=="bypassed"){bypass=entry.normalized;continue;}
            fields.add(object({{"key",entry.key},{"expected_physical",literal},{"expected_normalized",double(expectedNormalized)},{"actual_normalized",double(entry.normalized)},{"decoded_physical",double(decoded)}}));}
        require(physical.size()==36&&fields.size()==35&&bypass==1,"factory binding incomplete");
        const auto get=[&](const std::string& key){return physical.at("saturation."+key);};airack::SaturationSettings s;
        s.mode=static_cast<airack::SaturationMode>(int(get("mode")));s.inputTrimDb=get("input_trim_db");s.outputTrimDb=get("output_trim_db");s.mix=get("mix");s.driveCompensation=get("drive_compensation")>.5f;
        s.lowCrossoverHz=get("low_crossover_hz");s.highCrossoverHz=get("high_crossover_hz");s.preLowCutEnabled=get("pre_low_cut_enabled")>.5f;s.preLowCutHz=get("pre_low_cut_hz");s.postHighCutEnabled=get("post_high_cut_enabled")>.5f;s.postHighCutHz=get("post_high_cut_hz");
        const std::array<std::string,4> banks{"full","low","mid","high"};for(unsigned i=0;i<4;++i){const auto prefix=banks[i]+".";auto& lane=s.lanes[i];lane.bypassed=get(prefix+"bypassed")>.5f;lane.character=static_cast<airack::SaturationCharacter>(int(get(prefix+"character")));lane.driveDb=get(prefix+"drive_db");lane.preTiltDb=get(prefix+"pre_tilt_db");lane.mix=get(prefix+"mix");lane.trimDb=get(prefix+"trim_db");}
        require(airack::validateSaturation(s),"decoded actual settings invalid");result.push_back({name,std::string(name)=="Character Drive"?"character-drive":"band-color",s,object({{"name",name},{"rack_id",it->id.toString()},{"slot_bypassed_normalized",double(bypass)},{"settings",fields}})});
    }return result;
}
struct Input {std::string name;Audio audio;};
std::vector<Input> inputs(const fs::path& manifestPath,const var& manifest){
    require(number(manifest["sample_rate"])==rate&&number(manifest["frames"])==frames&&number(manifest["active_frames"])==active&&number(manifest["tail_frames"])==frames-active&&number(manifest["channels"])==2&&number(manifest["fixed_peak"])==1./16,"frozen input extent mismatch");
    const auto* rows=manifest["inputs"].getArray();require(rows&&rows->size()==4,"expected four frozen inputs");std::vector<Input> result;std::vector<std::string> names;
    for(const auto& row:*rows){const auto name=text(row["name"]);require(name=="bass"||name=="pluck"||name=="lead"||name=="multitone","unexpected passage");require(std::find(names.begin(),names.end(),name)==names.end(),"duplicate passage");names.push_back(name);
        const fs::path filename=text(row["file"]);require(filename==filename.filename()&&filename==name+".f64","unexpected input path");const auto path=manifestPath.parent_path()/filename;
        require(number(row["bytes"])==frames*sizeof(Frame)&&fs::file_size(path)==frames*sizeof(Frame)&&hash(path).toStdString()==text(row["sha256"]),"frozen input identity mismatch");
        Audio a(frames);std::ifstream f(path,std::ios::binary);f.read(reinterpret_cast<char*>(a.data()),static_cast<std::streamsize>(frames*sizeof(Frame)));require(bool(f),"input read failed");
        require(summarize(a).peak==1./16,"frozen peak mismatch");require(std::all_of(a.begin()+active,a.end(),[](Frame v){return v[0]==0&&v[1]==0;}),"frozen tail is not exact zero");result.push_back({name,std::move(a)});
    }return result;
}
Audio render(const Audio& input,const airack::SaturationSettings& settings,bool partitioned){
    ReferenceCandidate c;require(c.setSettings(std::bit_cast<airack_reference::SaturationSettings>(settings))&&c.prepare(rate,4),"candidate preparation failed");Audio out(input.size());
    constexpr std::array<std::size_t,8> sizes{1,7,64,13,511,3,128,1024};std::size_t start=0,partition=0;
    while(start<input.size()){const auto end=std::min(input.size(),start+(partitioned?sizes[partition++%sizes.size()]:input.size()));if(partitioned)require(c.setSettings(std::bit_cast<airack_reference::SaturationSettings>(settings)),"repeated partition settings rejected");for(auto i=start;i<end;++i)out[i]=c.process(input[i]);start=end;}
    summarize(out);return out;
}
var signal(const fs::path& root,const std::string& name,const Audio& raw,bool normalize){
    const auto rawName=name+".f64";writeRaw(root/rawName,raw);var r=object({{"raw_file",juce::String(rawName)},{"raw_sha256",hash(root/rawName)},{"raw",metrics(raw)}});
    if(normalize){double gain=0;const auto copy=matched(raw,gain);const auto file=name+"-matched.wav";writeWave(root/file,copy);put(r,"match_gain",gain);put(r,"matched_file",file);put(r,"matched_sha256",hash(root/file));put(r,"matched",metrics(copy));}return r;
}
int compare(const fs::path& manifestPath,const fs::path& expectedPath,const fs::path& root){
    require(!fs::exists(root),"existing comparison directory refused: "+root.string());const auto expected=loadJson(expectedPath),manifest=loadJson(manifestPath);
    const auto catalog=factories(expected);const auto frozen=inputs(manifestPath,manifest);fs::create_directories(root);
    juce::Array<var> factoryRows,records;for(const auto& f:catalog)factoryRows.add(f.receipt);
    for(const auto& f:catalog)for(const auto& input:frozen){auto drySettings=f.settings;drySettings.mix=0;
        const auto dry=render(input.audio,drySettings,false),wet=render(input.audio,f.settings,false);
        require(dry==render(input.audio,drySettings,true)&&wet==render(input.audio,f.settings,true),"irregular partition output mismatch");
        const auto name=f.slug+"-"+input.name;var r=object({{"factory",juce::String(f.name)},{"passage",juce::String(input.name)},{"partition_exact",true}});
        put(r,"input",signal(root,name+"-input",input.audio,false));put(r,"dry",signal(root,name+"-dry",dry,true));put(r,"wet",signal(root,name+"-wet",wet,true));
        double dryGain=0,wetGain=0;auto ab=matched(dry,dryGain);const auto b=matched(wet,wetGain);ab.insert(ab.end(),gap,Frame{});ab.insert(ab.end(),b.begin(),b.end());
        const auto abName=name+"-A-dry-B-wet.wav";writeWave(root/abName,ab);put(r,"ab_file",abName);put(r,"ab_sha256",hash(root/abName));
        for(const auto* domain:{"raw","matched"}){juce::Array<var> ratios;const auto dp=r["dry"][domain]["band_power"],wp=r["wet"][domain]["band_power"];for(int band=0;band<4;++band)ratios.add(number(dp[band])>0?var(number(wp[band])/number(dp[band])):var());put(r,(std::string(domain)+"_band_power_wet_over_dry").c_str(),ratios);}
        records.add(r);std::cout<<"Rendered "<<name<<" actual catalog, exact irregular partitions, raw + matched + A/B\n";
    }
    writeJson(root/"comparisons.json",object({{"sample_rate",rate},{"frames",int(frames)},{"fft_frames",int(fftFrames)},{"factor",4},{"selected_factor",var()},{"cpu","DEFERRED"},{"production","FORCED_BYPASS"},{"human_listening","NOT_RUN"},{"input_manifest_sha256",hash(manifestPath)},{"expected_settings_sha256",hash(expectedPath)},{"exporter_sha256",hash(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString())},{"factories",factoryRows},{"comparisons",records}}));return 0;
}
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--self-test")return selfTest(fs::absolute(argv[2]));
    if(argc==5&&std::string(argv[1])=="--render")return compare(fs::absolute(argv[2]),fs::absolute(argv[3]),fs::absolute(argv[4]));
    throw std::runtime_error("usage: --self-test FRESH_DIRECTORY | --render INPUT_MANIFEST EXPECTED_SETTINGS FRESH_DIRECTORY");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
