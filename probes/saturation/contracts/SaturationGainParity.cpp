#include "SaturationGain.h"
#include "SaturationGainRuntime.h"
#include "GainParitySource.h"
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <bit>
#include <algorithm>
#include <charconv>
#include <cfenv>
#include <xmmintrin.h>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <Windows.h>
#include <malloc.h>
#endif

namespace allocationWitness {
thread_local bool enabled=false;
thread_local std::size_t count=0;
void allocated() noexcept { if(enabled)++count; }
}
void* operator new(std::size_t size) { if(auto* p=std::malloc(size?size:1)){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
#if defined(_WIN32)
void* operator new(std::size_t size,std::align_val_t a) { if(auto* p=_aligned_malloc(size?size:1,std::size_t(a))){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size,std::align_val_t a) { return ::operator new(size,a); }
void operator delete(void* p,std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p,std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { _aligned_free(p); }
#endif

// Distinct named call sites keep object relocations and image disassembly reviewable.
__declspec(noinline) double originalPower10(double exponent) noexcept { return std::pow(10.,exponent); }
__declspec(noinline) double canonicalGainCall(double exponent) noexcept { return airack::gain_math::canonicalPower10(exponent); }
__declspec(noinline) double unrelatedSine(double x) noexcept { return std::sin(x); }

namespace {
namespace fs=std::filesystem;
using juce::var;
void require(bool ok,const std::string& why) { if(!ok)throw std::runtime_error(why); }
std::string read(const fs::path& path) {
    require(fs::is_regular_file(path),"missing file: "+path.string());
    std::ifstream in(path,std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)),{});
    require(!in.bad()&&raw.size()==fs::file_size(path),"incomplete read: "+path.string());return raw;
}
std::string digest(const std::string& bytes) { return juce::SHA256(bytes.data(),bytes.size()).toHexString().toStdString(); }
var object() { return var(new juce::DynamicObject); }
void set(var& obj,const char* key,const var& value) { obj.getDynamicObject()->setProperty(key,value); }
void set(var& obj,const char* key,const std::string& value) { set(obj,key,juce::var(juce::String(value))); }
void set(var& obj,const char* key,const char* value) { set(obj,key,juce::var(value)); }
var identity(const std::string& raw) { auto o=object();set(o,"bytes",juce::int64(raw.size()));set(o,"sha256",digest(raw));return o; }
std::string text(const var& value) { return value.toString().toStdString(); }
void write(const fs::path& path,const std::string& raw) {
    fs::create_directories(path.parent_path());require(!fs::exists(path),"fresh file required");
    std::ofstream out(path,std::ios::binary);out.write(raw.data(),std::streamsize(raw.size()));out.close();require(bool(out),"write failure");
}
std::string hex(double value) { std::ostringstream o;o<<std::hex<<std::setfill('0')<<std::setw(16)<<std::bit_cast<std::uint64_t>(value);return o.str(); }
double decode(const std::string& value) {
    std::uint64_t bits=0;const auto r=std::from_chars(value.data(),value.data()+value.size(),bits,16);
    require(value.size()==16&&r.ec==std::errc{}&&r.ptr==value.data()+value.size(),"invalid db word");
    const double x=std::bit_cast<double>(bits);require(std::isfinite(x)&&hex(x)==value,"nonfinite/noncanonical db");return x;
}
bool within(const fs::path& child,const fs::path& parent) {
    auto c=child.begin();
    for(auto p=parent.begin();p!=parent.end();++p,++c) {
        if(c==child.end())return false;
        const auto a=c->native(),b=p->native();
        if(CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)!=CSTR_EQUAL)return false;
    }
    return true;
}
void validateOutput(const fs::path& output,const fs::path& root) {
    require(output.is_absolute()&&!fs::exists(output),"absolute/fresh output required");
    for(auto p=output;!p.empty();p=p.parent_path()) {
#if defined(_WIN32)
        const auto attr=GetFileAttributesW(p.c_str());require(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_REPARSE_POINT),"redirected output path");
#else
        require(!fs::is_symlink(fs::symlink_status(p)),"redirected output path");
#endif
        if(p==p.parent_path())break;
    }
    const auto realOutput=fs::weakly_canonical(output);
    const auto realRoot=fs::weakly_canonical(root),realBuild=fs::weakly_canonical(root/"build");
    require(!within(realOutput,realRoot)||(within(realOutput,realBuild)&&!within(realBuild,realOutput)),"output source boundary");
}
var fixtureInformation(const fs::path& path,bool reparse) {
    const auto handle=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|(reparse?FILE_FLAG_OPEN_REPARSE_POINT:0),nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"boundary fixture handle failed");
    BY_HANDLE_FILE_INFORMATION info{};const auto ok=GetFileInformationByHandle(handle,&info);CloseHandle(handle);
    require(ok!=0,"boundary fixture information failed");auto out=object();
    for(const auto& [name,value]:std::initializer_list<std::pair<const char*,DWORD>>{
        {"attributes",info.dwFileAttributes},{"creation_low",info.ftCreationTime.dwLowDateTime},
        {"creation_high",info.ftCreationTime.dwHighDateTime},{"write_low",info.ftLastWriteTime.dwLowDateTime},
        {"write_high",info.ftLastWriteTime.dwHighDateTime},{"volume",info.dwVolumeSerialNumber},
        {"size_high",info.nFileSizeHigh},{"size_low",info.nFileSizeLow},{"links",info.nNumberOfLinks},
        {"index_high",info.nFileIndexHigh},{"index_low",info.nFileIndexLow}})
        set(out,name,juce::int64(value));
    return out;
}
var checkFixture(const var& expected) {
    const fs::path link=text(expected["link"]),target=text(expected["target"]);
    const auto linkAttr=GetFileAttributesW(link.c_str()),targetAttr=GetFileAttributesW(target.c_str());
    require(linkAttr!=INVALID_FILE_ATTRIBUTES&&(linkAttr&FILE_ATTRIBUTE_REPARSE_POINT)&&
        targetAttr!=INVALID_FILE_ATTRIBUTES&&!(targetAttr&FILE_ATTRIBUTE_REPARSE_POINT)&&
        fs::equivalent(link,target)&&fs::is_empty(target),"boundary fixture target/type/emptiness changed");
    auto actual=object();
    for(const auto* name:{"link","target","external_output"})set(actual,name,expected[name]);
    for(const auto* name:{"link_information","target_information"}) {
        auto info=fixtureInformation(name[0]=='l'?link:target,name[0]=='l');
        const auto saved=expected[name].getDynamicObject();require(saved!=nullptr,"boundary fixture metadata missing");
        require(saved->getProperties().size()==info.getDynamicObject()->getProperties().size(),"boundary fixture metadata extent");
        for(const auto& item:info.getDynamicObject()->getProperties())
            require(text(saved->getProperty(item.name))==text(item.value),"boundary fixture metadata changed");
        set(actual,name,info);
    }
    require(expected["target_members"].isArray()&&expected["target_members"].size()==0,"boundary target inventory changed");
    set(actual,"target_members",var(juce::Array<var>{}));return actual;
}
var checkOutputBoundary(const fs::path& root,const var& fixture) {
    auto proof=object();set(proof,"fixture_before",checkFixture(fixture));
    auto upper=root.native();std::transform(upper.begin(),upper.end(),upper.begin(),[](wchar_t c){return wchar_t(std::towupper(c));});
    struct Case { const char* name;fs::path path;bool accepted; };
    const Case cases[]={
        {"source",root/"tools/gain-boundary-must-not-exist",false},{"source-root",root,false},
        {"parent-traversal",root/"build/../docs/gain-boundary-must-not-exist",false},
        {"case-source",fs::path(upper)/"docs/gain-boundary-must-not-exist",false},
        {"redirected",fs::path(text(fixture["link"]))/"nested/gain-boundary-must-not-exist",false},
        {"redirected-parent",fs::path(text(fixture["link"]))/"../gain-boundary-must-not-exist",false},
        {"build-root",root/"build",false},{"build-child",root/"build/gain-boundary-must-not-exist",true},
        {"external-child",fs::path(text(fixture["external_output"])),true}};
    juce::Array<var> results;
    for(const auto& c:cases) {
        bool accepted=false;
        try { validateOutput(c.path,root);accepted=true; } catch(const std::runtime_error&) {}
        require(accepted==c.accepted,std::string("native output-boundary case failed: ")+c.name);
        auto result=object();set(result,"name",c.name);set(result,"path",c.path.generic_string());
        set(result,"accepted",accepted);results.add(result);
    }
    set(proof,"cases",var(results));return proof;
}
}

int main(int argc,char** argv) try {
    require(argc==5&&std::string(argv[1])=="--inputs"&&std::string(argv[3])=="--output","--inputs ABSOLUTE_FILE --output FRESH_DIRECTORY required");
    const fs::path inputs=argv[2],output=argv[4];require(inputs.is_absolute(),"absolute input required");
    const auto sourceRaw=read(GAIN_SOURCE_FILE);require(digest(sourceRaw)==GAIN_SOURCE_SHA,"compiled source descriptor changed");
    const auto descriptor=juce::JSON::parse(sourceRaw);require(descriptor.isObject(),"invalid source descriptor");
    const fs::path root=text(descriptor["root"]);
    validateOutput(output,root);
    auto boundaryProof=checkOutputBoundary(root,descriptor["boundary_fixture"]);
    std::vector<std::pair<std::string,std::string>> saved;
    const auto files=descriptor["source"]["files"].getDynamicObject();require(files!=nullptr,"source inventory missing");
    for(const auto& entry:files->getProperties()) {
        const auto name=entry.name.toString().toStdString(),raw=read(root/name);
        require(digest(raw)==text(entry.value["sha256"])&&raw.size()==std::size_t(juce::int64(entry.value["bytes"])),"source changed: "+name);
        saved.emplace_back("source/"+name,raw);
    }
    const auto inputRaw=read(inputs);require(digest(inputRaw)==text(descriptor["inputs"]["sha256"]),"input matrix identity mismatch");
    const auto image=read(fs::absolute(argv[0]));
    const auto obj=read(text(descriptor["object_path"])),archive=read(text(descriptor["archive_path"]));
    require(digest(obj)==text(descriptor["object"]["sha256"])&&digest(archive)==text(descriptor["archive"]["sha256"]),"generated import identity changed");
    const int roundingBefore=std::fegetround();const unsigned mxcsrBefore=_mm_getcsr();
    HMODULE provider=nullptr;
    const auto importedAddress=reinterpret_cast<LPCWSTR>(&airack_saturation_release_pow);
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              importedAddress,&provider)!=0,"import provider address lookup failed");
    wchar_t providerName[32768]{};
    const auto providerLength=GetModuleFileNameW(provider,providerName,32768);
    require(providerLength>0&&providerLength<32768,"import provider path failed");
    const fs::path providerPath(providerName);
    const auto providerRaw=read(providerPath);
    require(fs::weakly_canonical(providerPath)!=fs::weakly_canonical(fs::absolute(argv[0])),"import address is executable thunk");
    auto providerProof=identity(providerRaw);
    set(providerProof,"path",providerPath.string());
    set(providerProof,"function_address",juce::String::toHexString(juce::int64(reinterpret_cast<std::uintptr_t>(importedAddress))));
    set(providerProof,"module_base",juce::String::toHexString(juce::int64(reinterpret_cast<std::uintptr_t>(provider))));
    // No canonical invocation precedes this new thread's first call.
    std::size_t allocations=~std::size_t{},sanity=0;double first=0;
    std::thread fresh([&] {
        allocationWitness::enabled=true;
        void*(*volatile allocate)(std::size_t)=::operator new;
        void* p=allocate(1);::operator delete(p);sanity=allocationWitness::count;
        allocationWitness::count=0;
        volatile double db=std::bit_cast<double>(0x403424d560000000ull);
        first=canonicalGainCall(db/20);allocations=allocationWitness::count;
        allocationWitness::enabled=false;
    });fresh.join();
    require(sanity==1&&allocations==0&&hex(first)=="40245584c36bbe72","fresh-thread call/allocation witness failed");
    std::istringstream in(inputRaw);std::string line;std::getline(in,line);require(line=="id,db","input header");
    std::ostringstream rows;rows<<"id,db,exponent,original,canonical\n";
    std::size_t count=0;
    while(std::getline(in,line)) {
        const auto comma=line.find(',');require(comma!=std::string::npos&&line.substr(0,comma)==std::to_string(count),"input row order");
        const auto dbWord=line.substr(comma+1);const double db=decode(dbWord),exponent=db/20;
        const double original=originalPower10(exponent),canonical=canonicalGainCall(exponent);
        require(std::isfinite(exponent)&&std::isfinite(original)&&std::isfinite(canonical),"nonfinite matrix result");
        rows<<count<<','<<dbWord<<','<<hex(exponent)<<','<<hex(original)<<','<<hex(canonical)<<'\n';++count;
    }
    require(count==72752&&inputRaw.back()=='\n',"matrix extent/truncation");
    const airack::gain_math::GainCurve liveOriginal;
    std::ostringstream centers;centers<<"id,db,original\n";
    for(std::size_t i=0;i<liveOriginal.cells;++i)
        centers<<i<<','<<hex(liveOriginal.centerDb(int(i)))<<','<<hex(liveOriginal.centers[i])<<'\n';
    volatile double sineInput=.125;const double sine=unrelatedSine(sineInput);require(std::isfinite(sine),"unrelated sine failed");
    saved.emplace_back("provider.dll",providerRaw);
    saved.emplace_back("inputs.csv",inputRaw);saved.emplace_back("image.exe",image);
    saved.emplace_back("private.obj",obj);saved.emplace_back("private.lib",archive);
    saved.emplace_back("rows.csv",rows.str());saved.emplace_back("centers.csv",centers.str());
    for(const auto& entry:files->getProperties())
        require(digest(read(root/entry.name.toString().toStdString()))==text(entry.value["sha256"]),"source changed during capture");
    require(read(inputs)==inputRaw&&read(GAIN_SOURCE_FILE)==sourceRaw&&read(fs::absolute(argv[0]))==image&&read(text(descriptor["object_path"]))==obj&&read(text(descriptor["archive_path"]))==archive&&read(providerPath)==providerRaw,"producer changed during capture");
    const int roundingAfter=std::fegetround();const unsigned mxcsrAfter=_mm_getcsr();
    require(roundingBefore==FE_TONEAREST&&roundingAfter==FE_TONEAREST&&(mxcsrBefore&~0x3fu)==0x1f80u&&(mxcsrAfter&~0x3fu)==0x1f80u,"floating-point environment changed");
    set(boundaryProof,"fixture_after",checkFixture(descriptor["boundary_fixture"]));
    fs::create_directories(output);write(output/"source.json",sourceRaw);
    auto receipt=object(),bound=object();
    for(const auto& [name,raw]:saved) { write(output/name,raw);bound.getDynamicObject()->setProperty(juce::Identifier(juce::String(name)),identity(raw)); }
#if defined(_DEBUG)
    set(receipt,"profile","Debug");
#else
    set(receipt,"profile","Release");
#endif
    set(receipt,"source",descriptor["source"]);set(receipt,"source_sha256",GAIN_SOURCE_SHA);
    set(receipt,"import_sha256",descriptor["import_sha256"]);set(receipt,"files",bound);
    set(receipt,"provider",providerProof);
    set(receipt,"output_boundary",boundaryProof);
    set(receipt,"fegetround_before",roundingBefore);set(receipt,"fegetround_after",roundingAfter);
    set(receipt,"mxcsr_before",juce::int64(mxcsrBefore));set(receipt,"mxcsr_after",juce::int64(mxcsrAfter));
    set(receipt,"fresh_thread_cpp_allocations",juce::int64(allocations));
    set(receipt,"fresh_thread_instrumentation_sanity",juce::int64(sanity));
    set(receipt,"fresh_thread_first_result",hex(first));set(receipt,"unrelated_sine",hex(sine));
    write(output/"receipt.json",juce::JSON::toString(receipt).toStdString()+"\n");
    std::cout<<"rows="<<count<<" centers=768 fresh_thread_cpp_allocations="<<allocations<<'\n';return 0;
} catch(const std::exception& e) { std::cerr<<"gain parity refusal: "<<e.what()<<'\n';return 1; }
