#include "FrozenRackExamples.h"
#include "CompanionWire.h"
#include "AiCatalog.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "UserReach.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
using namespace airack;
using namespace airack::ai;
using namespace airack::companion;
int checks=0,failures=0;
void check(bool good,const char* text,int line){++checks;if(!good){++failures;if(failures<70)std::cerr<<"FAIL "<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
std::optional<RackDesign> spaceFactory(){for(const auto& d:airack::test::frozenFactoryRackDesigns())if(d.name=="Lumen Field")return d;CHECK(false);return {};}
bool corruptKey(juce::MemoryBlock& wire,const char* key){auto* first=static_cast<char*>(wire.getData());const auto length=std::strlen(key);auto* found=std::search(first,first+wire.getSize(),key,key+length);CHECK(found!=first+wire.getSize());if(found==first+wire.getSize())return false;found[length-1]='x';return true;}
void wireFocusAndCatalog(){
    std::cout<<"SECTION actual_companion_wire_space_focus_and_catalog97_roundtrips\n";
    CHECK(wireVersion==9&&wireHeaderBytes==28&&maximumRackReconciliationBytes==16384);
    const auto chosen=spaceFactory();if(!chosen)return;const auto reach=unitReach(NativeModule(18),0);CHECK(reach.size()==97);if(reach.size()!=97)return;
    GenerationRequest request;request.tag={juce::Uuid(),47,3,2};request.model="fixture-model";request.instruction="Shape this space";
    RackOperation place;place.design=*chosen;place.newInstance=juce::Uuid();place.destination={RackRole::fx,masterChain,0};place.allocation={{0,0}};
    CHECK(parameterCount==4519&&spaceReverbParameterStart==2735);
    for(std::size_t i=2063;i<2215;++i){request.source.sound.values[i]=parameterRegistry()[i].kind==ParameterKind::continuous?.375f:1.f;request.source.sound.controlLocks[i]=i%2==0;}
    RackDifference difference;SoundDocument source;CHECK(makeRackCandidate(request.source.sound,place,SoundEditOrigin::manual,source,difference)==RackCandidateStatus::ready);request.source.sound=source;
    // Fresh placement resets all numeric families in slot1, preserving other slots and every lock.
    const auto defaults=defaultParameterValues();
    for(std::size_t i=2063;i<2215;++i){CHECK(source.values[i]==(parameterRegistry()[i].id.startsWith("effect.1.")?defaults[i]:parameterRegistry()[i].kind==ParameterKind::continuous?.375f:1.f));CHECK(source.controlLocks[i]==(i%2==0));}
    RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(std::vector<RackDesign>{*chosen},{},{},snapshot));if(snapshot.entries.size()!=1){CHECK(false);return;}
    RackCatalogPage page;CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete&&page.entries.size()==1);
    auto authority=std::make_shared<CatalogAuthority>();authority->root=request.tag;authority->snapshot=snapshot.id;authority->descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
    CHECK(ai::detail::catalogAuthorityValid(*authority));CatalogRequestContext context;context.root=request.tag;context.authority=authority;
    context.baseIntent.racks={{RackEditKind::moveEffect,0,masterChain,0,EffectKind(16),false}};request.catalog=context;
    const auto prefix=juce::String("effect.1.");juce::MemoryBlock wire;
    for(const auto index:reach){const auto& field=parameterRegistry()[index];FocusedControl control=chosen->panel.controls.front();control.key=field.id.substring(prefix.length());
        control.label="Wire focus";control.minimum=field.minimum;control.maximum=field.maximum;control.response=FocusResponse::linear;
        control.widget=field.kind==ParameterKind::continuous?FocusWidget::rotary:field.kind==ParameterKind::toggle?FocusWidget::toggle:FocusWidget::choice;
        request.catalog->baseIntent.reconciliation.controls={{place.newInstance,0,control}};
        const bool encoded=encodeRequest(request,wire);CHECK(encoded);if(!encoded)continue;
        GenerationRequest decoded;CHECK(decodeRequest(bytes(wire),decoded));CHECK(decoded.tag==request.tag&&decoded.source==request.source);
        CHECK(decoded.catalog&&decoded.catalog->baseIntent.reconciliation.controls.size()==1);if(!decoded.catalog||decoded.catalog->baseIntent.reconciliation.controls.size()!=1)continue;
        const auto& roundtrip=decoded.catalog->baseIntent.reconciliation.controls.front();CHECK(roundtrip.instance==place.newInstance&&roundtrip.control==0&&roundtrip.replacement==control);
        CHECK(decoded.catalog&&decoded.catalog->authority&&*decoded.catalog->authority==*authority);juce::MemoryBlock again;CHECK(encodeRequest(decoded,again));CHECK(again==wire);
        CHECK(decoded.catalog&&decoded.catalog->baseIntent.racks.size()==1);if(decoded.catalog&&decoded.catalog->baseIntent.racks.size()==1)CHECK(decoded.catalog->baseIntent.racks.front().effect==EffectKind(16));
    }
    auto& focus=request.catalog->baseIntent.reconciliation.controls.front().replacement;focus=chosen->panel.controls.front();
    CHECK(encodeRequest(request,wire));auto corrupt=wire;if(corruptKey(corrupt,"space_reverb.size")){
        auto output=request;const auto before=wire;CHECK(!decodeRequest(bytes(corrupt),output));juce::MemoryBlock after;CHECK(encodeRequest(output,after));CHECK(after==before);}
    const auto rejectRequest=[&](const GenerationRequest& invalid){juce::MemoryBlock out("keep",4);CHECK(!encodeRequest(invalid,out));CHECK(out==juce::MemoryBlock("keep",4));};
    auto invalid=request;invalid.catalog->baseIntent.reconciliation.controls.front().replacement->key="space_reverb.unimplemented";rejectRequest(invalid);
    invalid=request;invalid.catalog->baseIntent.reconciliation.controls.front().replacement->maximum=std::numeric_limits<float>::quiet_NaN();rejectRequest(invalid);
    invalid=request;invalid.catalog->baseIntent.reconciliation.controls.front().replacement->key="effect.1.space_reverb.size";rejectRequest(invalid);
    invalid=request;invalid.catalog->baseIntent.racks.front().effect=EffectKind(19);rejectRequest(invalid);

    const auto& entry=snapshot.entries.front();GenerationResult result;result.tag=request.tag;result.status=GenerationStatus::catalogSelectionRequired;result.inferenceCalls=1;
    result.providerStatus=rack::oauth::Status::connected;result.httpStatus=200;result.proposalStatus=ProposalStatus::accepted;result.catalog=CatalogResultBinding{request.tag,snapshot.id};
    result.edits.catalogSelections={{7,chosen->id,chosen->revision,entry.contentSha256,{RackRole::fx,masterChain,1}}};
    for(const auto index:reach)result.edits.catalogParameters.push_back({7,0,parameterRegistry()[index].id.substring(prefix.length()),source.values[index]});
    CHECK(result.edits.catalogParameters.size()==97);const bool encoded=encodeResult(request,result,wire);CHECK(encoded);if(encoded){
        GenerationResult decoded;CHECK(decodeResult(bytes(wire),request,decoded));CHECK(decoded.tag==result.tag&&decoded.catalog==result.catalog&&decoded.edits.catalogSelections==result.edits.catalogSelections&&decoded.edits.catalogParameters==result.edits.catalogParameters);
        auto bad=wire;if(corruptKey(bad,"space_reverb.post.6.slope")){auto output=result;juce::MemoryBlock before,after;CHECK(encodeResult(request,output,before));CHECK(!decodeResult(bytes(bad),request,output));CHECK(encodeResult(request,output,after));CHECK(after==before);}}
    const auto rejectResult=[&](const GenerationResult& invalidResult){juce::MemoryBlock out("keep",4);CHECK(!encodeResult(request,invalidResult,out));CHECK(out==juce::MemoryBlock("keep",4));};
    auto bad=result;bad.edits.catalogParameters.back().key="space_reverb.unimplemented";rejectResult(bad);
    bad=result;bad.edits.catalogParameters.push_back(bad.edits.catalogParameters.front());rejectResult(bad);
    bad=result;bad.edits.catalogParameters.back().unit=1;rejectResult(bad);
    bad=result;bad.edits.catalogParameters.back().normalized=std::numeric_limits<float>::quiet_NaN();rejectResult(bad);
}
}
int main(){wireFocusAndCatalog();std::cout<<"SPACE_WIRE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
