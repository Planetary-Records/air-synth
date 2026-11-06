#include "InstrumentProcessor.h"
#include <algorithm>
#include <chrono>
#include <limits>
namespace airack
{
std::uint64_t InstrumentProcessor::openEqSpectrumView()
{
    reconcileGroupConnection();auto& s=groupControl->spectrum;if(!s.nextOwner)return 0;
    closeEqSpectrumView(s.owner);s.owner=s.nextOwner;
    s.nextOwner=s.nextOwner==std::numeric_limits<std::uint64_t>::max()?0:s.nextOwner+1;
    s.inbox=std::make_unique<companion::GroupSpectrumInbox>();s.view={};return s.owner;
}
bool InstrumentProcessor::configureEqSpectrumView(std::uint64_t owner,bool enabled,std::uint8_t slot,std::span<const GroupUuid> peers)
{
    auto& g=*groupControl;auto& s=g.spectrum;if(!owner||owner!=s.owner||slot>=8||peers.size()>4)return false;
    reconcileGroupConnection();
    auto sound=std::make_unique<SoundDocument>();std::uint64_t revision;
    if(!trySoundSnapshot(*sound,revision)||sound->layout.effects[slot].use!=SlotUse::present||sound->layout.effects[slot].kind!=EffectKind::dynamicEq)return false;
    const auto link=groupLinkView();
    for(std::size_t i=0;i<peers.size();++i){
        if(peers[i].isNull()||!link.intent||peers[i]==link.intent->member||std::find(peers.begin(),peers.begin()+i,peers[i])!=peers.begin()+i)return false;
        const bool present=std::any_of(g.members.begin(),g.members.end(),[&](const auto& member){return member.intent.group==link.intent->group&&member.intent.member==peers[i]&&member.runtime!=link.runtime;});
        const bool incumbent=std::find(s.view.peers.begin(),s.view.peers.begin()+s.view.peerCount,peers[i])!=s.view.peers.begin()+s.view.peerCount;
        if(!present&&!incumbent)return false;
    }
    if(s.view.enabled!=enabled||s.view.sendSlot!=slot){s.withdraw=s.withdraw||s.view.enabled;s.latest.reset();s.observed.reset();}
    s.view.enabled=enabled;s.view.sendSlot=slot;s.view.peerCount=std::uint8_t(peers.size());s.view.peers={};std::copy(peers.begin(),peers.end(),s.view.peers.begin());
    configureEqSpectrumInbox();
    // Existing restored consent stays pending. This view never resumes it.
    if(enabled&&(link.phase==GroupLinkPhase::authorized||link.phase==GroupLinkPhase::admitted))(void)requestGroupConnection();
    return true;
}
void InstrumentProcessor::closeEqSpectrumView(std::uint64_t owner)
{
    auto& s=groupControl->spectrum;if(!owner||s.owner!=owner)return;
    s.withdraw=s.withdraw||s.view.enabled;s.owner=0;s.view={};s.latest.reset();s.observed.reset();if(s.inbox)s.inbox->clear();
}
EqSpectrumView InstrumentProcessor::eqSpectrumView(std::uint64_t owner) const
{
    const auto& g=*groupControl;const auto& s=g.spectrum;if(!owner||s.owner!=owner)return {};
    const std::lock_guard lock(restoreMutex);const auto link=groupLinkState.view();auto result=s.view;
    result.capable=(aiClient.capabilities()&companion::spectrumCapability)!=0;
    result.admitted=g.admitted&&groupLinkState.eligible()&&g.session==aiClient.session()&&g.processed.runtime==link.runtime&&g.processed.loadRevision==link.loadRevision;
    return result;
}
bool InstrumentProcessor::captureEqSpectrum(std::uint64_t owner,std::uint8_t slot,std::uint64_t generation,std::uint64_t windows,double rate,const std::array<std::int16_t,128>& levels)
{
    auto& g=*groupControl;auto& s=g.spectrum;if(!owner||owner!=s.owner||slot!=s.view.sendSlot||!windows||s.sequence==std::numeric_limits<std::uint64_t>::max())return false;
    const auto link=groupLinkView();companion::GroupSpectrum packet{link.runtime,link.loadRevision,s.sequence+1,generation,slot,rate,levels};
    if(!companion::validateGroupSpectrum(packet))return false;
    const GroupControl::Spectrum::Window key{slot,generation,windows};const bool fresh=s.observed&&*s.observed!=key;s.observed=key;
    const auto view=eqSpectrumView(owner);
    if(!fresh||!view.enabled||!view.capable||!view.admitted||link.runtime!=g.processed.runtime||link.loadRevision!=g.processed.loadRevision)return false;
    // One replaceable unsent value. No stale display window gets a new sequence.
    s.latest=packet;s.sequence=packet.sequence;return true;
}
const companion::GroupSpectrum* InstrumentProcessor::peerEqSpectrum(std::uint64_t owner,GroupUuid runtime) const
{
    const auto view=eqSpectrumView(owner);const auto& s=groupControl->spectrum;
    return view.enabled&&view.capable&&view.admitted&&s.inbox?s.inbox->current(runtime,std::chrono::steady_clock::now()):nullptr;
}
void InstrumentProcessor::resetEqSpectrumConnection()
{
    auto& s=groupControl->spectrum;s.view.enabled=false;s.view.peerCount=0;s.view.peers={};s.pendingView={};s.withdraw=false;s.latest.reset();s.observed.reset();s.pendingOwner=s.submitted=s.publishTime=s.readTime=0;if(s.inbox)s.inbox->clear();
}
void InstrumentProcessor::configureEqSpectrumInbox()
{
    const auto& g=*groupControl;auto& s=groupControl->spectrum;if(!s.inbox)return;const auto view=eqSpectrumView(s.owner);
    if(!view.enabled||!view.capable||!view.admitted){s.inbox->clear();return;}
    const auto link=groupLinkView();std::array<companion::GroupSpectrumInbox::Peer,4> peers{};std::size_t count=0;
    for(std::size_t i=0;i<view.peerCount;++i){const auto found=std::find_if(g.members.begin(),g.members.end(),[&](const auto& member){return link.intent&&member.intent.group==link.intent->group&&member.intent.member==view.peers[i]&&member.runtime!=link.runtime;});
        if(found!=g.members.end())peers[count++]={found->runtime,found->loadRevision};
    }
    (void)s.inbox->configure(aiClient.session(),link.runtime,link.loadRevision,{peers.data(),count});
}
void InstrumentProcessor::receiveEqSpectrumReply(const companion::GroupCommand&,const companion::GroupReply& reply)
{
    auto& s=groupControl->spectrum;if(reply.action!=companion::GroupAction::spectrumRead||!s.owner||s.pendingOwner!=s.owner||!s.view.enabled||!s.inbox)return;
    if(s.pendingView!=s.view)return;
    configureEqSpectrumInbox();const auto view=eqSpectrumView(s.owner);if(!view.capable||!view.admitted)return;
    // Rate limiting defers refresh; the last admitted window keeps its original
    // arrival/expiry. It grants no new data and must not blank healthy peers.
    if(reply.status==companion::GroupReplyStatus::capacity)return;
    const auto now=std::chrono::steady_clock::now();
    if(reply.status!=companion::GroupReplyStatus::ready||!s.inbox->receive(aiClient.session(),reply.runtime,reply.loadRevision,reply.spectra,now))
        // Hide unavailable sources, retaining sequence high-water marks so a
        // cached reply cannot resurrect an old window after a transient failure.
        (void)s.inbox->receive(aiClient.session(),reply.runtime,reply.loadRevision,{},now);
}
bool InstrumentProcessor::submitEqSpectrum(std::uint64_t now,std::uint64_t before)
{
    auto& g=*groupControl;auto& s=g.spectrum;
    if(!g.admitted||g.session!=aiClient.session()||!(aiClient.capabilities()&companion::spectrumCapability))return false;
    if(s.withdraw){if(before==0||!submitGroup(GroupControl::Purpose::spectrumWithdraw,now))return false;s.withdraw=false;return true;}
    if(!s.owner||!s.view.enabled)return false;
    configureEqSpectrumInbox();
    // Integer milliseconds need one extra tick to guarantee >=100 real ms.
    const auto never=std::numeric_limits<std::uint64_t>::max();
    const auto publishDue=s.latest&&s.latest->sequence>s.submitted&&(s.publishTime==0||now-s.publishTime>=101)?(s.publishTime?s.publishTime+101:0):never;
    const auto readDue=s.view.peerCount&&(s.readTime==0||now-s.readTime>=101)?(s.readTime?s.readTime+101:0):never;
    if(std::min(publishDue,readDue)>=before)return false;
    if(publishDue<=readDue){
        const auto sequence=s.latest->sequence;if(!submitGroup(GroupControl::Purpose::spectrumPublish,now))return false;s.submitted=sequence;s.publishTime=now;return true;
    }
    if(readDue!=never){
        if(!submitGroup(GroupControl::Purpose::spectrumRead,now))return false;s.readTime=now;return true;
    }
    return false;
}
}
