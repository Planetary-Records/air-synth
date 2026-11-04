#include "DrumOwnership.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <new>
#include <type_traits>

namespace airack::drums
{
namespace {
std::uint64_t decoded(const SampleAsset& asset) noexcept
{ return std::uint64_t(asset.frames())*asset.channels()*sizeof(float); }
}
DrumOwnership::PublicationRecord::PublicationRecord(std::uint64_t generation,DrumDocumentPtr doc,const Owners& input)
    :document(std::move(doc)),owners(input)
{
    borrowed.generation=generation;
    for(std::size_t bank=0;bank<4;++bank)for(std::size_t pad=0;pad<16;++pad)
        borrowed.samples[bank][pad]=owners[bank*16+pad].get();
}
DrumOwnership::DrumOwnership()
{
    DrumDocumentPtr document;DrumError error;
    if(!makeDrumDocument({},document,error))throw std::bad_alloc();
    publications_[0]=std::make_unique<PublicationRecord>(1,std::move(document),Owners{});
    current_=publications_[0].get();published_.store(&current_->borrowed);
}
std::uint64_t DrumOwnership::retainedBytes() const noexcept
{
    auto total=reservedDecoded_;
    for(const auto& asset:ledger_)if(asset)total+=decoded(*asset);
    return total;
}
bool DrumOwnership::reserveDecoded(std::uint64_t bytes) noexcept
{
    const auto retained=retainedBytes();
    if(bytes>retainedDecodedLimit||retained>retainedDecodedLimit-bytes)return false;
    reservedDecoded_+=bytes;return true;
}
void DrumOwnership::releaseDecoded(std::uint64_t bytes) noexcept
{ assert(bytes<=reservedDecoded_);reservedDecoded_-=bytes; }
bool DrumOwnership::registerAsset(AssetPtr& asset) noexcept
{
    if(!asset)return false;
    AssetPtr* empty=nullptr;
    for(auto& entry:ledger_){
        if(!entry){if(!empty)empty=&entry;continue;}
        if(entry.get()==asset.get())return true;
        if(entry->id()==asset->id()){asset=entry;return true;}
    }
    if(!empty)return false;
    *empty=asset;return true;
}
DrumOwnership::AiImageStatus DrumOwnership::installAiImage(std::size_t role,DrumDocumentPtr document,const Owners& owners) noexcept
{
    if(role>=aiImages_.size()||!document)return AiImageStatus::invalid;
    for(std::size_t n=0;n<owners.size();++n){
        const auto& metadata=document->slots()[n/16][n%16];const auto& owner=owners[n];
        if(metadata.asset.has_value()!=bool(owner))return AiImageStatus::invalid;
        if(!owner)continue;
        if(*metadata.asset!=owner->id()||owner->channels()<1||owner->channels()>2||owner->frames()<1||owner->frames()>480000
            ||std::none_of(ledger_.begin(),ledger_.end(),[&](const auto& entry){return entry.get()==owner.get();}))return AiImageStatus::invalid;
    }
    if(!intendedFits(owners))return AiImageStatus::budget;
    if(!reserveDecoded(sizeof(AiImage)))return AiImageStatus::budget;
    std::unique_ptr<AiImage> image;
    try {image=std::make_unique<AiImage>();image->owners=owners;image->document=std::move(document);}
    catch(...){releaseDecoded(sizeof(AiImage));return AiImageStatus::allocation;}
    const bool replacing=bool(aiImages_[role]);aiImages_[role]=std::move(image);
    if(replacing)releaseDecoded(sizeof(AiImage));
    collect();return AiImageStatus::ready;
}
void DrumOwnership::clearAiImage(std::size_t role) noexcept
{
    if(role>=aiImages_.size()||!aiImages_[role])return;
    aiImages_[role].reset();releaseDecoded(sizeof(AiImage));collect();
}
std::shared_ptr<const DrumOwnership::McpImage> DrumOwnership::captureMcpImage() noexcept
{
    collect();
    if(mcpImage_){
        if(mcpImage_->document==current_->document&&mcpImage_->owners==current_->owners&&mcpImage_->generation==current_->borrowed.generation)return mcpImage_;
        return {};
    }
    for(std::size_t n=0;n<current_->owners.size();++n){
        const auto& metadata=current_->document->slots()[n/16][n%16];const auto& owner=current_->owners[n];
        if(metadata.asset.has_value()!=bool(owner))return {};
        if(owner&&(*metadata.asset!=owner->id()||owner->channels()<1||owner->channels()>2||owner->frames()<1||owner->frames()>480000
            ||std::none_of(ledger_.begin(),ledger_.end(),[&](const auto& asset){return asset.get()==owner.get();})))return {};
    }
    if(!intendedFits(current_->owners)||!reserveDecoded(sizeof(McpImage)))return {};
    try {mcpImage_=std::make_shared<const McpImage>(McpImage{current_->document,current_->owners,current_->borrowed.generation});return mcpImage_;}
    catch(...){releaseDecoded(sizeof(McpImage));return {};}
}
bool DrumOwnership::aiImagesBusy() const noexcept
{ return std::any_of(aiImages_.begin(),aiImages_.end(),[](const auto& image){return bool(image);}); }
std::size_t DrumOwnership::freePublicationSlot() const noexcept
{
    for(std::size_t i=0;i<publications_.size();++i)if(!publications_[i])return i;
    return publications_.size();
}
bool DrumOwnership::intendedFits(const Owners& owners) const noexcept
{
    std::uint64_t bytes=0,bodies=0;
    for(std::size_t i=0;i<owners.size();++i)if(owners[i]){
        bool prior=false;for(std::size_t j=0;j<i;++j)prior|=owners[j].get()==owners[i].get();
        if(prior)continue;
        const auto size=decoded(*owners[i]);
        if(size>intendedDecodedLimit-bytes||size+56>intendedDecodedLimit-bodies)return false;
        bytes+=size;bodies+=size+56;
    }
    return true;
}
void DrumOwnership::install(std::size_t index,std::unique_ptr<PublicationRecord> record) noexcept
{
    assert(index<publications_.size()&&!publications_[index]&&record);
    assert(record->borrowed.generation>current_->borrowed.generation);
    publications_[index]=std::move(record);current_=publications_[index].get();
    published_.store(&current_->borrowed);
}
bool DrumOwnership::enqueue(const ReferenceReport& report) noexcept
{
    const auto write=reportWrite_.load(std::memory_order_relaxed),next=(write+1)%2;
    if(next==reportRead_.load(std::memory_order_acquire))return false;
    reports_[write]=report;reportWrite_.store(next,std::memory_order_release);return true;
}
void DrumOwnership::drain() noexcept
{
    auto read=reportRead_.load(std::memory_order_relaxed);
    while(read!=reportWrite_.load(std::memory_order_acquire)){
        const auto report=reports_[read];
        read=(read+1)%2;reportRead_.store(read,std::memory_order_release);
        if(report.complete&&report.lifecycle==lifecycle_&&report.adoptedGeneration>=latest_.adoptedGeneration
            &&report.adoptedGeneration<=current_->borrowed.generation)latest_=report;
    }
    collect();
}
bool DrumOwnership::rooted(const SampleAsset* asset) const noexcept
{
    for(const auto& record:publications_)if(record)for(const auto& owner:record->owners)if(owner.get()==asset)return true;
    for(const auto& preparation:preparations_)if(preparation.asset.get()==asset)return true;
    for(const auto& image:aiImages_)if(image)for(const auto& owner:image->owners)if(owner.get()==asset)return true;
    if(mcpImage_)for(const auto& owner:mcpImage_->owners)if(owner.get()==asset)return true;
    for(const auto& owner:saveOwners_)if(owner.get()==asset)return true;
    for(const auto& owner:restoreOwners_)if(owner.get()==asset)return true;
    for(std::size_t i=0;i<historyRootCount_;++i)if(historyRoots_[i]==asset)return true;
    if(borrowing_)for(const auto* reference:latest_.references)if(reference==asset)return true;
    return false;
}
void DrumOwnership::collect() noexcept
{
    // External pins can drop on workers; the control owner retires their metadata and ledger roots here.
    if(mcpImage_&&mcpImage_.use_count()==1){mcpImage_.reset();releaseDecoded(sizeof(McpImage));}
    for(auto& record:publications_)if(record&&record.get()!=current_
        &&(!borrowing_||(latest_.complete&&latest_.lifecycle==lifecycle_
            &&latest_.adoptedGeneration>record->borrowed.generation)))record.reset();
    for(auto& asset:ledger_)if(asset&&!rooted(asset.get()))asset.reset();
}
void DrumOwnership::quiescent() noexcept
{
    borrowing_=false;if(lifecycle_!=std::numeric_limits<std::uint64_t>::max())++lifecycle_;latest_={};
    reportRead_.store(0,std::memory_order_relaxed);reportWrite_.store(0,std::memory_order_relaxed);
    reports_={};collect();
}
}
