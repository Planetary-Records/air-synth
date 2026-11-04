#include "FxAnalysisHub.h"
#include <algorithm>
#include <atomic>
#include <thread>

namespace airack::studio
{
namespace {std::atomic<std::uint64_t> nextLease{1};}
struct FxAnalysisHub::State
{
    const std::thread::id owner=std::this_thread::get_id();
    std::shared_ptr<FxMonitor> monitor;
    struct Entry {Lease lease;bool spectrum=true;};
    struct Slot {
        std::optional<FxMonitor::Token> token;
        std::unique_ptr<FxSpectrum> analyzer;
        std::uint64_t auditionOwner=0;
        std::size_t references=0,packets=0;
    };
    std::array<Entry,64> leases{};
    std::array<Slot,FxMonitor::slots> slots;
    std::size_t cursor=0,packets=0;
    bool retired=false;
    bool currentThread() const noexcept{return std::this_thread::get_id()==owner;}
    Entry* find(Lease lease) noexcept {
        if(!currentThread()||retired||!lease.id||lease.slot>=slots.size())return nullptr;
        const auto it=std::find_if(leases.begin(),leases.end(),[&](const auto& entry){return entry.lease==lease;});
        return it==leases.end()?nullptr:&*it;
    }
    bool spectrumEnabled(std::uint8_t slot) const noexcept {
        return std::any_of(leases.begin(),leases.end(),[&](const auto& entry){return entry.lease.id&&entry.lease.slot==slot&&entry.spectrum;});
    }
};
FxAnalysisHub::FxAnalysisHub(std::shared_ptr<FxMonitor> monitor):state(std::make_unique<State>()){state->monitor=std::move(monitor);}
FxAnalysisHub::~FxAnalysisHub(){retire();}
std::optional<FxAnalysisHub::Lease> FxAnalysisHub::acquire(std::uint8_t slot)
{
    auto& s=*state;if(!s.currentThread()||s.retired||!s.monitor||slot>=s.slots.size())return {};
    const auto entry=std::find_if(s.leases.begin(),s.leases.end(),[](const auto& e){return !e.lease.id;});
    if(entry==s.leases.end())return {};
    auto& stream=s.slots[slot];
    if(!stream.references){
        stream.analyzer=std::make_unique<FxSpectrum>();stream.token=s.monitor->subscribe(slot);
        if(!stream.token){stream.analyzer.reset();return {};}
    }
    ++stream.references;entry->lease={nextLease.fetch_add(1,std::memory_order_relaxed),slot};entry->spectrum=true;
    return entry->lease;
}
void FxAnalysisHub::release(Lease lease)
{
    auto& s=*state;auto* entry=s.find(lease);if(!entry)return;
    auto& stream=s.slots[lease.slot];
    if(stream.auditionOwner==lease.id){(void)s.monitor->requestAudition(*stream.token,{});stream.auditionOwner=0;}
    entry->lease={};
    if(--stream.references==0){(void)s.monitor->retire(*stream.token);stream.token.reset();stream.analyzer.reset();}
}
void FxAnalysisHub::update()
{
    auto& s=*state;if(!s.currentThread()||s.retired)return;
    std::size_t remaining=packetBudgetPerTick;s.packets=0;
    for(auto& slot:s.slots)slot.packets=0;
    for(std::size_t offset=0;offset<s.slots.size();++offset){
        const auto index=(s.cursor+offset)%s.slots.size();auto& slot=s.slots[index];
        if(!slot.token)continue;
        auto budget=std::min(remaining,FxMonitor::packetCapacity);const auto initial=budget;
        (void)slot.analyzer->update(*s.monitor,*slot.token,s.spectrumEnabled(std::uint8_t(index)),budget);
        slot.packets=initial-budget;remaining-=slot.packets;s.packets+=slot.packets;
    }
    s.cursor=(s.cursor+1)%s.slots.size();
}
const FxSpectrum::Result* FxAnalysisHub::result(Lease lease) const noexcept
{return state->find(lease)?&state->slots[lease.slot].analyzer->result():nullptr;}
std::size_t FxAnalysisHub::activeAnalyzers() const noexcept
{if(!state->currentThread()||state->retired)return 0;return std::count_if(state->slots.begin(),state->slots.end(),[](const auto& s){return s.references!=0;});}
bool FxAnalysisHub::setSpectrumEnabled(Lease lease,bool enabled) noexcept
{auto* entry=state->find(lease);if(!entry)return false;entry->spectrum=enabled;return true;}
bool FxAnalysisHub::requestAudition(Lease lease,std::optional<std::size_t> band,bool detector) noexcept
{
    auto& s=*state;if(!s.find(lease))return false;auto& stream=s.slots[lease.slot];
    if(!band&&stream.auditionOwner!=lease.id)return true;
    if(!s.monitor->requestAudition(*stream.token,band,detector))return false;
    stream.auditionOwner=band?lease.id:0;return true;
}
std::optional<std::size_t> FxAnalysisHub::requestedAudition(Lease lease) const noexcept
{if(!state->find(lease))return {};const auto& stream=state->slots[lease.slot];return stream.auditionOwner==lease.id?state->monitor->requestedAudition(*stream.token):std::nullopt;}
std::uint64_t FxAnalysisHub::generation(Lease lease) const noexcept
{return state->find(lease)?state->monitor->generation(*state->slots[lease.slot].token):0;}
void FxAnalysisHub::retire()
{
    auto& s=*state;if(!s.currentThread()||s.retired)return;
    for(auto& slot:s.slots){if(slot.token)(void)s.monitor->retire(*slot.token);slot={};}
    s.leases={};s.retired=true;
}
std::size_t FxAnalysisHub::packetsLastTick() const noexcept{return state->currentThread()?state->packets:0;}
std::size_t FxAnalysisHub::packetsLastTick(std::uint8_t slot) const noexcept{return state->currentThread()&&slot<state->slots.size()?state->slots[slot].packets:0;}
}
