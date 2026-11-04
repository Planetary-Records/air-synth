#include "ComfyHttpFixture.h"
#include "ComfyDrumPreparation.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <new>
#include <string_view>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <malloc.h>
#endif

namespace airack::companion::detail {
struct ComfyHttpTestAccess {
 static void control(ComfyHttpWindows& http,ComfyTestControl* control){http.setTestControl(control);}
 static std::unique_ptr<ComfyHttpWindows> fixture(ComfyLoopback kind,juce::File root,std::uint16_t port){
  return std::unique_ptr<ComfyHttpWindows>(new ComfyHttpWindows(kind,std::move(root),port));
 }
};
}
using namespace airack::drums;
using namespace airack::companion;
using namespace comfy_fixture;
namespace {
int checks=0,failures=0;
bool check(bool value,const char* text,int line){++checks;if(!value){++failures;std::cout<<"FAIL "<<line<<" "<<text<<std::endl;}return value;}
#define CHECK(x) check(bool(x),#x,__LINE__)
bool setupControls(){
 const auto before=failures;
 NativeComfyFixture positive("GET","/fixture","",response("{}"));
 std::string output;
 CHECK(positive.valid());CHECK(independentExchange(positive.port(),"GET /fixture HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",output));positive.join();
 CHECK(output=="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");CHECK(positive.captured().matched);
 NativeComfyFixture mismatch("GET","/expected","",response("{}"));output.clear();
 CHECK(mismatch.valid());CHECK(independentExchange(mismatch.port(),"GET /wrong HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",output));mismatch.join();CHECK(!mismatch.captured().matched&&mismatch.captured().target=="/wrong");
 std::cout<<"adapter_fixture_setup="<<(before==failures?"PASS":"FAIL")<<std::endl;return before==failures;
}
std::string makeFlac(const juce::File& file,double rate=44100,unsigned channels=2,int bits=16,int frames=442368){
 juce::FlacAudioFormat format;
 std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(new juce::FileOutputStream(file),rate,channels,bits,{},5));
 CHECK(writer!=nullptr);
 juce::AudioBuffer<float> audio(int(channels),frames);
 for(int c=0;c<int(channels);++c)for(int n=0;n<audio.getNumSamples();++n)audio.setSample(c,n,float(std::sin(double(n)*.02+c)*.25));
 if(writer)CHECK(writer->writeFromAudioSampleBuffer(audio,0,audio.getNumSamples()));writer.reset();
 juce::MemoryBlock bytes;CHECK(file.loadFileAsData(bytes));return {static_cast<const char*>(bytes.getData()),bytes.getSize()};
}
GeneratedOrigin origin(const ComfyRawFile& raw){
 GeneratedOrigin value;for(std::size_t n=0;n<32;++n)value.expectedFileId[n]=std::to_integer<std::uint8_t>(raw.sha256()[n]);
 value.promptId.fill(0x53);value.generator="Stable Audio 3 Small SFX";value.condition="stable-audio3-small-sfx-lcm8-flac-v1;execution=unverified";value.seed=17;return value;
}
void red(const juce::File& root){
 const auto encodedFile=root.getChildFile("fixture-source.flac");const auto body=makeFlac(encodedFile);
 std::atomic_bool cancel{false};PreparationError error{};
 std::unique_ptr<DrumSource> manual;
 CHECK(inspectDrumSource(encodedFile,{},cancel,manual,error));
 CHECK(manual&&manual->info().frames==442368&&manual->info().rate==44100&&manual->info().channels==2&&manual->info().bits==16&&manual->info().md5Verified);manual.reset();
 const auto sentinel=root.getChildFile("foreign-sentinel");CHECK(sentinel.replaceWithText("foreign"));
 const std::string target="/view?filename=seed-17_00001.flac&subfolder=airack%2F00112233-4455-6677-8899-aabbccddeeff%2F42%2F10213243-5465-7687-98a9-bacbdcedfe0f&type=output";
 NativeComfyFixture fixture("GET",target,"",response(body,"audio/flac"));CHECK(fixture.valid());
 const auto rawRoot=root.getChildFile("raw");CHECK(rawRoot.createDirectory().wasOk());
 auto http=airack::companion::detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4,rawRoot,fixture.port());
 airack::companion::detail::ComfyTestControl control;airack::companion::detail::ComfyHttpTestAccess::control(*http,&control);
 ComfyHttpRequest request;request.operation=ComfyOperation::download;request.owned={juce::Uuid("00112233445566778899aabbccddeeff"),juce::Uuid("102132435465768798a9bacbdcedfe0f"),42,17};request.filename="seed-17_00001.flac";request.subfolder="airack/00112233-4455-6677-8899-aabbccddeeff/42/10213243-5465-7687-98a9-bacbdcedfe0f";
 auto reply=http->perform(request,cancel,Clock::now()+std::chrono::seconds(5));fixture.join();CHECK(fixture.captured().matched&&fixture.requestCount()==1);CHECK(reply.status==ComfyHttpStatus::ok&&reply.raw&&reply.raw->size()==body.size());
 if(reply.raw){
  const auto expected=origin(*reply.raw);auto* identity=reply.raw.get();
  auto handle=CreateFileW(reply.raw->file().getFullPathName().toWideCharPointer(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(handle==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
  std::unique_ptr<DrumSource> source;
  auto mismatch=expected;mismatch.expectedFileId[0]^=1;
  CHECK(!inspectComfyDrumSource(reply.raw,mismatch,cancel,source,error)&&error==PreparationError::identityMismatch&&reply.raw.get()==identity&&!source);
  cancel=true;CHECK(!inspectComfyDrumSource(reply.raw,expected,cancel,source,error)&&error==PreparationError::cancelled&&reply.raw.get()==identity&&!source);cancel=false;
  control.fault=airack::companion::detail::ComfyTestFault::read;
  CHECK(!inspectComfyDrumSource(reply.raw,expected,cancel,source,error)&&error==PreparationError::ioError&&reply.raw.get()==identity&&!source);
  control.fault=airack::companion::detail::ComfyTestFault::none;
  const auto rawPath=reply.raw->file();
  auto writer=CreateFileW(rawPath.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);CHECK(writer==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
  CHECK(!DeleteFileW(rawPath.getFullPathName().toWideCharPointer()));CHECK(!MoveFileExW(rawPath.getFullPathName().toWideCharPointer(),root.getChildFile("renamed-raw").getFullPathName().toWideCharPointer(),0));CHECK(!MoveFileExW(sentinel.getFullPathName().toWideCharPointer(),rawPath.getFullPathName().toWideCharPointer(),MOVEFILE_REPLACE_EXISTING));CHECK(sentinel.loadFileAsString()=="foreign");
  const bool inspected=inspectComfyDrumSource(reply.raw,expected,cancel,source,error);
  CHECK(inspected);CHECK(!reply.raw&&source&&source->info().frames==442368);
  if(source){PreparedDrumSample prepared;CHECK(!prepareDrumSample(*source,0,442368,1,cancel,prepared,error));CHECK(error==PreparationError::invalidSelection&&!prepared.asset);CHECK(prepareDrumSample(*source,0,441000,.125f,cancel,prepared,error));CHECK(prepared.asset&&prepared.asset->frames()==480000&&prepared.source.end==441000&&prepared.source.sourceFrames==442368);}
  else CHECK(reply.raw.get()==identity);
  CHECK(!DeleteFileW(rawPath.getFullPathName().toWideCharPointer()));source.reset();CHECK(!rawPath.exists());CHECK(sentinel.loadFileAsString()=="foreign"&&sentinel.deleteFile());
 }
 reply.raw.reset();http.reset();CHECK(encodedFile.deleteFile());
}
class Bytes final:public GeneratedDrumBytes {
public:
 std::string data;bool fail=false;std::atomic_bool* cancelAfterRead=nullptr;
 explicit Bytes(std::string bytes):data(std::move(bytes)){}
 std::uint64_t size() const noexcept override{return data.size();}
 bool readAt(std::uint64_t offset,std::span<std::byte> out) const noexcept override{
  if(fail||offset>data.size()||out.size()>data.size()-offset)return false;
  std::memcpy(out.data(),data.data()+offset,out.size());if(cancelAfterRead)cancelAfterRead->store(true);return true;
 }
};
GeneratedOrigin byteOrigin(const std::string& bytes){GeneratedOrigin value;const auto sha=juce::SHA256(bytes.data(),bytes.size()).getRawData();std::copy(sha.begin(),sha.end(),value.expectedFileId.begin());value.promptId.fill(0x53);value.generator="Stable Audio 3 Small SFX";value.condition="stable-audio3-small-sfx-lcm8-flac-v1;execution=unverified";value.seed=17;return value;}
void generatedTransactions(const juce::File& root){
 const auto file=root.getChildFile("generated-reader-fixture.flac");const auto encoded=makeFlac(file);CHECK(file.deleteFile());std::atomic_bool cancel{false};PreparationError error{};
 std::unique_ptr<GeneratedDrumBytes> original=std::make_unique<Bytes>(encoded);const auto expected=byteOrigin(encoded);std::unique_ptr<DrumSource> source;
 CHECK(inspectDrumSource(original,expected,cancel,source,error)&&!original&&source);if(!source)return;const auto old=source.get();const auto pcm=source->info().decodedPcmId;
 std::string changed=encoded;std::size_t at=4,last=4;while(at+4<=changed.size()){last=at;const auto size=(std::uint32_t(std::uint8_t(changed[at+1]))<<16)|(std::uint32_t(std::uint8_t(changed[at+2]))<<8)|std::uint8_t(changed[at+3]);const bool final=(std::uint8_t(changed[at])&0x80)!=0;at+=4+size;if(final)break;}CHECK(at<changed.size());changed[last]=char(std::uint8_t(changed[last])&0x7f);changed.insert(at,std::string("\x81\0\0\1\0",5));
 std::unique_ptr<GeneratedDrumBytes> input=std::make_unique<Bytes>(changed);const auto identity=input.get();CHECK(!inspectDrumSource(input,expected,cancel,source,error)&&error==PreparationError::identityMismatch&&input.get()==identity&&source.get()==old);
 const auto changedOrigin=byteOrigin(changed);CHECK(changedOrigin.expectedFileId!=expected.expectedFileId);CHECK(inspectDrumSource(input,changedOrigin,cancel,source,error)&&!input&&source&&source->info().decodedPcmId==pcm&&source->info().encodedBytes==encoded.size()+5);
 const auto held=source.get();
 for(int mode=0;mode<3;++mode){auto bytes=std::make_unique<Bytes>(encoded);if(mode==0)bytes->fail=true;if(mode==1)bytes->cancelAfterRead=&cancel;input=std::move(bytes);auto* id=input.get();if(mode==2)cancel=true;CHECK(!inspectDrumSource(input,expected,cancel,source,error));CHECK(input.get()==id&&source.get()==held&&error==(mode==0?PreparationError::ioError:PreparationError::cancelled));cancel=false;}
 for(const auto suffix:{std::string(1,'\0'),std::string(1,char(0xff)),std::string("fLaC")}){input=std::make_unique<Bytes>(encoded+suffix);const auto id=input.get();CHECK(!inspectDrumSource(input,byteOrigin(encoded+suffix),cancel,source,error)&&input.get()==id&&source.get()==held);}
 auto truncated=encoded.substr(0,encoded.size()-1);input=std::make_unique<Bytes>(truncated);const auto truncatedId=input.get();CHECK(!inspectDrumSource(input,byteOrigin(truncated),cancel,source,error)&&input.get()==truncatedId&&source.get()==held);
}
void geometry(const juce::File& root){
 std::cout<<"GROUP generated_reader_strict_geometry_matching_hash"<<std::endl;
 const auto path=root.getChildFile("geometry.flac");const auto good=makeFlac(path);CHECK(path.deleteFile());
 std::atomic_bool cancel{false};PreparationError error{};std::unique_ptr<GeneratedDrumBytes> input=std::make_unique<Bytes>(good);std::unique_ptr<DrumSource> source;
 CHECK(inspectDrumSource(input,byteOrigin(good),cancel,source,error));if(!source)return;const auto held=source.get();const auto heldPcm=source->info().decodedPcmId;
 struct Shape{double rate;unsigned channels;int bits,frames;};
 for(const auto s:{Shape{48000,2,16,442368},Shape{44100,1,16,442368},Shape{44100,2,24,442368},Shape{44100,2,16,442367},Shape{44100,2,16,442369}}){
  const auto encoded=makeFlac(path,s.rate,s.channels,s.bits,s.frames);std::unique_ptr<DrumSource> manual;
  CHECK(inspectDrumSource(path,std::nullopt,cancel,manual,error));CHECK(manual&&manual->info().rate==s.rate&&manual->info().channels==s.channels&&manual->info().bits==unsigned(s.bits)&&manual->info().frames==unsigned(s.frames));manual.reset();CHECK(path.deleteFile());
  input=std::make_unique<Bytes>(encoded);const auto identity=input.get();
  CHECK(!inspectDrumSource(input,byteOrigin(encoded),cancel,source,error)&&error==PreparationError::identityMismatch&&input.get()==identity&&source.get()==held&&source->info().decodedPcmId==heldPcm);
 }
 auto noMd5=good;CHECK(noMd5.size()>42);std::fill(noMd5.begin()+26,noMd5.begin()+42,0);input=std::make_unique<Bytes>(noMd5);const auto identity=input.get();
 CHECK(!inspectDrumSource(input,byteOrigin(noMd5),cancel,source,error)&&error==PreparationError::identityMismatch&&input.get()==identity&&source.get()==held);
}

#if defined(_MSC_VER) && defined(_DEBUG)
namespace heap {
struct Row{long request=0;std::size_t bytes=0;};
struct Result{std::size_t live=0,peak=0,overlap=0,allocated=0,freed=0,baselineFreed=0,denials=0;std::array<std::size_t,512> requests{};unsigned count=0;bool valid=true;};
static std::array<Row,16384> rows;
static Result result;
static SRWLOCK lock=SRWLOCK_INIT;
static std::atomic_bool active{false};
thread_local bool inside=false,allowDeny=false;
static std::size_t denySize=0;
void remove(void* data,int use) noexcept{
 if(!data)return;long id=0;const auto n=_msize_dbg(data,use);
 if(n>UINT_MAX||!_CrtIsMemoryBlock(data,unsigned(n),&id,nullptr,nullptr)){result.valid=false;return;}
 const auto row=std::find_if(rows.begin(),rows.end(),[&](const Row&r){return r.request==id;});
 if(row==rows.end()){result.baselineFreed+=n;return;}if(row->bytes>result.live){result.valid=false;return;}
 result.live-=row->bytes;result.freed+=row->bytes;*row={};
}
int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char*,int){
 if(!active.load(std::memory_order_acquire)||inside)return 1;inside=true;AcquireSRWLockExclusive(&lock);
 struct Exit{~Exit(){ReleaseSRWLockExclusive(&lock);inside=false;}}exit;
 if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){use=_CrtReportBlockType(data);if(use==-1){result.valid=false;return 1;}}
 use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
 if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){
  if(result.count<result.requests.size())result.requests[result.count]=bytes;++result.count;
  if(allowDeny&&denySize&&bytes==denySize&&!result.denials){++result.denials;return 0;}
  if(kind==_HOOK_REALLOC){result.overlap=std::max(result.overlap,result.live+bytes);remove(data,use);}
  auto row=std::find_if(rows.begin(),rows.end(),[](const Row&r){return r.request==0;});if(request<=0||row==rows.end()){result.valid=false;return 1;}
  *row={request,bytes};result.live+=bytes;result.allocated+=bytes;result.peak=std::max(result.peak,result.live);
 }else if(kind==_HOOK_FREE)remove(data,use);return 1;
}
int __cdecl report(int type,wchar_t* text,int* value){
 if(allowDeny&&result.denials&&type==_CRT_WARN&&std::wstring_view(text?text:L"")==L"Client hook allocation failure.\n"){if(value)*value=0;return 1;}return 0;
}
template<class F>Result observe(F&& work,std::size_t deny=0){
 rows.fill({});result={};denySize=deny;inside=false;allowDeny=deny!=0;_CrtMemState before{},after{};_CrtMemCheckpoint(&before);
 const auto prior=_CrtSetAllocHook(hook);_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,report);active.store(true,std::memory_order_release);
 try{work();}catch(...){active=false;allowDeny=false;_CrtSetAllocHook(prior);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,report);throw;}
 active.store(false,std::memory_order_release);allowDeny=false;_CrtSetAllocHook(prior);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,report);_CrtMemCheckpoint(&after);
 const auto out=result;CHECK(out.valid);const auto delta=std::int64_t(after.lSizes[_NORMAL_BLOCK])-std::int64_t(before.lSizes[_NORMAL_BLOCK])+std::int64_t(after.lSizes[_CLIENT_BLOCK])-std::int64_t(before.lSizes[_CLIENT_BLOCK]);
 CHECK(std::int64_t(out.live)-std::int64_t(out.baselineFreed)==delta);return out;
}
bool saw(const Result&r,std::size_t bytes){return std::find(r.requests.begin(),r.requests.begin()+std::min<std::size_t>(r.count,r.requests.size()),bytes)!=r.requests.begin()+std::min<std::size_t>(r.count,r.requests.size());}
std::size_t peak(const Result&r){return std::max(r.peak,r.overlap);}
void calibrate(){
 auto block=observe([]{juce::MemoryBlock bytes(1024,false);});CHECK(block.live==0&&block.peak==1024&&block.allocated==1024&&block.freed==1024);
 const auto start=CreateEventW(nullptr,TRUE,FALSE,nullptr),done=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);CHECK(start&&done&&release);if(!start||!done||!release)return;
 std::atomic_bool allocated{false};std::thread other([&]{WaitForSingleObject(start,INFINITE);auto*p=_malloc_dbg(34567,_NORMAL_BLOCK,__FILE__,__LINE__);allocated=p!=nullptr;if(p)_free_dbg(p,_NORMAL_BLOCK);SetEvent(done);WaitForSingleObject(release,INFINITE);});
 bool complete=false;auto cross=observe([&]{SetEvent(start);complete=WaitForSingleObject(done,3000)==WAIT_OBJECT_0;});SetEvent(release);other.join();CloseHandle(start);CloseHandle(done);CloseHandle(release);
 CHECK(complete&&allocated&&saw(cross,34567)&&cross.allocated>=34567&&cross.live==0);
}
}
#endif
std::unique_ptr<ComfyRawFile> download(const std::string& encoded,const juce::File& rawRoot,std::uint64_t seed=17){
 const auto filename="seed-"+std::to_string(seed)+"_00001.flac";
 const std::string target="/view?filename="+filename+"&subfolder=airack%2F00112233-4455-6677-8899-aabbccddeeff%2F42%2F10213243-5465-7687-98a9-bacbdcedfe0f&type=output";
 NativeComfyFixture fixture("GET",target,"",response(encoded,"audio/flac"));CHECK(fixture.valid());auto http=airack::companion::detail::ComfyHttpTestAccess::fixture(ComfyLoopback::ipv4,rawRoot,fixture.port());
 ComfyHttpRequest request;request.operation=ComfyOperation::download;request.owned={juce::Uuid("00112233445566778899aabbccddeeff"),juce::Uuid("102132435465768798a9bacbdcedfe0f"),42,seed};request.filename=filename;request.subfolder="airack/00112233-4455-6677-8899-aabbccddeeff/42/10213243-5465-7687-98a9-bacbdcedfe0f";
 std::atomic_bool cancel{false};auto reply=http->perform(request,cancel,Clock::now()+std::chrono::seconds(5));fixture.join();CHECK(fixture.captured().matched&&fixture.requestCount()==1&&reply.status==ComfyHttpStatus::ok&&reply.raw&&reply.raw->size()==encoded.size());return std::move(reply.raw);
}
void allocationTransactions(const juce::File& root,bool memory){
 std::cout<<"GROUP generated_held_reader_calibrated_allocation_transactions memory="<<memory<<std::endl;
#if defined(_MSC_VER) && defined(_DEBUG)
 heap::calibrate();const auto file=root.getChildFile("allocation.flac");const auto encoded=makeFlac(file);CHECK(file.deleteFile());const auto rawRoot=root.getChildFile("allocation-raw");CHECK(rawRoot.createDirectory().wasOk());
 std::unique_ptr<ComfyRawFile> raw;const auto rawHeap=heap::observe([&]{raw=download(encoded,rawRoot);});if(!raw)return;
 const auto expected=origin(*raw);std::atomic_bool cancel{false};PreparationError error{};std::unique_ptr<DrumSource> source;bool accepted=false;
 const auto inspected=heap::observe([&]{accepted=inspectComfyDrumSource(raw,expected,cancel,source,error);});CHECK(accepted&&!raw&&source&&heap::saw(inspected,65536));if(!source)return;
 const auto held=source.get();const auto sourceId=source->info().fileId;PreparedDrumSample out;
 const auto prepared=heap::observe([&]{accepted=prepareDrumSample(*source,0,441000,.125f,cancel,out,error);});CHECK(accepted&&out.asset&&out.asset->frames()==480000&&out.asset->channels()==2);if(!out.asset)return;
 std::size_t pcmRequest=SIZE_MAX;for(std::size_t n=0;n<std::min<std::size_t>(prepared.count,prepared.requests.size());++n)if(prepared.requests[n]>=3840000&&prepared.requests[n]<3840256){pcmRequest=std::min(pcmRequest,prepared.requests[n]);std::cout<<"ADAPTER_CALIBRATED_REQUEST "<<prepared.requests[n]<<std::endl;}CHECK(pcmRequest!=SIZE_MAX);if(pcmRequest==SIZE_MAX)return;
 const auto old=out;const auto hashRequest=*std::max_element(prepared.requests.begin(),prepared.requests.end());CHECK(hashRequest>=3840020&&hashRequest<3840020+256);
 for(const auto target:{pcmRequest,hashRequest}){
  const auto denied=heap::observe([&]{accepted=prepareDrumSample(*source,0,441000,.125f,cancel,out,error);},target);
  CHECK(denied.denials==1&&!accepted&&error==PreparationError::allocationFailed);CHECK(out.asset==old.asset&&out.source==old.source&&out.peak==old.peak&&source.get()==held&&source->info().fileId==sourceId);
  std::cout<<"ADAPTER_PREPARE_REFUSAL requested_bytes="<<target<<" denials="<<denied.denials<<" unchanged_pcm_bytes="<<old.asset->samples().size_bytes()<<std::endl;
 }
 if(memory){constexpr std::size_t inspectBound=8u*1024*1024,prepareBound=16u*1024*1024,prototype=49153u*sizeof(double);static_assert(prototype==393224);
  CHECK(rawHeap.live+heap::peak(inspected)<=inspectBound);CHECK(rawHeap.live+inspected.live+heap::peak(prepared)<=prepareBound);
  std::cout<<"ADAPTER_REQUESTED_HEAP process_wide_CRT=1 ledger_rows=16384 raw_retained="<<rawHeap.live<<" inspect_peak="<<heap::peak(inspected)<<" held_source="<<inspected.live<<" prepare_peak="<<heap::peak(prepared)<<" prepared_retained="<<prepared.live<<" prototype_separate="<<prototype<<" logical_batch_encoded_capacity="<<48u*1024*1024<<" actual_single_encoded_file="<<encoded.size()<<" current_pcm_bytes="<<old.asset->samples().size_bytes()<<" RSS_measured=0 maximum_batch_claim=0"<<std::endl;
 }
 // Retry inspection with a real owner and an existing destination; the helper
 // payload allocation is calibrated above, not a proxy/synchronization block.
 source.reset();out={};raw=download(encoded,rawRoot);if(!raw)return;const auto rawIdentity=raw.get();const auto path=raw->file();
 std::unique_ptr<GeneratedDrumBytes> bytes=std::make_unique<Bytes>(encoded);CHECK(inspectDrumSource(bytes,byteOrigin(encoded),cancel,source,error));if(!source)return;const auto oldSource=source.get();
 const auto denied=heap::observe([&]{accepted=inspectComfyDrumSource(raw,origin(*raw),cancel,source,error);},65536);
 CHECK(denied.denials==1&&!accepted&&error==PreparationError::allocationFailed&&raw.get()==rawIdentity&&source.get()==oldSource&&path.exists());
 std::array<std::byte,4> signature{};CHECK(raw->readAt(0,signature)&&std::memcmp(signature.data(),"fLaC",4)==0);
 cancel=true;const auto canceled=heap::observe([&]{accepted=inspectComfyDrumSource(raw,expected,cancel,source,error);},65536);
 CHECK(!accepted&&error==PreparationError::cancelled&&canceled.denials==0&&raw.get()==rawIdentity&&source.get()==oldSource);cancel=false;
 raw.reset();source.reset();CHECK(!path.exists());
#else
 CHECK(false&&"Debug MSVC actual allocation calibration is required");
#endif
}

std::string loadBytes(const juce::File& file){juce::MemoryBlock data;CHECK(file.loadFileAsData(data));return {static_cast<const char*>(data.getData()),data.getSize()};}
AssetId digest(std::string_view bytes){AssetId value{};const auto hash=juce::SHA256(bytes.data(),bytes.size()).getRawData();std::copy(hash.begin(),hash.end(),value.begin());return value;}
std::uint64_t little(const char* bytes,unsigned count){std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(std::uint8_t(bytes[i]))<<(8*i);return n;}
void qualified(const juce::File& root,const juce::File& fixtures){
 std::cout<<"GROUP retained_qualified_originals_generated_reader_equivalence read_only=1 inference=0 listening=0"<<std::endl;
 struct Row{const char* name;unsigned seed;const char* flacHash;const char* wavHash;};
    constexpr std::array<Row, 12> rows{
        {{"kick", 17, "ed55fa9465f8a64830c868037bc793e3f8fadf2b97180650518dcb73feb139b0",
          "42199ed3e6542c6d450ce61326db0d0e4f176f9bb2ccf4c9b50390d7c9fd7794"},
         {"kick", 73, "954af084cebc6dc4a8a8a81ffa53338436265b878e87412dd8a9781435de9c54",
          "d286e79471cdeed962aed7756cd92c2218190869607d635010862c0a8fc4c84e"},
         {"kick", 211, "be0885b66587735fbd66ba83bf9cf4d8bc5a3e3114024d4b195c3a5ba8ac981b",
          "a7c2856dbdcc698ef66b77ebb7bc5ca8d92edffa7acec2412c3f5aeacc1c8805"},
         {"snare", 17, "829ad2a93d4a7718100220aae90855099ea4eaa8a9d227afdab94cf5b0789c4b",
          "a7d4491d7e9bbd6a4f9a0d74b9fce3facc48466307ae4b15f4c372c2de42a53d"},
         {"snare", 73, "5a20feb4807845c888411f38e43ca47e7552204576031403ec9390a4d1d09c99",
          "dfc2a88fd93cc272635eba518e7d3fe616e8cf560d1b58a21118ba7a8db60d94"},
         {"snare", 211, "81236116c5506c7bdd6a41ec9b894c8143c49ee212ab3703c4d3480ab99a6cf7",
          "02175cddf037b5cddf331a0cc88d45450ec0cde5fa4e77792f6529dbe6d82060"},
         {"hat", 17, "08f530a7e9b7632dfa41d3be451c7dcd7069472757caa7bc68cda48f6117126d",
          "4339e5eb9963daf98561ce03e0e224aa5955d47e898fe53bdddbc8df387b187a"},
         {"hat", 73, "ff5d51d321fbb8796e78f2aaa1a606704d38f119d8caee4906598ed639ca167b",
          "869ef527692e493885f36b40d38bd7155fc7b8393c2a383024e2b2649dff8169"},
         {"hat", 211, "a54f324174f7caa4e962c3ecb647ca992f9407617a42eb2a93a27ec896bf2938",
          "ba32e9bc70e2db65be3f4fe537eb35c75d0f4d4584d1d7527de0a6616389d353"},
         {"percussion", 17, "e419ffd6dc85c17a838ded7e49a4c02d65d1b4ffc1f1d59fd01091376a35e693",
          "55121d17f0f2ffddd3f5ec886d0f5c8e0882f5d21f51d305e6b8a6e1251d3552"},
         {"percussion", 73, "a2b31a65aea522e5c23815e0add3b828da352a51f9f5e17407910ea2e5ea1eb6",
          "7ebfdb47625f574009690a4c8639996d6071673ca85d5621a47e7316ef5e8d1f"},
         {"percussion", 211, "30e47cb9538ae4d91625ae144358f1558d71b9c41447369cea27da7299653dac",
          "15ef1ab424de03836bc4655a01bff06bd35cc17410ea01bf2501c3085913538e"}}};

 const auto rawRoot=root.getChildFile("qualified-raw");CHECK(rawRoot.createDirectory().wasOk());std::atomic_bool cancel{false};PreparationError error{};
 for(const auto& row:rows){
  const auto original=fixtures.getChildFile("base/output/trial/"+juce::String(row.name)+"-"+juce::String(row.seed)+"_00001.flac");
  const auto reference=fixtures.getChildFile("attempts/"+juce::String(row.name)+"/"+juce::String(row.seed)+"-original.wav");
  const auto encoded=loadBytes(original),wav=loadBytes(reference);const bool identities=assetIdText(digest(encoded))==row.flacHash&&assetIdText(digest(wav))==row.wavHash;CHECK(identities);if(!identities)continue;
  bool format=false;std::string_view pcm;
  if(CHECK(wav.size()>=12&&std::memcmp(wav.data(),"RIFF",4)==0&&std::memcmp(wav.data()+8,"WAVE",4)==0&&little(wav.data()+4,4)+8==wav.size())){
   std::size_t at=12;while(at+8<=wav.size()){const auto n=little(wav.data()+at+4,4);if(!CHECK(n<=wav.size()-at-8))break;const auto* data=wav.data()+at+8;
    if(std::memcmp(wav.data()+at,"fmt ",4)==0){CHECK(n>=16);if(n>=16)format=little(data,2)==3&&little(data+2,2)==2&&little(data+4,4)==44100&&little(data+12,2)==8&&little(data+14,2)==32;}
    if(std::memcmp(wav.data()+at,"data",4)==0){CHECK(pcm.empty());pcm={data,std::size_t(n)};}at+=8+std::size_t(n)+(n&1);
   }
  }
  CHECK(format&&pcm.size()==442368u*2*4);if(!format||pcm.size()!=442368u*2*4)continue;const auto expectedPcm=digest(pcm);
  auto expected=byteOrigin(encoded);expected.seed=row.seed;std::unique_ptr<GeneratedDrumBytes> input=std::make_unique<Bytes>(encoded);std::unique_ptr<DrumSource> generated,adapter,manual;
  CHECK(inspectDrumSource(input,expected,cancel,generated,error)&&!input&&generated);if(!generated)continue;
  auto raw=download(encoded,rawRoot,row.seed);CHECK(raw);if(!raw)continue;CHECK(inspectComfyDrumSource(raw,expected,cancel,adapter,error)&&!raw&&adapter);
  CHECK(inspectDrumSource(reference,std::nullopt,cancel,manual,error)&&manual);if(!adapter||!manual)continue;
  for(const auto* source:{generated.get(),adapter.get()}){const auto& info=source->info();CHECK(info.rate==44100&&info.channels==2&&info.bits==16&&info.frames==442368&&info.md5Verified&&info.fileId==expected.expectedFileId&&info.decodedPcmId==expectedPcm);}
  CHECK(manual->info().decodedPcmId==expectedPcm);
  PreparedDrumSample actual,referencePrepared;CHECK(!prepareDrumSample(*adapter,0,442368,.125f,cancel,actual,error)&&error==PreparationError::invalidSelection&&!actual.asset);
  CHECK(prepareDrumSample(*adapter,0,441000,.125f,cancel,actual,error));CHECK(prepareDrumSample(*manual,0,441000,.125f,cancel,referencePrepared,error));
  CHECK(actual.asset&&referencePrepared.asset&&actual.asset->id()==referencePrepared.asset->id()&&actual.asset->frames()==480000&&actual.source.kind==SourceKind::generated&&actual.source.seed==row.seed);
  CHECK(assetIdText(digest(loadBytes(original)))==row.flacHash&&assetIdText(digest(loadBytes(reference)))==row.wavHash);
  std::cout<<"ADAPTER_QUALIFIED class="<<row.name<<" seed="<<row.seed<<" pcm_sha256="<<assetIdText(expectedPcm)<<" frames=442368 source_rate=44100 channels=2 bits=16 md5_verified=1 explicit_test_crop=441000 explicit_test_gain=0.125"<<std::endl;
 }
}

}
int main(int argc,char** argv){
 std::string selected="all";juce::File fixtures;
 if(argc==3&&std::string_view(argv[1])=="--section")selected=argv[2];
 else if(argc==3&&std::string_view(argv[1])=="--qualified-fixtures"){selected="qualified";if(!juce::File::isAbsolutePath(juce::String::fromUTF8(argv[2])))return 2;fixtures=juce::File(juce::String::fromUTF8(argv[2]));}
 else if(argc!=1)return 2;
 if(selected!="all"&&selected!="red"&&selected!="geometry"&&selected!="allocation"&&selected!="memory"&&selected!="qualified")return 2;
 if(selected=="qualified"&&(fixtures==juce::File{}||!fixtures.isDirectory()))return 2;
 WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
 const auto* diagnostic=std::getenv("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");if(!diagnostic){WSACleanup();return 2;}
 const auto root=juce::File(juce::String::fromUTF8(diagnostic)).getChildFile("comfy-adapter-"+juce::Uuid().toString());CHECK(root.createDirectory().wasOk());
 if(!setupControls()){WSACleanup();return 2;}
 if(selected=="all"||selected=="red"){red(root);generatedTransactions(root);}
 if(selected=="all"||selected=="geometry")geometry(root);
 if(selected=="all"||selected=="allocation"||selected=="memory")allocationTransactions(root,selected=="memory");
 if(selected=="qualified")qualified(root,fixtures);
 std::cout<<"ComfyDrumPreparation checks="<<checks<<" failures="<<failures<<std::endl;WSACleanup();return failures?1:0;
}
