#include "SpaceReverbMailbox.h"
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <memory>
#include <thread>

namespace {bool tracking=false;std::size_t allocations=0,frees=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
namespace {
struct Packet {std::uint64_t generation=0;std::array<std::uint64_t,48> values{};bool operator==(const Packet&) const=default;};
Packet packet(std::uint64_t n){Packet p;p.generation=n;for(std::size_t i=0;i<p.values.size();++i)p.values[i]=(n*0x9e3779b97f4a7c15ULL)^std::uint64_t(i);return p;}
using Mailbox=airack::space::detail::LatestMailbox<Packet>;
}
int main(){
    int checks=0,failures=0;const auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';}};
    Mailbox mailbox;Packet output=packet(999);const auto initial=output;
    check(!mailbox.take(output),"initial_empty");check(output==initial,"empty_preserves_output");
    mailbox.publish(packet(1));check(mailbox.take(output),"published_available");check(output==packet(1),"published_exact");check(!mailbox.take(output),"no_replay");
    for(unsigned n=2;n<=17;++n)mailbox.publish(packet(n));
    check(mailbox.take(output),"coalesced_available");check(output==packet(17),"coalesced_latest");
    const auto retained=output;for(unsigned n=18;n<100;++n)mailbox.publish(packet(n));check(output==retained,"copied_output_retained");
    std::array<Mailbox,8> slots;
    for(std::size_t i=0;i<slots.size();++i)slots[i].publish(packet(100+i));
    for(std::size_t i=0;i<slots.size();++i){check(slots[i].take(output),"independent_available");check(output==packet(100+i),"independent_exact");}
    tracking=true;for(unsigned n=0;n<1000;++n){mailbox.publish(packet(n));mailbox.take(output);mailbox.take(output);}tracking=false;
    check(allocations==0,"operations_no_allocation");check(frees==0,"operations_no_free");
    Mailbox concurrent;std::atomic<bool> done{false};constexpr std::uint64_t last=100000;
    std::thread producer([&]{for(std::uint64_t n=1;n<=last;++n)concurrent.publish(packet(n));done.store(true,std::memory_order_release);});
    std::uint64_t seen=0,count=0;bool consistent=true,increasing=true;
    const auto consume=[&]{Packet value;if(!concurrent.take(value))return false;consistent=consistent&&(value==packet(value.generation));increasing=increasing&&value.generation>seen;seen=value.generation;++count;return true;};
    for(;;){if(consume()){if(seen==last)break;}else if(done.load(std::memory_order_acquire)){if(!consume())break;}else std::this_thread::yield();}
    producer.join();check(consistent,"concurrent_untorn");check(increasing,"concurrent_increasing");check(count>0,"concurrent_progress");check(seen==last,"final_generation_delivered");
    Mailbox views;check(views.takeView()==nullptr,"view_initial_empty");views.publish(packet(1));const auto* held=views.takeView();check(held&&*held==packet(1),"view_published_exact");check(views.takeView()==nullptr,"view_empty_no_replay");check(held&&*held==packet(1),"empty_view_keeps_ownership");
    for(unsigned n=2;n<=1000;++n)views.publish(packet(n));check(held&&*held==packet(1),"publisher_cannot_overwrite_held_view");const auto* latest=views.takeView();check(latest&&latest!=held&&*latest==packet(1000),"view_latest_transfers_new_buffer");views.publish(packet(1001));Packet copied;check(views.take(copied)&&copied==packet(1001),"copied_take_after_view");
    const auto savedCopy=copied;for(unsigned n=1002;n<1100;++n)views.publish(packet(n));check(copied==savedCopy,"copied_payload_remains_owned");
    bool exact=true;tracking=true;for(unsigned n=0;n<1000;++n){views.publish(packet(n));const auto* value=views.takeView();exact=exact&&value&&*value==packet(n);exact=exact&&views.takeView()==nullptr;}tracking=false;check(exact,"repeated_view_exact");check(allocations==0&&frees==0,"views_no_allocation_free");
    struct Large {std::uint64_t generation=0;std::array<std::uint64_t,16384> words{};};static_assert(sizeof(Large)==131080);
    auto large=std::make_unique<airack::space::detail::LatestMailbox<Large>>();auto largeInput=std::make_unique<Large>();const auto fill=[&](std::uint64_t n){largeInput->generation=n;for(std::size_t i=0;i<largeInput->words.size();++i)largeInput->words[i]=(n*0x9e3779b97f4a7c15ULL)^i;};
    fill(1);large->publish(*largeInput);const auto* largeHeld=large->takeView();for(unsigned n=2;n<=17;++n){fill(n);large->publish(*largeInput);}bool largeStable=largeHeld&&largeHeld->generation==1;if(largeHeld)for(std::size_t i=0;i<largeHeld->words.size();++i)largeStable=largeStable&&largeHeld->words[i]==(0x9e3779b97f4a7c15ULL^i);check(largeStable,"large_view_stable_during_publication");
    const auto* largeLatest=large->takeView();bool largeExact=largeLatest&&largeLatest->generation==17;if(largeLatest)for(std::size_t i=0;i<largeLatest->words.size();++i)largeExact=largeExact&&largeLatest->words[i]==((17ULL*0x9e3779b97f4a7c15ULL)^i);check(largeExact,"large_latest_exact");
    Mailbox heldConcurrent;heldConcurrent.publish(packet(1));const auto* pinned=heldConcurrent.takeView();std::atomic<bool> viewDone{false};bool heldConsistent=pinned&&*pinned==packet(1);std::thread viewProducer([&]{for(std::uint64_t n=2;n<=last;++n)heldConcurrent.publish(packet(n));viewDone.store(true,std::memory_order_release);});
    while(!viewDone.load(std::memory_order_acquire))if(pinned)heldConsistent=heldConsistent&&*pinned==packet(1);viewProducer.join();if(pinned)heldConsistent=heldConsistent&&*pinned==packet(1);check(heldConsistent,"concurrent_held_view_untorn");const auto* finalView=heldConcurrent.takeView();check(finalView&&*finalView==packet(last),"concurrent_held_view_final_delivered");
    std::cout<<"MAILBOX_CONCURRENCY published="<<last<<" observed="<<count<<" last="<<seen<<'\n';
    std::cout<<"SpaceReverbMailbox checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
