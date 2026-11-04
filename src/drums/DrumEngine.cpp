#include "DrumEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace airack::drums
{
namespace
{
bool within(float x,float low,float high) noexcept{return std::isfinite(x)&&x>=low&&x<=high;}
bool valid(const RackSettings& p) noexcept
{
    if(!within(p.level,0,1)||!within(p.pan,-1,1))return false;
    for(const auto& pad:p.pads){
        if(!within(pad.gainDb,-60,12)||!within(pad.pan,-1,1)||!within(pad.tuneSemitones,-24,24)
            ||!within(pad.trimStart,0,1)||!within(pad.trimEnd,0,1)||pad.trimStart>=pad.trimEnd
            ||!within(pad.attackSeconds,0,1)||!within(pad.decaySeconds,0,10)
            ||pad.midiNote<0||pad.midiNote>127||pad.chokeGroup<0||pad.chokeGroup>16)return false;
        if(pad.sample&&(pad.sample->channels()<1||pad.sample->channels()>2||pad.sample->frames()<1
            ||pad.sample->frames()>480000||pad.sample->samples().size()!=std::size_t(pad.sample->frames())*pad.sample->channels()))return false;
    }
    return true;
}
double balance(float pan,int channel) noexcept{return std::min(1.,1.+(channel==0?-double(pan):double(pan)));}
}
DrumEngine::ReadTable::ReadTable() noexcept {
    const auto i0=[](double x) noexcept {
        double sum=1,term=1;
        for(int k=1;k<60;++k){const double a=x/(2*k);term*=a*a;sum+=term;}
        return sum;
    };
    const double denominator=i0(7.5);
    for(std::size_t i=0;i<=cells;++i){
        const double u=double(i)/1024.,z=.9*u;
        const double sinc=z==0?1:std::sin(std::numbers::pi*z)/(std::numbers::pi*z);
        values[i]=.9*sinc*i0(7.5*std::sqrt(1-(u/24)*(u/24)))/denominator;
    }
}
double DrumEngine::ReadTable::at(double u) const noexcept {
    if(u>=24)return 0;
    const double coordinate=u*1024.;
    const auto i=static_cast<int>(coordinate);
    return values[i]+(values[i+1]-values[i])*(coordinate-double(i));
}
const DrumEngine::ReadTable& DrumEngine::preparedReadTable() noexcept {
    static const ReadTable table;return table;
}
std::array<double,2> DrumEngine::reconstructed(const Hit& hit,double position) noexcept {
    const auto channels=hit.sample->channels();const auto samples=hit.sample->samples();
    const auto literal=[&](std::uint32_t frame,unsigned channel) noexcept {
        return double(samples[std::size_t(frame)*channels+(channels==1?0:channel)]);
    };
    if(hit.step==1){const auto frame=static_cast<std::uint32_t>(position);
        return {literal(frame,0),literal(frame,1)};}
    std::array<double,2> sum{};double normalization=0;
    const double readScale=hit.readScale,readRadius=hit.readRadius;
    const int first=static_cast<int>(std::ceil(position-readRadius));
    const int last=static_cast<int>(std::floor(position+readRadius));
    const int begin=static_cast<int>(hit.begin),end=static_cast<int>(hit.end),count=last-first+1;
    const bool cached=count>0&&count<=int(readWeights_.values.size());
    const auto accumulate=[&]<bool Stereo,bool Interior,bool Cached,bool FillMemo>() noexcept {
#if defined(_M_X64) || defined(__SSE2__)
        if constexpr(Cached&&Stereo){
            const auto* pcm=samples.data();const auto* weights=readWeights_.values.data();
            __m128d pair=_mm_setzero_pd();
            for(int j=first;j<=last;++j){
                const int selected=Interior?j:std::clamp(j,begin,end-1);
                const __m128i frame=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pcm+selected*2));
                const __m128d sample=_mm_cvtps_pd(_mm_castsi128_ps(frame));
                pair=_mm_add_pd(pair,_mm_mul_pd(_mm_set1_pd(weights[std::size_t(j-first)]),sample));
            }
            _mm_storeu_pd(sum.data(),pair);return;
        }
#endif
#if defined(_M_X64) || defined(__SSE2__)
        if constexpr(!Cached&&Stereo&&FillMemo){
            const auto* pcm=samples.data();__m128d pair=_mm_setzero_pd();double orderedNormalization=0;
            for(int j=first;j<=last;++j){
                const double weight=readTable_->at(std::abs((double(j)-position)*readScale));
                readWeights_.values[std::size_t(j-first)]=weight;
                const int selected=Interior?j:std::clamp(j,begin,end-1);
                orderedNormalization+=weight;
                const __m128i frame=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pcm+selected*2));
                const __m128d sample=_mm_cvtps_pd(_mm_castsi128_ps(frame));
                pair=_mm_add_pd(pair,_mm_mul_pd(_mm_set1_pd(weight),sample));
            }
            _mm_storeu_pd(sum.data(),pair);normalization=orderedNormalization;return;
        }
#endif
        const auto* pcm=samples.data();double left=0,right=0,orderedNormalization=0;
        for(int j=first;j<=last;++j){
            const double weight=Cached?readWeights_.values[std::size_t(j-first)]:readTable_->at(std::abs((double(j)-position)*readScale));
            if constexpr(FillMemo)readWeights_.values[std::size_t(j-first)]=weight;
            const int selected=Interior?j:std::clamp(j,begin,end-1);
            const auto* sample=pcm+selected*(Stereo?2:1);
            if constexpr(!Cached)orderedNormalization+=weight;
            left+=weight*double(sample[0]);if constexpr(Stereo)right+=weight*double(sample[1]);
        }
        sum[0]=left;if constexpr(Stereo)sum[1]=right;
        if constexpr(!Cached)normalization=orderedNormalization;
    };
    const bool interior=first>=begin&&last<end;
    const auto read=[&]<bool Cached,bool FillMemo>() noexcept {
        if(channels==1){
            if(interior)accumulate.operator()<false,true,Cached,FillMemo>();else accumulate.operator()<false,false,Cached,FillMemo>();
        }else{
            if(interior)accumulate.operator()<true,true,Cached,FillMemo>();else accumulate.operator()<true,false,Cached,FillMemo>();
        }
    };
    if(cached){
        const int base=static_cast<int>(position),relativeFirst=first-base,relativeLast=last-base;
        const double phase=position-double(base);auto& weights=readWeights_;
        if(!weights.valid||weights.readScale!=readScale||weights.readRadius!=readRadius
            ||weights.phase!=phase||weights.relativeFirst!=relativeFirst||weights.relativeLast!=relativeLast){
            weights.valid=false;read.operator()<false,true>();
            weights.readScale=readScale;weights.readRadius=readRadius;weights.phase=phase;
            weights.relativeFirst=relativeFirst;weights.relativeLast=relativeLast;weights.normalization=normalization;
            weights.valid=true;
        }else{normalization=weights.normalization;read.operator()<true,false>();}
    }else read.operator()<false,false>();
    if(channels==1)sum[1]=sum[0];
    return {sum[0]/normalization,sum[1]/normalization};
}

void DrumEngine::Ramp::set(double x,int frames,bool immediately) noexcept
{
    if(x==target&&!immediately)return;
    target=x;if(immediately){snap();return;}
    remaining=frames;increment=(target-value)/frames;
}
double DrumEngine::Ramp::next() noexcept
{
    if(remaining>0){value+=increment;if(--remaining==0)value=target;}return value;
}
void DrumEngine::Ramp::snap() noexcept{value=target;increment=0;remaining=0;}
void DrumEngine::gains(bool snap) noexcept
{
    for(int channel=0;channel<2;++channel){
        kitGains_[channel].set(double(settings_.level)*balance(settings_.pan,channel),rampFrames_,snap);
        for(std::size_t pad=0;pad<16;++pad){const auto& p=settings_.pads[pad];
            padGains_[pad][channel].set(std::pow(10.,double(p.gainDb)/20)*balance(p.pan,channel),rampFrames_,snap);
        }
    }
}
bool DrumEngine::prepare(double physicalSampleRate,int maximumPhysicalBlock) noexcept
{
    if(!std::isfinite(physicalSampleRate)||physicalSampleRate<8000||physicalSampleRate>768000
        ||maximumPhysicalBlock<1||maximumPhysicalBlock>131072)return false;
    readTable_=&preparedReadTable();
    rate_=physicalSampleRate;maximumBlock_=maximumPhysicalBlock;rampFrames_=static_cast<int>(std::ceil(rate_*.005));
    gains(true);reset();return true;
}
bool DrumEngine::setSettings(const RackSettings& p) noexcept
{
    if(!validSettings(p))return false;
    settings_=p;gains(rate_==0);if(!settings_.enabled)reset();return true;
}
bool DrumEngine::validSettings(const RackSettings& p) noexcept{return valid(p);}
void DrumEngine::reset() noexcept
{
    hits_={};for(auto& pair:padGains_)for(auto& gain:pair)gain.snap();for(auto& gain:kitGains_)gain.snap();
}
void DrumEngine::retire(std::size_t slot) noexcept
{
    if(!hits_[slot].sample)return;const int rank=hits_[slot].rank;hits_[slot]={};
    for(auto& hit:hits_)if(hit.sample&&hit.rank>rank)--hit.rank;
}
void DrumEngine::trigger(int pad,int channel,int velocity) noexcept
{
    const auto& p=settings_.pads[pad];if(!settings_.enabled||!p.sample)return;
    std::size_t slot=0;while(slot<hits_.size()&&hits_[slot].sample)++slot;
    std::array<double,2> held{};bool heldValid=false;
    if(slot==hits_.size()){
        slot=0;for(std::size_t n=1;n<hits_.size();++n)if(hits_[n].rank>hits_[slot].rank)slot=n;
        held=hits_[slot].lastContribution;heldValid=hits_[slot].hasRenderedContribution;
        retire(slot);
    }
    for(auto& hit:hits_)if(hit.sample)++hit.rank;
    auto& hit=hits_[slot];hit.sample=p.sample;hit.pad=pad;hit.channel=channel;hit.group=p.chokeGroup;
    const auto frames=p.sample->frames();
    const auto begin=std::min(frames-1,static_cast<std::uint32_t>(std::floor(double(p.trimStart)*frames)));
    hit.end=std::clamp(static_cast<std::uint32_t>(std::ceil(double(p.trimEnd)*frames)),begin+1,frames);
    const double pitch=std::exp2(double(p.tuneSemitones)/12);
    hit.begin=begin;hit.step=48000/rate_*pitch;hit.velocity=velocity/127.;
    const double scale=std::max(1.,hit.step);hit.readScale=1/scale;hit.readRadius=24*scale;
    hit.naturalFrames=static_cast<std::uint32_t>(std::ceil(double(hit.end-begin)*rate_/(48000*pitch)));
    hit.attack=static_cast<std::uint32_t>(std::ceil(double(p.attackSeconds)*rate_));
    hit.decay=static_cast<std::uint32_t>(std::ceil(double(p.decaySeconds)*rate_));
    hit.lastContribution=held;hit.hasRenderedContribution=heldValid;
    const auto support=hit.decay?std::min(hit.naturalFrames,hit.attack+hit.decay):hit.naturalFrames;
    const auto intervals=support>1?std::min<std::uint32_t>(std::uint32_t(rampFrames_),support-1):0u;
    if(heldValid&&intervals>0){hit.stealFrom=held;hit.stealIntervals=hit.stealRemaining=static_cast<int>(intervals);}
}
void DrumEngine::event(const juce::MidiMessageMetadata& midi,bool admitNoteOns) noexcept
{
    if(midi.numBytes<1)return;const auto* data=midi.data;
    if(midi.numBytes==1&&data[0]==0xfc){hits_={};return;}
    if(midi.numBytes!=3||data[0]<0x80||data[0]>=0xf0||data[1]>127||data[2]>127)return;
    const int kind=data[0]&0xf0,channel=(data[0]&15)+1;
    if(kind==0xb0&&(data[1]==120||data[1]==123)){
        for(std::size_t n=0;n<hits_.size();++n)if(hits_[n].sample&&hits_[n].channel==channel)retire(n);return;
    }
    if(kind!=0x90||data[2]==0||!settings_.enabled||!admitNoteOns)return;
    // Mark older groups first, so duplicated note assignments all start together.
    for(const auto& pad:settings_.pads)if(pad.sample&&pad.midiNote==data[1]&&pad.chokeGroup!=0)
        for(auto& hit:hits_)if(hit.sample&&hit.group==pad.chokeGroup&&hit.release==0)hit.release=rampFrames_;
    for(int pad=0;pad<16;++pad)if(settings_.pads[pad].sample&&settings_.pads[pad].midiNote==data[1])trigger(pad,channel,data[2]);
}
void DrumEngine::render(juce::AudioBuffer<float>& out,const juce::MidiBuffer& midi) noexcept
{
    out.clear();if(beginRender(out,midi.cbegin(),midi.cend(),1)){(void)renderRange(0,out.getNumSamples());endRender();}
}
void DrumEngine::event(const RenderEvent& e,bool admitNoteOns,std::span<const std::uint64_t,16> installedTargets,bool& unavailable) noexcept
{
    if(e.kind==RenderEventKind::stopAll){hits_={};return;}
    if(e.kind==RenderEventKind::stopChannel){for(std::size_t n=0;n<hits_.size();++n)if(hits_[n].sample&&hits_[n].channel==e.channel)retire(n);return;}
    if(e.kind==RenderEventKind::exactPad){
        if(e.bank!=bank_)return;
        if(e.padOrNote>=16||!e.token||e.token!=installedTargets[e.padOrNote]||!admitNoteOns||!settings_.enabled){unavailable=true;return;}
        const auto& pad=settings_.pads[e.padOrNote];
        if(pad.sample&&pad.chokeGroup)for(auto& hit:hits_)if(hit.sample&&hit.group==pad.chokeGroup&&hit.release==0)hit.release=rampFrames_;
        trigger(e.padOrNote,e.channel,e.velocity);return;
    }
    const juce::uint8 raw[]{static_cast<juce::uint8>(0x90+e.channel-1),e.padOrNote,e.velocity};
    event({raw,3,0},admitNoteOns);
}
bool DrumEngine::beginRender(juce::AudioBuffer<float>& out,juce::MidiBufferIterator first,juce::MidiBufferIterator last,int factor) noexcept
{
    const int frames=out.getNumSamples();
    if(rendering_||rate_==0||out.getNumChannels()!=2||frames==0||frames>maximumBlock_||(factor!=1&&factor!=2))return false;
    auto* left=out.getWritePointer(0);auto* right=out.getWritePointer(1);
    const auto a=reinterpret_cast<std::uintptr_t>(left),b=reinterpret_cast<std::uintptr_t>(right);
    const auto distance=a<b?b-a:a-b;
    if(distance<std::size_t(frames)*sizeof(float))return false;
    output_={left,right};next_=first;end_=last;factor_=factor;sessionFrames_=frames;cursor_=0;rendering_=true;
    authoritative_=false;events_={};eventCursor_=0;
    if(next_!=end_)nextOffset_=std::int64_t((*next_).samplePosition)*factor_;
    return true;
}
bool DrumEngine::beginRender(juce::AudioBuffer<float>& out,std::span<const RenderEvent> events,std::uint8_t bank,int factor) noexcept
{
    if(!beginRender(out,juce::MidiBufferIterator{},juce::MidiBufferIterator{},factor))return false;
    events_=events;bank_=bank;authoritative_=true;return true;
}
bool DrumEngine::renderRange(int first,int count,bool admitNoteOns,bool writeOutput) noexcept
{
    static constexpr std::array<std::uint64_t,16> zero{};bool ignored=false;
    return renderRange(first,count,admitNoteOns,writeOutput,zero,ignored);
}
bool DrumEngine::renderRange(int first,int count,bool admitNoteOns,bool writeOutput,std::span<const std::uint64_t,16> installedTargets,bool& unavailable) noexcept
{
    if(!rendering_||first!=cursor_||count<0||count>sessionFrames_-first)return false;
    auto* left=output_[0];auto* right=output_[1];
    if(writeOutput){std::fill_n(left+first,count,0.f);std::fill_n(right+first,count,0.f);}
    for(int frame=first;frame<first+count;++frame){
        while(authoritative_&&eventCursor_<events_.size()&&std::int64_t(events_[eventCursor_].hostOffset)*factor_<=frame)
            event(events_[eventCursor_++],admitNoteOns,installedTargets,unavailable);
        while(!authoritative_&&next_!=end_&&nextOffset_<=frame){const auto message=*next_;++next_;if(message.samplePosition>=0)event(message,admitNoteOns);
            if(next_!=end_)nextOffset_=std::int64_t((*next_).samplePosition)*factor_;}
        double kit[2]{kitGains_[0].next(),kitGains_[1].next()},pad[16][2]{};
        for(std::size_t n=0;n<16;++n)for(int channel=0;channel<2;++channel)pad[n][channel]=padGains_[n][channel].next();
        double sum[2]{};
        for(std::size_t n=0;n<hits_.size();++n){auto& hit=hits_[n];if(!hit.sample)continue;
            const double position=std::min(double(hit.begin)+double(hit.elapsed)*hit.step,double(hit.end-1));
            const auto source=reconstructed(hit,position);
            double envelope=hit.attack&&hit.elapsed<hit.attack?double(hit.elapsed)/hit.attack:1.;
            if(hit.decay&&hit.elapsed>=hit.attack)envelope*=std::max(0.,1.-double(hit.elapsed-hit.attack)/hit.decay);
            if(hit.release>0)envelope*=double(hit.release)/rampFrames_;
            for(int channel=0;channel<2;++channel){
                const double contribution=source[channel]*hit.velocity*envelope*pad[hit.pad][channel];
                double value=contribution;
                if(hit.stealRemaining>0){
                    const double alpha=1-double(hit.stealRemaining)/hit.stealIntervals;
                    const double releaseFactor=hit.release>0?double(hit.release)/rampFrames_:1.;
                    value=(1-alpha)*(hit.stealFrom[channel]*releaseFactor)+alpha*contribution;
                }
                hit.lastContribution[channel]=value;sum[channel]+=value;
            }
            hit.hasRenderedContribution=true;if(hit.stealRemaining>0)--hit.stealRemaining;
            ++hit.elapsed;const bool released=hit.release>0&&--hit.release==0;
            if(hit.elapsed>=hit.naturalFrames||(hit.decay&&hit.elapsed>=hit.attack+hit.decay)||released)retire(n);
        }
        if(writeOutput){left[frame]=static_cast<float>(sum[0]*kit[0]);right[frame]=static_cast<float>(sum[1]*kit[1]);}
    }
    cursor_+=count;return true;
}
void DrumEngine::endRender() noexcept{output_={};next_={};end_={};events_={};eventCursor_=0;authoritative_=false;rendering_=false;sessionFrames_=cursor_=0;}
double DrumEngine::tailSeconds() const noexcept
{
    if(rate_<=0)return 0;double frames=0;
    if(settings_.enabled)for(const auto& p:settings_.pads)if(p.sample){
        const auto length=p.sample->frames();const auto begin=std::min(length-1,static_cast<std::uint32_t>(std::floor(double(p.trimStart)*length)));
        const auto end=std::clamp(static_cast<std::uint32_t>(std::ceil(double(p.trimEnd)*length)),begin+1,length);
        double support=std::ceil(double(end-begin)*rate_/(48000*std::exp2(double(p.tuneSemitones)/12)));
        if(p.decaySeconds>0)support=std::min(support,std::ceil(double(p.attackSeconds)*rate_)+std::ceil(double(p.decaySeconds)*rate_));
        frames=std::max(frames,support);
    }
    for(const auto& hit:hits_)if(hit.sample){double support=double(hit.naturalFrames)-hit.elapsed;
        if(hit.decay)support=std::min(support,double(hit.attack)+hit.decay-hit.elapsed);
        if(hit.release>0)support=std::min(support,double(hit.release));frames=std::max(frames,support);
    }
    return std::clamp(frames/rate_,0.,40.);
}
bool DrumEngine::references(const SampleAsset* sample) const noexcept
{
    if(!sample)return false;for(const auto& pad:settings_.pads)if(pad.sample==sample)return true;
    for(const auto& hit:hits_)if(hit.sample==sample)return true;return false;
}
void DrumEngine::borrowedReferences(std::array<const SampleAsset*,48>& out) const noexcept
{
    for(std::size_t pad=0;pad<settings_.pads.size();++pad)out[pad]=settings_.pads[pad].sample;
    for(std::size_t hit=0;hit<hits_.size();++hit)out[16+hit]=hits_[hit].sample;
}
void DrumEngine::clearBorrowedSamplesQuiescent() noexcept
{
    endRender();for(auto& pad:settings_.pads)pad.sample=nullptr;reset();
    nextOffset_=0;factor_=1;
}
std::size_t DrumEngine::activeHitCount() const noexcept
{std::size_t count=0;for(const auto& hit:hits_)count+=hit.sample!=nullptr;return count;}
}
