#include "LocalPreviewExchange.h"
#include "CompanionWire.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace airack::companion
{
namespace
{
ai::PreviewReceipt failure(ai::PreviewBinding binding,ai::PreviewFailure reason)
{
    ai::PreviewReceipt result;result.binding=std::move(binding);result.failure=reason;
    result.status=reason==ai::PreviewFailure::cancelled?ai::RenderStatus::cancelled:ai::RenderStatus::renderFailure;
    return result;
}
bool geometry(const ai::PreviewReceipt& receipt,const ai::AuditionContext& context)
{
    if(receipt.status!=ai::RenderStatus::ready&&receipt.status!=ai::RenderStatus::clipped&&receipt.status!=ai::RenderStatus::nonFinite)return true;
    const auto requested=std::floor(patternLengthBeats(context.phrase)*60/context.bpm*ai::previewSampleRate+.5);
    return receipt.bodyFrames==std::uint32_t(std::max(1.,std::min(requested,double(ai::maximumPreviewBodyFrames))))
        &&receipt.phraseTruncated==(requested>ai::maximumPreviewBodyFrames);
}
}
LocalPreviewExchange::LocalPreviewExchange(std::array<std::uint64_t,3> values,std::chrono::steady_clock::time_point expires)
    :tickets(values),deadline(expires)
{
    if(!tickets[0]||!tickets[1]||!tickets[2]||tickets[0]==tickets[1]||tickets[0]==tickets[2]||tickets[1]==tickets[2])
        throw std::invalid_argument("local preview tickets");
}
ai::PreviewReceipt LocalPreviewExchange::evaluate(const ai::PreviewEvaluate& value,const ai::CancelCheck& stop)
{
    std::unique_lock lock(mutex);
    auto binding=value.binding;binding.ticket=tickets[std::min(next,tickets.size()-1)];
    if(cancelled||std::chrono::steady_clock::now()>=deadline)return failure(binding,ai::PreviewFailure::cancelled);
    if(offered||receipt)return failure(binding,ai::PreviewFailure::busy);
    if(next==tickets.size())return failure(binding,ai::PreviewFailure::budget);
    lock.unlock();const bool stopped=stop&&stop();lock.lock();
    if(stopped||cancelled||std::chrono::steady_clock::now()>=deadline){cancelled=true;return failure(binding,ai::PreviewFailure::cancelled);}
    if(offered||receipt)return failure(binding,ai::PreviewFailure::busy);
    if(next==tickets.size())return failure(binding,ai::PreviewFailure::budget);
    binding.ticket=tickets[next];
    try{
        auto copy=value;copy.binding=binding;juce::MemoryBlock bounded;
        if(!encodePreviewEvaluate(copy,bounded))return failure(binding,ai::PreviewFailure::invalidBinding);
        offered.emplace(std::move(copy));taken=false;++next;
    }catch(const std::bad_alloc&){return failure(binding,ai::PreviewFailure::allocation);}
    catch(const std::length_error&){return failure(binding,ai::PreviewFailure::allocation);}
    while(!cancelled&&!receipt){
        lock.unlock();const bool stopped=stop&&stop();lock.lock();
        if(stopped||std::chrono::steady_clock::now()>=deadline){cancelled=true;changed.notify_all();break;}
        changed.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(10)));
    }
    auto result=cancelled?failure(binding,ai::PreviewFailure::cancelled):*receipt;
    offered.reset();receipt.reset();taken=false;return result;
}
bool LocalPreviewExchange::takeEvaluation(ai::PreviewEvaluate& output)
{
    std::lock_guard lock(mutex);
    if(cancelled||taken||!offered)return false;
    try{auto copy=*offered;output=std::move(copy);taken=true;return true;}
    catch(const std::bad_alloc&){return false;}catch(const std::length_error&){return false;}
}
bool LocalPreviewExchange::acceptReceipt(const ai::PreviewReceipt& value)
{
    std::lock_guard lock(mutex);
    if(cancelled||!taken||!offered||receipt||std::chrono::steady_clock::now()>=deadline
        ||value.binding!=offered->binding||!ai::validPreviewReceipt(value)||!geometry(value,offered->context))return false;
    receipt=value;last=value;changed.notify_all();return true;
}
void LocalPreviewExchange::cancel() noexcept
{
    std::lock_guard lock(mutex);cancelled=true;changed.notify_all();
}
bool LocalPreviewExchange::lastReceipt(ai::PreviewReceipt& output) const
{
    std::lock_guard lock(mutex);if(!last)return false;output=*last;return true;
}
}
