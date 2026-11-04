#include "LocalPreview.h"
#include "DrumDocument.h"
#include "PatternIO.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace airack::ai
{
namespace {
template<class F> bool allocating(F&& f) {
    try{return f();}catch(const std::bad_alloc&){return false;}catch(const std::length_error&){return false;}
}
bool nameValid(std::string_view s) noexcept {
    return s.size()<=64&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;})
        &&(s.empty()||juce::CharPointer_UTF8::isValidString(s.data(),int(s.size())));
}
template<class U> void append(std::vector<std::byte>& bytes,U n) {
    static_assert(std::is_unsigned_v<U>);
    for(std::size_t i=0;i<sizeof(U);++i)bytes.push_back(std::byte(static_cast<unsigned char>(n>>(8*i))));
}
void appendRaw(std::vector<std::byte>& bytes,const void* data,std::size_t size) {
    const auto* p=static_cast<const std::byte*>(data);bytes.insert(bytes.end(),p,p+size);
}
struct MapReader {
    std::span<const std::byte> bytes;std::size_t position=0;
    bool raw(void* output,std::size_t size) {
        if(size>bytes.size()-position)return false;
        if(size)std::memcpy(output,bytes.data()+position,size);position+=size;return true;
    }
    template<class U> bool integer(U& output) {
        static_assert(std::is_unsigned_v<U>);
        if(sizeof(U)>bytes.size()-position)return false;
        U v=0;for(std::size_t i=0;i<sizeof(U);++i)v|=U(U(std::to_integer<unsigned char>(bytes[position++]))<<(8*i));
        output=v;return true;
    }
};
bool digest(std::string_view domain,const juce::MemoryBlock& bytes,PreviewDigest& output) {
    std::vector<std::byte> input;input.reserve(domain.size()+8+bytes.getSize());
    appendRaw(input,domain.data(),domain.size());append(input,std::uint64_t(bytes.getSize()));appendRaw(input,bytes.getData(),bytes.getSize());
    const auto hash=juce::SHA256(input.data(),input.size()).getRawData();
    std::copy(hash.begin(),hash.end(),output.begin());return true;
}
bool sameReal(double a,double b) noexcept{return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
bool zeroMeasurements(const AudioMeasurements& m) noexcept {
    return m.finite&&m.clippedSamples==0&&sameReal(m.peak,0)&&sameReal(m.rms,0)&&!m.onsetSeconds
        &&sameReal(m.activityEndSeconds,0)&&sameReal(m.decaySeconds,0)&&!m.tailActiveAtEnd
        &&std::all_of(m.bandFractions.begin(),m.bandFractions.end(),[](double n){return sameReal(n,0);});
}
}
AuditionContext previewContextFor(const GenerationRequest& r,const std::optional<PatternDocument>& proposed,
    const PreviewSampleMap* staged,const SoundDocument* admitted)
{
    AuditionContext context;
    context.bpm = r.capturedBpm.value_or(120);
    context.tempoSource = r.capturedBpm ? TempoSource::captured : TempoSource::defaultTempo;
    if (proposed) { context.phrase = *proposed; context.phraseSource = PhraseSource::proposed; }
    else if (r.capturedPhrase) context.phrase = *r.capturedPhrase;
    else if (r.source.pattern.noteCount != 0) context.phrase = r.source.pattern;
    else
    {
        context.phraseSource = PhraseSource::defaultNotes;
        context.phrase.noteCount = 4;
        for (std::size_t i = 0; i < 4; ++i) context.phrase.notes[i] = {static_cast<double>(i), .25, 60, 96, 1};
        if(staged){const auto& sound=admitted?*admitted:r.source.sound;std::array<std::uint8_t,4> notes{};std::size_t count=0;
            const auto append=[&](std::size_t bank,std::size_t pad){const auto index=drumParameterStart+bank*drumParametersPerSlot+pad*drumParametersPerPad+7;
                const auto pitch=std::uint8_t(std::clamp(std::lround(parameterPhysicalValue(sound.values,index)),0l,127l));
                if(count<notes.size()&&std::find(notes.begin(),notes.begin()+count,pitch)==notes.begin()+count)notes[count++]=pitch;};
            for(std::uint8_t bank=0;bank<4;++bank)if(sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(sound,bank)==NativeModule::drum)
                for(std::size_t pad=0;pad<16;++pad)if(staged->pads[bank*16+pad].asset)append(bank,pad);
            if(!count)for(std::uint8_t bank=0;bank<4;++bank)if(sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(sound,bank)==NativeModule::drum)
                for(std::size_t pad=0;pad<16&&count<notes.size();++pad)append(bank,pad);
            if(count){context.phrase.notes={};context.phrase.noteCount=std::uint16_t(count);for(std::size_t n=0;n<count;++n)context.phrase.notes[n]={double(n),.25,notes[n],96,1};}
        }
    }
    return context;
}
bool validPreviewMap(const PreviewSampleMap& map) noexcept {
    if(map.assets.size()>64)return false;
    std::array<bool,64> used{};
    for(std::size_t i=0;i<map.assets.size();++i){const auto& a=map.assets[i];
        if((a.channels!=1&&a.channels!=2)||!a.frames||a.frames>480000||(i&&!(map.assets[i-1].id<a.id)))return false;
    }
    for(std::size_t i=0;i<map.banks.size();++i){const auto& b=map.banks[i];
        if(b.unit>8||(b.rack.isNull()&&b.unit))return false;
        for(std::size_t j=0;j<i;++j)if(!b.rack.isNull()&&map.banks[j].rack==b.rack&&map.banks[j].unit==b.unit)return false;
    }
    for(const auto& p:map.pads){if(p.asset>map.assets.size()||!nameValid(p.name))return false;if(p.asset)used[p.asset-1]=true;}
    return std::all_of(used.begin(),used.begin()+map.assets.size(),[](bool n){return n;});
}
bool encodePreviewMap(const PreviewSampleMap& map,juce::MemoryBlock& destination) {
    return allocating([&]{if(!validPreviewMap(map))return false;std::vector<std::byte> b;b.reserve(maximumPreviewMapBytes);append(b,std::uint16_t(1));
        for(const auto& bank:map.banks){appendRaw(b,bank.rack.getRawData(),16);append(b,bank.unit);append(b,bank.generation);}
        append(b,std::uint8_t(map.assets.size()));
        for(const auto& a:map.assets){appendRaw(b,a.id.data(),a.id.size());append(b,a.channels);append(b,a.frames);}
        for(const auto& p:map.pads){append(b,p.asset);append(b,std::uint32_t(p.name.size()));appendRaw(b,p.name.data(),p.name.size());appendRaw(b,p.metadata.data(),p.metadata.size());}
        if(b.size()>maximumPreviewMapBytes)return false;juce::MemoryBlock result(b.data(),b.size());destination.swapWith(result);return true;});
}
bool decodePreviewMap(std::span<const std::byte> bytes,PreviewSampleMap& destination) {
    return allocating([&]{if(bytes.size()>maximumPreviewMapBytes||bytes.size()<2+4*25+1+64*37)return false;
        MapReader reader{bytes};PreviewSampleMap map;std::uint16_t version;std::uint8_t count;
        if(!reader.integer(version)||version!=1)return false;
        for(auto& bank:map.banks){std::array<std::uint8_t,16> id{};
            if(!reader.raw(id.data(),id.size())||!reader.integer(bank.unit)||!reader.integer(bank.generation))return false;bank.rack=juce::Uuid(id.data());}
        if(!reader.integer(count)||count>64||std::size_t(count)*37>bytes.size()-reader.position)return false;
        map.assets.resize(count);
        for(auto& a:map.assets)if(!reader.raw(a.id.data(),a.id.size())||!reader.integer(a.channels)||!reader.integer(a.frames))return false;
        for(auto& p:map.pads){std::uint32_t size;
            if(!reader.integer(p.asset)||!reader.integer(size)||size>64||size>bytes.size()-reader.position)return false;
            const auto text=bytes.subspan(reader.position,size);const std::string_view name(reinterpret_cast<const char*>(text.data()),text.size());
            if(!nameValid(name))return false;p.name.assign(name);reader.position+=size;
            if(!reader.raw(p.metadata.data(),p.metadata.size()))return false;}
        if(reader.position!=bytes.size()||!validPreviewMap(map))return false;destination=std::move(map);return true;});
}
bool digestPreviewPad(const drums::DrumPad& pad,PreviewDigest& output) {
    return allocating([&]{auto slots=std::make_unique<drums::DrumSlots>();(*slots)[0][0]=pad;
        drums::DrumDocumentPtr document;drums::DrumError error;juce::MemoryBlock bytes;
        return drums::makeDrumDocument(*slots,document,error)&&drums::encodeDrumDocument(*document,bytes,error)
            &&digest("airack.local.preview.pad.v1",bytes,output);});
}
bool digestPreviewMap(const PreviewSampleMap& map,PreviewDigest& output) {
    return allocating([&]{juce::MemoryBlock bytes;return encodePreviewMap(map,bytes)&&digest("airack.local.preview.map.v1",bytes,output);});
}
bool digestPreviewCandidate(const ProjectDocument& candidate,PreviewDigest& output) {
    return allocating([&]{juce::MemoryBlock bytes;return encodeProjectState(candidate,bytes)&&digest("airack.local.preview.candidate.v1",bytes,output);});
}
bool digestPreviewContext(const AuditionContext& context,PreviewDigest& output) {
    return allocating([&]{if(!validatePatternDocument(context.phrase)||!std::isfinite(context.bpm)||context.bpm<20||context.bpm>400
            ||context.phraseSource>PhraseSource::defaultNotes||context.tempoSource>TempoSource::defaultTempo)return false;
        juce::MemoryBlock pattern;if(!encodePatternAsset(context.phrase,pattern))return false;
        const double requested=std::floor(patternLengthBeats(context.phrase)*60/context.bpm*previewSampleRate+.5);
        const auto body=std::uint32_t(std::max(1.,std::min(requested,double(maximumPreviewBodyFrames))));
        std::vector<std::byte> b;b.reserve(pattern.getSize()+32);append(b,std::uint32_t(pattern.getSize()));appendRaw(b,pattern.getData(),pattern.getSize());
        append(b,std::bit_cast<std::uint64_t>(context.bpm));append(b,std::uint8_t(context.phraseSource));append(b,std::uint8_t(context.tempoSource));
        append(b,body);append(b,std::uint32_t(body+previewTailFrames));append(b,std::uint8_t(requested>maximumPreviewBodyFrames));
        const juce::MemoryBlock bytes(b.data(),b.size());return digest("airack.local.preview.context.v1",bytes,output);});
}
bool stagePreviewMap(const PreviewSampleMap& source,const SourceAssignmentTrace& trace,PreviewSampleMap& destination) {
    return allocating([&]{if(!validPreviewMap(source)||trace.count>trace.effects.size())return false;
        auto result=std::make_unique<PreviewSampleMap>(source);PreviewPadRef empty;
        if(!digestPreviewPad({},empty.metadata))return false;
        for(std::size_t first=0;first<trace.count;){std::size_t end=first+1;
            while(end<trace.count&&trace.effects[end].operation==trace.effects[first].operation)++end;
            const auto entryPads=result->pads;const auto entryBanks=result->banks;
            for(std::size_t i=first;i<end;++i){const auto& e=trace.effects[i];
                if(e.bank>=4||e.kind>SourceAssignmentEffectKind::copy||e.beforeUnit>8||e.afterUnit>8
                    ||entryBanks[e.bank].rack!=e.beforeOwner||entryBanks[e.bank].unit!=e.beforeUnit)return false;
                if(e.kind==SourceAssignmentEffectKind::clear)std::fill_n(result->pads.begin()+e.bank*16,16,empty);
                if(e.kind==SourceAssignmentEffectKind::copy){if(e.copyFrom>=4)return false;std::copy_n(entryPads.begin()+e.copyFrom*16,16,result->pads.begin()+e.bank*16);}
                auto& bank=result->banks[e.bank];bank.rack=e.afterOwner;bank.unit=e.afterUnit;
                if(e.kind!=SourceAssignmentEffectKind::preserve||e.beforeOwner!=e.afterOwner||e.beforeUnit!=e.afterUnit||e.freshLogicalOwner)bank.generation=0;
            }first=end;
        }
        std::array<bool,64> used{};std::array<std::uint8_t,64> indices{};
        for(const auto& p:result->pads)if(p.asset)used[p.asset-1]=true;
        std::vector<PreviewAssetRef> assets;assets.reserve(source.assets.size());
        for(std::size_t i=0;i<source.assets.size();++i)if(used[i]){assets.push_back(source.assets[i]);indices[i]=std::uint8_t(assets.size());}
        for(auto& p:result->pads)if(p.asset)p.asset=indices[p.asset-1];result->assets=std::move(assets);
        if(!validPreviewMap(*result))return false;destination=std::move(*result);return true;});
}
bool validLocalPreviewContext(const LocalGenerationRequest& input) {
    const auto& r=input.request;const auto& c=input.local;PreviewDigest digest{};
    if(c.root.runtime.isNull()||c.root.runtime!=r.tag.runtime||!c.root.requestId||c.root.requestId>r.tag.requestId
        ||c.root.editRevision!=r.tag.editRevision||c.root.contextRevision!=r.tag.contextRevision
        ||c.sourceWord!=r.structuralGeneration||!digestPreviewMap(c.sourceMap,digest)||digest!=c.sourceMapDigest)return false;
    if(!validateProjectDocument(r.source))return false;
    for(std::uint8_t bank=0;bank<4;++bank){juce::Uuid rack=juce::Uuid::null();std::uint8_t unit=0;
        if(r.source.sound.personalities)for(const auto& instance:r.source.sound.personalities->instances)for(const auto& source:instance.design.units)
            if(isSourceModule(source.module))if(const auto* binding=rackUnitBinding(instance,source.id);binding&&binding->slot==bank){rack=instance.id;unit=source.id;}
        if(c.sourceMap.banks[bank].rack!=rack||c.sourceMap.banks[bank].unit!=unit)return false;
    }
    if(r.catalog)return r.catalog->authority&&r.catalog->root==c.root&&r.catalog->authority->snapshot==c.catalogSnapshot;
    return c.catalogSnapshot.isNull();
}
bool validPreviewBinding(const PreviewBinding& b) noexcept {
    return !b.request.runtime.isNull()&&b.request.runtime==b.root.runtime&&b.root.requestId&&b.request.requestId>=b.root.requestId
        &&b.request.editRevision==b.root.editRevision&&b.request.contextRevision==b.root.contextRevision
        &&b.job&&b.ticket&&b.sourceWord==b.structuralGeneration&&b.phase<=CatalogPhase::materialize
        &&b.providerAttempt>=1&&b.providerAttempt<=3&&b.renderOrdinal>=1&&b.renderOrdinal<=3;
}
bool samePreviewContext(const AuditionContext& a,const AuditionContext& b) noexcept {
    return a.phrase==b.phrase&&sameReal(a.bpm,b.bpm)&&a.phraseSource==b.phraseSource&&a.tempoSource==b.tempoSource;
}
bool validPreviewReceipt(const PreviewReceipt& receipt) noexcept {
    if(!validPreviewBinding(receipt.binding)||receipt.status>RenderStatus::renderFailure||receipt.failure>PreviewFailure::invalidBinding)return false;
    const auto& m=receipt.measurements;
    const bool measured=receipt.status==RenderStatus::ready||receipt.status==RenderStatus::clipped||receipt.status==RenderStatus::nonFinite;
    if(!measured)return receipt.bodyFrames==0&&receipt.frames==0&&!receipt.phraseTruncated&&zeroMeasurements(m);
    if(receipt.failure!=PreviewFailure::none||!receipt.bodyFrames||receipt.bodyFrames>maximumPreviewBodyFrames
        ||receipt.frames!=receipt.bodyFrames+previewTailFrames||receipt.frames>maximumPreviewFrames||m.clippedSamples>2*receipt.frames)return false;
    const auto time=[](double n,double cap){return std::isfinite(n)&&n>=0&&n<=cap+1e-9;};
    if(!std::isfinite(m.peak)||m.peak<0||(m.onsetSeconds&&!time(*m.onsetSeconds,double(receipt.frames)/previewSampleRate))
        ||!time(m.activityEndSeconds,double(receipt.frames)/previewSampleRate)||!time(m.decaySeconds,4))return false;
    double sum=0;for(double n:m.bandFractions){if(!std::isfinite(n)||n<0||n>1)return false;sum+=n;}
    if(sum!=0&&std::abs(sum-1)>1e-6)return false;
    if(receipt.status==RenderStatus::nonFinite)return !m.finite&&!std::isfinite(m.rms)&&sum==0;
    if(!m.finite||!std::isfinite(m.rms)||m.rms<0)return false;
    return receipt.status==RenderStatus::ready?m.clippedSamples==0&&m.peak<1&&m.rms<1:m.clippedSamples>0&&m.peak>=1;
}
bool samePreviewReceipt(const PreviewReceipt& a,const PreviewReceipt& b) noexcept {
    if(a.binding!=b.binding||a.status!=b.status||a.failure!=b.failure||a.bodyFrames!=b.bodyFrames||a.frames!=b.frames||a.phraseTruncated!=b.phraseTruncated)return false;
    const auto& x=a.measurements;const auto& y=b.measurements;
    if(x.finite!=y.finite||x.clippedSamples!=y.clippedSamples||!sameReal(x.peak,y.peak)||!sameReal(x.rms,y.rms)
        ||x.onsetSeconds.has_value()!=y.onsetSeconds.has_value()||(x.onsetSeconds&&!sameReal(*x.onsetSeconds,*y.onsetSeconds))
        ||!sameReal(x.activityEndSeconds,y.activityEndSeconds)||!sameReal(x.decaySeconds,y.decaySeconds)||x.tailActiveAtEnd!=y.tailActiveAtEnd)return false;
    for(std::size_t i=0;i<x.bandFractions.size();++i)if(!sameReal(x.bandFractions[i],y.bandFractions[i]))return false;return true;
}
}
