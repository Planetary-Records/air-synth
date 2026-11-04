#include "GroupLinkState.h"
#include <windows.h>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

using namespace airack;
namespace
{
int checks=0,failures=0,sections=0;const char* section="";
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;if(failures<100)std::cerr<<section<<':'<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
GroupUuid id(std::uint8_t value){GroupUuid result;result.bytes[0]=value;result.bytes[15]=64;return result;}
GroupText text(std::string_view value){GroupText result;CHECK(setGroupText(value,result));return result;}
GroupIntent intent()
{GroupIntent result;result.group=id(10);result.member=id(11);result.name=text("Session");result.role=text("bass");result.key=text("A");result.scale=text("minor");result.bpm=120;CHECK(validateGroupIntent(result));return result;}
GroupIntent invalid(GroupIntent value,int field)
{
    switch(field){case 0:value.group={};break;case 1:value.member={};break;case 2:value.name={};break;case 3:value.role.length=129;break;
        case 4:value.key.bytes[127]=1;break;case 5:value.scale.bytes[0]=static_cast<char>(0xff);break;case 6:value.name.bytes[0]=0;break;
        case 7:value.bpm=std::numeric_limits<double>::quiet_NaN();break;case 8:value.bpm=19;break;case 9:value.bpm=401;break;}
    CHECK(!validateGroupIntent(value));return value;
}
void rejectedAck(GroupLinkState& state,GroupUuid runtime,std::uint64_t load,const GroupIntent& requested,const GroupIntent& normalized,GroupLinkStatus expected)
{const auto before=state.view();const auto eligible=state.eligible();CHECK(state.acknowledge(runtime,load,requested,normalized)==expected);CHECK(state.view()==before);CHECK(state.eligible()==eligible);}
void explicit_permission_and_load()
{
    begin(__func__);static_assert(!std::is_copy_constructible_v<GroupLinkState> && std::is_trivially_copyable_v<GroupLinkView>);
    const auto saved=intent();GroupLinkState state(id(1));const auto first=state.view();CHECK(first.runtime==id(1));CHECK(first.loadRevision==1);
    CHECK(!first.intent&&first.phase==GroupLinkPhase::unlinked&&!first.exhausted);CHECK(!state.eligible());
    CHECK(state.resume()==GroupLinkStatus::notLinked);CHECK(state.view()==first);state.disconnected();CHECK(state.view()==first);
    rejectedAck(state,id(1),1,saved,saved,GroupLinkStatus::staleReply);
    for(int field=0;field<10;++field){const auto bad=invalid(saved,field);CHECK(state.restore(bad)==GroupLinkStatus::invalidIntent);CHECK(state.view()==first);
        CHECK(state.link(bad)==GroupLinkStatus::invalidIntent);CHECK(state.view()==first);}
    CHECK(state.restore(saved)==GroupLinkStatus::ready);const auto pending=state.view();CHECK(pending.runtime==first.runtime&&pending.loadRevision==2);
    CHECK(pending.intent==saved&&pending.phase==GroupLinkPhase::pending&&!pending.exhausted);CHECK(!state.eligible());
    state.disconnected();CHECK(state.view()==pending);rejectedAck(state,id(1),pending.loadRevision,saved,saved,GroupLinkStatus::staleReply);
    CHECK(state.restore(saved)==GroupLinkStatus::ready);CHECK(state.view().loadRevision==3&&state.view().phase==GroupLinkPhase::pending);CHECK(!state.eligible());
    CHECK(state.resume()==GroupLinkStatus::ready);const auto authorized=state.view();CHECK(authorized.loadRevision==4&&authorized.phase==GroupLinkPhase::authorized);
    CHECK(authorized.intent==saved&&state.eligible());CHECK(state.resume()==GroupLinkStatus::ready);CHECK(state.view()==authorized);
    CHECK(state.link(saved)==GroupLinkStatus::ready);CHECK(state.view()==authorized);
    CHECK(state.acknowledge(id(1),authorized.loadRevision,saved,saved)==GroupLinkStatus::ready);const auto admitted=state.view();
    CHECK(admitted.phase==GroupLinkPhase::admitted&&admitted.intent==saved&&admitted.loadRevision==authorized.loadRevision);CHECK(state.eligible());
    CHECK(state.acknowledge(id(1),admitted.loadRevision,saved,saved)==GroupLinkStatus::ready);CHECK(state.view()==admitted);
    CHECK(state.resume()==GroupLinkStatus::ready);CHECK(state.link(saved)==GroupLinkStatus::ready);CHECK(state.view()==admitted);
    state.disconnected();CHECK(state.view()==authorized);CHECK(state.eligible());
    CHECK(state.restore(saved)==GroupLinkStatus::ready);CHECK(state.view().loadRevision==5&&state.view().phase==GroupLinkPhase::pending);CHECK(!state.eligible());
    rejectedAck(state,id(1),authorized.loadRevision,saved,saved,GroupLinkStatus::staleReply);
    CHECK(state.restore(std::nullopt)==GroupLinkStatus::ready);CHECK(state.view().loadRevision==6&&!state.view().intent&&state.view().phase==GroupLinkPhase::unlinked);CHECK(!state.eligible());
    CHECK(state.link(saved)==GroupLinkStatus::ready);CHECK(state.view().loadRevision==7&&state.eligible());
    auto settings=saved;settings.role=text("lead");CHECK(state.link(settings)==GroupLinkStatus::ready);CHECK(state.view().loadRevision==8&&state.view().intent==settings);CHECK(state.eligible());
    state.leave();CHECK(state.view().loadRevision==9&&!state.view().intent&&state.view().phase==GroupLinkPhase::unlinked);CHECK(!state.eligible());
    state.leave();CHECK(state.view().loadRevision==10&&!state.view().intent);CHECK(state.resume()==GroupLinkStatus::notLinked);
    GroupLinkState copied(id(2));CHECK(copied.restore(saved)==GroupLinkStatus::ready);CHECK(copied.view().phase==GroupLinkPhase::pending&&!copied.eligible());
    CHECK(state.view().runtime!=copied.view().runtime);CHECK(copied.resume()==GroupLinkStatus::ready);CHECK(!state.eligible());
    auto maximum=saved;maximum.name=text(std::string(128,'n'));maximum.role=text(std::string(128,'r'));maximum.key=text(std::string(128,'k'));maximum.scale=text(std::string(128,'s'));maximum.bpm=400;
    CHECK(state.restore(maximum)==GroupLinkStatus::ready);CHECK(state.view().intent==maximum&&!state.eligible());CHECK(state.resume()==GroupLinkStatus::ready);
    CHECK(state.view().intent==maximum&&state.eligible());
}
void late_reply_and_normalization()
{
    begin(__func__);const auto requested=intent();GroupLinkState state(id(1));CHECK(state.link(requested)==GroupLinkStatus::ready);const auto capture=state.view();
    rejectedAck(state,id(2),capture.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    rejectedAck(state,GroupUuid{},capture.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    rejectedAck(state,id(1),0,requested,requested,GroupLinkStatus::staleReply);rejectedAck(state,id(1),capture.loadRevision+1,requested,requested,GroupLinkStatus::staleReply);
    auto normalized=requested;normalized.member=GroupUuid(juce::Uuid(id(99).getRawData()));CHECK(normalized.member==id(99));
    CHECK(state.acknowledge(id(1),capture.loadRevision,requested,normalized)==GroupLinkStatus::ready);const auto admitted=state.view();
    CHECK(admitted.runtime==capture.runtime&&admitted.loadRevision==capture.loadRevision&&admitted.intent==normalized&&admitted.phase==GroupLinkPhase::admitted);CHECK(state.eligible());
    auto exceptMember=normalized;exceptMember.member=requested.member;CHECK(exceptMember==requested);
    rejectedAck(state,id(1),capture.loadRevision,requested,normalized,GroupLinkStatus::staleReply);
    CHECK(state.acknowledge(id(1),admitted.loadRevision,normalized,normalized)==GroupLinkStatus::ready);CHECK(state.view()==admitted);
    for(int field=0;field<10;++field){const auto bad=invalid(normalized,field);rejectedAck(state,id(1),admitted.loadRevision,normalized,bad,GroupLinkStatus::invalidIntent);
        rejectedAck(state,id(1),admitted.loadRevision,bad,normalized,GroupLinkStatus::invalidIntent);}
    for(int field=0;field<6;++field){auto changed=normalized;switch(field){case 0:changed.group=id(90);break;case 1:changed.name=text("Other");break;case 2:changed.role=text("lead");break;
        case 3:changed.key=text("B");break;case 4:changed.scale=text("major");break;case 5:changed.bpm=121;break;}
        CHECK(validateGroupIntent(changed));rejectedAck(state,id(1),admitted.loadRevision,normalized,changed,GroupLinkStatus::invalidIntent);
        rejectedAck(state,id(1),admitted.loadRevision,changed,changed,GroupLinkStatus::staleReply);}
    auto absent=normalized;absent.bpm.reset();rejectedAck(state,id(1),admitted.loadRevision,normalized,absent,GroupLinkStatus::invalidIntent);
    state.disconnected();CHECK(state.view().phase==GroupLinkPhase::authorized&&state.view().loadRevision==admitted.loadRevision&&state.view().intent==normalized);CHECK(state.eligible());
    CHECK(state.acknowledge(id(1),admitted.loadRevision,normalized,normalized)==GroupLinkStatus::ready);CHECK(state.view()==admitted);
    auto changed=normalized;changed.role=text("pluck");CHECK(state.link(changed)==GroupLinkStatus::ready);const auto settings=state.view();
    CHECK(settings.loadRevision==admitted.loadRevision+1&&settings.phase==GroupLinkPhase::authorized&&settings.intent==changed);
    rejectedAck(state,id(1),admitted.loadRevision,normalized,normalized,GroupLinkStatus::staleReply);
    rejectedAck(state,id(1),settings.loadRevision,normalized,normalized,GroupLinkStatus::staleReply);
    CHECK(state.restore(requested)==GroupLinkStatus::ready);const auto loaded=state.view();CHECK(loaded.intent==requested&&loaded.phase==GroupLinkPhase::pending&&!state.eligible());
    rejectedAck(state,id(1),settings.loadRevision,changed,changed,GroupLinkStatus::staleReply);
    rejectedAck(state,id(1),loaded.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    CHECK(state.resume()==GroupLinkStatus::ready);const auto resumed=state.view();CHECK(resumed.loadRevision==loaded.loadRevision+1&&state.eligible());
    rejectedAck(state,id(1),loaded.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    state.leave();CHECK(!state.view().intent&&!state.eligible());rejectedAck(state,id(1),resumed.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    CHECK(state.link(requested)==GroupLinkStatus::ready);rejectedAck(state,id(1),resumed.loadRevision,requested,requested,GroupLinkStatus::staleReply);
    GroupLinkState other(id(2));CHECK(other.restore(requested)==GroupLinkStatus::ready);CHECK(other.resume()==GroupLinkStatus::ready);
    rejectedAck(other,id(1),other.view().loadRevision,requested,requested,GroupLinkStatus::staleReply);CHECK(other.view().intent==requested&&other.eligible());
}
void reconnect_and_exhaustion()
{
    begin(__func__);const auto saved=intent();const auto maximum=std::numeric_limits<std::uint64_t>::max();
    GroupLinkState state(id(1));CHECK(state.restore(saved)==GroupLinkStatus::ready);state.disconnected();CHECK(!state.eligible()&&state.view().phase==GroupLinkPhase::pending);
    CHECK(state.resume()==GroupLinkStatus::ready);const auto owner=state.view();CHECK(state.acknowledge(id(1),owner.loadRevision,saved,saved)==GroupLinkStatus::ready);
    state.disconnected();CHECK(state.view()==owner&&state.eligible());CHECK(state.acknowledge(id(1),owner.loadRevision,saved,saved)==GroupLinkStatus::ready);
    detail::GroupLinkStateTestAccess::setLoadRevision(state,maximum-1);CHECK(state.restore(saved)==GroupLinkStatus::ready);const auto last=state.view();
    CHECK(last.loadRevision==maximum&&!last.exhausted&&last.intent==saved&&last.phase==GroupLinkPhase::pending&&!state.eligible());
    CHECK(state.resume()==GroupLinkStatus::revisionExhausted);const auto exhausted=state.view();CHECK(exhausted.loadRevision==maximum&&exhausted.exhausted&&exhausted.intent==saved&&exhausted.phase==GroupLinkPhase::pending);
    auto imported=saved;imported.role=text("restored");CHECK(state.restore(imported)==GroupLinkStatus::revisionExhausted);CHECK(state.view().intent==imported&&state.view().loadRevision==maximum&&!state.eligible());
    CHECK(state.link(saved)==GroupLinkStatus::revisionExhausted);CHECK(state.view().intent==imported);
    for(int field=0;field<10;++field){const auto before=state.view();const auto bad=invalid(saved,field);CHECK(state.restore(bad)==GroupLinkStatus::invalidIntent);CHECK(state.view()==before);
        CHECK(state.link(bad)==GroupLinkStatus::invalidIntent);CHECK(state.view()==before);rejectedAck(state,id(1),maximum,imported,bad,GroupLinkStatus::invalidIntent);}
    rejectedAck(state,id(1),maximum,imported,imported,GroupLinkStatus::staleReply);
    state.leave();CHECK(state.view().exhausted&&state.view().loadRevision==maximum&&!state.view().intent&&state.view().phase==GroupLinkPhase::unlinked);
    CHECK(state.resume()==GroupLinkStatus::notLinked);CHECK(state.link(saved)==GroupLinkStatus::revisionExhausted);CHECK(!state.view().intent&&!state.eligible());
    CHECK(state.restore(std::nullopt)==GroupLinkStatus::revisionExhausted);CHECK(!state.view().intent&&state.view().phase==GroupLinkPhase::unlinked);state.disconnected();CHECK(!state.eligible());
    GroupLinkState eligible(id(2));CHECK(eligible.link(saved)==GroupLinkStatus::ready);detail::GroupLinkStateTestAccess::setLoadRevision(eligible,maximum);
    const auto saturated=eligible.view();CHECK(saturated.loadRevision==maximum&&!saturated.exhausted&&eligible.eligible());
    CHECK(eligible.link(saved)==GroupLinkStatus::ready);CHECK(eligible.resume()==GroupLinkStatus::ready);CHECK(eligible.view()==saturated);
    CHECK(eligible.acknowledge(id(2),maximum,saved,saved)==GroupLinkStatus::ready);const auto admitted=eligible.view();CHECK(admitted.phase==GroupLinkPhase::admitted&&eligible.eligible());
    const auto bad=invalid(saved,0);CHECK(eligible.restore(bad)==GroupLinkStatus::invalidIntent);CHECK(eligible.view()==admitted);
    CHECK(eligible.link(imported)==GroupLinkStatus::revisionExhausted);CHECK(eligible.view().intent==saved&&eligible.view().exhausted&&eligible.view().phase==GroupLinkPhase::pending&&!eligible.eligible());
    rejectedAck(eligible,id(2),maximum,saved,saved,GroupLinkStatus::staleReply);
    GroupLinkState restoreAtMax(id(3));CHECK(restoreAtMax.link(saved)==GroupLinkStatus::ready);detail::GroupLinkStateTestAccess::setLoadRevision(restoreAtMax,maximum);
    CHECK(restoreAtMax.restore(imported)==GroupLinkStatus::revisionExhausted);CHECK(restoreAtMax.view().intent==imported&&restoreAtMax.view().exhausted&&!restoreAtMax.eligible());
    GroupLinkState leaveAtMax(id(4));CHECK(leaveAtMax.link(saved)==GroupLinkStatus::ready);detail::GroupLinkStateTestAccess::setLoadRevision(leaveAtMax,maximum);
    leaveAtMax.leave();CHECK(leaveAtMax.view().exhausted&&!leaveAtMax.view().intent&&leaveAtMax.view().loadRevision==maximum&&!leaveAtMax.eligible());
    GroupLinkState nullRuntime(GroupUuid{});const auto nullView=nullRuntime.view();CHECK(nullView.runtime.isNull()&&nullView.exhausted&&!nullRuntime.eligible());
    CHECK(nullRuntime.restore(bad)==GroupLinkStatus::invalidIntent);CHECK(nullRuntime.view()==nullView);CHECK(nullRuntime.link(bad)==GroupLinkStatus::invalidIntent);CHECK(nullRuntime.view()==nullView);
    CHECK(nullRuntime.resume()==GroupLinkStatus::notLinked);CHECK(nullRuntime.restore(saved)==GroupLinkStatus::revisionExhausted);const auto nullPending=nullRuntime.view();
    CHECK(nullPending.intent==saved&&nullPending.phase==GroupLinkPhase::pending&&nullPending.exhausted&&nullPending.loadRevision==1&&!nullRuntime.eligible());
    CHECK(nullRuntime.resume()==GroupLinkStatus::revisionExhausted);CHECK(nullRuntime.link(imported)==GroupLinkStatus::revisionExhausted);CHECK(nullRuntime.view()==nullPending);
    rejectedAck(nullRuntime,GroupUuid{},1,saved,saved,GroupLinkStatus::staleReply);nullRuntime.leave();CHECK(!nullRuntime.view().intent&&nullRuntime.view().exhausted&&!nullRuntime.eligible());
}
}
int main(int argc,char** argv)
{
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);ULARGE_INTEGER time{};time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
    explicit_permission_and_load();late_reply_and_normalization();reconnect_and_exhaustion();CHECK(sections==3);
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
}
