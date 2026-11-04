#include "FxSpectrum.h"
#include "FxAnalysisHub.h"
#include "SpectrumPeaks.h"
#include "EqReferenceAnalysis.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <limits>
#include <thread>
#include <juce_audio_formats/juce_audio_formats.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
using studio::FxSpectrum;
int checks=0,failures=0;
void check(bool good,const char* expression,int line){++checks;if(!good){++failures;if(failures<40)std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void matchRemovesGlobalLevelDifference()
{
    std::cout<<"SECTION match_removes_global_level_difference\n";
    studio::EqReference source,reference;
    source.sampleRate=reference.sampleRate=48000;
    source.valid.set();reference.valid.set();
    source.spectrum.db.fill(-36);reference.spectrum.db.fill(-24);
    const auto correction=studio::eqMatchCorrection(source,reference,1.f);
    CHECK(correction.has_value());
    if(correction)for(float db:correction->db)CHECK(std::abs(db)<.001f);
    source.valid.reset();CHECK(!studio::eqMatchCorrection(source,reference,1.f));
}
studio::EqReference flatReference(float db=-36)
{
    studio::EqReference r;r.sampleRate=48000;r.valid.set();r.spectrum.db.fill(db);r.label="Fixture";r.captureId="fixture-1";return r;
}
void matchCorrectionBounds()
{
    std::cout<<"SECTION match_correction_threshold_smoothing_and_limits\n";
    auto source=flatReference(),reference=source;
    for(int i=0;i<129;++i)reference.spectrum.db[std::size_t(i)]=-42+12*float(i)/128;
    const auto tilt=studio::eqMatchCorrection(source,reference,1);CHECK(tilt);
    if(tilt){CHECK(std::abs(tilt->db[16]+4.5f)<.001f);CHECK(std::abs(tilt->db[112]-4.5f)<.001f);}
    reference=source;for(int i=0;i<129;++i)reference.spectrum.db[std::size_t(i)]+=6*std::exp(-.5f*std::pow(float(i-64)/5,2));
    const auto bell=studio::eqMatchCorrection(source,reference,1);CHECK(bell);if(bell){CHECK(bell->db[64]>5.5f&&bell->db[64]<6);CHECK(std::abs(bell->db[0])<.001f);}
    reference.valid.reset(64);const auto missing=studio::eqMatchCorrection(source,reference,1);CHECK(missing);if(missing)CHECK(missing->db[64]==0);
    reference=source;reference.spectrum.db[64]+=3;
    auto correction=studio::eqMatchCorrection(source,reference,1);CHECK(correction);
    if(correction){CHECK(std::abs(correction->db[64]-.6f)<.001f);CHECK(std::abs(correction->db[62]-.6f)<.001f);CHECK(correction->db[61]==0);}
    reference=source;for(int i=20;i<41;++i)reference.spectrum.db[std::size_t(i)]+=20;
    for(float amount:{0.f,.5f,1.f}){correction=studio::eqMatchCorrection(source,reference,amount);CHECK(correction);if(correction)CHECK(std::abs(correction->db[30]-std::min(12.f,20*amount))<.001f);}
    for(int i=20;i<41;++i)reference.spectrum.db[std::size_t(i)]-=40;
    correction=studio::eqMatchCorrection(source,reference,1);CHECK(correction);if(correction)CHECK(correction->db[30]==-12);
    for(float amount:{-.1f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})CHECK(!studio::eqMatchCorrection(source,reference,amount));
    source.valid.reset();for(int i=0;i<95;++i)source.valid.set(std::size_t(i));CHECK(!studio::eqMatchCorrection(source,reference,1));source.valid.set(95);CHECK(studio::eqMatchCorrection(source,reference,1));
    source.spectrum.db[95]=-90;CHECK(!studio::eqMatchCorrection(source,reference,1));source.spectrum.db[95]=-89.99f;CHECK(studio::eqMatchCorrection(source,reference,1));
    source=flatReference();reference=source;reference.spectrum.db.fill(-100);CHECK(!studio::eqMatchCorrection(source,reference,1));reference=source;source.spectrum.db[0]=std::numeric_limits<float>::quiet_NaN();CHECK(!studio::eqMatchCorrection(source,reference,1));
    source=flatReference();source.sampleRate=std::numeric_limits<double>::infinity();CHECK(!studio::eqMatchCorrection(source,reference,1));
}
void matchReferenceCodec()
{
    std::cout<<"SECTION match_reference_strict_codec_and_atomic_refusal\n";
    auto r=flatReference();r.valid.reset(120);r.spectrum.db[17]=-72.25f;
    juce::String text="unchanged";CHECK(studio::encodeEqReference(r,text));CHECK(text.getNumBytesAsUTF8()<=16384);
    studio::EqReference decoded;CHECK(studio::decodeEqReference(text,decoded));CHECK(decoded.sampleRate==r.sampleRate&&decoded.label==r.label&&decoded.captureId==r.captureId&&decoded.valid==r.valid&&decoded.spectrum.db==r.spectrum.db);
    const auto good=text;const auto reject=[&](const juce::String& bad,const char* why="raw"){auto out=r;const auto accepted=studio::decodeEqReference(bad,out);if(accepted)std::cerr<<"UNEXPECTED_REFERENCE "<<why<<' '<<bad.substring(0,160)<<'\n';CHECK(!accepted);CHECK(out.label==r.label&&out.spectrum.db==r.spectrum.db&&out.valid==r.valid);};
    reject(good.replaceFirstOccurrenceOf("{","{\"version\":1,"));reject(good.replaceFirstOccurrenceOf("{","{\"groupAdmission\":true,"));reject(juce::String::repeatedString(" ",16385)+good);reject("{}");reject("[]");
    reject(good.replaceFirstOccurrenceOf("-36.0","NaN"));reject(good.replaceFirstOccurrenceOf("-36.0","1e999"));
    juce::var object;CHECK(juce::JSON::parse(good,object).wasOk());auto* properties=object.getDynamicObject();CHECK(properties);if(!properties)return;
    const auto mutate=[&](const char* key,const juce::var& value){auto copy=object.clone();copy.getDynamicObject()->removeProperty(key);copy.getDynamicObject()->setProperty(key,value);reject(juce::JSON::toString(copy),key);};
    mutate("version",2);mutate("version",juce::int64(4294967297LL));mutate("version","1");mutate("sampleRate",0);mutate("sampleRate",1536001);mutate("sampleRate","48000");mutate("label",juce::String::repeatedString("a",257));mutate("captureId",juce::String::repeatedString("b",129));mutate("format","other");
    juce::Array<juce::var> wrong;wrong.add(-30);mutate("db",wrong);mutate("valid",wrong);
    auto flags=*properties->getProperty("valid").getArray();flags.set(0,1);mutate("valid",flags);
    auto db=*properties->getProperty("db").getArray();db.set(0,"NaN");mutate("db",db);db.set(0,121.);mutate("db",db);
    r.sampleRate=0;text="unchanged";CHECK(!studio::encodeEqReference(r,text));CHECK(text=="unchanged");r=flatReference();r.spectrum.db[0]=std::numeric_limits<float>::infinity();CHECK(!studio::encodeEqReference(r,text));
    r=flatReference();r.sampleRate=384000;CHECK(studio::encodeEqReference(r,text));CHECK(studio::eqMatchCorrection(r,r,1));
}
juce::File referenceWav(const juce::File& root,const juce::String& name,int rate,int channels,double seconds,float amplitude,bool anti=false,bool noise=false,int bits=32,float rightScale=1,bool pcm32=false)
{
    const auto file=root.getChildFile(name);juce::FileOutputStream out(file);CHECK(out.openedOk());
    const auto frames=int(seconds*rate),bytes=frames*channels*(bits/8);
    out.write("RIFF",4);out.writeInt(36+bytes);out.write("WAVEfmt ",8);out.writeInt(16);out.writeShort(short((bits==32&&!pcm32)||bits==64?3:1));out.writeShort(short(channels));out.writeInt(rate);out.writeInt(rate*channels*(bits/8));out.writeShort(short(channels*(bits/8)));out.writeShort(short(bits));out.write("data",4);out.writeInt(bytes);
    const auto fftBin=std::round(632.5*FxSpectrum::fftSize/rate);std::uint32_t rng=123;
    for(int n=0;n<frames;++n){rng=rng*1664525+1013904223;const auto x=amplitude*(noise?float(double(rng)/4294967296.*2-1):float(std::sin(2*std::numbers::pi*fftBin*n/FxSpectrum::fftSize)));
        for(int ch=0;ch<channels;++ch){const auto sample=x*(ch==1?rightScale:1.f)*(anti&&ch==1?-1.f:1.f);
            if(bits==64)out.writeDouble(sample);else if(bits==32&&!pcm32)out.writeFloat(sample);else if(bits==32)out.writeInt(int(std::lround(double(sample)*2147483647)));
            else if(bits==24){const auto value=int(std::lround(double(sample)*8388607));for(int byte=0;byte<3;++byte)out.writeByte(char((value>>(byte*8))&255));}
            else if(bits==8)out.writeByte(char(std::lround(sample*127)+128));else out.writeShort(short(std::lround(sample*32767)));}}
    return file;
}
void matchWavAnalysis()
{
    std::cout<<"SECTION match_bounded_wav_calibration_stereo_validation_cleanup\n";
    const auto parent=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName());
    const auto root=juce::File(parent).getChildFile("eq-match-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());std::atomic_bool cancel=false;
    for(int rate:{44100,48000,96000,192000}){const auto mono=referenceWav(root,"mono-"+juce::String(rate)+".wav",rate,1,2,.5f);const auto stereo=referenceWav(root,"stereo-"+juce::String(rate)+".wav",rate,2,2,.5f,true);const auto a=studio::analyzeEqReferenceWav(mono,cancel),b=studio::analyzeEqReferenceWav(stereo,cancel);CHECK(a&&b);if(a&&b){CHECK(a->sampleRate==rate&&a->valid.count()==129);CHECK(std::abs(a->spectrum.db[64]+6.0206f)<.06f);CHECK(std::abs(a->spectrum.db[64]-b->spectrum.db[64])<.001f);CHECK(a->label==mono.getFileName()&&a->captureId.isNotEmpty());}CHECK(mono.moveFileTo(root.getChildFile("renamed-"+mono.getFileName())));}
    const auto pcm=studio::analyzeEqReferenceWav(referenceWav(root,"pcm16.wav",48000,1,2,.5f,false,false,16),cancel);CHECK(pcm);if(pcm)CHECK(std::abs(pcm->spectrum.db[64]+6.0206f)<.06f);
    const auto extensible=root.getChildFile("extensible.wav");juce::MemoryBlock raw;CHECK(root.getChildFile("pcm16.wav").loadFileAsData(raw));
    {juce::FileOutputStream out(extensible);out.write("RIFF",4);out.writeInt(int(raw.getSize())+16);out.write("WAVEfmt ",8);out.writeInt(40);out.writeShort(short(0xfffe));out.write(static_cast<const char*>(raw.getData())+22,14);out.writeShort(22);out.writeShort(16);out.writeInt(4);out.writeInt(1);out.writeShort(0);out.writeShort(16);const std::array<juce::uint8,8> tail{0x80,0,0,0xaa,0,0x38,0x9b,0x71};out.write(tail.data(),tail.size());out.write("data",4);out.writeInt(int(raw.getSize())-44);out.write(static_cast<const char*>(raw.getData())+44,raw.getSize()-44);}
    const auto extended=studio::analyzeEqReferenceWav(extensible,cancel);CHECK(extended);if(extended)CHECK(std::abs(extended->spectrum.db[64]+6.0206f)<.06f);
    {juce::FileOutputStream out(extensible);CHECK(out.setPosition(38));out.writeShort(0);}CHECK(!studio::analyzeEqReferenceWav(extensible,cancel));
    for(int bits:{8,24,32,64}){const auto r=studio::analyzeEqReferenceWav(referenceWav(root,"format-"+juce::String(bits)+".wav",48000,1,2,.5f,false,false,bits,1,true),cancel);if(!r)std::cerr<<"REJECTED_WAV_BITS "<<bits<<'\n';CHECK(r);if(r)CHECK(std::abs(r->spectrum.db[64]+6.0206f)<.15f);}
    const auto unequal=studio::analyzeEqReferenceWav(referenceWav(root,"unequal.wav",48000,2,2,.5f,true,false,32,.5f),cancel);CHECK(unequal);if(unequal)CHECK(std::abs(unequal->spectrum.db[64]+8.0618f)<.06f);
    auto a=studio::analyzeEqReferenceWav(referenceWav(root,"noise-a.wav",48000,2,2,.1f,false,true),cancel),b=studio::analyzeEqReferenceWav(referenceWav(root,"noise-b.wav",48000,2,2,.2f,false,true),cancel);CHECK(a&&b);if(a&&b)for(int i=0;i<129;++i)CHECK(std::abs(b->spectrum.db[std::size_t(i)]-a->spectrum.db[std::size_t(i)]-6.0206f)<.01f);
    CHECK(!studio::analyzeEqReferenceWav(referenceWav(root,"silent.wav",48000,1,2,0),cancel));CHECK(!studio::analyzeEqReferenceWav(referenceWav(root,"short.wav",48000,1,1.99,.5f),cancel));CHECK(!studio::analyzeEqReferenceWav(referenceWav(root,"long.wav",8000,1,30.01,.5f),cancel));CHECK(!studio::analyzeEqReferenceWav(referenceWav(root,"channels.wav",48000,3,2,.5f),cancel));CHECK(!studio::analyzeEqReferenceWav(referenceWav(root,"rate.wav",192001,1,2,.5f),cancel));
    const auto nan=referenceWav(root,"nan.wav",48000,1,2,.5f);{juce::FileOutputStream out(nan);CHECK(out.setPosition(44));out.writeFloat(std::numeric_limits<float>::quiet_NaN());}CHECK(!studio::analyzeEqReferenceWav(nan,cancel));
    {juce::FileOutputStream out(nan);CHECK(out.setPosition(44));out.writeFloat(std::numeric_limits<float>::infinity());}CHECK(!studio::analyzeEqReferenceWav(nan,cancel));
    const auto exactMaximum=studio::analyzeEqReferenceWav(referenceWav(root,"maximum.wav",8000,1,30,.5f),cancel);CHECK(exactMaximum);CHECK(!studio::analyzeEqReferenceWav(root.getChildFile("missing.wav"),cancel));
    const auto truncated=referenceWav(root,"truncated.wav",48000,1,2,.5f);CHECK(truncated.replaceWithData("RIFF",4));CHECK(!studio::analyzeEqReferenceWav(truncated,cancel));
    const auto oversized=root.getChildFile("oversized.wav");{juce::FileOutputStream out(oversized);CHECK(out.setPosition(256LL*1024*1024));out.writeByte(0);}CHECK(!studio::analyzeEqReferenceWav(oversized,cancel));
    cancel=true;CHECK(!studio::analyzeEqReferenceWav(root.getChildFile("stereo-48000.wav"),cancel));CHECK(root.deleteRecursively());CHECK(!root.exists());
}
void sharedAnalysisOwnership()
{
    std::cout<<"SECTION shared_analysis_ownership\n";
    auto monitor=std::make_shared<FxMonitor>();
    studio::FxAnalysisHub hub(monitor);
    const auto graph=hub.acquire(0),overview=hub.acquire(0);
    CHECK(graph&&overview);CHECK(hub.activeAnalyzers()==1);
    if(graph)hub.release(*graph);
    CHECK(hub.activeAnalyzers()==1);
    if(overview)hub.release(*overview);
    CHECK(hub.activeAnalyzers()==0);
}
void analysisPositionsIdentifyDiscontinuity()
{
    std::cout<<"SECTION capture_stream_positions_and_continuity\n";
    FxMonitor monitor;FxSpectrum spectrum;FxMonitor::ActiveSlots active{};active[0]=true;const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    CHECK(monitor.beginBlock(48000,active));const auto feed=[&](int count){for(int i=0;i<count;++i)monitor.capture(0,{.1,.1},{.1,.1});CHECK(spectrum.update(monitor,*token));};
    feed(FxSpectrum::fftSize);CHECK(spectrum.result().endSample==FxSpectrum::fftSize);const auto continuity=spectrum.result().continuity;CHECK(continuity!=0);
    feed(FxSpectrum::fftSize/2);CHECK(spectrum.result().endSample==FxSpectrum::fftSize*3/2);CHECK(spectrum.result().continuity==continuity);
    monitor.invalidate();CHECK(monitor.beginBlock(48000,active));feed(FxSpectrum::fftSize);CHECK(spectrum.result().continuity!=continuity);
}
void sharedAnalysisBudgetsAndAuthority()
{
    std::cout<<"SECTION hub_budget_generation_audition_and_retirement\n";
    auto monitor=std::make_shared<FxMonitor>();studio::FxAnalysisHub hub(monitor),other(monitor);
    CHECK(!hub.acquire(8));std::array<studio::FxAnalysisHub::Lease,8> leases;
    for(std::uint8_t i=0;i<8;++i){const auto lease=hub.acquire(i);CHECK(lease);if(!lease)return;leases[i]=*lease;}
    CHECK(hub.activeAnalyzers()==8);const auto view=hub.acquire(0);CHECK(view);if(!view)return;
    CHECK(hub.result(*view)==hub.result(leases[0]));CHECK(!other.result(*view));
    other.release(*view);CHECK(hub.activeAnalyzers()==8);CHECK(!other.acquire(0));
    FxMonitor::ActiveSlots active;active.fill(true);
    const auto produce=[&](int packets){CHECK(monitor->beginBlock(192000,active));for(int n=0;n<packets*256;++n)for(std::uint8_t slot=0;slot<8;++slot){const auto x=.25*std::sin(2*std::numbers::pi*128*n/FxSpectrum::fftSize);monitor->capture(slot,{x,-x},{x*.5,-x*.5});}};
    produce(200);
    std::array<int,8> queued{};for(std::uint8_t i=0;i<8;++i)queued[i]=monitor->stats(i).queued;
    std::thread wrong([&]{CHECK(!hub.acquire(0));hub.release(*view);hub.update();CHECK(!hub.result(*view));CHECK(!hub.requestAudition(*view,0));hub.retire();});wrong.join();
    for(std::uint8_t i=0;i<8;++i)CHECK(queued[i]==monitor->stats(i).queued);
    hub.update();CHECK(hub.packetsLastTick()<=1024);
    for(std::uint8_t i=0;i<8;++i){CHECK(hub.packetsLastTick(i)==std::size_t(queued[i]));CHECK(hub.packetsLastTick(i)<=128);const auto* result=hub.result(leases[i]);CHECK(result&&result->valid&&result->windows==1);CHECK(result&&result->sampleRate==192000);}
    const auto first=*hub.result(*view);hub.update();CHECK(hub.packetsLastTick()==0&&hub.result(*view)->windows==first.windows);
    CHECK(hub.requestAudition(leases[0],2));CHECK(hub.requestedAudition(leases[0])==2);CHECK(!hub.requestedAudition(*view));
    CHECK(hub.requestAudition(*view,3,true));CHECK(!hub.requestedAudition(leases[0]));
    hub.release(leases[0]);CHECK(hub.requestedAudition(*view)==3);
    CHECK(!hub.requestAudition(leases[0],1));hub.release(leases[0]);CHECK(hub.activeAnalyzers()==8);
    CHECK(hub.requestAudition(*view,{}));CHECK(!hub.requestedAudition(*view));
    CHECK(hub.setSpectrumEnabled(*view,false));produce(64);hub.update();CHECK(!hub.result(*view)->valid&&hub.result(*view)->windows==0);
    CHECK(hub.setSpectrumEnabled(*view,true));produce(32);hub.update();CHECK(!hub.result(*view)->valid);produce(32);hub.update();CHECK(hub.result(*view)->valid&&hub.result(*view)->windows==1);
    monitor->invalidate();hub.update();CHECK(!hub.result(*view)->valid);produce(64);hub.update();CHECK(hub.result(*view)->valid&&hub.result(*view)->generation!=first.generation);
    CHECK(hub.requestAudition(*view,4));hub.release(*view);CHECK(!hub.requestAudition(*view,5));CHECK(!hub.result(*view));CHECK(hub.activeAnalyzers()==7);
    const auto reopened=hub.acquire(0);CHECK(reopened&&*reopened!=*view);if(reopened)CHECK(hub.result(*reopened)&&!hub.result(*reopened)->valid);
    hub.retire();CHECK(hub.activeAnalyzers()==0&&!hub.acquire(0));CHECK(!hub.result(leases[1]));
    for(std::uint8_t i=0;i<8;++i){const auto token=monitor->subscribe(i);CHECK(token);if(token)CHECK(monitor->retire(*token));}
    hub.update();
}
void referenceBudgetCountsActualReads()
{
    std::cout<<"SECTION spectrum_reference_budget_counts_stale_and_wrong_thread\n";
    FxMonitor monitor;FxSpectrum analyzer;const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    FxMonitor::ActiveSlots active{};active[0]=true;CHECK(monitor.beginBlock(48000,active));
    for(int n=0;n<2560;++n)monitor.capture(0,{.1,.1},{.1,.1});
    monitor.invalidate();CHECK(monitor.beginBlock(48000,active));for(int n=0;n<512;++n)monitor.capture(0,{.2,.2},{.2,.2});
    std::size_t budget=5;std::thread wrong([&]{CHECK(!analyzer.update(monitor,*token,true,budget));});wrong.join();CHECK(budget==5&&monitor.stats(0).queued==12);
    CHECK(!analyzer.update(monitor,*token,true,budget));CHECK(budget==0&&monitor.stats(0).queued==7);
    budget=8;CHECK(!analyzer.update(monitor,*token,false,budget));CHECK(budget==1&&monitor.stats(0).queued==0);CHECK(analyzer.result().sampleRate==48000&&!analyzer.result().valid);
}
void boundedLeasesOwnTheMonitorLifetime()
{
    std::cout<<"SECTION bounded_leases_and_monitor_lifetime\n";
    auto monitor=std::make_shared<FxMonitor>();std::weak_ptr<FxMonitor> lifetime=monitor;
    auto hub=std::make_unique<studio::FxAnalysisHub>(monitor);monitor.reset();CHECK(!lifetime.expired());
    std::array<studio::FxAnalysisHub::Lease,64> leases;
    for(std::size_t n=0;n<leases.size();++n){const auto lease=hub->acquire(std::uint8_t(n%8));CHECK(lease);if(!lease)return;leases[n]=*lease;}
    CHECK(!hub->acquire(0));CHECK(hub->activeAnalyzers()==8);hub->release(leases[0]);const auto next=hub->acquire(0);CHECK(next&&next->id!=leases[0].id);CHECK(!hub->result(leases[0]));
    hub->retire();CHECK(!lifetime.expired());hub.reset();CHECK(lifetime.expired());
}
void detectorMetersDoNotWaitForAnFft()
{
    std::cout<<"SECTION detector_readbacks_before_first_fft_and_epoch_clear\n";
    FxMonitor monitor;FxSpectrum spectrum;FxMonitor::ActiveSlots active{};active[0]=true;const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    CHECK(!spectrum.result().detectorReadbacksValid&&!spectrum.result().externalAvailable);
    DynamicEq eq;DynamicEqSettings settings;auto& band=settings.bands[0];band.active=true;band.autoThreshold=true;band.adaptiveTiming=true;band.detectorSource=DynamicEqDetectorSource::externalFree;band.detectorLowHz=50;band.detectorHighHz=500;
    CHECK(eq.setSettings(settings)&&eq.prepare(48000));CHECK(monitor.beginBlock(48000,active,true));
    for(int n=0;n<256;++n){const DynamicEq::Frame x{.1,-.1},external{.5*std::sin(n*.03),0};monitor.capture(0,x,eq.process(x,external),&eq);}
    CHECK(!spectrum.update(monitor,*token));const auto& result=spectrum.result();CHECK(!result.valid&&result.detectorReadbacksValid&&result.externalAvailable);
    const auto expected=eq.detectorReadbacks();for(std::size_t n=0;n<8;++n){const auto actual=result.detectorReadbacks[n];CHECK(actual.thresholdDb==expected[n].thresholdDb&&actual.attackMs==expected[n].attackMs&&actual.releaseMs==expected[n].releaseMs);}
    CHECK(monitor.beginBlock(48000,active));for(int n=0;n<256;++n)monitor.capture(0,{},{},nullptr);
    CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().detectorReadbacksValid&&!spectrum.result().externalAvailable);
    monitor.invalidate();CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid&&!spectrum.result().detectorReadbacksValid&&!spectrum.result().externalAvailable);
}
void detectorOnlyUpdatesSkipFftHistory()
{
    std::cout<<"SECTION detector_only_consumption_has_no_fft_and_restarts_spectrum_history\n";
    FxMonitor monitor;FxSpectrum spectrum;FxMonitor::ActiveSlots active{};active[0]=true;const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    DynamicEq eq;CHECK(eq.prepare(48000));CHECK(monitor.beginBlock(48000,active,true));int frame=0;
    const auto produce=[&](int count){for(int n=0;n<count;++n,++frame){const auto x=.25*std::sin(2*std::numbers::pi*171*frame/FxSpectrum::fftSize);const DynamicEq::Frame input{x,-x};monitor.capture(0,input,eq.process(input),&eq);}};
    produce(2*FxSpectrum::fftSize);CHECK(!spectrum.update(monitor,*token,false));CHECK(monitor.stats(0).queued==0);
    CHECK(!spectrum.result().valid&&spectrum.result().windows==0&&spectrum.result().detectorReadbacksValid&&spectrum.result().externalAvailable);
    produce(FxSpectrum::fftSize/2);CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    produce(FxSpectrum::fftSize/2);CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().windows==1);
    CHECK(std::abs(spectrum.dbAt(48000.*171/FxSpectrum::fftSize,false)-20*std::log10(.25))<.02);
    produce(256);CHECK(!spectrum.update(monitor,*token,false));CHECK(!spectrum.result().valid&&spectrum.result().windows==0&&spectrum.result().detectorReadbacksValid);
}
void tones()
{
    std::cout<<"SECTION pre_post_stereo_energy_frequency_and_peak_level\n";
    for(double rate:{44100.,48000.,96000.,192000.,768000.})for(int mode=0;mode<3;++mode){
        FxMonitor monitor;const auto token=monitor.subscribe(0);CHECK(token);if(!token)continue;
        FxMonitor::ActiveSlots active{};active[0]=true;CHECK(monitor.beginBlock(rate,active));FxSpectrum spectrum;
        CHECK(!spectrum.result().valid);CHECK(spectrum.dbAt(1000,false)==FxSpectrum::floorDb);
        constexpr int bin=171;const auto frequency=rate*bin/FxSpectrum::fftSize;
        for(int n=0;n<FxSpectrum::fftSize;++n){const auto x=.25*std::sin(2*std::numbers::pi*bin*n/FxSpectrum::fftSize);
            const auto right=mode==0?x:mode==1?-x:0.;monitor.capture(0,{x,right},{x*2,right*2});}
        const auto updated=spectrum.update(monitor,*token);CHECK(updated);CHECK(spectrum.result().valid);if(!updated)continue;
        CHECK(spectrum.result().sampleRate==rate);const auto& result=spectrum.result();
        const auto peak=std::max_element(result.inputDb.begin()+1,result.inputDb.end());CHECK(peak-result.inputDb.begin()==bin);
        const auto expected=20*std::log10(.25)+(mode==2?10*std::log10(.5):0);
        CHECK(std::abs(spectrum.dbAt(frequency,false)-expected)<.02);
        CHECK(std::abs(spectrum.dbAt(frequency,true)-spectrum.dbAt(frequency,false)-20*std::log10(2.))<.02);
        const auto grabbed=studio::findSpectrumPeaks(result);CHECK(grabbed.size>0);
        if(grabbed.size){CHECK(std::abs(grabbed.values[0].frequencyHz-frequency)<rate/FxSpectrum::fftSize*.05);CHECK(std::abs(grabbed.values[0].levelDb-expected)<.02);}
        CHECK(result.windows==1&&result.skipped==0);CHECK(!spectrum.update(monitor,*token));
        for(const auto db:result.inputDb)CHECK(std::isfinite(db)&&db>=FxSpectrum::floorDb&&db<=120);
        for(const auto db:result.outputDb)CHECK(std::isfinite(db)&&db>=FxSpectrum::floorDb&&db<=120);
    }
}
void extremeFloatHeadroom()
{
    std::cout<<"SECTION finite_fft_at_extreme_captured_float_levels\n";
    for(int mode=0;mode<3;++mode){
        FxMonitor monitor;const auto token=monitor.subscribe(0);CHECK(token);if(!token)continue;
        FxMonitor::ActiveSlots active{};active[0]=true;CHECK(monitor.beginBlock(48000,active));FxSpectrum spectrum;
        const double peak=std::numeric_limits<float>::max();
        for(int n=0;n<FxSpectrum::fftSize;++n){const auto x=peak*(mode==0?1.:mode==1?(n%2?1.:-1.):std::sin(2*std::numbers::pi*171*n/FxSpectrum::fftSize));monitor.capture(0,{x,-x},{x*.5,-x*.5});}
        CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().valid);
        for(const auto db:spectrum.result().inputDb)CHECK(std::isfinite(db)&&db>=FxSpectrum::floorDb&&db<=120);
        for(const auto db:spectrum.result().outputDb)CHECK(std::isfinite(db)&&db>=FxSpectrum::floorDb&&db<=120);
        const int bin=mode==0?0:mode==1?FxSpectrum::fftSize/2:171;CHECK(spectrum.dbAt(48000.*bin/FxSpectrum::fftSize,false)==120);CHECK(spectrum.dbAt(48000.*bin/FxSpectrum::fftSize,true)==120);
        monitor.invalidate();for(int n=0;n<FxSpectrum::fftSize;++n)monitor.capture(0,{.25,-.25},{.5,-.5});CHECK(spectrum.update(monitor,*token));CHECK(std::abs(spectrum.dbAt(0,false)-20*std::log10(.25))<.02);
    }
}
void clearingAndBudgets()
{
    std::cout<<"SECTION discontinuities_silence_endpoints_and_bounded_catchup\n";
    FxMonitor monitor;FxMonitor::ActiveSlots active{};active[0]=true;auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    CHECK(monitor.beginBlock(48000,active));FxSpectrum spectrum;int frame=0;
    const auto produce=[&](int samples,int kind){for(int n=0;n<samples;++n,++frame){
        double x=0;if(kind==1)x=.25;if(kind==2)x=frame%2?.25:-.25;
        if(kind==3)x=.25*std::sin(2*std::numbers::pi*171*frame/FxSpectrum::fftSize)+.125*std::sin(2*std::numbers::pi*311*frame/FxSpectrum::fftSize);
        monitor.capture(0,{x,-x},{x,-x});}};
    produce(FxSpectrum::fftSize/2,3);CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    produce(FxSpectrum::fftSize/2,3);CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().windows==1);
    CHECK(std::abs(spectrum.dbAt(48000.*171/FxSpectrum::fftSize,false)-20*std::log10(.25))<.02);
    CHECK(std::abs(spectrum.dbAt(48000.*311/FxSpectrum::fftSize,false)-20*std::log10(.125))<.02);
    monitor.invalidate();CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    produce(FxSpectrum::fftSize,0);CHECK(spectrum.update(monitor,*token));for(auto x:spectrum.result().inputDb)CHECK(x==FxSpectrum::floorDb);
    monitor.invalidate();produce(FxSpectrum::fftSize,1);CHECK(spectrum.update(monitor,*token));CHECK(std::abs(spectrum.dbAt(0,false)-20*std::log10(.25))<.02);
    monitor.invalidate();produce(FxSpectrum::fftSize,2);CHECK(spectrum.update(monitor,*token));CHECK(std::abs(spectrum.dbAt(24000,false)-20*std::log10(.25))<.02);
    CHECK(spectrum.dbAt(-1,false)==FxSpectrum::floorDb&&spectrum.dbAt(24001,false)==FxSpectrum::floorDb);
    CHECK(spectrum.dbAt(std::numeric_limits<double>::quiet_NaN(),false)==FxSpectrum::floorDb);
    CHECK(monitor.beginBlock(96000,active));CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    produce(FxSpectrum::fftSize,3);CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().sampleRate==96000);
    monitor.invalidate();spectrum.clear();produce(3*FxSpectrum::fftSize,3);
    const auto drops=monitor.stats(0).dropped;CHECK(drops>0);CHECK(spectrum.update(monitor,*token));
    CHECK(spectrum.result().windows==1&&spectrum.result().skipped==2);CHECK(monitor.stats(0).queued==0);
    // The next packet follows the dropped interval: start a new FFT window.
    produce(FxSpectrum::fftSize/2,3);CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    produce(FxSpectrum::fftSize/2,3);CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().windows==1);
    CHECK(monitor.retire(*token));CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
    const auto old=*token;token=monitor.subscribe(0);CHECK(token);if(!token)return;CHECK(!monitor.retire(old));CHECK(monitor.beginBlock(48000,active));
    produce(FxSpectrum::fftSize,3);CHECK(spectrum.update(monitor,*token));CHECK(spectrum.result().sampleRate==48000);
    active[0]=false;CHECK(monitor.beginBlock(48000,active));CHECK(!spectrum.update(monitor,*token));CHECK(!spectrum.result().valid);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool watching=false;thread_local int allocations=0,frees=0;
int heapHook(int operation,void*,std::size_t,int type,long,const unsigned char*,int)
{if(watching&&type!=_CRT_BLOCK){if(operation==_HOOK_FREE)++frees;else ++allocations;}return 1;}
#endif
void preparedWork()
{
    std::cout<<"SECTION prepared_off_callback_fft_work_storage\n";
    FxMonitor monitor;const auto token=monitor.subscribe(0);CHECK(token);if(!token)return;
    FxMonitor::ActiveSlots active{};active[0]=true;CHECK(monitor.beginBlock(48000,active));FxSpectrum spectrum;
    for(int n=0;n<FxSpectrum::fftSize;++n){const auto x=.25*std::sin(n*.13);monitor.capture(0,{x,-x},{x,-x});}
    bool wrongThreadUpdated=false;std::jthread wrongThread([&]{wrongThreadUpdated=spectrum.update(monitor,*token);});wrongThread.join();
    CHECK(!wrongThreadUpdated);CHECK(!spectrum.result().valid);CHECK(monitor.stats(0).queued==FxSpectrum::fftSize/FxMonitor::packetFrames);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto old=_CrtSetAllocHook(heapHook);allocations=frees=0;watching=true;
#endif
    const auto updated=spectrum.update(monitor,*token);
#if defined(_MSC_VER) && defined(_DEBUG)
    watching=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);std::cout<<"FFT allocations="<<allocations<<" frees="<<frees<<'\n';
#endif
    CHECK(updated);CHECK(spectrum.result().valid);
}
void resetPeakFixture(FxSpectrum::Result& result,double rate=48000)
{
    result.valid=true;result.sampleRate=rate;result.generation=1;result.windows=1;result.skipped=0;
    result.inputDb.fill(FxSpectrum::floorDb);result.outputDb.fill(FxSpectrum::floorDb);result.movementDb.fill(0);
}
void addPeak(std::array<float,FxSpectrum::bins>& levels,double center,float level=-12,float width=3)
{
    for(int bin=std::max(0,int(center)-16);bin<=std::min(FxSpectrum::bins-1,int(center)+16);++bin){
        const auto x=(bin-center)/width;levels[std::size_t(bin)]=std::max(levels[std::size_t(bin)],float(level-3*x*x));
    }
}
void spectrumGrabPeaks()
{
    std::cout<<"SECTION bounded_spectrum_grab_peak_frequency_width_and_order\n";
    for(const auto rate:{44100.,48000.,96000.,192000.,1536000.}){
        auto storage=std::make_unique<FxSpectrum::Result>();auto& result=*storage;resetPeakFixture(result,rate);CHECK(studio::findSpectrumPeaks(result).size==0);
        constexpr double center=100.25;addPeak(result.inputDb,center);
        auto peaks=studio::findSpectrumPeaks(result);CHECK(peaks.size==1);
        if(peaks.size){const auto& peak=peaks.values[0];
            CHECK(std::abs(peak.frequencyHz-rate*center/FxSpectrum::fftSize)<.001);
            CHECK(std::abs(peak.levelDb+12)<.1f);CHECK(peak.prominenceDb>=6);
            CHECK(std::abs(peak.q-float(center/6))<.2f);
        }
        CHECK(studio::findSpectrumPeaks(result,true).size==0);
        addPeak(result.outputDb,150,-18,6);peaks=studio::findSpectrumPeaks(result,true);CHECK(peaks.size==1);
        if(peaks.size)CHECK(std::abs(peaks.values[0].frequencyHz-rate*150/FxSpectrum::fftSize)<.001);
    }
    auto storage=std::make_unique<FxSpectrum::Result>();auto& result=*storage;resetPeakFixture(result);addPeak(result.inputDb,100,-18,.5f);addPeak(result.inputDb,105,-6,.5f);
    auto peaks=studio::findSpectrumPeaks(result);CHECK(peaks.size==1);
    if(peaks.size)CHECK(std::abs(peaks.values[0].frequencyHz-48000.*105/FxSpectrum::fftSize)<1);
    resetPeakFixture(result);for(const int bin:{30,60,120,240,480,960,1920,3840,5000,6500})addPeak(result.inputDb,bin,-12,1);
    peaks=studio::findSpectrumPeaks(result);CHECK(peaks.size==8);
    for(std::size_t n=1;n<peaks.size;++n){CHECK(peaks.values[n-1].frequencyHz<peaks.values[n].frequencyHz);CHECK(std::log2(peaks.values[n].frequencyHz/peaks.values[n-1].frequencyHz)>=1./12.);}
    if(peaks.size==8)CHECK(std::abs(peaks.values[7].frequencyHz-48000.*3840/FxSpectrum::fftSize)<.001);
    resetPeakFixture(result);addPeak(result.inputDb,100,-30,1);addPeak(result.inputDb,300,-6,1);addPeak(result.inputDb,1000,-18,1);
    peaks=studio::findSpectrumPeaks(result);CHECK(peaks.size==3);
    if(peaks.size==3){CHECK(peaks.values[0].frequencyHz>peaks.values[2].frequencyHz);CHECK(peaks.values[0].prominenceDb>peaks.values[1].prominenceDb);CHECK(peaks.values[1].prominenceDb>peaks.values[2].prominenceDb);}
    resetPeakFixture(result);result.inputDb[100]=result.inputDb[101]=-12;peaks=studio::findSpectrumPeaks(result);CHECK(peaks.size==1);
    if(peaks.size)CHECK(std::abs(peaks.values[0].frequencyHz-48000.*100.5/FxSpectrum::fftSize)<.001);
    resetPeakFixture(result);result.inputDb[0]=0;result.inputDb.back()=0;CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result);addPeak(result.inputDb,7000);CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result);addPeak(result.inputDb,2,-6,.5f);CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result,8000);addPeak(result.inputDb,8100,-6,.5f);CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result);result.inputDb[99]=-18;result.inputDb[100]=-13;result.inputDb[101]=-18;
    result.inputDb.fill(-18);result.inputDb[100]=-13;CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result);addPeak(result.inputDb,100);result.valid=false;CHECK(studio::findSpectrumPeaks(result).size==0);
    result.valid=true;result.generation=0;CHECK(studio::findSpectrumPeaks(result).size==0);
    result.generation=1;result.windows=0;CHECK(studio::findSpectrumPeaks(result).size==0);
    result.windows=1;result.sampleRate=0;CHECK(studio::findSpectrumPeaks(result).size==0);
    result.sampleRate=std::numeric_limits<double>::infinity();CHECK(studio::findSpectrumPeaks(result).size==0);
    result.sampleRate=48000;result.inputDb[300]=std::numeric_limits<float>::quiet_NaN();CHECK(studio::findSpectrumPeaks(result).size==0);
    result.inputDb[300]=-120;result.inputDb[10]=std::numeric_limits<float>::infinity();CHECK(studio::findSpectrumPeaks(result).size==0);
    resetPeakFixture(result);addPeak(result.inputDb,100);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto old=_CrtSetAllocHook(heapHook);allocations=frees=0;watching=true;
#endif
    peaks=studio::findSpectrumPeaks(result);
#if defined(_MSC_VER) && defined(_DEBUG)
    watching=false;_CrtSetAllocHook(old);CHECK(allocations==0&&frees==0);
#endif
    CHECK(peaks.size==1);
}
}
int main(){analysisPositionsIdentifyDiscontinuity();matchRemovesGlobalLevelDifference();matchCorrectionBounds();matchReferenceCodec();matchWavAnalysis();sharedAnalysisOwnership();boundedLeasesOwnTheMonitorLifetime();sharedAnalysisBudgetsAndAuthority();referenceBudgetCountsActualReads();detectorOnlyUpdatesSkipFftHistory();detectorMetersDoNotWaitForAnFft();tones();extremeFloatHeadroom();clearingAndBudgets();preparedWork();spectrumGrabPeaks();std::cout<<"FX spectrum: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
