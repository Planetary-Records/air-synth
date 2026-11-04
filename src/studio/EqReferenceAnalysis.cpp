#include "EqReferenceAnalysis.h"
#include "StrictJson.h"
#include "FxSpectrum.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numeric>

namespace airack::studio
{
namespace
{
constexpr std::size_t count=129,maximumJsonBytes=16384;
constexpr juce::int64 maximumWavBytes=256LL*1024*1024;
double frequency(std::size_t i){return 20*std::pow(1000.,double(i)/128);}
bool numeric(const juce::var& v){return v.isInt()||v.isInt64()||v.isDouble();}
bool validReference(const EqReference& r,bool identity)
{
    if(!std::isfinite(r.sampleRate)||r.sampleRate<=0||r.sampleRate>1536000)return false;
    if(identity&&(r.label.isEmpty()||r.captureId.isEmpty()||r.label.getNumBytesAsUTF8()>256||r.captureId.getNumBytesAsUTF8()>128))return false;
    for(std::size_t i=0;i<count;++i)if(!std::isfinite(r.spectrum.db[i])||r.spectrum.db[i]<-120||r.spectrum.db[i]>120||(r.valid[i]&&frequency(i)>r.sampleRate*.5))return false;
    return true;
}
struct WavLayout {int rate=0,channels=0,bits=0;juce::int64 frames=0,bytes=0,dataStart=0;};
std::optional<WavLayout> inspectWav(const juce::File& file,const std::atomic_bool& cancel)
{
    juce::FileInputStream in(file);const auto bytes=in.getTotalLength();
    if(cancel.load()||!in.openedOk()||bytes<44||bytes>maximumWavBytes||in.readInt()!=0x46464952)return {};
    if(juce::uint32(in.readInt())+juce::int64(8)!=bytes||in.readInt()!=0x45564157)return {};
    WavLayout result;result.bytes=bytes;juce::int64 dataBytes=-1;bool haveFormat=false;
    for(int chunks=0;in.getPosition()<bytes;++chunks){
        if(chunks==4096||cancel.load()||bytes-in.getPosition()<8)return {};
        const auto type=in.readInt();const auto size=juce::int64(juce::uint32(in.readInt())),start=in.getPosition(),end=start+size;
        if(end>bytes||end+(size&1)>bytes)return {};
        if(type==0x20746d66){
            if(haveFormat||size<16)return {};haveFormat=true;
            std::array<juce::uint8,40> fmt{};const auto read=int(std::min<juce::int64>(40,size));if(in.read(fmt.data(),read)!=read)return {};
            auto code=int(juce::ByteOrder::littleEndianShort(fmt.data()));result.channels=juce::ByteOrder::littleEndianShort(fmt.data()+2);result.rate=int(juce::ByteOrder::littleEndianInt(fmt.data()+4));result.bits=juce::ByteOrder::littleEndianShort(fmt.data()+14);
            if(code==0xfffe){
                static constexpr std::array<juce::uint8,14> tail{0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                const auto extension=juce::ByteOrder::littleEndianShort(fmt.data()+16),validBits=juce::ByteOrder::littleEndianShort(fmt.data()+18);
                if(size<40||extension<22||extension>size-18||validBits==0||validBits>result.bits||!std::equal(tail.begin(),tail.end(),fmt.begin()+26))return {};
                code=juce::ByteOrder::littleEndianShort(fmt.data()+24);
            }
            if(result.rate<=0||result.rate>192000||result.channels<1||result.channels>2)return {};
            if(!((code==1&&(result.bits==8||result.bits==16||result.bits==24||result.bits==32))||(code==3&&(result.bits==32||result.bits==64))))return {};
            const auto align=result.channels*(result.bits/8);
            if(juce::ByteOrder::littleEndianShort(fmt.data()+12)!=align||juce::ByteOrder::littleEndianInt(fmt.data()+8)!=juce::uint32(result.rate*align))return {};
        }else if(type==0x61746164){if(dataBytes>=0)return {};dataBytes=size;result.dataStart=start;}
        if(!in.setPosition(end+(size&1)))return {};
    }
    if(!haveFormat||dataBytes<0)return {};
    const auto align=result.channels*(result.bits/8);if(dataBytes%align)return {};
    result.frames=dataBytes/align;
    if(result.frames<2LL*result.rate||result.frames>30LL*result.rate)return {};
    return result;
}
}
std::optional<EqReference> analyzeEqReferenceWav(const juce::File& file,const std::atomic_bool& cancel)
{
    const auto layout=inspectWav(file,cancel);if(!layout)return {};
    auto stream=file.createInputStream();if(!stream)return {};
    std::unique_ptr<juce::AudioFormatReader> reader;
    if(layout->bits==64){if(!stream->setPosition(layout->dataStart))return {};}
    else{
        juce::WavAudioFormat format;reader.reset(format.createReaderFor(stream.release(),true));
        if(!reader||reader->sampleRate!=layout->rate||reader->numChannels!=unsigned(layout->channels)||reader->lengthInSamples!=layout->frames||reader->bitsPerSample!=unsigned(layout->bits))return {};
    }
    struct Work {
        juce::dsp::FFT fft{FxSpectrum::fftOrder};
        juce::AudioBuffer<float> decoded{2,4096};
        std::array<juce::uint8,4096*2*8> float64Bytes{};
        std::array<std::array<float,FxSpectrum::fftSize>,2> ring{};
        std::array<float,FxSpectrum::fftSize> window{};
        std::array<std::complex<float>,FxSpectrum::fftSize> input{},output{};
        std::array<double,FxSpectrum::bins> power{};
    };
    auto work=std::make_unique<Work>();auto& w=*work;
    juce::dsp::WindowingFunction<float>::fillWindowingTables(w.window.data(),w.window.size(),juce::dsp::WindowingFunction<float>::hann,false);
    const auto windowSum=std::accumulate(w.window.begin(),w.window.end(),0.);std::size_t write=0;juce::int64 total=0;std::uint64_t windows=0;
    for(juce::int64 frame=0;frame<layout->frames;){
        if(cancel.load())return {};
        const auto n=int(std::min<juce::int64>(4096,layout->frames-frame));
        if(layout->bits==64){
            // JUCE's WAV decoder handles PCM/float32; decode float64 in the same
            // fixed4096-frame scratch rather than buffering the selected file.
            const auto bytes=n*layout->channels*8;if(stream->read(w.float64Bytes.data(),bytes)!=bytes)return {};
            for(int i=0;i<n;++i)for(int ch=0;ch<layout->channels;++ch){const auto sample=std::bit_cast<double>(juce::ByteOrder::littleEndianInt64(w.float64Bytes.data()+8*(i*layout->channels+ch)));if(!std::isfinite(sample))return {};w.decoded.setSample(ch,i,float(sample));}
        }else if(!reader->read(&w.decoded,0,n,frame,true,layout->channels==2))return {};
        for(int i=0;i<n;++i){
            for(int channel=0;channel<layout->channels;++channel){const auto sample=w.decoded.getSample(channel,i);if(!std::isfinite(sample))return {};w.ring[std::size_t(channel)][write]=sample;}
            write=(write+1)%FxSpectrum::fftSize;++total;
            if(total>=FxSpectrum::fftSize&&(total-FxSpectrum::fftSize)%(FxSpectrum::fftSize/2)==0){
                if(cancel.load())return {}; ++windows;
                // Average channel energies; summing L+R would erase anti-phase audio.
                for(int channel=0;channel<layout->channels;++channel){
                    for(std::size_t k=0;k<w.input.size();++k)w.input[k]={w.ring[std::size_t(channel)][(write+k)%FxSpectrum::fftSize]*w.window[k],0};
                    w.fft.perform(w.input.data(),w.output.data(),false);
                    for(std::size_t k=0;k<w.power.size();++k){const auto re=double(w.output[k].real()),im=double(w.output[k].imag());const auto power=re*re+im*im;if(!std::isfinite(power))return {};w.power[k]+=power;}
                }
            }
        }
        frame+=n;
    }
    if(!windows||cancel.load()||file.getSize()!=layout->bytes)return {};
    EqReference r;r.sampleRate=layout->rate;r.label=file.getFileName();while(r.label.getNumBytesAsUTF8()>256)r.label=r.label.dropLastCharacters(1);r.captureId=juce::Uuid().toString();r.spectrum.db.fill(-120);
    double maximum=-120;
    for(std::size_t i=0;i<count;++i){
        const auto centre=frequency(i);if(centre>layout->rate*.5)continue;
        const auto low=std::max(1,int(std::ceil(centre/std::pow(1000.,.5/128)*FxSpectrum::fftSize/layout->rate)));
        const auto high=std::min(FxSpectrum::bins-1,int(std::floor(centre*std::pow(1000.,.5/128)*FxSpectrum::fftSize/layout->rate)));
        double energy=0;
        for(int k=low;k<=high;++k){const auto scale=(k==FxSpectrum::bins-1?1.:2.)/windowSum;energy=std::max(energy,w.power[std::size_t(k)]*scale*scale/(double(windows)*layout->channels));}
        if(low>high){
            // A log cell narrower than an FFT bin uses the measured neighbours;
            // interpolation does not increase the FFT's frequency resolution.
            const auto position=centre*FxSpectrum::fftSize/layout->rate;const auto lo=std::size_t(std::floor(position)),hi=std::min(lo+1,w.power.size()-1);
            const auto calibrated=[&](std::size_t k){const auto scale=(k==0||k==w.power.size()-1?1.:2.)/windowSum;return w.power[k]*scale*scale/(double(windows)*layout->channels);};
            energy=std::lerp(calibrated(lo),calibrated(hi),position-lo);
        }
        const auto db=std::clamp(10*std::log10(std::max(energy,1e-12)),-120.,120.);r.spectrum.db[i]=float(db);r.valid.set(i);maximum=std::max(maximum,db);
    }
    if(cancel.load()||maximum<=-90||!validReference(r,true))return {};
    return r;
}
bool encodeEqReference(const EqReference& r,juce::String& output)
{
    if(!validReference(r,true))return false;
    juce::var object(new juce::DynamicObject);auto* p=object.getDynamicObject();p->setProperty("format","airack.eq-reference");p->setProperty("version",1);p->setProperty("sampleRate",r.sampleRate);p->setProperty("label",r.label);p->setProperty("captureId",r.captureId);
    juce::Array<juce::var> db,valid;for(std::size_t i=0;i<count;++i){db.add(double(r.spectrum.db[i]));valid.add(bool(r.valid[i]));}
    p->setProperty("db",db);p->setProperty("valid",valid);auto text=juce::JSON::toString(object,true);
    if(text.getNumBytesAsUTF8()>maximumJsonBytes)return false;output=std::move(text);return true;
}
bool decodeEqReference(const juce::String& text,EqReference& output)
{
    if(text.getNumBytesAsUTF8()>maximumJsonBytes)return false;juce::var object;
    if(!ai::parseStrictJson(std::string_view(text.toRawUTF8(),text.getNumBytesAsUTF8()),object,maximumJsonBytes))return false;
    const auto* p=object.getDynamicObject();if(!p||p->getProperties().size()!=7)return false;
    for(const auto* key:{"format","version","sampleRate","label","captureId","db","valid"})if(!p->hasProperty(key))return false;
    const auto v=p->getProperty("version"),rate=p->getProperty("sampleRate");
    if(!p->getProperty("format").isString()||p->getProperty("format").toString()!="airack.eq-reference"||!(v.isInt()||v.isInt64())||juce::int64(v)!=1||!numeric(rate)||!p->getProperty("label").isString()||!p->getProperty("captureId").isString())return false;
    const auto* db=p->getProperty("db").getArray();const auto* valid=p->getProperty("valid").getArray();if(!db||!valid||db->size()!=int(count)||valid->size()!=int(count))return false;
    EqReference r;r.sampleRate=double(rate);r.label=p->getProperty("label").toString();r.captureId=p->getProperty("captureId").toString();
    for(std::size_t i=0;i<count;++i){const auto value=(*db)[int(i)],flag=(*valid)[int(i)];if(!numeric(value)||!flag.isBool())return false;r.spectrum.db[i]=float(double(value));r.valid[i]=bool(flag);}
    if(!validReference(r,true))return false;output=std::move(r);return true;
}
std::optional<EqCurve> eqMatchCorrection(const EqReference& source,const EqReference& reference,float amount)
{
    if(!std::isfinite(amount)||amount<0||amount>1||!validReference(source,false)||!validReference(reference,false))return {};
    std::array<float,count> differences{},sorted{};std::bitset<count> common;std::size_t used=0;
    for(std::size_t i=0;i<count;++i)if(source.valid[i]&&reference.valid[i]&&source.spectrum.db[i]>-90&&reference.spectrum.db[i]>-90){common.set(i);differences[i]=reference.spectrum.db[i]-source.spectrum.db[i];sorted[used++]=differences[i];}
    if(used<96)return {};
    std::sort(sorted.begin(),sorted.begin()+used);const auto median=used%2?sorted[used/2]:(sorted[used/2-1]+sorted[used/2])*.5f;EqCurve correction;
    const auto stepOctaves=std::log2(1000.)/128;
    for(std::size_t i=0;i<count;++i)if(common[i]){
        double sum=0;int samples=0;
        for(std::size_t j=0;j<count;++j)if(common[j]&&std::abs(double(i)-double(j))*stepOctaves<=1./6){sum+=differences[j]-median;++samples;}
        correction.db[i]=float(std::clamp(sum/samples*amount,-12.,12.));
    }
    return correction;
}
}
