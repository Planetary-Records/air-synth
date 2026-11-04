#include "CompanionWire.h"
#include "GroupSpectrum.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
using namespace airack;using namespace airack::ai;using namespace airack::companion;
int checks=0,failures=0,sections=0;const char* current="";
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;if(failures<100)std::cerr<<current<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void section(const char* value){current=value;++sections;std::cout<<value<<'\n';}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::MemoryBlock sentinel(){const std::uint8_t b[]{0xde,0xad,0xbe,0xef};return {b,4};}
GroupUuid id(unsigned n){GroupUuid value;value.bytes[0]=std::uint8_t(n);value.bytes[15]=0x80;return value;}
void boundedSpectrumRoundTrip()
{
 section("bounded_spectrum_round_trip_and_transactional_rejection");
 GroupSpectrum source;source.runtime=id(7);source.loadRevision=2;source.sequence=3;source.generation=4;source.slot=7;source.sampleRate=192000;
 for(std::size_t n=0;n<128;++n)source.postDb100[n]=std::int16_t(-12000+int(n)*100);
 juce::MemoryBlock wire;CHECK(encodeGroupSpectrum(source,wire));CHECK(wire.getSize()<=4096);
 GroupSpectrum out;CHECK(decodeGroupSpectrum(wire.getData(),wire.getSize(),out));CHECK(out==source);
 const auto original=out;std::array<std::byte,4097> oversized{};CHECK(!decodeGroupSpectrum(oversized.data(),oversized.size(),out));CHECK(out==original);
 for(std::size_t size=0;size<wire.getSize();++size){CHECK(!decodeGroupSpectrum(wire.getData(),size,out));CHECK(out==original);}
 auto trailing=wire;trailing.append("x",1);CHECK(!decodeGroupSpectrum(trailing.getData(),trailing.getSize(),out));CHECK(out==original);
 for(int invalid=0;invalid<12;++invalid){auto value=source;switch(invalid){case 0:value.runtime={};break;case 1:value.loadRevision=0;break;case 2:value.sequence=0;break;case 3:value.generation=0;break;case 4:value.slot=8;break;case 5:value.sampleRate=0;break;case 6:value.sampleRate=7999;break;case 7:value.sampleRate=1536001;break;case 8:value.sampleRate=std::numeric_limits<double>::infinity();break;case 9:value.sampleRate=std::numeric_limits<double>::quiet_NaN();break;case 10:value.postDb100[0]=-12001;break;case 11:value.postDb100[127]=2401;break;}
  auto destination=sentinel();const auto saved=destination;CHECK(!encodeGroupSpectrum(value,destination));CHECK(destination==saved);
 }
 for(std::size_t offset:{std::size_t(0),std::size_t(44)}){auto corrupt=wire;static_cast<std::uint8_t*>(corrupt.getData())[offset]=255;CHECK(!decodeGroupSpectrum(corrupt.getData(),corrupt.getSize(),out));CHECK(out==original);}
 auto badLevel=wire;static_cast<std::uint8_t*>(badLevel.getData())[53]=0;static_cast<std::uint8_t*>(badLevel.getData())[54]=128;CHECK(!decodeGroupSpectrum(badLevel.getData(),badLevel.getSize(),out));CHECK(out==original);
 source.sequence=source.loadRevision=source.generation=std::numeric_limits<std::uint64_t>::max();source.sampleRate=1536000;source.postDb100.front()=-12000;source.postDb100.back()=2400;CHECK(encodeGroupSpectrum(source,wire));CHECK(decodeGroupSpectrum(wire.getData(),wire.getSize(),out)&&out==source);
}
juce::Uuid uuid(GroupUuid value){return juce::Uuid(value.getRawData());}
void negotiatedSpectrumMessages()
{
 section("spectrum_negotiation_and_small_frame_caps");
 ControlMessage hello{{uuid(id(1)),0,0,0},RuntimeStatus::ready,spectrumCapability};juce::MemoryBlock wire;
 CHECK(encodeControl(hello,wire));ControlMessage decoded;CHECK(decodeControl(bytes(wire),decoded));CHECK(decoded.capabilities==spectrumCapability);
 GroupCommand command;command.operationId=1;command.runtime=id(1);command.loadRevision=1;command.action=GroupAction::spectrumPublish;
 command.spectrum.emplace();auto& value=*command.spectrum;value.runtime=command.runtime;value.loadRevision=1;value.sequence=1;value.generation=1;value.sampleRate=48000;value.postDb100.fill(-6000);
 CHECK(encodeGroupCommand(command,wire));GroupCommand copy;CHECK(decodeGroupCommand(bytes(wire),copy)&&copy==command);
 Frame frame{MessageKind::spectrumCommand,juce::Uuid(),wire};juce::MemoryBlock encoded;CHECK(encodeFrame(frame,encoded));Frame out;CHECK(decodeFrame(bytes(encoded),out));CHECK(messagePayloadCap(MessageKind::spectrumCommand)==4096&&messagePayloadCap(MessageKind::spectrumReply)==4096);
 frame.payload.setSize(4097);CHECK(!encodeFrame(frame,encoded));
 const auto captured=value;command.action=GroupAction::spectrumRead;command.spectrum.reset();command.spectrumPeers={id(2),id(3),id(4),id(5)};CHECK(encodeGroupCommand(command,wire));CHECK(decodeGroupCommand(bytes(wire),copy)&&copy==command);
 command.spectrumPeers.push_back(id(6));CHECK(!encodeGroupCommand(command,wire));
 GroupReply reply;reply.operationId=1;reply.action=GroupAction::spectrumRead;reply.runtime=id(1);reply.loadRevision=1;
 for(int i=0;i<4;++i){auto spectrum=captured;spectrum.runtime=id(unsigned(i+2));reply.spectra.push_back(spectrum);}
 CHECK(encodeGroupReply(reply,wire)&&wire.getSize()<=4096);GroupReply response;CHECK(decodeGroupReply(bytes(wire),response)&&response==reply);
 reply.spectra.push_back(reply.spectra.front());CHECK(!encodeGroupReply(reply,wire));
}
void spectrumArrivalDoesNotRefreshOldWindows()
{
 section("spectrum_receiver_authority_and_monotonic_arrival");
 GroupSpectrumInbox inbox;const juce::Uuid session;const std::array<GroupSpectrumInbox::Peer,1> peers{{{id(2),7}}};
 CHECK(inbox.configure(session,id(1),4,peers));GroupSpectrum value;value.runtime=id(2);value.loadRevision=7;value.sequence=1;value.generation=3;value.sampleRate=48000;value.postDb100.fill(-5000);
 const auto at=[](int ms){return std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(ms);};const std::array<GroupSpectrum,1> values{value};
 CHECK(inbox.receive(session,id(1),4,values,at(100)));CHECK(inbox.current(id(2),at(2100)));
 CHECK(inbox.receive(session,id(1),4,values,at(2100)));CHECK(!inbox.current(id(2),at(2101)));
 CHECK(!inbox.receive(juce::Uuid(),id(1),4,values,at(2200)));CHECK(!inbox.receive(session,id(1),5,values,at(2200)));
 auto next=values;++next[0].sequence;CHECK(inbox.receive(session,id(1),4,next,at(2300)));CHECK(inbox.current(id(2),at(2300)));
 CHECK(!inbox.receive(session,id(1),4,values,at(2400)));const auto* received=inbox.current(id(2),at(2400));CHECK(received&&received->sequence==2);
 auto forged=next;forged[0].postDb100[0]=-4000;CHECK(!inbox.receive(session,id(1),4,forged,at(2400)));
 CHECK(inbox.receive(session,id(1),4,{},at(2500)));CHECK(!inbox.current(id(2),at(2500)));CHECK(inbox.receive(session,id(1),4,next,at(2600)));CHECK(!inbox.current(id(2),at(2600)));
 ++next[0].sequence;CHECK(inbox.receive(session,id(1),4,next,at(2700)));CHECK(inbox.configure(session,id(1),4,peers));CHECK(inbox.current(id(2),at(2700)));
 std::thread wrong([&]{CHECK(!inbox.configure(session,id(1),4,peers));CHECK(!inbox.receive(session,id(1),4,next,at(2800)));CHECK(!inbox.current(id(2),at(2800)));inbox.clear();});wrong.join();CHECK(inbox.current(id(2),at(2800)));
 CHECK(!inbox.receive(session,id(1),4,next,at(2600)));CHECK(!inbox.current(id(2),at(2600)));
 CHECK(inbox.configure(session,id(1),5,peers));CHECK(!inbox.current(id(2),at(2800)));inbox.clear();CHECK(!inbox.receive(session,id(1),5,next,at(2900)));
}
GroupText text(std::string_view s){GroupText value;CHECK(setGroupText(s,value));return value;}
GroupIntent intent(unsigned member=1,bool maximum=false)
{GroupIntent value;value.group=id(90);value.member=id(member);value.name=text(maximum?std::string(128,'n'):"Cr\xc3\xa8me");
 value.role=text(maximum?std::string(128,'r'):"bass");value.key=text(maximum?std::string(128,'k'):"A");value.scale=text(maximum?std::string(128,'s'):"minor");value.bpm=120;CHECK(validateGroupIntent(value));return value;}
PatternDocument pattern(bool maximum=false)
{PatternDocument p;p.bars=8;p.noteCount=maximum?256:1;for(std::size_t n=0;n<p.noteCount;++n)p.notes[n]={double(n)/16,.03125,std::uint8_t(n%128),std::uint8_t(1+n%127),std::uint8_t(1+n%16)};CHECK(validatePatternDocument(p));return p;}
ProjectDocument project(unsigned member=1,bool maximum=false)
{ProjectDocument d;d.group=intent(member,maximum);d.pattern=pattern(maximum);d.sound.values[0]=-0.f;CHECK(validateProjectDocument(d));return d;}
PeerPlaybackObservation playback(bool maximum=false)
{PeerPlaybackObservation p;p.controls.availability=ObservedMidiAvailability::fresh;p.controls.reason=LiveControlReason::none;p.controls.values=defaultParameterValues();
 p.controls.values[0]=-0.f;p.controls.ageMs=2000;p.notes.meter=PhraseMeter{4,4};p.notes.bpm=120;p.notes.windowBeats=16;p.notes.ageMs=2000;
 p.notes.availability=ObservedMidiAvailability::fresh;p.notes.reason=ObservedMidiReason::none;p.notes.truncated=maximum;p.notes.noteCount=maximum?256:1;
 for(std::size_t n=0;n<p.notes.noteCount;++n)p.notes.notes[n]={n?double(n)/32:-0.,.015625,std::uint8_t(n%128),std::uint8_t(1+n%127),std::uint8_t(1+n%16),n%2==0};
 CHECK(validateObservedMidiSnapshot(p.notes));return p;}
std::size_t index(const char* name){auto r=parameterRegistry();auto it=std::find_if(r.begin(),r.end(),[&](const auto& p){return p.id==name;});CHECK(it!=r.end());return it==r.end()?0:std::size_t(it-r.begin());}
void byte(juce::MemoryBlock& b,std::size_t offset,unsigned value){CHECK(offset<b.getSize());if(offset<b.getSize())static_cast<std::uint8_t*>(b.getData())[offset]=std::uint8_t(value);}
void u32(juce::MemoryBlock& b,std::size_t offset,std::uint32_t value){CHECK(offset+4<=b.getSize());if(offset+4<=b.getSize())for(unsigned n=0;n<4;++n)static_cast<std::uint8_t*>(b.getData())[offset+n]=std::uint8_t(value>>(8*n));}
std::uint32_t read32(const juce::MemoryBlock& b,std::size_t offset){CHECK(offset+4<=b.getSize());if(offset+4>b.getSize())return 0;auto* p=static_cast<const std::uint8_t*>(b.getData())+offset;return std::uint32_t(p[0])|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
template<class T> bool bits(T a,T b){return std::memcmp(&a,&b,sizeof(T))==0;}
bool requestEqual(const GenerationRequest& a,const GenerationRequest& b)
{return a.structuralGeneration==b.structuralGeneration&&a.tag==b.tag&&a.source==b.source&&a.policy.scope==b.policy.scope&&a.policy.allowMacroChanges==b.policy.allowMacroChanges&&a.policy.allowSlotReuse==b.policy.allowSlotReuse&&a.policy.requirePattern==b.policy.requirePattern&&a.model==b.model&&a.instruction==b.instruction&&a.role==b.role&&a.key==b.key&&a.scale==b.scale&&a.capturedPhrase==b.capturedPhrase&&a.capturedBpm==b.capturedBpm;}
GroupGenerationCommand generation(bool maximum=false)
{GroupGenerationCommand c;c.request.tag={uuid(id(40)),UINT64_MAX,UINT64_MAX,0};c.request.source=project(1,maximum);c.request.policy={RequestScope::both,true,true,true};
 c.request.model=maximum?juce::String::repeatedString("m",256):juce::String("selected:model-1");c.request.instruction=maximum?std::string(8192,'i'):"Complement selected peers";
 c.request.role=maximum?std::string(128,'r'):"bass";c.request.key=maximum?std::string(128,'k'):"A";c.request.scale=maximum?std::string(128,'s'):"minor";
 c.request.capturedPhrase=pattern(maximum);c.request.capturedBpm=120;c.selection.loadRevision=UINT64_MAX;c.selection.peerCount=4;
 for(unsigned n=0;n<4;++n)c.selection.members[n]=id(n+2);c.parentProposal=c.request.tag;--c.parentProposal->requestId;return c;}
GroupCommand command(GroupAction action,bool maximum=false)
{GroupCommand c;c.operationId=UINT64_MAX;c.action=action;c.runtime=id(40);c.loadRevision=action==GroupAction::browse?0:UINT64_MAX;
 if(action==GroupAction::admit||action==GroupAction::publish)c.publication=GroupPublication{c.runtime,c.loadRevision,0,project(1,maximum),action==GroupAction::publish,playback(maximum)};
 if(action==GroupAction::validateProposal)c.proposal=RequestTag{uuid(c.runtime),1,0,UINT64_MAX};return c;}
GroupMemberSummary member(unsigned n,bool maximum=false)
{GroupMemberSummary m;m.runtime=id(n+50);m.loadRevision=UINT64_MAX;m.editRevision=0;m.intent=intent(n+1,maximum);m.mode=n%2?PlaybackMode::internalPattern:PlaybackMode::externalMidi;
 m.busy=n%2;m.conflicted=n%3==0;m.noteAvailability=ObservedMidiAvailability::fresh;m.noteReason=ObservedMidiReason::none;m.noteCount=256;m.noteAgeMs=2000;m.notesTruncated=true;
 m.controlAvailability=ObservedMidiAvailability::fresh;m.controlReason=LiveControlReason::none;m.controlAgeMs=2000;return m;}
GroupReply reply(GroupAction action,bool maximum=false)
{GroupReply r;r.operationId=UINT64_MAX;r.action=action;r.runtime=id(40);r.loadRevision=action==GroupAction::browse?0:UINT64_MAX;
 if(action==GroupAction::admit||action==GroupAction::publish){r.normalized=intent(1,maximum);r.registryRevision=UINT64_MAX;r.conflicted=true;}
 if(action==GroupAction::browse)for(unsigned n=0;n<(maximum?16u:2u);++n)r.members.push_back(member(n,maximum));
 if(action==GroupAction::validateProposal)r.proposal=RequestTag{uuid(r.runtime),1,0,UINT64_MAX};return r;}
template<class Decode,class T> void malformed(const juce::MemoryBlock& encoded,Decode decode,const T& before)
{auto reject=[&](const juce::MemoryBlock& b){auto target=before;CHECK(!decode(bytes(b),target));CHECK(target==before);};
 for(std::size_t n=0;n<std::min<std::size_t>(encoded.getSize(),45);++n)reject(juce::MemoryBlock(encoded.getData(),n));
 for(std::size_t n=45;n<encoded.getSize();n+=std::max<std::size_t>(1,encoded.getSize()/32))reject(juce::MemoryBlock(encoded.getData(),n));
 if(encoded.getSize())reject(juce::MemoryBlock(encoded.getData(),encoded.getSize()-1));auto trailing=encoded;const std::uint8_t extra=0;trailing.append(&extra,1);reject(trailing);
 reject(juce::MemoryBlock(maximumControlPayloadBytes+1,true));}
void group_performance_validation_and_age()
{
 section("group_performance_validation_and_age");GroupSelection selection;CHECK(!validateGroupSelection(selection));selection.loadRevision=1;CHECK(validateGroupSelection(selection));
 for(unsigned count=1;count<=4;++count){selection.peerCount=std::uint8_t(count);selection.members[count-1]=id(count);CHECK(validateGroupSelection(selection));}
 for(int mutation=0;mutation<4;++mutation){auto bad=selection;if(mutation==0)bad.loadRevision=0;if(mutation==1)bad.peerCount=5;if(mutation==2)bad.members[1]=bad.members[0];if(mutation==3)bad.members[3]={};CHECK(!validateGroupSelection(bad));}
 selection.peerCount=3;CHECK(!validateGroupSelection(selection));selection.members[3]={};CHECK(validateGroupSelection(selection));
 auto d=project();PeerPlaybackObservation absent;CHECK(validatePeerPlaybackObservation(absent,d));auto p=playback(true);CHECK(validatePeerPlaybackObservation(p,d));
 CHECK(std::bit_cast<std::uint32_t>(p.controls.values[0])==0x80000000u);CHECK(std::bit_cast<std::uint64_t>(p.notes.notes[0].startBeats)==0x8000000000000000ull);
 for(int mutation=0;mutation<16;++mutation){auto bad=p;
   if(mutation==0)bad.controls.availability=static_cast<ObservedMidiAvailability>(3);if(mutation==1)bad.controls.reason=static_cast<LiveControlReason>(6);
   if(mutation==2)bad.controls.ageMs=2001;if(mutation==3)bad.controls.reason=LiveControlReason::notCaptured;
   if(mutation==4)bad.controls.values[0]=std::numeric_limits<float>::quiet_NaN();if(mutation==5)bad.controls.values[0]=std::numeric_limits<float>::infinity();
   if(mutation==6)bad.controls.values[0]=-.01f;if(mutation==7)bad.controls.values[0]=1.01f;
   if(mutation==8)bad.controls.values[index("synth.1.modulation.1.target")]=1;
   if(mutation==9)bad.notes.noteCount=257;if(mutation==10)bad.notes.notes[0].channel=0;if(mutation==11)bad.notes.notes[0].velocity=0;
   if(mutation==12)bad.notes.notes[0].startBeats=std::numeric_limits<double>::infinity();if(mutation==13)bad.notes.notes[0].durationBeats=-1;
   if(mutation==14)bad.notes.reason=ObservedMidiReason::stopped;if(mutation==15)bad.notes.ageMs=2001;CHECK(!validatePeerPlaybackObservation(bad,d));}
 for(auto why:{LiveControlReason::notCaptured,LiveControlReason::readConflict,LiveControlReason::invalidRouting}){auto a=absent;a.controls.reason=why;CHECK(validatePeerPlaybackObservation(a,d));a.controls.values[0]=-0.f;CHECK(!validatePeerPlaybackObservation(a,d));}
 for(auto why:{LiveControlReason::stalePublisher,LiveControlReason::clockRollback}){auto a=p;a.controls.availability=ObservedMidiAvailability::stale;a.controls.reason=why;a.controls.ageMs=0;CHECK(validatePeerPlaybackObservation(a,d));}
 auto badProject=d;badProject.mode=static_cast<PlaybackMode>(2);CHECK(!validatePeerPlaybackObservation(absent,badProject));
 auto cap=std::make_unique<GroupPerformanceCapture>();cap->ownership.registry=id(99);cap->ownership.requesterRuntime=id(40);cap->ownership.loadRevision=1;cap->ownership.revision=1;cap->ownership.requester=*d.group;
 CHECK(validateGroupPerformanceCapture(*cap));for(unsigned n=0;n<4;++n){cap->ownership.peers.push_back({id(n+50),1,0,project(n+2)});cap->playback[n]=p;cap->peerCount=std::uint8_t(n+1);CHECK(validateGroupPerformanceCapture(*cap));}
 auto copy=std::make_unique<GroupPerformanceCapture>(*cap);copy->playback[0].notes.notes[0].pitch=127;copy->ownership.peers[0].project.sound.values[0]=.25f;CHECK(cap->playback[0].notes.notes[0].pitch!=127);CHECK(cap->ownership.peers[0].project.sound.values[0]!=.25f);
 for(int mutation=0;mutation<14;++mutation){auto bad=std::make_unique<GroupPerformanceCapture>(*cap);
   if(mutation==0)bad->ownership.registry={};if(mutation==1)bad->ownership.requesterRuntime={};if(mutation==2)bad->ownership.loadRevision=0;if(mutation==3)bad->ownership.revision=0;
   if(mutation==4)bad->peerCount=3;if(mutation==5)bad->ownership.peers[0].runtime=bad->ownership.requesterRuntime;if(mutation==6)bad->ownership.peers[1].runtime=bad->ownership.peers[0].runtime;
   if(mutation==7)bad->ownership.peers[0].loadRevision=0;if(mutation==8)bad->ownership.peers[0].project.group.reset();if(mutation==9)bad->ownership.peers[0].project.group->group=id(88);
   if(mutation==10)bad->ownership.peers[0].project.group->member=bad->ownership.requester.member;if(mutation==11)bad->ownership.peers[1].project.group->member=bad->ownership.peers[0].project.group->member;
   if(mutation==12)bad->ownership.peers[0].project.group->key=text("B");if(mutation==13)bad->ownership.peers[0].project.group->bpm=121;CHECK(!validateGroupPerformanceCapture(*bad));}
 auto unused=std::make_unique<GroupPerformanceCapture>(*cap);unused->ownership.peers.resize(3);unused->peerCount=3;unused->playback[3]={};CHECK(validateGroupPerformanceCapture(*unused));unused->playback[3].notes.ageMs=1;CHECK(!validateGroupPerformanceCapture(*unused));
 auto peerConflict=std::make_unique<GroupPerformanceCapture>(*cap);peerConflict->ownership.requester.key={};peerConflict->ownership.peers[1].project.group->key=text("B");CHECK(!validateGroupPerformanceCapture(*peerConflict));
 auto ages=playback();ages.controls.ageMs=ages.notes.ageMs=0;agePeerPlaybackObservation(ages,2000);CHECK(ages.controls.availability==ObservedMidiAvailability::fresh&&ages.notes.availability==ObservedMidiAvailability::fresh);CHECK(ages.controls.ageMs==2000&&ages.notes.ageMs==2000);
 agePeerPlaybackObservation(ages,1);CHECK(ages.controls.reason==LiveControlReason::stalePublisher&&ages.notes.reason==ObservedMidiReason::stalePublisher);CHECK(ages.controls.availability==ObservedMidiAvailability::stale&&ages.notes.availability==ObservedMidiAvailability::stale);CHECK(ages.controls.ageMs==2001&&ages.notes.ageMs==2001);
 ages.notes.reason=ObservedMidiReason::stopped;ages.controls.ageMs=ages.notes.ageMs=UINT64_MAX-1;agePeerPlaybackObservation(ages,10);CHECK(ages.controls.ageMs==UINT64_MAX&&ages.notes.ageMs==UINT64_MAX&&ages.notes.reason==ObservedMidiReason::stopped);
 ages=playback();agePeerPlaybackObservation(ages,0,true);CHECK(ages.controls.reason==LiveControlReason::clockRollback&&ages.notes.reason==ObservedMidiReason::clockRollback);CHECK(ages.controls.availability==ObservedMidiAvailability::stale&&ages.notes.availability==ObservedMidiAvailability::stale);CHECK(ages.controls.ageMs==UINT64_MAX&&ages.notes.ageMs==UINT64_MAX);
 auto unavailable=absent;unavailable.controls.reason=LiveControlReason::readConflict;agePeerPlaybackObservation(unavailable,UINT64_MAX,true);CHECK(unavailable.controls.reason==LiveControlReason::readConflict&&unavailable.notes.reason==ObservedMidiReason::noNotes);CHECK(unavailable.controls.ageMs==UINT64_MAX&&unavailable.notes.ageMs==UINT64_MAX);
}
// Retain released-main byte oracles and add eight35-control banks plus
// eight compatibility bytes. Live publication also carries one float per control.
constexpr std::size_t appendedStateBytes=8*saturationParametersPerSlot*9+8;
constexpr std::size_t appendedLiveBytes=8*saturationParametersPerSlot*4;
static_assert(appendedStateBytes==2528&&appendedLiveBytes==1120);
void group_commands_and_rosters()
{
 section("group_commands_and_rosters");
 for(auto action:{GroupAction::admit,GroupAction::publish,GroupAction::withdraw,GroupAction::browse,GroupAction::validateProposal}){
   auto c=command(action);juce::MemoryBlock encoded;bool ok=encodeGroupCommand(c,encoded);CHECK(ok);if(ok){auto out=command(GroupAction::browse);CHECK(decodeGroupCommand(bytes(encoded),out));CHECK(out==c);malformed(encoded,decodeGroupCommand,command(GroupAction::validateProposal));}
   auto reject=[&](const GroupCommand& bad){auto out=sentinel();CHECK(!encodeGroupCommand(bad,out));CHECK(out==sentinel());};
   auto bad=c;bad.operationId=0;reject(bad);bad=c;bad.runtime={};reject(bad);bad=c;bad.action=static_cast<GroupAction>(255);reject(bad);
   bad=c;bad.loadRevision=action==GroupAction::browse?1:0;reject(bad);bad=c;if(action==GroupAction::admit||action==GroupAction::publish)bad.publication.reset();else bad.publication=command(GroupAction::admit).publication;reject(bad);
   bad=c;if(action==GroupAction::validateProposal)bad.proposal.reset();else bad.proposal=command(GroupAction::validateProposal).proposal;reject(bad);
   if(c.publication){bad=c;bad.publication->runtime=id(41);reject(bad);bad=c;--bad.publication->loadRevision;reject(bad);bad=c;bad.publication->project.group.reset();reject(bad);bad=c;bad.publication->playback.controls.values[0]=NAN;reject(bad);if(action==GroupAction::admit){bad=c;bad.publication->busy=true;reject(bad);}}
   if(c.proposal){bad=c;bad.proposal->runtime=uuid(id(41));reject(bad);bad=c;bad.proposal->requestId=0;reject(bad);}
   auto r=reply(action);ok=encodeGroupReply(r,encoded);CHECK(ok);if(ok){auto out=reply(GroupAction::validateProposal);CHECK(decodeGroupReply(bytes(encoded),out));CHECK(out==r);malformed(encoded,decodeGroupReply,reply(GroupAction::validateProposal));}
   auto rejectReply=[&](const GroupReply& invalid){auto out=sentinel();CHECK(!encodeGroupReply(invalid,out));CHECK(out==sentinel());};
   auto invalid=r;invalid.operationId=0;rejectReply(invalid);invalid=r;invalid.runtime={};rejectReply(invalid);invalid=r;invalid.action=static_cast<GroupAction>(255);rejectReply(invalid);invalid=r;invalid.status=static_cast<GroupReplyStatus>(9);rejectReply(invalid);
   invalid=r;invalid.loadRevision=action==GroupAction::browse?1:0;rejectReply(invalid);
   invalid=r;if(action==GroupAction::admit||action==GroupAction::publish)invalid.normalized.reset();else invalid.normalized=intent();rejectReply(invalid);
   invalid=r;if(action==GroupAction::validateProposal)invalid.proposal.reset();else invalid.proposal=command(GroupAction::validateProposal).proposal;rejectReply(invalid);
   for(unsigned status=1;status<=8;++status){auto error=r;error.status=static_cast<GroupReplyStatus>(status);error.normalized.reset();error.registryRevision=0;error.conflicted=false;error.members.clear();CHECK(encodeGroupReply(error,encoded));auto out=r;CHECK(decodeGroupReply(bytes(encoded),out));CHECK(out==error);
     invalid=error;invalid.normalized=intent();rejectReply(invalid);invalid=error;invalid.registryRevision=1;rejectReply(invalid);invalid=error;invalid.conflicted=true;rejectReply(invalid);invalid=error;invalid.members.push_back(member(0));rejectReply(invalid);}
 }
 // Current combined codec sizes include keyed spectral controls and name fields.
 auto maximum=command(GroupAction::publish,true);juce::MemoryBlock encoded;bool ok=encodeGroupCommand(maximum,encoded);CHECK(ok);std::cout<<"GROUP_PUBLISH_BYTES wire="<<encoded.getSize()<<" project="<<read32(encoded,66)<<" controls="<<parameterCount<<'\n';CHECK(encoded.getSize()==66249+appendedStateBytes+appendedLiveBytes);
 if(ok){auto out=command(GroupAction::browse);CHECK(decodeGroupCommand(bytes(encoded),out));CHECK(out==maximum);CHECK(std::bit_cast<std::uint32_t>(out.publication->playback.controls.values[0])==0x80000000u);CHECK(std::bit_cast<std::uint64_t>(out.publication->playback.notes.notes[0].startBeats)==0x8000000000000000ull);
   for(std::size_t n=0;n<parameterCount;++n)CHECK(std::bit_cast<std::uint32_t>(out.publication->playback.controls.values[n])==std::bit_cast<std::uint32_t>(maximum.publication->playback.controls.values[n]));
   const auto projectLength=read32(encoded,66);CHECK(projectLength==44058+appendedStateBytes);const auto controls=71+projectLength;const auto notes=controls+10+4*parameterCount;
   auto reject=[&](const juce::MemoryBlock& bad){auto target=command(GroupAction::validateProposal),before=target;CHECK(!decodeGroupCommand(bytes(bad),target));CHECK(target==before);};
   for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned>>{{8,5},{33,2},{70+projectLength,2},{controls,3},{controls+1,6},{notes,2},{notes+1,0},{notes+2,3},{notes+3,2},{notes+28,3},{notes+29,12},{notes+30,2},{notes+33+19,2},{encoded.getSize()-1,2}}){auto bad=encoded;byte(bad,offset,value);reject(bad);}
   auto bad=encoded;u32(bad,66,UINT32_MAX);reject(bad);bad=encoded;u32(bad,controls+10,0x7f800000);reject(bad);bad=encoded;byte(bad,notes+31,1);byte(bad,notes+32,1);reject(bad);
 }
 auto roster=reply(GroupAction::browse,true);ok=encodeGroupReply(roster,encoded);CHECK(ok);CHECK(encoded.getSize()==9941);
 if(ok){GroupReply out;CHECK(decodeGroupReply(bytes(encoded),out));CHECK(out==roster);const auto prefix=36u;CHECK(out.members.size()==16);
   auto reject=[&](const juce::MemoryBlock& bad){auto target=reply(GroupAction::validateProposal),before=target;CHECK(!decodeGroupReply(bytes(bad),target));CHECK(target==before);};
   for(auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned>>{{8,5},{33,9},{34,2},{35,17},{prefix+593,2},{prefix+594,2},{prefix+595,2},{prefix+596,3},{prefix+597,12},{prefix+608,2},{prefix+609,3},{prefix+610,6},{encoded.getSize()-1,2}}){auto bad=encoded;byte(bad,offset,value);reject(bad);}
   auto bad=encoded;byte(bad,prefix+64,129);byte(bad,prefix+65,0);reject(bad);bad=encoded;byte(bad,prefix+66,0);reject(bad);bad=encoded;byte(bad,prefix+66,0xff);reject(bad);
   bad=encoded;std::memcpy(static_cast<std::uint8_t*>(bad.getData())+prefix+619,static_cast<const std::uint8_t*>(encoded.getData())+prefix,16);reject(bad);
   bad=encoded;std::memcpy(static_cast<std::uint8_t*>(bad.getData())+prefix+619+48,static_cast<const std::uint8_t*>(encoded.getData())+prefix+48,16);reject(bad);
 }
 auto rejectRoster=[](const GroupReply& bad){auto out=sentinel();CHECK(!encodeGroupReply(bad,out));CHECK(out==sentinel());};
 auto invalid=roster;invalid.members.push_back(member(17));rejectRoster(invalid);invalid=roster;invalid.members[0].mode=static_cast<PlaybackMode>(2);rejectRoster(invalid);
 invalid=roster;invalid.members[1].runtime=invalid.members[0].runtime;rejectRoster(invalid);invalid=roster;invalid.members[1].intent.member=invalid.members[0].intent.member;rejectRoster(invalid);
 invalid=roster;invalid.members[1].intent.member=invalid.members[0].intent.member;invalid.members[1].intent.group=id(91);CHECK(encodeGroupReply(invalid,encoded));
 for(int mutation=0;mutation<12;++mutation){auto r=roster;auto& m=r.members[0];if(mutation==0)m.loadRevision=0;if(mutation==1)m.runtime={};if(mutation==2)m.intent.name.length=129;if(mutation==3)m.intent.name.bytes[0]='\0';if(mutation==4)m.intent.name.bytes[0]=char(0xff);
   if(mutation==5)m.noteCount=257;if(mutation==6)m.noteCount=0;if(mutation==7)m.noteAgeMs=2001;if(mutation==8)m.noteReason=ObservedMidiReason::stopped;if(mutation==9)m.controlAgeMs=2001;if(mutation==10)m.controlReason=LiveControlReason::readConflict;if(mutation==11)m.intent.bpm=INFINITY;rejectRoster(r);}
 for(auto why:{ObservedMidiReason::stopped,ObservedMidiReason::stalePublisher,ObservedMidiReason::clockRollback}){auto r=roster;r.members[0].noteAvailability=ObservedMidiAvailability::stale;r.members[0].noteReason=why;r.members[0].noteAgeMs=0;CHECK(encodeGroupReply(r,encoded));}
 auto available=roster;available.members[0].noteAvailability=ObservedMidiAvailability::unavailable;available.members[0].noteReason=ObservedMidiReason::invalidTiming;available.members[0].noteCount=0;available.members[0].notesTruncated=true;available.members[0].controlAvailability=ObservedMidiAvailability::unavailable;available.members[0].controlReason=LiveControlReason::invalidRouting;CHECK(encodeGroupReply(available,encoded));
 auto admission=reply(GroupAction::admit,true);CHECK(encodeGroupReply(admission,encoded));CHECK(encoded.getSize()==607);auto validate=reply(GroupAction::validateProposal);CHECK(encodeGroupReply(validate,encoded));CHECK(encoded.getSize()==77);
}
void group_generation_and_frame_bounds()
{
 section("group_generation_and_frame_bounds");auto c=generation(true);juce::MemoryBlock ordinary;CHECK(encodeRequest(c.request,ordinary));CHECK(ordinary.getSize()==57904+appendedStateBytes);
 juce::MemoryBlock encoded;bool ok=encodeGroupGeneration(c,encoded);CHECK(ok);std::cout<<"GROUP_GENERATION_BYTES request="<<ordinary.getSize()<<" wire="<<encoded.getSize()<<'\n';CHECK(encoded.getSize()==58062+appendedStateBytes);
 if(ok){GroupGenerationCommand out;CHECK(decodeGroupGeneration(bytes(encoded),out));CHECK(requestEqual(out.request,c.request)&&out.selection==c.selection&&out.parentProposal==c.parentProposal);
   CHECK(read32(encoded,40)==ordinary.getSize());CHECK(std::memcmp(static_cast<const std::uint8_t*>(encoded.getData())+44,ordinary.getData(),ordinary.getSize())==0);
   RequestTag tag;CHECK(peekResultTag(bytes(encoded),tag));CHECK(tag==c.request.tag);const auto selected=44+ordinary.getSize();
   auto reject=[&](const juce::MemoryBlock& bad){auto target=generation(),before=target;CHECK(!decodeGroupGeneration(bytes(bad),target));CHECK(requestEqual(target.request,before.request)&&target.selection==before.selection&&target.parentProposal==before.parentProposal);};
   for(std::size_t length:{std::size_t(0),std::size_t(39),std::size_t(40),std::size_t(43),std::size_t(44),selected,selected+8,selected+9,selected+9+63,encoded.getSize()-1})reject(juce::MemoryBlock(encoded.getData(),length));
   auto bad=encoded;u32(bad,40,UINT32_MAX);reject(bad);bad=encoded;byte(bad,44,41);reject(bad);bad=encoded;byte(bad,selected+8,5);reject(bad);bad=encoded;byte(bad,selected+9+64,2);reject(bad);
   bad=encoded;std::memcpy(static_cast<std::uint8_t*>(bad.getData())+selected+9+16,static_cast<const std::uint8_t*>(encoded.getData())+selected+9,16);reject(bad);
   bad=encoded;const std::uint8_t extra=0;bad.append(&extra,1);reject(bad);reject(juce::MemoryBlock(maximumControlPayloadBytes+1,true));
 }
 auto reject=[](const GroupGenerationCommand& bad){auto out=sentinel();CHECK(!encodeGroupGeneration(bad,out));CHECK(out==sentinel());};
 for(int mutation=0;mutation<13;++mutation){auto bad=c;if(mutation==0)bad.request.source.group.reset();if(mutation==1)bad.request.tag.requestId=0;if(mutation==2)bad.request.model="invalid model";if(mutation==3)bad.selection.loadRevision=0;
   if(mutation==4)bad.selection.peerCount=5;if(mutation==5)bad.selection.members[1]=bad.selection.members[0];if(mutation==6)bad.selection.members[0]=bad.request.source.group->member;
   if(mutation==7)bad.parentProposal->runtime=uuid(id(41));if(mutation==8)bad.parentProposal->editRevision=0;if(mutation==9)bad.parentProposal->contextRevision=1;if(mutation==10)bad.parentProposal->requestId=0;
   if(mutation==11)bad.parentProposal->requestId=bad.request.tag.requestId;if(mutation==12)bad.request.instruction.append(1,'i');reject(bad);}
 auto generate=c;generate.parentProposal.reset();generate.selection.peerCount=0;generate.selection.members={};CHECK(encodeGroupGeneration(generate,encoded));GroupGenerationCommand out;CHECK(decodeGroupGeneration(bytes(encoded),out));CHECK(requestEqual(out.request,generate.request)&&out.selection==generate.selection&&!out.parentProposal);
 for(auto kind:{MessageKind::groupCommand,MessageKind::groupReply,MessageKind::generateGroup}){Frame f{kind,uuid(id(40)),sentinel()};CHECK(encodeFrame(f,encoded));CHECK(encoded.getSize()==32);Frame decoded;CHECK(decodeFrame(bytes(encoded),decoded));CHECK(decoded.kind==kind&&decoded.payload==f.payload);
   if(encoded.getSize()==32){auto old=encoded;byte(old,4,2);byte(old,5,0);Frame target{MessageKind::account,f.session,sentinel()};CHECK(!decodeFrame(bytes(old),target));CHECK(target.kind==MessageKind::account&&target.payload==sentinel());FrameHeader h{MessageKind::account,f.session,123};CHECK(!decodeFrameHeader(bytes(old).first(28),h));CHECK(h.kind==MessageKind::account&&h.payloadBytes==123);}
   f.payload=juce::MemoryBlock(messagePayloadCap(kind),true);CHECK(encodeFrame(f,encoded));CHECK(encoded.getSize()==messagePayloadCap(kind)+28);f.payload.append("x",1);auto unchanged=sentinel();CHECK(!encodeFrame(f,unchanged));CHECK(unchanged==sentinel());}
 CHECK(wireVersion==9&&wireHeaderBytes==28&&maximumControlPayloadBytes==1048576&&maximumResultPayloadBytes==8388608);
 auto header=sentinel();Frame invalid{static_cast<MessageKind>(255),uuid(id(40)),sentinel()};CHECK(!encodeFrame(invalid,header));CHECK(header==sentinel());
 auto r=c.request;for(unsigned status=0;status<=5;++status){GenerationResult result;result.tag=r.tag;result.status=static_cast<GenerationStatus>(status);juce::MemoryBlock payload;
   if(status!=0){CHECK(encodeResult(r,result,payload));GenerationStatus found=GenerationStatus::ready;CHECK(peekGenerationResultStatus(bytes(payload),found));CHECK(found==result.status);}
   juce::MemoryBlock prefix;CHECK(encodeRequest(r,prefix));byte(prefix,40,status);GenerationStatus found=GenerationStatus::providerFailure;CHECK(peekGenerationResultStatus(bytes(prefix).first(41),found));CHECK(found==static_cast<GenerationStatus>(status));}
 juce::MemoryBlock prefix;CHECK(encodeRequest(r,prefix));auto rejectPeek=[](WireBytes bad){auto status=GenerationStatus::cancelled;CHECK(!peekGenerationResultStatus(bad,status));CHECK(status==GenerationStatus::cancelled);};
 rejectPeek(bytes(prefix).first(40));byte(prefix,40,8);rejectPeek(bytes(prefix).first(41));auto bad=prefix;std::memset(bad.getData(),0,16);rejectPeek(bytes(bad).first(41));bad=prefix;std::memset(static_cast<std::uint8_t*>(bad.getData())+16,0,8);rejectPeek(bytes(bad).first(41));juce::MemoryBlock oversized(maximumResultPayloadBytes+1,true);rejectPeek(bytes(oversized));
}
void complete_group_capture_oracle()
{
 section(__func__);auto a=generation();a.request.structuralGeneration=UINT64_MAX;auto next=a.request;--next.structuralGeneration;CHECK(!requestEqual(a.request,next));juce::MemoryBlock wire;CHECK(encodeGroupGeneration(a,wire));GroupGenerationCommand out;CHECK(decodeGroupGeneration(bytes(wire),out));CHECK(requestEqual(a.request,out.request));
}

}
int main()
{
 spectrumArrivalDoesNotRefreshOldWindows();negotiatedSpectrumMessages();boundedSpectrumRoundTrip();complete_group_capture_oracle();
 FILETIME c{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER time{};time.LowPart=c.dwLowDateTime;time.HighPart=c.dwHighDateTime;
 wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(path)<<'\n';
 group_performance_validation_and_age();group_commands_and_rosters();group_generation_and_frame_bounds();CHECK(sections==7);std::cout<<"checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;
}
