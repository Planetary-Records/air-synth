#include "ProcessHighQos.h"
#include <iostream>
#include <string_view>
#include <type_traits>
namespace {
using namespace airack::space::probe;
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"POWER_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
struct Fake {
    ProcessPowerState value{1,0,0};
    unsigned gets=0,sets=0,failGet=0,failSet=0,lieGet=0;
    bool mutateOnFailure=false;
    ProcessPowerApi api() noexcept {return {this,get,set};}
    static bool get(void* p,ProcessPowerState& value,std::uint32_t& error) noexcept {
        auto& f=*static_cast<Fake*>(p);++f.gets;
        if(f.gets==f.failGet){error=701;return false;}
        value=f.value;if(f.gets==f.lieGet)value.state^=1;return true;
    }
    static bool set(void* p,const ProcessPowerState& value,std::uint32_t& error) noexcept {
        auto& f=*static_cast<Fake*>(p);++f.sets;
        if(f.sets==f.failSet){if(f.mutateOnFailure)f.value=value;error=702;return false;}
        f.value=value;return true;
    }
};
void success(){
    for(auto original:{ProcessPowerState{1,0,0},ProcessPowerState{1,4,4},ProcessPowerState{1,5,5},ProcessPowerState{1,1,0},ProcessPowerState{1,8,8},ProcessPowerState{1,13,13}}){
        Fake f;f.value=original;ProcessHighQos q;
        CHECK(!q.attempted()&&!q.opened()&&!q.restored()&&!q.qualified());
        CHECK(q.open(f.api()));CHECK(q.attempted()&&q.opened()&&!q.restored());
        const ProcessPowerState desired{original.version,original.control|5U,original.state&~5U};
        CHECK(q.before()==original);CHECK(q.during()==desired);CHECK(f.value==desired);
        CHECK(f.gets==2&&f.sets==1);CHECK(q.setupError()==0&&q.cleanupError()==0);
        CHECK(q.close());CHECK(q.after()==original&&f.value==original);CHECK(q.restored()&&q.qualified());
        CHECK(f.gets==3&&f.sets==2);CHECK(q.close());CHECK(f.gets==3&&f.sets==2);
    }
}
void setupFailures(){
    {Fake f;ProcessHighQos q;CHECK(q.close());CHECK(!q.open({}));CHECK(q.setupError()!=0);CHECK(!q.attempted());CHECK(q.close());CHECK(f.gets==0&&f.sets==0);}
    {Fake f;f.failGet=1;ProcessHighQos q;CHECK(!q.open(f.api()));CHECK(q.setupError()==701);CHECK(!q.attempted());CHECK(f.sets==0);CHECK(q.close());CHECK(!q.qualified());}
    for(bool mutate:{false,true}){Fake f;f.failSet=1;f.mutateOnFailure=mutate;const auto original=f.value;ProcessHighQos q;
        CHECK(!q.open(f.api()));CHECK(q.attempted()&&!q.opened());CHECK(q.setupError()==702);CHECK(f.sets==2&&f.value==original);CHECK(q.restored());CHECK(q.close());CHECK(!q.qualified());}
    for(bool lie:{false,true}){Fake f;if(lie)f.lieGet=2;else f.failGet=2;const auto original=f.value;ProcessHighQos q;
        CHECK(!q.open(f.api()));CHECK(q.setupError()!=0);CHECK(f.value==original&&f.sets==2);CHECK(q.restored());CHECK(!q.qualified());}
    {Fake f;f.value.version=2;ProcessHighQos q;CHECK(!q.open(f.api()));CHECK(q.setupError()!=0);CHECK(!q.attempted()&&f.sets==0);}
    {Fake f;ProcessHighQos q;CHECK(q.open(f.api()));CHECK(!q.open(f.api()));CHECK(q.setupError()!=0);CHECK(f.gets==2&&f.sets==1);CHECK(q.close());CHECK(!q.qualified());CHECK(!q.open(f.api()));CHECK(f.sets==2);}
}
void cleanupFailures(){
    for(unsigned mode=0;mode<3;++mode){Fake f;const auto original=f.value;ProcessHighQos q;CHECK(q.open(f.api()));
        if(mode==0)f.failSet=2;else if(mode==1)f.failGet=3;else f.lieGet=3;
        CHECK(!q.close());CHECK(q.cleanupError()!=0);CHECK(!q.restored());const auto savedError=q.cleanupError();
        CHECK(q.close());CHECK(q.restored());CHECK(f.value==original&&q.after()==original);CHECK(q.cleanupError()==savedError);CHECK(!q.qualified());
        const auto sets=f.sets,gets=f.gets;CHECK(q.close());CHECK(f.sets==sets&&f.gets==gets);
    }
    {Fake f;const auto original=f.value;{ProcessHighQos q;CHECK(q.open(f.api()));}CHECK(f.value==original&&f.sets==2&&f.gets==3);}
    {Fake f;f.failGet=2;f.failSet=2;const auto original=f.value;{ProcessHighQos q;CHECK(!q.open(f.api()));CHECK(q.setupError()==701&&q.cleanupError()==702);CHECK(!q.restored());}CHECK(f.value==original&&f.sets==3);}
}
int live(){ProcessHighQos q;const bool opened=q.open(nativeProcessPowerApi());const bool one=q.close(),two=q.close();
    const auto b=q.before(),d=q.during(),a=q.after();
    std::cout<<"PROCESS_HIGH_QOS_LIVE opened="<<opened<<" close1="<<one<<" close2="<<two<<" qualified="<<q.qualified()
      <<" before="<<b.version<<','<<b.control<<','<<b.state<<" during="<<d.version<<','<<d.control<<','<<d.state
      <<" after="<<a.version<<','<<a.control<<','<<a.state<<" setup_error="<<q.setupError()<<" cleanup_error="<<q.cleanupError()<<'\n';
    return opened&&one&&two&&q.qualified()?0:1;}
}
int main(int argc,char** argv){static_assert(!std::is_copy_constructible_v<airack::space::probe::ProcessHighQos>);
    if(argc==2&&std::string_view(argv[1])=="--live")return live();if(argc!=1)return 2;
    success();setupFailures();cleanupFailures();std::cout<<"PROCESS_HIGH_QOS_TESTS checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
