#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using J=juce::var;
constexpr int rate=48000,blockSize=512,maximumDiscreteSteps=4096;
void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
J object(){return J(new juce::DynamicObject);}
void put(J& value,const char* key,const J& item){value.getDynamicObject()->setProperty(key,item);}
void putTail(J& value,double seconds){
    const bool finite=std::isfinite(seconds);put(value,"tail_seconds",finite?J(seconds):J());put(value,"tail_finite",finite);
    put(value,"tail_kind",finite?"finite":std::isnan(seconds)?"nan":seconds>0?"positive_infinity":"negative_infinity");
}
struct Setting {juce::String id;float value=0;};
struct Arguments {
    juce::File plugin,output;bool inspect=false,render=false,selfTest=false;double seconds=30,gateSeconds=0;
    juce::String impulse="both",parameter,pluginClass;std::vector<float> values;std::vector<Setting> baseline;std::vector<int> notes;int velocity=100;
};
double number(const juce::String& text,double low,double high){
    const auto utf8=text.toStdString();char* end=nullptr;errno=0;const double value=std::strtod(utf8.c_str(),&end);
    require(!utf8.empty()&&end==utf8.c_str()+utf8.size()&&errno!=ERANGE&&std::isfinite(value)&&value>=low&&value<=high,"Invalid numeric argument");return value;
}
Arguments parse(const juce::StringArray& args){
    Arguments result;bool havePlugin=false,haveOutput=false,haveSeconds=false,haveImpulse=false,haveValues=false,haveParameter=false,haveVelocity=false,haveGate=false;
    for(int i=0;i<args.size();++i){const auto key=args[i];
        if(key=="--self-test"){require(args.size()==1,"--self-test takes no other arguments");result.selfTest=true;return result;}
        if(key=="--inspect"){require(!result.inspect,"Duplicate --inspect");result.inspect=true;continue;}
        if(key=="--render"){require(!result.render,"Duplicate --render");result.render=true;continue;}
        require(i+1<args.size(),"Argument requires a value");const auto value=args[++i];
        if(key=="--plugin"){require(!havePlugin&&juce::File::isAbsolutePath(value),"Plugin path must be absolute and supplied once");result.plugin=juce::File(value);havePlugin=true;}
        else if(key=="--output"){require(!haveOutput&&juce::File::isAbsolutePath(value),"Output path must be absolute and supplied once");result.output=juce::File(value);haveOutput=true;}
        else if(key=="--class"){require(result.pluginClass.isEmpty()&&value.isNotEmpty(),"Plugin class name must be supplied once");result.pluginClass=value;}
        else if(key=="--seconds"){require(!haveSeconds,"Duplicate --seconds");result.seconds=number(value,1,120);haveSeconds=true;}
        else if(key=="--impulse"){require(!haveImpulse&&(value=="mono"||value=="stereo"||value=="both"),"Impulse must be mono, stereo or both");result.impulse=value;haveImpulse=true;}
        else if(key=="--parameter"){require(!haveParameter&&value.isNotEmpty(),"Parameter ID must be supplied once");result.parameter=value;haveParameter=true;}
        else if(key=="--values"){
            require(!haveValues,"Duplicate --values");haveValues=true;const auto tokens=juce::StringArray::fromTokens(value,",","");
            require(tokens.size()>0&&tokens.size()<=41,"Supply 1 to 41 normalized variation values");
            for(const auto& token:tokens)result.values.push_back(float(number(token,0,1)));
        }else if(key=="--note"){
            require(result.notes.empty(),"Duplicate --note");const auto tokens=juce::StringArray::fromTokens(value,",","");
            require(tokens.size()>0&&tokens.size()<=8,"Supply 1 to 8 MIDI notes");
            for(const auto& token:tokens){const auto note=number(token,0,127);require(note==std::floor(note),"MIDI notes must be integers");result.notes.push_back(int(note));}
            for(const auto note:result.notes)require(std::count(result.notes.begin(),result.notes.end(),note)==1,"MIDI notes must be distinct");
        }else if(key=="--velocity"){require(!haveVelocity,"Duplicate --velocity");const auto v=number(value,1,127);require(v==std::floor(v),"Velocity must be an integer");result.velocity=int(v);haveVelocity=true;}
        else if(key=="--gate-seconds"){require(!haveGate,"Duplicate --gate-seconds");result.gateSeconds=number(value,.001,120);haveGate=true;
        }else if(key=="--set"){
            const auto at=value.indexOfChar('=');require(at>0&&at<value.length()-1,"Baseline setting must be ID=normalized");
            Setting setting{value.substring(0,at),float(number(value.substring(at+1),0,1))};
            for(const auto& existing:result.baseline)require(existing.id!=setting.id,"Duplicate baseline parameter ID");result.baseline.push_back(setting);
        }else throw std::runtime_error("Unknown argument: "+key.toStdString());
    }
    require(havePlugin&&haveOutput&&(result.inspect||result.render),"Use --inspect and/or --render with --plugin ABSOLUTE_VST3 --output FRESH_ABSOLUTE_DIRECTORY");
    require(result.render||(!haveSeconds&&!haveImpulse&&!haveParameter&&!haveValues&&result.baseline.empty()&&result.notes.empty()),"Render options require --render");
    require(haveParameter==haveValues,"--parameter and --values must be supplied together");
    require(!result.notes.empty()||(!haveVelocity&&!haveGate),"--velocity and --gate-seconds require --note");require(result.notes.empty()||!haveImpulse,"--note replaces --impulse");
    if(!result.notes.empty()){if(!haveGate)result.gateSeconds=result.seconds/2;require(result.gateSeconds<=result.seconds,"--gate-seconds must not exceed --seconds");}
    return result;
}
std::vector<float> samples(int steps,bool discrete){
    std::vector<float> result;for(int n=0;n<=20;++n)result.push_back(float(n)/20);
    if(discrete&&steps>1&&steps<=maximumDiscreteSteps)for(int n=0;n<steps;++n)result.push_back(float(n)/float(steps-1));
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
juce::String parameterId(juce::AudioProcessorParameter& parameter){
    if(auto* hosted=dynamic_cast<juce::HostedAudioProcessorParameter*>(&parameter))return hosted->getParameterID();
    return "index:"+juce::String(parameter.getParameterIndex());
}
juce::File binaryIn(const juce::File& plugin){
    if(plugin.existsAsFile())return plugin;
    return plugin.getChildFile("Contents/x86_64-win/"+plugin.getFileName());
}
J identity(const juce::File& file){
    require(file.existsAsFile(),"Identity file is missing");J result=object();put(result,"path",file.getFullPathName());put(result,"bytes",J(file.getSize()));put(result,"sha256",juce::SHA256(file).toHexString());return result;
}
juce::String architecture(const juce::File& binary){
    auto stream=binary.createInputStream();require(stream&&stream->getTotalLength()>=64&&stream->readShort()==0x5a4d,"Plugin is not a PE image");
    require(stream->setPosition(0x3c),"Cannot read PE offset");const auto offset=std::uint32_t(stream->readInt());
    require(offset<=std::uint64_t(stream->getTotalLength())-6&&stream->setPosition(offset)&&stream->readInt()==0x4550,"Invalid PE header");
    const auto machine=std::uint16_t(stream->readShort());return machine==0x8664?"x64":machine==0xaa64?"arm64":machine==0x14c?"x86":"unknown";
}
void writeJson(const juce::File& file,const J& value){
    require(!file.exists(),"Refusing artifact overwrite");auto stream=file.createOutputStream();require(stream&&stream->openedOk(),"Cannot create JSON artifact");
    const auto text=juce::JSON::toString(value,false,17);require(stream->writeText(text,false,false,"\n"),"JSON write failed");stream->flush();require(stream->getStatus().wasOk(),"JSON flush failed");
}
J parameterValues(juce::AudioPluginInstance& plugin){
    J result(juce::Array<J>{});for(auto* parameter:plugin.getParameters()){J row=object();put(row,"index",parameter->getParameterIndex());put(row,"id",parameterId(*parameter));
        put(row,"name",parameter->getName(256));put(row,"normalized",double(parameter->getValue()));put(row,"text",parameter->getText(parameter->getValue(),256));result.append(row);}return result;
}
J inspect(juce::AudioPluginInstance& plugin){
    J result=object(),parameters(juce::Array<J>{}),buses(juce::Array<J>{}),programs(juce::Array<J>{});
    for(auto* parameter:plugin.getParameters()){
        J row=object(),texts(juce::Array<J>{});const auto steps=parameter->getNumSteps();put(row,"index",parameter->getParameterIndex());put(row,"id",parameterId(*parameter));
        put(row,"id_source",dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter)?"hosted_parameter_id":"index_fallback");
        put(row,"name",parameter->getName(256));put(row,"default_normalized",double(parameter->getDefaultValue()));put(row,"current_normalized",double(parameter->getValue()));
        put(row,"steps",steps);put(row,"discrete",parameter->isDiscrete());put(row,"boolean",parameter->isBoolean());put(row,"automatable",parameter->isAutomatable());put(row,"label",parameter->getLabel());
        put(row,"all_discrete_steps_sampled",!parameter->isDiscrete()||steps<=maximumDiscreteSteps);put(row,"discrete_sampling_cap",maximumDiscreteSteps);
        for(float value:samples(steps,parameter->isDiscrete())){J sample=object();put(sample,"normalized",double(value));put(sample,"text",parameter->getText(value,256));texts.append(sample);}put(row,"display_samples",texts);parameters.append(row);
    }
    for(bool input:{true,false})for(int n=0;n<plugin.getBusCount(input);++n){const auto* bus=plugin.getBus(input,n);J row=object();put(row,"direction",input?"input":"output");put(row,"index",n);put(row,"name",bus->getName());put(row,"enabled",bus->isEnabled());put(row,"default_layout",bus->getDefaultLayout().getDescription());put(row,"current_layout",bus->getCurrentLayout().getDescription());put(row,"channels",bus->getNumberOfChannels());buses.append(row);}
    const auto count=plugin.getNumPrograms();require(count>=0&&count<=4096,"Program list exceeds bounded inspection limit");
    for(int n=0;n<count;++n){J row=object();put(row,"index",n);put(row,"name",plugin.getProgramName(n));programs.append(row);}
    put(result,"parameters",parameters);put(result,"parameter_count",plugin.getParameters().size());put(result,"buses",buses);put(result,"programs",programs);put(result,"current_program",plugin.getCurrentProgram());
    put(result,"latency_samples",plugin.getLatencySamples());putTail(result,plugin.getTailLengthSeconds());put(result,"accepts_midi",plugin.acceptsMidi());put(result,"produces_midi",plugin.producesMidi());put(result,"editor_created",false);return result;
}
std::unique_ptr<juce::AudioPluginInstance> load(juce::VST3PluginFormat& format,const juce::PluginDescription& description){
    juce::String error;auto plugin=format.createInstanceFromDescription(description,rate,blockSize,error);
    if(!plugin)throw std::runtime_error("VST3 creation failed: "+error.toStdString());return plugin;
}
void set(juce::AudioPluginInstance& plugin,const Setting& setting){
    juce::AudioProcessorParameter* found=nullptr;for(auto* parameter:plugin.getParameters())if(parameterId(*parameter)==setting.id){require(found==nullptr,"Ambiguous parameter ID");found=parameter;}
    require(found!=nullptr,"Requested parameter ID is absent");found->beginChangeGesture();found->setValueNotifyingHost(setting.value);found->endChangeGesture();
}
void settle(juce::AudioPluginInstance& plugin){
    juce::AudioBuffer<float> silence(2,blockSize);juce::MidiBuffer midi;
    for(int first=0;first<rate;first+=blockSize){silence.setSize(2,std::min(blockSize,rate-first),false,false,true);silence.clear();plugin.processBlock(silence,midi);}plugin.reset();
}
void writeWav(const juce::File& file,const juce::AudioBuffer<float>& audio){
    require(!file.exists(),"Refusing WAV overwrite");std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();require(stream!=nullptr,"Cannot create WAV");
    juce::WavAudioFormat format;auto options=juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer=format.createWriterFor(stream,options);require(writer&&writer->writeFromAudioSampleBuffer(audio,0,audio.getNumSamples())&&writer->flush(),"Float WAV write failed");writer.reset();
    auto input=file.createInputStream();require(input!=nullptr,"Cannot reopen WAV");std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(),true));
    require(reader&&reader->usesFloatingPointData&&reader->bitsPerSample==32&&reader->numChannels==2&&reader->sampleRate==rate&&reader->lengthInSamples==audio.getNumSamples(),"WAV format readback failed");
    juce::AudioBuffer<float> scratch(2,blockSize);for(int first=0;first<audio.getNumSamples();first+=blockSize){const auto count=std::min(blockSize,audio.getNumSamples()-first);require(reader->read(&scratch,0,count,first,true,true),"WAV sample readback failed");for(int ch=0;ch<2;++ch)require(std::memcmp(scratch.getReadPointer(ch),audio.getReadPointer(ch,first),std::size_t(count)*sizeof(float))==0,"WAV altered float samples");}
}
struct Head final:juce::AudioPlayHead {juce::int64 sample=0;juce::Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setTimeInSamples(sample);p.setBpm(120);p.setIsPlaying(true);return p;}};
J render(juce::VST3PluginFormat& format,const juce::PluginDescription& description,const Arguments& args,bool stereo,const std::optional<Setting>& variation,int ordinal,J& expectedBaseline){
    const bool notes=!args.notes.empty();auto plugin=load(format,description);plugin->setNonRealtime(true);plugin->disableNonMainBuses();
    auto layout=plugin->getBusesLayout();require((notes||layout.inputBuses.size()>0)&&layout.outputBuses.size()>0,"Reference must have audio input and output buses");layout.outputBuses.getReference(0)=juce::AudioChannelSet::stereo();
    if(layout.inputBuses.size()>0)layout.inputBuses.getReference(0)=notes?juce::AudioChannelSet::disabled():juce::AudioChannelSet::stereo();
    require(plugin->setBusesLayout(layout)&&plugin->getTotalNumInputChannels()==(notes?0:2)&&plugin->getTotalNumOutputChannels()==2,"Reference rejected stereo audio layout");
    Head head;plugin->setPlayHead(&head);plugin->prepareToPlay(rate,blockSize);for(const auto& setting:args.baseline)set(*plugin,setting);settle(*plugin);
    const auto baseline=parameterValues(*plugin);if(expectedBaseline.isVoid())expectedBaseline=baseline;else require(juce::JSON::toString(baseline)==juce::JSON::toString(expectedBaseline),"Fresh instances disagree on baseline settings");
    if(variation){set(*plugin,*variation);settle(*plugin);}const auto actual=parameterValues(*plugin);
    const auto frames=int(std::ceil(args.seconds*rate));juce::AudioBuffer<float> audio(2,frames),block(2,blockSize);audio.clear();juce::MidiBuffer midi;
    std::array<double,2> peaks{},energy{};std::array<int,2> firstNonzero{-1,-1};
    const int gate=int(std::round(args.gateSeconds*rate));
    for(int first=0;first<frames;first+=blockSize){const int count=std::min(blockSize,frames-first);block.setSize(2,count,false,false,true);block.clear();midi.clear();head.sample=first;
        if(notes){for(const auto note:args.notes){if(first==0)midi.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(args.velocity)),0);if(gate>=first&&gate<first+count)midi.addEvent(juce::MidiMessage::noteOff(1,note),gate-first);}}
        else if(first==0){block.setSample(0,0,1);if(stereo)block.setSample(1,0,1);}plugin->processBlock(block,midi);
        for(int ch=0;ch<2;++ch){audio.copyFrom(ch,first,block,ch,0,count);for(int n=0;n<count;++n){const double v=block.getSample(ch,n);require(std::isfinite(v),"Reference returned nonfinite audio");peaks[std::size_t(ch)]=std::max(peaks[std::size_t(ch)],std::abs(v));energy[std::size_t(ch)]+=v*v;if(v!=0&&firstNonzero[std::size_t(ch)]<0)firstNonzero[std::size_t(ch)]=first+n;}}
    }
    J row=object();put(row,"ordinal",ordinal);put(row,"signal",notes?"midi_notes":stereo?"stereo_equal_impulse":"mono_left_impulse");
    if(notes){juce::Array<J> list;for(const auto note:args.notes)list.add(note);put(row,"notes",list);put(row,"velocity",args.velocity);put(row,"note_off_sample",gate);}else put(row,"input_impulse_amplitude",1);put(row,"rate",rate);put(row,"maximum_block",blockSize);put(row,"frames",frames);put(row,"baseline_settings",baseline);put(row,"actual_settings",actual);put(row,"baseline_settle_frames",rate);put(row,"variation_settle_frames",variation?rate:0);
    if(variation){J value=object();put(value,"id",variation->id);put(value,"requested_normalized",double(variation->value));put(row,"single_parameter_variation",value);}
    put(row,"latency_samples",plugin->getLatencySamples());putTail(row,plugin->getTailLengthSeconds());put(row,"peak",juce::Array<J>{peaks[0],peaks[1]});put(row,"rms",juce::Array<J>{std::sqrt(energy[0]/frames),std::sqrt(energy[1]/frames)});put(row,"first_nonzero_sample",juce::Array<J>{firstNonzero[0],firstNonzero[1]});
    plugin->releaseResources();plugin->setPlayHead(nullptr);plugin.reset();const auto file=args.output.getChildFile((notes?"note-":"impulse-")+juce::String(ordinal).paddedLeft('0',3)+".wav");writeWav(file,audio);put(row,"wav",identity(file));put(row,"wav_readback_bit_exact",true);return row;
}
int run(const Arguments& args){
    require(args.plugin.hasFileExtension("vst3"),"Plugin must have .vst3 extension");const auto binary=binaryIn(args.plugin);const auto before=identity(binary);require(architecture(binary)=="x64","Reference must be a Windows x64 VST3");
    const auto outputPath=std::filesystem::path(args.output.getFullPathName().toWideCharPointer());require(std::filesystem::symlink_status(outputPath).type()==std::filesystem::file_type::not_found,"Output directory must be fresh, including links");
    require(args.output.createDirectory().wasOk(),"Cannot create output directory");J report=object();put(report,"schema",1);put(report,"success",false);put(report,"started_utc",juce::Time::getCurrentTime().toISO8601(true));put(report,"plugin_before",before);put(report,"architecture","x64");put(report,"helper",identity(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    writeJson(args.output.getChildFile("incomplete.json"),report);
    try {
        juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> descriptions;format.findAllTypesForFile(descriptions,args.plugin.getFullPathName());
        if(args.pluginClass.isNotEmpty())for(int n=descriptions.size();--n>=0;)if(descriptions[n]->name!=args.pluginClass)descriptions.remove(n);
        require(descriptions.size()==1,"Expected exactly one VST3 plugin class; name one with --class");
        const auto& description=*descriptions[0];put(report,"plugin_class_requested",args.pluginClass);put(report,"plugin_name",description.name);put(report,"manufacturer",description.manufacturerName);put(report,"host_reported_version",description.version);put(report,"plugin_unique_id",description.uniqueId);put(report,"editor_created",false);put(report,"normalization_applied",false);
        if(args.inspect){auto plugin=load(format,description);put(report,"inspection_before_prepare",inspect(*plugin));plugin->setNonRealtime(true);plugin->prepareToPlay(rate,blockSize);put(report,"inspection_prepared_48000_512",inspect(*plugin));plugin->releaseResources();}
        if(args.render){J renders(juce::Array<J>{}),baseline;int ordinal=0;
            for(bool stereo:{false,true}){if(!args.notes.empty()?stereo:(stereo&&args.impulse=="mono")||(!stereo&&args.impulse=="stereo"))continue;
                renders.append(render(format,description,args,stereo,{},++ordinal,baseline));
                for(float value:args.values)renders.append(render(format,description,args,stereo,Setting{args.parameter,value},++ordinal,baseline));}
            put(report,"neutral_baseline",baseline);put(report,"neutral_baseline_source",args.baseline.empty()?"Fresh instance settings; no claim that these form a neutral wet-only response":"Fresh instance plus explicit --set overrides; actual normalized settings recorded");put(report,"renders",renders);
        }
        const auto after=identity(binary);put(report,"plugin_after",after);require(before["sha256"]==after["sha256"]&&before["bytes"]==after["bytes"],"Reference plugin changed during capture");put(report,"plugin_unchanged",true);put(report,"success",true);put(report,"ended_utc",juce::Time::getCurrentTime().toISO8601(true));writeJson(args.output.getChildFile("report.json"),report);std::cout<<"REFERENCE_PLUGIN_PROBE success: "<<args.output.getFullPathName()<<'\n';return 0;
    }catch(const std::exception& error){put(report,"failure",juce::String(error.what()));put(report,"plugin_after",identity(binary));put(report,"ended_utc",juce::Time::getCurrentTime().toISO8601(true));writeJson(args.output.getChildFile("failure.json"),report);throw;}
}
int selfTest(){
    int checks=0;const auto check=[&](bool good){++checks;require(good,"Reference probe self-test failed");};
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory);const auto plugin=root.getChildFile("reference.vst3").getFullPathName(),output=root.getChildFile("reference-probe-self-test").getFullPathName();
    auto args=juce::StringArray{"--inspect","--plugin",plugin,"--output",output};check(parse(args).inspect);check(parse({"--self-test"}).selfTest);
    const auto renderArgs=juce::StringArray{"--render","--plugin",plugin,"--output",output,"--set","123=0.25","--parameter","456","--values","0,.5,1","--seconds","2","--impulse","mono"};const auto parsed=parse(renderArgs);check(parsed.render&&parsed.values.size()==3&&parsed.baseline.size()==1&&parsed.baseline[0].value==.25f&&parsed.parameter=="456"&&parsed.seconds==2&&parsed.impulse=="mono");
    const auto rejects=[&](juce::StringArray bad){bool rejected=false;try{(void)parse(bad);}catch(const std::exception&){rejected=true;}check(rejected);};
    rejects({"--inspect","--plugin","relative.vst3","--output",output});rejects({"--inspect","--plugin",plugin,"--output",output,"--set","1=0"});
    for(const auto* invalid:{"nan","inf","-0.1","1.1","0.5junk",""}){auto bad=renderArgs;bad.set(bad.indexOf("0,.5,1"),invalid);rejects(bad);}
    auto missing=renderArgs;missing.removeRange(missing.indexOf("--values"),2);rejects(missing);
    auto duplicate=renderArgs;duplicate.addArray(juce::StringArray{"--set","123=.5"});rejects(duplicate);
    const auto noteArgs=juce::StringArray{"--render","--plugin",plugin,"--output",output,"--note","48,55","--velocity","90","--seconds","4","--gate-seconds","3","--parameter","7","--values","0,1"};
    const auto noted=parse(noteArgs);check(noted.notes==std::vector<int>{48,55}&&noted.velocity==90&&noted.gateSeconds==3&&noted.values.size()==2);
    check(parse({"--render","--plugin",plugin,"--output",output,"--note","60","--seconds","4"}).gateSeconds==2);
    check(parse({"--inspect","--plugin",plugin,"--output",output,"--class","Serum 2"}).pluginClass=="Serum 2");
    rejects({"--inspect","--plugin",plugin,"--output",output,"--class",""});rejects({"--inspect","--plugin",plugin,"--output",output,"--class","A","--class","B"});
    rejects({"--inspect","--plugin",plugin,"--output",output,"--note","60"});rejects({"--render","--plugin",plugin,"--output",output,"--note","60","--impulse","mono"});
    rejects({"--render","--plugin",plugin,"--output",output,"--velocity","90"});rejects({"--render","--plugin",plugin,"--output",output,"--note","60","--seconds","2","--gate-seconds","3"});
    for(const auto* invalid:{"128","60.5","60,60","-1",""}){auto bad=noteArgs;bad.set(bad.indexOf("48,55"),invalid);rejects(bad);}
    for(const auto* invalid:{"0","128","64.5"}){auto bad=noteArgs;bad.set(bad.indexOf("90"),invalid);rejects(bad);}
    const auto continuous=samples(std::numeric_limits<int>::max(),false);check(continuous.size()==21&&continuous.front()==0&&continuous.back()==1);
    const auto binary=samples(2,true);check(binary==continuous);const auto stepped=samples(8,true);check(stepped.size()==27&&std::is_sorted(stepped.begin(),stepped.end()));for(int n=0;n<8;++n)check(std::find(stepped.begin(),stepped.end(),float(n)/7)!=stepped.end());
    check(samples(maximumDiscreteSteps+1,true)==continuous);
    for(const auto& pair:std::array<std::pair<double,const char*>,4>{{{2.5,"finite"},{std::numeric_limits<double>::infinity(),"positive_infinity"},{-std::numeric_limits<double>::infinity(),"negative_infinity"},{std::numeric_limits<double>::quiet_NaN(),"nan"}}}){
        J tail=object();putTail(tail,pair.first);const auto encoded=juce::JSON::parse(juce::JSON::toString(tail));
        check(encoded["tail_kind"].toString()==pair.second);check(bool(encoded["tail_finite"])==std::isfinite(pair.first));
        check(std::isfinite(pair.first)?double(encoded["tail_seconds"])==pair.first:encoded["tail_seconds"].isVoid());
    }
    std::cout<<"REFERENCE_PLUGIN_PROBE_SELF_TEST checks="<<checks<<" failures=0; no plugin loaded\n";return 0;
}
}
int main(int argc,char** argv){try{juce::StringArray args;for(int n=1;n<argc;++n)args.add(juce::String::fromUTF8(argv[n]));const auto parsed=parse(args);if(parsed.selfTest)return selfTest();juce::ScopedJuceInitialiser_GUI juce;return run(parsed);}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}}
