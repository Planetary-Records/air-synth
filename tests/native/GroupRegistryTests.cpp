#include "GroupRegistry.h"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

using namespace airack;
using namespace airack::companion;
namespace
{
int checks=0,failures=0,sections=0;
const char* section="";
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;if(failures<100)std::cerr<<section<<':'<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
GroupUuid id(std::uint16_t value)
{GroupUuid result;result.bytes[0]=static_cast<std::uint8_t>(value);result.bytes[1]=static_cast<std::uint8_t>(value>>8);result.bytes[15]=64;return result;}
GroupText text(std::string_view value){GroupText result;CHECK(setGroupText(value,result));return result;}
GroupRegistration registration(std::uint64_t connection,std::uint16_t member=0,std::uint16_t group=1,PipePeer host={101,1001})
{
    GroupRegistration r;r.connectionId=connection;r.host=host;r.runtime=id(static_cast<std::uint16_t>(1000+connection));r.loadRevision=1;r.editRevision=3;
    GroupIntent intent;intent.group=id(200+group);intent.member=id(100+(member?member:static_cast<std::uint16_t>(connection)));
    intent.name=text("Session");intent.role=text("role-"+std::to_string(connection));intent.key=text("A");intent.scale=text("minor");intent.bpm=120;
    r.project.group=intent;r.project.sound.values[0]=.1f+static_cast<float>(connection%20)/100;
    r.project.pattern.noteCount=1;r.project.pattern.notes[0]={0,.25,static_cast<std::uint8_t>(40+connection%40),90,1};r.project.mode=PlaybackMode::internalPattern;
    CHECK(validateProjectDocument(r.project));return r;
}
GroupAdmission sentinelAdmission(){return { *registration(90).project.group,777,true };}
bool same(const GroupAdmission& a,const GroupAdmission& b){return a.intent==b.intent&&a.revision==b.revision&&a.conflicted==b.conflicted;}
GroupContext sentinelContext(){GroupContext out;out.registry=id(500);out.requesterRuntime=id(501);out.loadRevision=999;out.revision=777;out.requester=*registration(91).project.group;
    const auto r=registration(92);out.peers.push_back({r.runtime,r.loadRevision,r.editRevision,r.project});return out;}
struct Fixture
{
    GroupRegistry registry;std::vector<GroupRegistration> records;
    Fixture(){records.reserve(16);}
    GroupRegistration& add(GroupRegistration r)
    {const auto before=r.project;GroupAdmission out;const auto status=registry.admit(r,out);CHECK(status==GroupStatus::ready);CHECK(r.project==before);
        if(status==GroupStatus::ready){CHECK(out.intent.group==r.project.group->group);CHECK(validateGroupIntent(out.intent));CHECK(out.revision!=0);r.project.group=out.intent;}
        records.push_back(r);return records.back();}
    GroupContext view(std::uint64_t connection,std::initializer_list<GroupUuid> selected={})
    {auto out=sentinelContext();CHECK(registry.capture(connection,{selected.begin(),selected.size()},out)==GroupStatus::ready);CHECK(registry.current(connection,out));return out;}
};
void badAdmit(Fixture& f,const GroupRegistration& r,GroupStatus expected)
{auto out=sentinelAdmission();const auto before=out;const auto count=f.registry.memberCount();CHECK(f.registry.admit(r,out)==expected);CHECK(same(out,before));CHECK(f.registry.memberCount()==count);}
void badCapture(Fixture& f,std::uint64_t connection,std::initializer_list<GroupUuid> selected,GroupStatus expected)
{auto out=sentinelContext();const auto before=out;const auto count=f.registry.memberCount();CHECK(f.registry.capture(connection,{selected.begin(),selected.size()},out)==expected);CHECK(out==before);CHECK(f.registry.memberCount()==count);}
void badUpdate(Fixture& f,const GroupRegistration& owner,GroupUuid runtime,std::uint64_t load,const GroupIntent& intent,GroupStatus expected)
{auto out=sentinelAdmission();const auto before=out;const auto snapshot=f.view(1,{f.records[1].project.group->member});const auto count=f.registry.memberCount();
    CHECK(f.registry.updateIntent(owner.connectionId,runtime,load,intent,out)==expected);CHECK(same(out,before));CHECK(f.registry.memberCount()==count);CHECK(f.view(1,{f.records[1].project.group->member})==snapshot);CHECK(f.registry.current(1,snapshot));}
void badPublish(Fixture& f,const GroupRegistration& owner,GroupUuid runtime,std::uint64_t load,std::uint64_t edit,const ProjectDocument& project,GroupStatus expected)
{const auto snapshot=f.view(1,{f.records[1].project.group->member});const auto count=f.registry.memberCount();CHECK(f.registry.publish(owner.connectionId,runtime,load,edit,project)==expected);
    CHECK(f.registry.memberCount()==count);CHECK(f.view(1,{f.records[1].project.group->member})==snapshot);CHECK(f.registry.current(1,snapshot));}
void admittedSpectrumIsBoundedAndReadOnly()
{
    begin("admitted_spectrum_latest_value_and_read_only_authority");
    GroupRegistry registry;auto a=registration(1),b=registration(2);
    b.project.sound.layout.effects[0]={SlotUse::present,EffectKind::dynamicEq,masterChain};b.project.sound.layout.effectOrder[masterChain][0]=0;b.project.sound.layout.effectCount[masterChain]=1;CHECK(validateProjectDocument(b.project));
    GroupAdmission admission;CHECK(registry.admit(a,admission)==GroupStatus::ready);CHECK(registry.admit(b,admission)==GroupStatus::ready);
    GroupSpectrum spectrum;spectrum.runtime=b.runtime;spectrum.loadRevision=b.loadRevision;spectrum.sequence=1;spectrum.generation=1;spectrum.sampleRate=48000;spectrum.postDb100.fill(-3000);
    const auto now=std::chrono::steady_clock::time_point{};
    CHECK(registry.publishSpectrum(2,spectrum,now)==GroupStatus::ready);
    const std::array<GroupUuid,1> peers{b.project.group->member};std::vector<GroupSpectrum> result;
    CHECK(registry.readSpectra(1,a.runtime,a.loadRevision,peers,now,result)==GroupStatus::ready);CHECK(result.size()==1&&result.front()==spectrum);
    CHECK(registry.publishSpectrum(2,spectrum,now+std::chrono::seconds(1))==GroupStatus::staleRegistration);
    const auto at=[&](int ms){return now+std::chrono::milliseconds(ms);};
    GroupContext musical;CHECK(registry.capture(1,peers,musical)==GroupStatus::ready);
    CHECK(registry.publishSpectrum(99,spectrum,at(100))==GroupStatus::notAdmitted);
    auto wrong=spectrum;wrong.runtime=a.runtime;CHECK(registry.publishSpectrum(2,wrong,at(100))==GroupStatus::staleRegistration);
    wrong=spectrum;++wrong.loadRevision;CHECK(registry.publishSpectrum(2,wrong,at(100))==GroupStatus::staleRegistration);
    ++spectrum.sequence;spectrum.postDb100[0]=-3100;
    CHECK(registry.publishSpectrum(2,spectrum,at(99))==GroupStatus::capacity);CHECK(registry.publishSpectrum(2,spectrum,at(100))==GroupStatus::ready);
    const auto saved=result;CHECK(registry.readSpectra(1,a.runtime,a.loadRevision,peers,at(99),result)==GroupStatus::capacity);CHECK(result==saved);
    CHECK(registry.readSpectra(1,a.runtime,a.loadRevision,peers,at(100),result)==GroupStatus::ready);CHECK(result.size()==1&&result.front()==spectrum);
    CHECK(registry.readSpectra(1,b.runtime,a.loadRevision,peers,at(200),result)==GroupStatus::staleRegistration);
    CHECK(registry.readSpectra(1,a.runtime,2,peers,at(200),result)==GroupStatus::staleRegistration);
    ++spectrum.sequence;CHECK(registry.publishSpectrum(2,spectrum,at(50))==GroupStatus::staleRegistration);
    CHECK(registry.readSpectra(1,a.runtime,1,peers,at(50),result)==GroupStatus::staleRegistration);
    auto foreign=registration(3,0,1,{102,1002});auto otherGroup=registration(4,0,2);CHECK(registry.admit(foreign,admission)==GroupStatus::ready);CHECK(registry.admit(otherGroup,admission)==GroupStatus::ready);
    for(const auto member:{a.project.group->member,foreign.project.group->member,otherGroup.project.group->member,id(999)}){const std::array<GroupUuid,1> bad{member};CHECK(registry.readSpectra(1,a.runtime,1,bad,at(200),result)==GroupStatus::invalidSelection);CHECK(result.size()==1);}
    const std::array<GroupUuid,2> duplicate{peers[0],peers[0]};CHECK(registry.readSpectra(1,a.runtime,1,duplicate,at(200),result)==GroupStatus::invalidSelection);
    const std::array<GroupUuid,5> excess{};CHECK(registry.readSpectra(1,a.runtime,1,excess,at(200),result)==GroupStatus::invalidSelection);
    CHECK(registry.readSpectra(1,a.runtime,1,peers,at(2100),result)==GroupStatus::ready&&result.size()==1);
    CHECK(registry.readSpectra(1,a.runtime,1,peers,at(2201),result)==GroupStatus::ready&&result.empty());
    CHECK(registry.current(1,musical)); // Display summaries never invalidate musical/project authority.
    CHECK(registry.publishSpectrum(2,spectrum,at(2400))==GroupStatus::ready);
    CHECK(registry.withdrawSpectrum(2,a.runtime,1)==GroupStatus::staleRegistration);
    CHECK(registry.withdrawSpectrum(2,b.runtime,1)==GroupStatus::ready);
    CHECK(registry.readSpectra(1,a.runtime,1,peers,at(2400),result)==GroupStatus::ready&&result.empty());
    CHECK(registry.publishSpectrum(2,spectrum,at(2500))==GroupStatus::staleRegistration);
    ++spectrum.sequence;wrong=spectrum;wrong.slot=1;CHECK(registry.publishSpectrum(2,wrong,at(2500))==GroupStatus::invalidInput);
    CHECK(registry.publishSpectrum(2,spectrum,at(2500))==GroupStatus::ready);
    auto removed=b.project;removed.sound.layout.effects[0].use=SlotUse::retired;removed.sound.layout.effectOrder[masterChain][0]=noSlot;removed.sound.layout.effectCount[masterChain]=0;
    CHECK(registry.updatePublication(2,b.runtime,1,4,removed,admission)==GroupStatus::ready);
    CHECK(registry.readSpectra(1,a.runtime,1,peers,at(2600),result)==GroupStatus::ready&&result.empty());
    CHECK(registry.publishSpectrum(2,spectrum,at(2600))==GroupStatus::invalidInput);
    CHECK(registry.withdraw(2));CHECK(registry.readSpectra(1,a.runtime,1,peers,at(2700),result)==GroupStatus::invalidSelection);
}
void empty_metadata_authority() {
    // Empty metadata has the same semantic state as null metadata in the codec.
    for(bool left:{false,true})for(bool right:{false,true}) {
        auto a=registration(1).project,b=a;
        if(left)a.sound.personalities=std::make_shared<RackPersonalityState>();
        if(right)b.sound.personalities=std::make_shared<RackPersonalityState>();
        CHECK(validateProjectDocument(a)&&validateProjectDocument(b));CHECK(exactProject(a,b));CHECK(exactProject(b,a));
        Fixture f;auto r=registration(1);r.project=a;auto& record=f.add(r);const auto captured=f.view(1);
        CHECK(f.registry.publish(1,record.runtime,record.loadRevision,record.editRevision,b)==GroupStatus::ready);CHECK(f.registry.current(1,captured));
    }
}
void personality_deep_and_signed_zero_authority()
{
    begin(__func__);Fixture f;auto a=registration(1),b=registration(2);auto personalities=std::make_shared<RackPersonalityState>();RackInstance instance;instance.design.name="Exact peer";instance.design.units={{0,NativeModule::analog,{{0,"analog.filter.cutoff",0.f}}}};instance.design.panel.sections={"Tone"};instance.design.panel.controls={{"Width",0,0,"analog.osc.1.pulse_width",FocusWidget::rotary,FocusResponse::linear,.05f,.95f}};instance.bindings={{0,0}};personalities->instances.push_back(instance);b.project.sound.personalities=personalities;CHECK(validateProjectDocument(b.project));f.add(a);const auto owner=f.add(b);const auto context=f.view(1,{owner.project.group->member});
    auto changed=owner.project;auto p=std::make_shared<RackPersonalityState>(*changed.sound.personalities);p->instances[0].design.units[0].defaults[0].normalized=-0.f;changed.sound.personalities=p;CHECK(changed==owner.project);
    CHECK(f.registry.publish(owner.connectionId,owner.runtime,owner.loadRevision,owner.editRevision,changed)==GroupStatus::staleRegistration);CHECK(f.registry.current(1,context));
    p->instances[0].design.name="Metadata-only peer update";CHECK(f.registry.publish(owner.connectionId,owner.runtime,owner.loadRevision,owner.editRevision+1,changed)==GroupStatus::ready);CHECK(!f.registry.current(1,context));
}


void personality_range_signed_zero_authority()
{
    begin(__func__);Fixture f;auto a=registration(1),b=registration(2);auto state=std::make_shared<RackPersonalityState>();RackInstance i;i.design.name="Range bits";i.design.units={{0,NativeModule::analog,{}}};i.design.panel.sections={"Tone"};i.design.panel.controls={{"Resonance",0,0,"analog.filter.resonance",FocusWidget::rotary,FocusResponse::linear,0.f,.5f}};i.bindings={{0,0}};state->instances.push_back(i);b.project.sound.personalities=state;CHECK(validateProjectDocument(b.project));f.add(a);const auto owner=f.add(b);const auto context=f.view(1,{owner.project.group->member});auto changed=owner.project;auto copy=std::make_shared<RackPersonalityState>(*changed.sound.personalities);copy->instances[0].design.panel.controls[0].minimum=-0.f;changed.sound.personalities=copy;CHECK(changed==owner.project);CHECK(f.registry.publish(owner.connectionId,owner.runtime,owner.loadRevision,owner.editRevision,changed)==GroupStatus::staleRegistration);CHECK(f.registry.current(1,context));
}

void admission_and_member_copy()
{
    begin(__func__);static_assert(!std::is_copy_constructible_v<GroupRegistry> && std::is_copy_constructible_v<GroupPeerSnapshot> && !std::is_trivially_copyable_v<GroupPeerSnapshot> && std::is_trivially_copyable_v<AudioProjectState>);
    CHECK(maximumGroupMembers==16&&maximumSelectedGroupPeers==4);Fixture f;CHECK(f.registry.memberCount()==0);
    auto& first=f.add(registration(1));const auto original=first.project;const auto old=f.view(1);
    auto copied=registration(2,1);const auto source=copied.project;GroupAdmission admitted;CHECK(f.registry.admit(copied,admitted)==GroupStatus::ready);
    CHECK(copied.project==source);CHECK(admitted.intent.member!=source.group->member&&!admitted.intent.member.isNull());
    CHECK(juce::Uuid(admitted.intent.member.getRawData())!=juce::Uuid(source.group->member.getRawData()));
    CHECK(admitted.intent.group==source.group->group&&admitted.intent.role==source.group->role);CHECK(f.registry.memberCount()==2);CHECK(!f.registry.current(1,old));
    auto capture=f.view(1,{admitted.intent.member});CHECK(capture.requester==*original.group);CHECK(capture.peers.size()==1);
    if(capture.peers.size()==1){auto expected=source;expected.group=admitted.intent;CHECK(capture.peers[0].project==expected);CHECK(capture.peers[0].runtime==copied.runtime);}
    auto incumbent=f.view(2,{first.project.group->member});if(incumbent.peers.size()==1)CHECK(incumbent.peers[0].project==original);
    auto duplicate=registration(3);duplicate.runtime=GroupUuid(juce::Uuid(first.runtime.getRawData()));badAdmit(f,duplicate,GroupStatus::duplicateRuntime);
    duplicate=registration(3,3,2);duplicate.runtime=first.runtime;badAdmit(f,duplicate,GroupStatus::duplicateRuntime);
    duplicate=registration(1,9);badAdmit(f,duplicate,GroupStatus::staleRegistration);CHECK(f.registry.current(1,capture));
    for(int field=0;field<8;++field){auto bad=registration(4);switch(field){case 0:bad.connectionId=0;break;case 1:bad.host.processId=0;break;case 2:bad.host.creationTime=0;break;
        case 3:bad.runtime=GroupUuid{};break;case 4:bad.loadRevision=0;break;case 5:bad.project.group.reset();break;case 6:bad.project.group->name={};break;case 7:bad.project.sound.values[0]=std::numeric_limits<float>::quiet_NaN();break;}
        badAdmit(f,bad,GroupStatus::invalidInput);CHECK(f.registry.current(1,capture));}
    Fixture capacity;for(std::uint64_t i=1;i<=16;++i)capacity.add(registration(i));CHECK(capacity.registry.memberCount()==16);
    const auto full=capacity.view(1,{capacity.records[15].project.group->member});badAdmit(capacity,registration(17),GroupStatus::capacity);CHECK(capacity.registry.current(1,full));
}
void process_and_owner_isolation()
{
    begin(__func__);Fixture f;auto& a=f.add(registration(1));auto& b=f.add(registration(2));
    auto reused=registration(3,3,1,{101,2002});reused.runtime=a.runtime;f.add(reused);
    auto other=registration(4,4,1,{202,1001});other.runtime=a.runtime;f.add(other);f.add(registration(5,5,2));
    const auto local=f.view(1,{b.project.group->member});const auto pidReuse=f.view(3);const auto differentProcess=f.view(4);const auto otherGroup=f.view(5);
    CHECK(local.requester.group==pidReuse.requester.group&&local.requester.group==differentProcess.requester.group);
    badCapture(f,1,{reused.project.group->member},GroupStatus::invalidSelection);badCapture(f,1,{other.project.group->member},GroupStatus::invalidSelection);
    badCapture(f,1,{f.records[4].project.group->member},GroupStatus::invalidSelection);badCapture(f,99,{},GroupStatus::notAdmitted);
    badUpdate(f,b,id(44),b.loadRevision,*b.project.group,GroupStatus::staleRegistration);
    badUpdate(f,b,b.runtime,b.loadRevision+1,*b.project.group,GroupStatus::staleRegistration);
    badPublish(f,b,id(44),b.loadRevision,4,b.project,GroupStatus::staleRegistration);
    badPublish(f,b,b.runtime,b.loadRevision+1,4,b.project,GroupStatus::staleRegistration);
    auto changed=*b.project.group;changed.group=id(8);badUpdate(f,b,b.runtime,b.loadRevision,changed,GroupStatus::invalidInput);
    changed=*b.project.group;changed.member=id(8);badUpdate(f,b,b.runtime,b.loadRevision,changed,GroupStatus::invalidInput);
    changed=*b.project.group;changed.key.bytes[127]=1;badUpdate(f,b,b.runtime,b.loadRevision,changed,GroupStatus::invalidInput);
    GroupAdmission output=sentinelAdmission();const auto sentinel=output;CHECK(f.registry.updateIntent(99,id(1),1,*b.project.group,output)==GroupStatus::notAdmitted);CHECK(same(output,sentinel));
    CHECK(f.registry.publish(99,id(1),1,1,b.project)==GroupStatus::notAdmitted);
    CHECK(f.registry.current(3,pidReuse)&&f.registry.current(4,differentProcess)&&f.registry.current(5,otherGroup));
    CHECK(f.registry.withdraw(2));CHECK(!f.registry.current(1,local));CHECK(!f.registry.withdraw(2));CHECK(f.registry.memberCount()==4);
    auto replacement=b;replacement.runtime=id(2000);replacement.loadRevision=2;GroupAdmission admitted;CHECK(f.registry.admit(replacement,admitted)==GroupStatus::ready);
    auto fresh=f.view(1,{replacement.project.group->member});CHECK(!f.registry.current(1,local));
    badPublish(f,b,b.runtime,b.loadRevision,b.editRevision,b.project,GroupStatus::staleRegistration);CHECK(f.registry.current(1,fresh));
    CHECK(f.registry.current(3,pidReuse)&&f.registry.current(4,differentProcess)&&f.registry.current(5,otherGroup));
}
void explicit_conflict_resolution()
{
    begin(__func__);Fixture f;f.add(registration(1));f.add(registration(2));auto third=registration(3);third.project.group->key={};third.project.group->scale={};third.project.group->bpm.reset();
    auto& peer=f.add(third);const auto selected=f.records[1].project.group->member;const auto before=f.view(1,{selected});GroupAdmission out;
    CHECK(!out.conflicted);auto intent=*peer.project.group;intent.key=text("B");CHECK(f.registry.updateIntent(3,peer.runtime,1,intent,out)==GroupStatus::ready);CHECK(out.conflicted);
    badCapture(f,1,{selected},GroupStatus::conflict);CHECK(!f.registry.current(1,before));
    intent.key=text("A");CHECK(f.registry.updateIntent(3,peer.runtime,1,intent,out)==GroupStatus::ready);CHECK(!out.conflicted);peer.project.group=out.intent;
    auto resolved=f.view(1,{selected});CHECK(resolved.requester.key==text("A")&&resolved.requester.bpm==120);if(resolved.peers.size()==1)CHECK(resolved.peers[0].project==f.records[1].project);
    for(int field=0;field<2;++field){intent=*peer.project.group;if(field==0)intent.scale=text("major");else intent.bpm=121;
        CHECK(f.registry.updateIntent(3,peer.runtime,1,intent,out)==GroupStatus::ready);CHECK(out.conflicted);badCapture(f,1,{},GroupStatus::conflict);
        if(field==0)intent.scale={};else intent.bpm.reset();CHECK(f.registry.updateIntent(3,peer.runtime,1,intent,out)==GroupStatus::ready);CHECK(!out.conflicted);peer.project.group=out.intent;
        resolved=f.view(1,{selected});CHECK(resolved.requester.scale==text("minor")&&resolved.requester.bpm==120);}
    auto conflicting=registration(4);conflicting.project.group->key=text("C");CHECK(f.registry.admit(conflicting,out)==GroupStatus::ready);CHECK(out.conflicted);badCapture(f,1,{selected},GroupStatus::conflict);
    CHECK(f.registry.withdraw(4));resolved=f.view(1,{selected});CHECK(resolved.requester==*f.records[0].project.group);
    intent=*peer.project.group;intent.name=text("Other name");intent.role=text("lead");const auto prior=resolved;
    CHECK(f.registry.updateIntent(3,peer.runtime,1,intent,out)==GroupStatus::ready);CHECK(!out.conflicted);peer.project.group=out.intent;CHECK(!f.registry.current(1,prior));CHECK(f.registry.current(1,f.view(1,{selected})));
}
void committed_edits_and_epochs()
{
    begin(__func__);Fixture f;f.add(registration(1));auto& peer=f.add(registration(2));f.add(registration(3,3,2));f.add(registration(4,4,1,{101,2002}));
    auto view=f.view(1,{peer.project.group->member});const auto unrelated=f.view(3);const auto incarnation=f.view(4);GroupAdmission out;
    CHECK(f.registry.updateIntent(2,peer.runtime,1,*peer.project.group,out)==GroupStatus::ready);CHECK(out.revision==view.revision);
    CHECK(f.registry.publish(2,peer.runtime,1,peer.editRevision,peer.project)==GroupStatus::ready);CHECK(f.registry.current(1,view));
    auto changed=peer.project;changed.sound.values[0]=.7f;badPublish(f,peer,peer.runtime,1,peer.editRevision,changed,GroupStatus::staleRegistration);
    badPublish(f,peer,peer.runtime,1,peer.editRevision-1,peer.project,GroupStatus::staleRegistration);
    changed=peer.project;changed.group->key=text("B");badPublish(f,peer,peer.runtime,1,4,changed,GroupStatus::invalidInput);
    changed=peer.project;changed.pattern.notes[0].durationBeats=std::numeric_limits<double>::infinity();badPublish(f,peer,peer.runtime,1,4,changed,GroupStatus::invalidInput);
    for(int edit=0;edit<4;++edit){changed=peer.project;if(edit==0)changed.sound.values[0]=-0.0f;if(edit==1)changed.sound.controlLocks[0]=true;
        if(edit==2)changed.pattern.notes[0].velocity=91;if(edit==3)changed.sound.values[0]=.6f;
        ++peer.editRevision;CHECK(f.registry.publish(2,peer.runtime,1,peer.editRevision,changed)==GroupStatus::ready);CHECK(!f.registry.current(1,view));peer.project=changed;
        const auto newer=f.view(1,{peer.project.group->member});CHECK(newer.revision>view.revision);CHECK(newer.peers.size()==1);
        if(newer.peers.size()==1){CHECK(newer.peers[0].project==peer.project);CHECK(newer.peers[0].editRevision==peer.editRevision);
            if(edit==0){CHECK(std::bit_cast<std::uint32_t>(newer.peers[0].project.sound.values[0])==0x80000000U);auto altered=newer;altered.peers[0].project.sound.values[0]=0.0f;CHECK(!f.registry.current(1,altered));
                auto positive=peer.project;positive.sound.values[0]=0.0f;badPublish(f,peer,peer.runtime,1,peer.editRevision,positive,GroupStatus::staleRegistration);}}
        view=newer;CHECK(f.registry.current(3,unrelated)&&f.registry.current(4,incarnation));}
    ++peer.editRevision;CHECK(f.registry.publish(2,peer.runtime,1,peer.editRevision,peer.project)==GroupStatus::ready);CHECK(!f.registry.current(1,view));view=f.view(1,{peer.project.group->member});
    const auto before=view;CHECK(f.registry.withdraw(2));CHECK(!f.registry.current(1,before));CHECK(f.registry.withdraw(1));CHECK(f.registry.memberCount()==2);
    auto rejoin=registration(1);rejoin.runtime=id(3000);rejoin.loadRevision=2;CHECK(f.registry.admit(rejoin,out)==GroupStatus::ready);const auto renewed=f.view(1);CHECK(renewed.revision>before.revision);CHECK(!f.registry.current(1,before));
    Fixture restart;restart.add(rejoin);const auto restarted=restart.view(1);CHECK(restarted.registry!=renewed.registry);CHECK(!restart.registry.current(1,renewed));CHECK(!f.registry.current(1,restarted));
    CHECK(f.registry.current(3,unrelated)&&f.registry.current(4,incarnation));
}
void selected_capture_and_transactional_limits()
{
    begin(__func__);Fixture f;
    for(std::uint64_t i=1;i<=6;++i){auto r=registration(i);r.project.group->name=text(std::string(128,'n'));r.project.group->role=text(std::string(128,'r'));
        r.project.group->key=text(std::string(128,'k'));r.project.group->scale=text(std::string(128,'s'));r.project.pattern.bars=8;r.project.pattern.noteCount=256;
        for(std::size_t n=0;n<256;++n)r.project.pattern.notes[n]={double(n)/8,.125,static_cast<std::uint8_t>(n%128),90,1};f.add(r);}
    auto member=[&](std::size_t n){return f.records[n-1].project.group->member;};const auto ordered=f.view(1,{member(5),member(2),member(4),member(3)});CHECK(ordered.peers.size()==4);
    if(ordered.peers.size()==4){for(std::size_t n=0;n<4;++n){const auto index=std::array<std::size_t,4>{4,1,3,2}[n];CHECK(ordered.peers[n].project==f.records[index].project);CHECK(ordered.peers[n].runtime==f.records[index].runtime);CHECK(ordered.peers[n].project.pattern.noteCount==256);}}
    const auto reverse=f.view(1,{member(3),member(4),member(2),member(5)});CHECK(f.registry.current(1,ordered)&&f.registry.current(1,reverse));CHECK(ordered!=reverse);
    const auto empty=f.view(1);CHECK(empty.peers.empty());CHECK(f.registry.current(1,ordered));
    badCapture(f,1,{member(2),member(3),member(4),member(5),member(6)},GroupStatus::invalidSelection);
    badCapture(f,1,{member(2),member(2)},GroupStatus::invalidSelection);badCapture(f,1,{member(1)},GroupStatus::invalidSelection);badCapture(f,1,{id(65000)},GroupStatus::invalidSelection);
    CHECK(f.registry.current(1,ordered));
    for(int field=0;field<10;++field){auto altered=ordered;switch(field){case 0:altered.registry=id(65000);break;case 1:altered.requesterRuntime=id(65000);break;case 2:++altered.loadRevision;break;
        case 3:++altered.revision;break;case 4:altered.requester.role=text("changed");break;
        case 5:if(!altered.peers.empty())altered.peers[0].runtime=id(65000);break;case 6:if(!altered.peers.empty())++altered.peers[0].loadRevision;break;
        case 7:if(!altered.peers.empty())++altered.peers[0].editRevision;break;case 8:if(!altered.peers.empty())altered.peers[0].project.pattern.notes[0].velocity=99;break;
        case 9:if(altered.peers.size()>1)altered.peers[1]=altered.peers[0];break;}
        CHECK(!f.registry.current(1,altered));CHECK(f.registry.current(1,ordered));}
    auto extra=ordered;if(!extra.peers.empty())extra.peers.push_back(extra.peers[0]);CHECK(!f.registry.current(1,extra));CHECK(!f.registry.current(99,ordered));
    if(!ordered.peers.empty()){auto altered=ordered;altered.peers[0].project.group.reset();CHECK(!f.registry.current(1,altered));}
}
void epoch_exhaustion_withdraws_permission()
{
    begin(__func__);Fixture f;auto& first=f.add(registration(1));auto& peer=f.add(registration(2));f.add(registration(3,3,2));const auto unrelated=f.view(3);
    const auto old=f.view(1,{peer.project.group->member});airack::companion::detail::GroupRegistryTestAccess::setNextRevision(f.registry,std::numeric_limits<std::uint64_t>::max());
    ++first.editRevision;CHECK(f.registry.publish(1,first.runtime,1,first.editRevision,first.project)==GroupStatus::ready);
    const auto last=f.view(1,{peer.project.group->member});CHECK(last.revision==std::numeric_limits<std::uint64_t>::max());CHECK(!f.registry.current(1,old));CHECK(f.registry.current(3,unrelated));
    GroupAdmission out;CHECK(f.registry.updateIntent(1,first.runtime,1,*first.project.group,out)==GroupStatus::ready);CHECK(out.revision==last.revision);
    CHECK(f.registry.publish(1,first.runtime,1,first.editRevision,first.project)==GroupStatus::ready);CHECK(f.registry.current(1,last));
    badAdmit(f,registration(4),GroupStatus::revisionExhausted);CHECK(f.registry.current(1,last));
    auto intent=*peer.project.group;intent.role=text("changed");badUpdate(f,peer,peer.runtime,1,intent,GroupStatus::revisionExhausted);
    auto changed=peer.project;changed.sound.values[0]=.8f;badPublish(f,peer,peer.runtime,1,peer.editRevision+1,changed,GroupStatus::revisionExhausted);CHECK(f.registry.current(1,last));
    CHECK(f.registry.withdraw(1));CHECK(f.registry.memberCount()==2);CHECK(!f.registry.current(1,last));badCapture(f,2,{},GroupStatus::revisionExhausted);CHECK(!f.registry.current(2,last));
    CHECK(f.registry.current(3,unrelated));badAdmit(f,first,GroupStatus::revisionExhausted);CHECK(f.registry.memberCount()==2);
    CHECK(f.registry.withdraw(2));CHECK(f.registry.memberCount()==1);badCapture(f,2,{},GroupStatus::notAdmitted);CHECK(!f.registry.withdraw(2));CHECK(f.registry.current(3,unrelated));
}
}
int main(int argc,char** argv)
{
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);ULARGE_INTEGER time{};time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
    admittedSpectrumIsBoundedAndReadOnly();empty_metadata_authority();personality_range_signed_zero_authority();personality_deep_and_signed_zero_authority();admission_and_member_copy();process_and_owner_isolation();explicit_conflict_resolution();committed_edits_and_epochs();selected_capture_and_transactional_limits();epoch_exhaustion_withdraws_permission();
    CHECK(sections==9);std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
}
