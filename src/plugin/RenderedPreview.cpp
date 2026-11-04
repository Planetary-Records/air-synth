#include "RenderedPreview.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace airack
{
static_assert(std::atomic<const void*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct RenderedPreview::Buffer
{
    std::vector<float> left, right;
    std::uint64_t revision;
};
RenderedPreview::RenderedPreview() = default;
// Hosts serialize lifecycle/destruction against audio, as for the processor engine.
RenderedPreview::~RenderedPreview() { release(); }
void RenderedPreview::resetUnderLock(double hostRate)
{
    borrowed=nullptr;audioGeneration=0;frame=0;lastGain=0;fading=false;
    publishedBuffer.store(nullptr,std::memory_order_relaxed);owner.reset();rate=hostRate;
    completed.store(published.load(std::memory_order_relaxed),std::memory_order_release);
}
void RenderedPreview::prepare(double hostRate)
{
    if(!std::isfinite(hostRate)||hostRate<8000||hostRate>384000)return;
    const std::lock_guard lock(controlMutex);resetUnderLock(hostRate);
}
void RenderedPreview::release()
{
    const std::lock_guard lock(controlMutex);resetUnderLock(0);
}
bool RenderedPreview::start(const ai::PreviewAudio& source,std::uint64_t revision)
{
    const std::lock_guard lock(controlMutex);
    const auto generation=published.load(std::memory_order_relaxed);
    if(rate==0||completed.load(std::memory_order_acquire)!=generation
        ||generation==std::numeric_limits<std::uint64_t>::max()||source.sampleRate!=48000
        ||source.left.empty()||source.left.size()>576000||source.left.size()!=source.right.size())return false;
    for(std::size_t n=0;n<source.left.size();++n)
        if(!std::isfinite(source.left[n])||!std::isfinite(source.right[n]))return false;
    std::unique_ptr<const Buffer> next;
    try {auto prepared=std::make_unique<Buffer>();prepared->left=std::vector<float>(source.left.size());prepared->right=std::vector<float>(source.right.size());prepared->revision=revision;
        if(prepared->left.capacity()!=source.left.size()||prepared->right.capacity()!=source.right.size())return false;
        std::copy(source.left.begin(),source.left.end(),prepared->left.begin());std::copy(source.right.begin(),source.right.end(),prepared->right.begin());next=std::move(prepared);}
    catch(const std::bad_alloc&) {return false;}
    // The old borrow was cleared before its acquire-observed completion.
    // Only this control path retires storage; a reused address has a new tag.
    owner=std::move(next);publishedBuffer.store(owner.get(),std::memory_order_relaxed);
    published.store(generation+1,std::memory_order_release);return true;
}
bool RenderedPreview::startCanonical(std::span<const float> source,std::uint32_t channels,std::uint64_t revision)
{
    if((channels!=1&&channels!=2)||source.empty()||source.size()%channels||source.size()/channels>480000)return false;
    for(const auto value:source)if(!std::isfinite(value))return false;
    const std::lock_guard lock(controlMutex);const auto generation=published.load(std::memory_order_relaxed);
    if(rate==0||completed.load(std::memory_order_acquire)!=generation||generation==std::numeric_limits<std::uint64_t>::max())return false;
    const auto frames=source.size()/channels;std::unique_ptr<Buffer> next;
    try {
        next=std::make_unique<Buffer>();next->left=std::vector<float>(frames);next->right=std::vector<float>(frames);next->revision=revision;
        // The pinned MSVC size constructors allocate exactly the admitted two planes.
        if(next->left.capacity()!=frames||next->right.capacity()!=frames)return false;
        for(std::size_t n=0;n<frames;++n){next->left[n]=source[n*channels];next->right[n]=source[n*channels+(channels==2?1:0)];}
    }catch(const std::bad_alloc&){return false;}
    owner=std::move(next);publishedBuffer.store(owner.get(),std::memory_order_relaxed);published.store(generation+1,std::memory_order_release);return true;
}
std::uint64_t RenderedPreview::retainedPcmBytes() const
{
    const std::lock_guard lock(controlMutex);return owner?std::uint64_t(owner->left.capacity()+owner->right.capacity())*sizeof(float):0;
}
bool RenderedPreview::clearStopped()
{
    const std::lock_guard lock(controlMutex);
    if(completed.load(std::memory_order_acquire)!=published.load(std::memory_order_relaxed))return false;
    publishedBuffer.store(nullptr,std::memory_order_relaxed);owner.reset();return true;
}
bool RenderedPreview::retireStoppedWithoutAudio()
{
    const std::lock_guard lock(controlMutex);
    if(audioGate.test_and_set(std::memory_order_acquire))return false;
    struct Release {std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{audioGate};
    const auto generation=published.load(std::memory_order_acquire);
    if(stopGeneration.load(std::memory_order_acquire)!=generation)return false;
    borrowed=nullptr;publishedBuffer.store(nullptr,std::memory_order_relaxed);
    completed.store(generation,std::memory_order_release);owner.reset();return true;
}
void RenderedPreview::stop() noexcept
{
    const auto generation=published.load(std::memory_order_acquire);
    auto old=stopGeneration.load(std::memory_order_relaxed);
    // Monotonic stop tags prevent a delayed old stop overwriting a newer stop.
    while(old<generation&&!stopGeneration.compare_exchange_weak(old,generation,std::memory_order_release,std::memory_order_relaxed)){}
}
bool RenderedPreview::stopped() const noexcept
{return published.load(std::memory_order_acquire)==completed.load(std::memory_order_acquire);}
bool RenderedPreview::playing() const noexcept {return !stopped();}
void RenderedPreview::acknowledge() noexcept
{
    const auto generation=audioGeneration;borrowed=nullptr;
    completed.store(generation,std::memory_order_release);
}
void RenderedPreview::mix(juce::AudioBuffer<float>& output,std::uint64_t revision) noexcept
{
    if(audioGate.test_and_set(std::memory_order_acquire))return;
    struct Release {std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{audioGate};
    const auto generation=published.load(std::memory_order_acquire);
    if(completed.load(std::memory_order_acquire)==generation||output.getNumChannels()!=2
        ||output.getNumSamples()<1||output.getNumSamples()>65536)return;
    if(audioGeneration!=generation||borrowed==nullptr)
    {
        borrowed=publishedBuffer.load(std::memory_order_relaxed);audioGeneration=generation;
        if(!borrowed)return;
        frame=0;lastGain=0;fading=false;stopFrames=0;
        targetFrames=static_cast<std::uint64_t>(std::ceil(borrowed->left.size()*rate/48000.));
        fadeFrames=static_cast<std::uint64_t>(std::max(1.,std::ceil(rate*.005)));
    }
    if(!fading&&(stopGeneration.load(std::memory_order_acquire)==generation||borrowed->revision!=revision))
    {fading=true;stopGain=lastGain;stopFrames=0;}
    if(fading&&stopGain==0){acknowledge();return;}
    for(int n=0;n<output.getNumSamples();++n)
    {
        double gain=std::min({1.,double(frame)/fadeFrames,double(targetFrames-1-frame)/fadeFrames});
        if(fading)gain=std::min(gain,std::max(0.,stopGain-double(++stopFrames)/fadeFrames));
        if(gain>0)
        {
            // ponytail: linear interpolation aliases below 48k; use a bandlimited
            // resampler if listening acceptance requires better low-rate audition.
            const double position=frame*48000./rate;
            const auto low=std::min(static_cast<std::size_t>(position),borrowed->left.size()-1);
            const auto high=std::min(low+1,borrowed->left.size()-1);
            const double fraction=position-std::floor(position);
            for(int c=0;c<2;++c)
            {
                const auto& source=c==0?borrowed->left:borrowed->right;
                const float sample=fraction==0||low==high?source[low]
                    :static_cast<float>(double(source[low])+(double(source[high])-source[low])*fraction);
                // Direct endpoints/steady output retain the source's signed zero.
                if(gain==1)output.setSample(c,n,sample);
                else output.setSample(c,n,static_cast<float>(output.getSample(c,n)*(1-gain)+sample*gain));
            }
        }
        lastGain=gain;++frame;
        if(frame==targetFrames||(fading&&gain==0)){acknowledge();return;}
    }
}
}
