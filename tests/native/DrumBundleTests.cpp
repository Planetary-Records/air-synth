#include "DrumBundle.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <bit>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <climits>
#include <crtdbg.h>
#endif
#include <string_view>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace allocation_test { bool failNext = false; std::size_t minimumSize = 0; }
void* operator new(std::size_t n)
{
    if (allocation_test::failNext && n >= allocation_test::minimumSize) { allocation_test::failNext = false; throw std::bad_alloc(); }
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
using namespace airack::drums;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
bool check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 1000) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
    return pass;
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* s) { section = s; ++sections; std::cout << s << '\n'; }
using Bytes = std::vector<std::uint8_t>;
Bytes bytes(const juce::MemoryBlock& b)
{
    const auto* p = static_cast<const std::uint8_t*>(b.getData());
    return b.getSize() == 0 ? Bytes{} : Bytes(p, p + b.getSize());
}
void put(Bytes& b, std::size_t offset, std::uint64_t value, unsigned width = 4)
{ for (unsigned i = 0; i < width; ++i) b[offset + i] = static_cast<std::uint8_t>(value >> (8 * i)); }
std::uint32_t get(const Bytes& b, std::size_t offset)
{ return b[offset] | std::uint32_t(b[offset + 1]) << 8 | std::uint32_t(b[offset + 2]) << 16 | std::uint32_t(b[offset + 3]) << 24; }
void rehash(Bytes& b)
{
    const auto d = juce::SHA256(b.data(), b.size() - 32).getRawData();
    std::memcpy(b.data() + b.size() - 32, d.getData(), 32);
}
Bytes goldenDocument()
{
    Bytes b(232, 0); const char magic[] = "ARDOC001"; std::memcpy(b.data(), magic, 8);
    const std::array<std::uint8_t, 32> d {
        0x76,0xb1,0x01,0x1e,0xb4,0xf7,0xd2,0x53,0x9e,0xfb,0x87,0x91,0x2b,0x79,0x42,0x71,
        0x1d,0x37,0x7e,0x6b,0xbd,0x39,0xf3,0x41,0xe6,0x7b,0x7d,0x9c,0xd6,0x1c,0x87,0xb0 };
    std::copy(d.begin(), d.end(), b.begin() + 200); return b;
}
Bytes goldenBundle()
{
    Bytes b(292, 0); const char magic[] = "ARBNDL01"; std::memcpy(b.data(), magic, 8);
    b[8] = 4; b[12] = 232;
    const std::array<std::uint8_t, 4> compact { 'A','I','P','J' };
    std::copy(compact.begin(), compact.end(), b.begin() + 24);
    const auto doc = goldenDocument(); std::copy(doc.begin(), doc.end(), b.begin() + 28);
    const std::array<std::uint8_t, 32> d {
        0x11,0x6d,0x8c,0x2e,0x7e,0x41,0x14,0xb2,0xd3,0x92,0x8b,0xed,0xdb,0x5d,0x40,0x2c,
        0x6e,0x36,0xa3,0x7a,0xd4,0x0b,0x6b,0x39,0xae,0x97,0xf0,0xb6,0x81,0x99,0x4e,0xab };
    std::copy(d.begin(), d.end(), b.begin() + 260); return b;
}
AssetPtr asset(float x = .25f)
{
    const std::array<float, 2> data { x, -.5f }; AssetPtr a; AssetError e;
    CHECK(makeSampleAsset(data, 1, a, e)); return a;
}
DrumDocumentPtr document(const DrumSlots& slots = {})
{ DrumDocumentPtr d; DrumError e; CHECK(makeDrumDocument(slots, d, e)); CHECK(e == DrumError::none); return d; }
SourceProvenance source()
{
    SourceProvenance s; s.kind = SourceKind::generated; s.sourceFileId.fill(0x21);
    s.sourceRate = 48000; s.sourceChannels = 1; s.sourceFrames = 2; s.end = 2; s.gain = .5f;
    s.generator = "m"; s.condition = "c"; s.seed = 211; s.promptId.fill(0x42); return s;
}
void rejectSlots(const DrumSlots& slots, const DrumDocumentPtr& old)
{ auto d = old; DrumError e = DrumError::none; CHECK(!makeDrumDocument(slots, d, e)); CHECK(e == DrumError::invalidDocument); CHECK(d == old); }
void rejectDocument(const Bytes& b, const DrumDocumentPtr& old)
{ auto d = old; DrumError e = DrumError::none; CHECK(!decodeDrumDocument(b, d, e)); CHECK(e != DrumError::none); CHECK(d == old); }
void rejectBundle(const Bytes& b, const DrumBundlePtr& old, DrumError expected)
{ auto d = old; DrumError e = DrumError::none; CHECK(!decodeDrumBundle(b, d, e)); CHECK(e == expected); CHECK(d == old); }
#if defined(_MSC_VER) && defined(_DEBUG)
namespace bundleHeap {
struct Row { long request=0; std::size_t bytes=0; };
struct Result {
    std::size_t live=0,peak=0,overlap=0,allocated=0,freed=0,baselineFreed=0;
    std::size_t allocations=0,reallocations=0,denials=0;
    bool valid=true;
};
static std::array<Row,1024> rows;
thread_local Result result;
thread_local bool recording=false,inside=false;
thread_local std::size_t denySize=0;
void remove(void* data,int use) noexcept {
    if(!data)return; long request=0; const auto n=_msize_dbg(data,use);
    if(n>UINT_MAX||!_CrtIsMemoryBlock(data,unsigned(n),&request,nullptr,nullptr))
        {result.valid=false;return;}
    const auto row=std::find_if(rows.begin(),rows.end(),[&](auto r){return r.request==request;});
    if(row==rows.end()){result.baselineFreed+=n;return;}
    if(row->bytes>result.live){result.valid=false;return;}
    result.live-=row->bytes;result.freed+=row->bytes;*row={};
}
int __cdecl hook(int kind,void* data,std::size_t n,int use,long request,const unsigned char*,int) {
    if(!recording||inside)return 1;inside=true;
    struct Exit {~Exit(){inside=false;}} exit;
    if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)) {
        use=_CrtReportBlockType(data);if(use==-1){result.valid=false;return 1;}
    }
    use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
    if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC) {
        if(denySize&&n==denySize&&result.denials==0){++result.denials;return 0;}
        if(kind==_HOOK_REALLOC) {
            ++result.reallocations;result.overlap=std::max(result.overlap,result.live+n);
            remove(data,use);
        }
        const auto row=std::find_if(rows.begin(),rows.end(),[](auto r){return r.request==0;});
        if(request<=0||row==rows.end()){result.valid=false;return 1;}
        *row={request,n};result.live+=n;result.allocated+=n;++result.allocations;
        result.peak=std::max(result.peak,result.live);
    } else if(kind==_HOOK_FREE)remove(data,use);
    return 1;
}
template<class F> Result observe(F&& work,std::size_t deny=0) {
    rows.fill({});result={};denySize=deny;inside=false;
    _CrtMemState before{},after{};_CrtMemCheckpoint(&before);
    const auto previous=_CrtSetAllocHook(hook);recording=true;
    try{work();}catch(...){recording=false;_CrtSetAllocHook(previous);throw;}
    recording=false;_CrtSetAllocHook(previous);_CrtMemCheckpoint(&after);
    const auto observed=result;
    const auto delta=std::int64_t(after.lSizes[_NORMAL_BLOCK])-std::int64_t(before.lSizes[_NORMAL_BLOCK])
        +std::int64_t(after.lSizes[_CLIENT_BLOCK])-std::int64_t(before.lSizes[_CLIENT_BLOCK]);
    CHECK(observed.valid);
    if(deny==0)CHECK(std::int64_t(observed.live)-std::int64_t(observed.baselineFreed)==delta);
    return observed;
}
void calibrate() {
    // Proves MemoryBlock's malloc/free is actually observed, beyond operator new.
    const auto block=observe([]{juce::MemoryBlock b(1024,false);});
    CHECK(block.live==0&&block.peak==1024&&block.allocated==1024&&block.freed==1024);
    void* baseline=_malloc_dbg(80,_NORMAL_BLOCK,__FILE__,__LINE__);
    if(!CHECK(baseline!=nullptr))return;
    bool ok=false;
    const auto resize=observe([&]{
        void* p=_malloc_dbg(64,_NORMAL_BLOCK,__FILE__,__LINE__);
        if(p){void* q=_realloc_dbg(p,128,_NORMAL_BLOCK,__FILE__,__LINE__);
            ok=q!=nullptr;_free_dbg(q?q:p,_NORMAL_BLOCK);}
        _free_dbg(baseline,_NORMAL_BLOCK);
    });
    CHECK(ok&&resize.live==0&&resize.peak==128&&resize.overlap==192);
    CHECK(resize.allocated==192&&resize.freed==192&&resize.baselineFreed==80);
}
} // namespace bundleHeap
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
void serializationWorkspace(std::size_t count,std::uint32_t frames,std::size_t compactBytes) {
    begin("transactional encoded workspace");
    DrumSlots slots;std::array<AssetPtr,64> assets;
    Bytes compact(compactBytes,0x5a);std::vector<float> pcm(count?std::size_t(frames)*2:0);
    for(std::size_t i=0;i<count;++i) {
        std::fill(pcm.begin(),pcm.end(),float(i+1)/128.f);AssetError ae;
        if(!CHECK(makeSampleAsset(pcm,2,assets[i],ae)))return;
        auto& pad=slots[i/16][i%16];pad.asset=assets[i]->id();pad.name.assign(64,'x');
        pad.source=source();pad.source->sourceFrames=frames;pad.source->end=frames;
        pad.source->sourceChannels=2;pad.source->gain=1;
        pad.source->generator.assign(96,'m');pad.source->condition.assign(128,'c');
    }
    std::vector<float>().swap(pcm);
    const auto doc=document(slots);if(!CHECK(doc!=nullptr))return;
    DrumBundlePtr bundle;DrumError e;
    if(!CHECK(makeDrumBundle(doc,compact,std::span<const AssetPtr>(assets).first(count),bundle,e)))return;
    Bytes().swap(compact); // Bundle owns the compact bytes; don't retain a fixture copy.
    juce::MemoryBlock manifest;if(!CHECK(encodeDrumDocument(*doc,manifest,e)))return;
    std::size_t size=24+bundle->compactDocument().size()+manifest.getSize()+32,largest=0;
    for(const auto& a:bundle->assets()) {
        const auto blob=52+a->samples().size()*4;size+=4+blob;largest=std::max(largest,blob);
    }
    manifest.reset();juce::MemoryBlock wire;bool encoded=false;
    const auto r=bundleHeap::observe([&]{encoded=encodeDrumBundle(*bundle,wire,e);});
    CHECK(encoded&&e==DrumError::none&&wire.getSize()==size);
    const auto bound=size+4*largest+128*1024;
    CHECK(std::max(r.peak,r.overlap)<=bound);
    CHECK(r.live==size&&r.baselineFreed==0);
    std::cout<<"BUNDLE_WORKSPACE decoded="<<bundle->decodedAudioBytes()<<" old_dest=0 encoded="<<size
             <<" largest_blob="<<largest<<" requested_peak="<<r.peak<<" realloc_overlap_upper="<<r.overlap
             <<" bound="<<bound<<" retained="<<r.live<<" allocations="<<r.allocations
             <<" reallocations="<<r.reallocations<<" recorder_valid="<<r.valid<<'\n';
    if(!encoded||wire.getSize()!=size)return;
    const auto expectedHash=juce::SHA256(wire).toHexString();
    std::cout<<"BUNDLE_BYTES sha256="<<expectedHash<<'\n';
    const auto* raw=static_cast<const std::uint8_t*>(wire.getData());
    const auto word=[&](std::size_t at){return std::uint32_t(raw[at])|(std::uint32_t(raw[at+1])<<8)
        |(std::uint32_t(raw[at+2])<<16)|(std::uint32_t(raw[at+3])<<24);};
    CHECK(std::memcmp(raw,"ARBNDL01",8)==0);CHECK(word(8)==bundle->compactDocument().size());
    CHECK(word(16)==bundle->assets().size()&&word(20)==0);
    if(!CHECK(encodeDrumDocument(*doc,manifest,e)))return;
    CHECK(word(12)==manifest.getSize());const auto bodyEnd=size-32;
    const auto currentCompact=bundle->compactDocument();
    if(!currentCompact.empty())CHECK(std::memcmp(raw+24,currentCompact.data(),currentCompact.size())==0);
    std::size_t at=24+currentCompact.size();
    if(!CHECK(at<=bodyEnd&&manifest.getSize()<=bodyEnd-at))return;
    CHECK(std::memcmp(raw+at,manifest.getData(),manifest.getSize())==0);at+=manifest.getSize();
    for(const auto& a:bundle->assets()) {
        if(!CHECK(at<=bodyEnd&&bodyEnd-at>=4))return;
        const auto declared=word(at);at+=4;juce::MemoryBlock blob;AssetError ae;
        if(!CHECK(encodeSampleAsset(*a,blob,ae)))return;
        if(!CHECK(declared==blob.getSize()&&declared<=bodyEnd-at))return;
        CHECK(std::memcmp(raw+at,blob.getData(),declared)==0);at+=declared;
    }
    CHECK(at==bodyEnd);const auto digest=juce::SHA256(raw,bodyEnd).getRawData();
    CHECK(std::memcmp(raw+bodyEnd,digest.getData(),32)==0);manifest.reset();
    if(count==0) {
        const auto oldBytes=wire.getSize();bool repeated=false;
        const auto again=bundleHeap::observe([&]{repeated=encodeDrumBundle(*bundle,wire,e);});
        CHECK(repeated&&wire.getSize()==size&&juce::SHA256(wire).toHexString()==expectedHash);
        CHECK(again.live==size&&again.baselineFreed==oldBytes);
        CHECK(std::max(again.peak,again.overlap)<=bound);
        std::cout<<"BUNDLE_WORKSPACE old_dest="<<oldBytes<<" requested_peak="<<again.peak
                 <<" realloc_overlap_upper="<<again.overlap<<" baseline_freed="<<again.baselineFreed<<'\n';
    }
    const std::array<std::uint8_t,3> marker{1,2,3};
    wire=juce::MemoryBlock(marker.data(),marker.size());const auto* old=wire.getData();
    bool accepted=true;e=DrumError::none;
    const auto denied=bundleHeap::observe([&]{accepted=encodeDrumBundle(*bundle,wire,e);},size);
    CHECK(denied.denials==1&&!accepted&&e==DrumError::allocationFailed);
    CHECK(wire.getData()==old&&wire.getSize()==marker.size());
    CHECK(std::memcmp(wire.getData(),marker.data(),marker.size())==0);
    if(count) {
        // Manifest is small; target the first per-asset canonical vector, after body allocation.
        allocation_test::minimumSize=largest-32;allocation_test::failNext=true;
        accepted=encodeDrumBundle(*bundle,wire,e);
        const bool consumed=!allocation_test::failNext;
        allocation_test::failNext=false;allocation_test::minimumSize=0;
        CHECK(consumed&&!accepted&&e==DrumError::allocationFailed);
        CHECK(wire.getData()==old&&wire.getSize()==marker.size());
        CHECK(std::memcmp(wire.getData(),marker.data(),marker.size())==0);
    }
}
#endif
void goldenAndOpaqueBytes()
{
    begin("independent empty manifest and bundle goldens");
    static_assert(std::is_same_v<DrumDocumentPtr, std::shared_ptr<const DrumDocument>>);
    static_assert(std::is_same_v<DrumBundlePtr, std::shared_ptr<const DrumBundle>>);
    static_assert(!std::is_default_constructible_v<DrumDocument> && !std::is_copy_assignable_v<DrumDocument>);
    static_assert(!std::is_default_constructible_v<DrumBundle> && !std::is_copy_assignable_v<DrumBundle>);
    auto doc = document(); if (!CHECK(doc != nullptr)) return;
    juce::MemoryBlock b; DrumError e = DrumError::invalidBlob;
    CHECK(encodeDrumDocument(*doc, b, e)); CHECK(e == DrumError::none); CHECK(bytes(b) == goldenDocument());
    DrumDocumentPtr decoded; CHECK(decodeDrumDocument(goldenDocument(), decoded, e));
    if (CHECK(decoded != nullptr)) CHECK(decoded->slots() == doc->slots());
    const std::array<std::uint8_t, 4> compact { 'A','I','P','J' };
    DrumBundlePtr bundle; CHECK(makeDrumBundle(doc, compact, {}, bundle, e));
    if (!CHECK(bundle != nullptr)) return;
    CHECK(encodeDrumBundle(*bundle, b, e)); CHECK(bytes(b) == goldenBundle());
    DrumBundlePtr loaded; CHECK(decodeDrumBundle(goldenBundle(), loaded, e));
    if (!CHECK(loaded != nullptr)) return;
    CHECK(loaded->assets().empty()); CHECK(loaded->decodedAudioBytes() == 0);
    CHECK(Bytes(loaded->compactDocument().begin(), loaded->compactDocument().end()) == Bytes(compact.begin(), compact.end()));
    const std::array<std::uint8_t, 5> opaque { 0, 0xff, 0x80, 0, 7 };
    CHECK(makeDrumBundle(doc, opaque, {}, bundle, e)); CHECK(encodeDrumBundle(*bundle, b, e));
    CHECK(decodeDrumBundle(bytes(b), loaded, e));
    CHECK(Bytes(loaded->compactDocument().begin(), loaded->compactDocument().end()) == Bytes(opaque.begin(), opaque.end()));
}
void closureDedupAndOwnedRecall()
{
    begin("dedup, deterministic sorting and recall without source owners");
    auto a = asset(), same = asset(), b = asset(.75f); if (!CHECK(a && same && b)) return;
    const std::weak_ptr<const SampleAsset> weakA = a, weakSame = same, weakB = b;
    DrumSlots slots; slots[0][0] = { a->id(), "K", source() }; slots[3][15] = { b->id(), "caf\xc3\xa9", {} };
    slots[2][7].asset = a->id(); auto doc = document(slots); if (!CHECK(doc != nullptr)) return;
    slots[0][0].name = "changed"; CHECK(doc->slots()[0][0].name == "K");
    std::array<AssetPtr, 3> input { b, same, a }; const std::array<std::uint8_t, 2> compact { 9, 3 };
    DrumBundlePtr bundle; DrumError e; CHECK(makeDrumBundle(doc, compact, input, bundle, e));
    if (!CHECK(bundle != nullptr)) return;
    CHECK(bundle->assets().size() == 2); CHECK(bundle->decodedAudioBytes() == 16);
    CHECK(bundle->assets()[0]->id() < bundle->assets()[1]->id());
    CHECK(bundle->findAsset(a->id()) != nullptr); AssetId absent {}; CHECK(bundle->findAsset(absent) == nullptr);
    juce::MemoryBlock wire, repeated; CHECK(encodeDrumBundle(*bundle, wire, e));
    std::reverse(input.begin(), input.end()); DrumBundlePtr other;
    CHECK(makeDrumBundle(doc, compact, input, other, e)); CHECK(encodeDrumBundle(*other, repeated, e)); CHECK(wire == repeated);
    auto reversed = bytes(wire);
    const auto start = 24 + get(reversed, 8) + get(reversed, 12), firstSize = 4 + get(reversed, start);
    std::rotate(reversed.begin() + start, reversed.begin() + start + firstSize, reversed.end() - 32); rehash(reversed);
    auto unchanged = bundle; CHECK(!decodeDrumBundle(reversed, unchanged, e)); CHECK(e == DrumError::invalidBlob); CHECK(unchanged == bundle);
    const auto wantedId = a->id(), wantedBId = b->id();
    input.fill({}); a.reset(); same.reset(); b.reset(); doc.reset(); other.reset(); unchanged.reset(); bundle.reset();
    CHECK(weakA.expired()); CHECK(weakSame.expired()); CHECK(weakB.expired());
    DrumBundlePtr recalled; CHECK(decodeDrumBundle(bytes(wire), recalled, e));
    if (!CHECK(recalled != nullptr)) return;
    const auto* saved = recalled->findAsset(wantedId); const auto* savedB = recalled->findAsset(wantedBId);
    if (!CHECK(saved != nullptr && savedB != nullptr)) return;
    CHECK(saved->channels() == 1); CHECK(saved->frames() == 2); CHECK(savedB->channels() == 1); CHECK(savedB->frames() == 2);
    const std::array<float, 2> wanted { .25f, -.5f }, wantedB { .75f, -.5f };
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        CHECK(std::bit_cast<std::uint32_t>(saved->samples()[i]) == std::bit_cast<std::uint32_t>(wanted[i]));
        CHECK(std::bit_cast<std::uint32_t>(savedB->samples()[i]) == std::bit_cast<std::uint32_t>(wantedB[i]));
    }
    const auto& recalledSource = recalled->document().slots()[0][0].source;
    if (CHECK(recalledSource.has_value())) {
        CHECK(*recalledSource == source());
        CHECK(std::bit_cast<std::uint32_t>(recalledSource->gain) == std::bit_cast<std::uint32_t>(.5f));
    }
}
void metadataValidation()
{
    begin("document metadata, provenance and bounded UTF8 validation");
    const auto old = document(); const auto a = asset(); if (!CHECK(old && a)) return;
    DrumSlots valid; valid[0][0] = { a->id(), "K", source() };
    auto bad = valid; bad[0][0].name.assign(65, 'x'); rejectSlots(bad, old);
    bad = valid; bad[0][0].name = std::string(1, '\0'); rejectSlots(bad, old);
    bad = valid; bad[0][0].name = std::string(1, static_cast<char>(0xff)); rejectSlots(bad, old);
    bad = valid; bad[0][0].asset.reset(); rejectSlots(bad, old);
    bad = valid; bad[0][0].source->generator.assign(97, 'm'); rejectSlots(bad, old);
    bad = valid; bad[0][0].source->generator = std::string(1, static_cast<char>(0xff)); rejectSlots(bad, old);
    bad = valid; bad[0][0].source->condition = std::string(1, static_cast<char>(0xff)); rejectSlots(bad, old);
    for (unsigned field = 0; field < 12; ++field)
    {
        bad = valid; auto& s = *bad[0][0].source;
        switch (field) {
            case 0: s.kind = static_cast<SourceKind>(0); break;
            case 1: s.sourceRate = 0; break;
            case 2: s.sourceRate = 384001; break;
            case 3: s.sourceChannels = 3; break;
            case 4: s.sourceFrames = 0; break;
            case 5: s.begin = s.end; break;
            case 6: s.end = s.sourceFrames + 1; break;
            case 7: s.gain = 0; break;
            case 8: s.gain = std::numeric_limits<float>::quiet_NaN(); break;
            case 9: s.gain = 16.01f; break;
            case 10: s.generator.clear(); break;
            case 11: s.condition.assign(129, 'x'); break;
        }
        rejectSlots(bad, old);
    }
    bad = valid; bad[0][0].source->sourceFrames = 480001; bad[0][0].source->end = 480001; rejectSlots(bad, old);
    bad = valid; bad[0][0].source->kind = SourceKind::imported; rejectSlots(bad, old);
    auto imported = source(); imported.kind = SourceKind::imported; imported.generator.clear(); imported.condition.clear(); imported.seed = 0; imported.promptId = {};
    valid[0][0].source = imported; valid[0][0].name.assign(64, 'x');
    DrumDocumentPtr d; DrumError e; CHECK(makeDrumDocument(valid, d, e));
    valid[0][0].source.reset(); CHECK(makeDrumDocument(valid, d, e));
    valid[0][0].asset.reset(); CHECK(makeDrumDocument(valid, d, e));
}
void malformedDocuments()
{
    begin("manifest truncation, flags and signed malformed source metadata");
    const auto old = document(); const auto a = asset(); if (!CHECK(old && a)) return;
    const auto good = goldenDocument();
    for (std::size_t n = 0; n < good.size(); ++n) rejectDocument(Bytes(good.begin(), good.begin() + n), old);
    auto bad = good; bad.push_back(0); rejectDocument(bad, old);
    bad = good; bad[0] ^= 1; rehash(bad); rejectDocument(bad, old);
    bad = good; bad.back() ^= 1; rejectDocument(bad, old);
    bad = good; bad[10] = 2; rehash(bad); rejectDocument(bad, old);
    bad = good; put(bad, 8, 0xffff, 2); rehash(bad); rejectDocument(bad, old);
    bad.assign(maxDrumDocumentBytes + 1, 0); rejectDocument(bad, old);
    DrumSlots slots; slots[0][0] = { a->id(), "K", source() }; const auto doc = document(slots);
    juce::MemoryBlock encoded; DrumError e; CHECK(encodeDrumDocument(*doc, encoded, e)); const auto rich = bytes(encoded);
    for (auto offset : { 10u, 11u, 44u, 45u, 82u })
    { bad = rich; bad[offset] = 0xff; rehash(bad); rejectDocument(bad, old); }
    bad = rich; put(bad, 78, 0); rehash(bad); rejectDocument(bad, old);
    bad = rich; put(bad, 91, 3, 8); rehash(bad); rejectDocument(bad, old);
    bad = rich; put(bad, 107, 0x7f800000); rehash(bad); rejectDocument(bad, old);
}
void bundleAdmissionAndMalformedInput()
{
    begin("closure, bounded counts and malformed self-contained bundles");
    auto a = asset(); auto empty = document(); if (!CHECK(a && empty)) return;
    DrumBundlePtr old; DrumError e; CHECK(makeDrumBundle(empty, {}, {}, old, e)); if (!CHECK(old != nullptr)) return;
    DrumSlots slots; slots[0][0].asset = a->id(); auto doc = document(slots);
    auto dest = old; CHECK(!makeDrumBundle(doc, {}, {}, dest, e)); CHECK(e == DrumError::missingAsset); CHECK(dest == old);
    std::array<AssetPtr, 1> one { a };
    CHECK(!makeDrumBundle(empty, {}, one, dest, e)); CHECK(e == DrumError::unexpectedAsset); CHECK(dest == old);
    one[0].reset(); CHECK(!makeDrumBundle(doc, {}, one, dest, e)); CHECK(e == DrumError::invalidAsset); CHECK(dest == old); one[0] = a;
    CHECK(!makeDrumBundle({}, {}, {}, dest, e)); CHECK(e == DrumError::invalidDocument); CHECK(dest == old);
    std::array<AssetPtr, 65> tooMany; tooMany.fill(a);
    CHECK(!makeDrumBundle(doc, {}, tooMany, dest, e)); CHECK(e == DrumError::tooManyAssets); CHECK(dest == old);
    Bytes hugeCompact(maxCompactDocumentBytes + 1);
    CHECK(!makeDrumBundle(empty, hugeCompact, {}, dest, e)); CHECK(e == DrumError::tooLarge); CHECK(dest == old);
    CHECK(makeDrumBundle(doc, {}, one, dest, e)); juce::MemoryBlock encoded; CHECK(encodeDrumBundle(*dest, encoded, e)); const auto good = bytes(encoded);
    for (std::size_t n = 0; n < good.size(); ++n)
    { auto target = old; CHECK(!decodeDrumBundle(std::span<const std::uint8_t>(good.data(), n), target, e)); CHECK(e != DrumError::none); CHECK(target == old); }
    auto bad = good; bad.push_back(0); rehash(bad); rejectBundle(bad, old, DrumError::invalidBlob);
    bad = good; bad.back() ^= 1; rejectBundle(bad, old, DrumError::digestMismatch);
    bad = good; bad[0] ^= 1; rehash(bad); rejectBundle(bad, old, DrumError::invalidBlob);
    bad = good; put(bad, 20, 1); rehash(bad); rejectBundle(bad, old, DrumError::invalidBlob);
    bad = good; put(bad, 8, maxCompactDocumentBytes + 1); rehash(bad); rejectBundle(bad, old, DrumError::tooLarge);
    bad = good; put(bad, 12, maxDrumDocumentBytes + 1); rehash(bad); rejectBundle(bad, old, DrumError::tooLarge);
    bad = good; put(bad, 16, 65); rehash(bad); rejectBundle(bad, old, DrumError::tooManyAssets);
    const auto assetLengthOffset = 24 + get(good, 8) + get(good, 12);
    bad = good; put(bad, assetLengthOffset, 0xffffffffu); rehash(bad); rejectBundle(bad, old, DrumError::invalidBlob);
    bad = good; bad[assetLengthOffset + 4 + 20] ^= 1; rehash(bad); rejectBundle(bad, old, DrumError::invalidAsset);
    bad = good; bad.erase(bad.begin() + assetLengthOffset, bad.end() - 32); put(bad, 16, 0); rehash(bad); rejectBundle(bad, old, DrumError::missingAsset);
    const auto record = Bytes(good.begin() + assetLengthOffset, good.end() - 32);
    bad = good; bad.insert(bad.end() - 32, record.begin(), record.end()); put(bad, 16, 2); rehash(bad); rejectBundle(bad, old, DrumError::invalidBlob);
}
void fixedOccupancyAndAllocationFailure()
{
    begin("four sixteen-pad kits and allocation transactionality");
    DrumSlots slots; std::array<AssetPtr, 64> all;
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = asset(static_cast<float>(i) / 64); if (!CHECK(all[i] != nullptr)) return;
        auto& pad = slots[i / 16][i % 16]; pad.asset = all[i]->id(); pad.name.assign(64, 'x'); pad.source = source();
        pad.source->generator.assign(96, 'm'); pad.source->condition.assign(128, 'c');
    }
    auto doc = document(slots); DrumBundlePtr bundle; DrumError e; CHECK(makeDrumBundle(doc, {}, all, bundle, e));
    if (!CHECK(bundle != nullptr)) return;
    CHECK(bundle->assets().size() == 64); CHECK(bundle->decodedAudioBytes() == 64 * 2 * 4);
    CHECK(std::uint64_t(64) * 480000 * 2 * 4 == 245760000);
    CHECK(std::uint64_t(64) * (4 + 20 + 480000 * 2 * 4 + 32) == 245763584);
    CHECK(std::uint64_t(64) * (4 + 20 + 480000 * 2 * 4 + 32) < maxBundleAudioBytes);
    CHECK(maxDrumBundleBytes == 269549624);
    juce::MemoryBlock manifest; CHECK(encodeDrumDocument(*doc, manifest, e)); CHECK(manifest.getSize() == 27816);
    juce::MemoryBlock wire; CHECK(encodeDrumBundle(*bundle, wire, e)); DrumBundlePtr loaded;
    CHECK(decodeDrumBundle(bytes(wire), loaded, e)); if (CHECK(loaded != nullptr)) CHECK(loaded->assets().size() == 64);
    auto oldDoc = doc; auto targetDoc = oldDoc; auto oldBundle = bundle; auto targetBundle = oldBundle;
    const std::array<std::uint8_t, 3> marker { 1, 2, 3 }; juce::MemoryBlock output(marker.data(), marker.size());
    allocation_test::failNext = true; CHECK(!makeDrumDocument(slots, targetDoc, e)); CHECK(e == DrumError::allocationFailed); CHECK(targetDoc == oldDoc); CHECK(!allocation_test::failNext);
    allocation_test::failNext = true; CHECK(!encodeDrumDocument(*doc, output, e)); CHECK(e == DrumError::allocationFailed); CHECK(bytes(output) == Bytes(marker.begin(), marker.end())); CHECK(!allocation_test::failNext);
    const auto emptyWire = goldenDocument();
    // MSVC Debug's noexcept string constructors allocate proxies; target the throwing factory allocation.
    allocation_test::minimumSize = sizeof(DrumDocument);
    allocation_test::failNext = true; CHECK(!decodeDrumDocument(emptyWire, targetDoc, e)); CHECK(e == DrumError::allocationFailed); CHECK(targetDoc == oldDoc); CHECK(!allocation_test::failNext);
    allocation_test::minimumSize = 0;
    allocation_test::failNext = true; CHECK(!makeDrumBundle(doc, {}, all, targetBundle, e)); CHECK(e == DrumError::allocationFailed); CHECK(targetBundle == oldBundle); CHECK(!allocation_test::failNext);
    allocation_test::failNext = true; CHECK(!encodeDrumBundle(*bundle, output, e)); CHECK(e == DrumError::allocationFailed); CHECK(bytes(output) == Bytes(marker.begin(), marker.end())); CHECK(!allocation_test::failNext);
    const auto emptyBundleWire = goldenBundle();
    allocation_test::minimumSize = sizeof(DrumDocument);
    allocation_test::failNext = true; CHECK(!decodeDrumBundle(emptyBundleWire, targetBundle, e)); CHECK(e == DrumError::allocationFailed); CHECK(targetBundle == oldBundle); CHECK(!allocation_test::failNext);
    allocation_test::minimumSize = 0;
}
void geometryPreflight()
{
    begin("bundle geometry preflight before allocation or hash");
    const BundleGeometry sentinel{17,19,23,29,31,37};
    auto out=sentinel;DrumError error=DrumError::digestMismatch;
    const auto empty=goldenBundle();
    CHECK(inspectDrumBundleGeometry(empty,out,error));
    CHECK(error==DrumError::none&&out.decodedBytes==0&&out.largestDecodedAsset==0
        &&out.assets==0&&out.encodedBytes==empty.size()&&out.compactBytes==4&&out.manifestBytes==232);
    const auto a=asset();DrumSlots slots{};slots[0][0].asset=a->id();
    DrumBundlePtr bundle;const std::array<AssetPtr,1> owners{a};
    CHECK(makeDrumBundle(document(slots),{},owners,bundle,error));
    juce::MemoryBlock wire;CHECK(encodeDrumBundle(*bundle,wire,error));const auto valid=bytes(wire);
    CHECK(inspectDrumBundleGeometry(valid,out,error));
    CHECK(out.decodedBytes==8&&out.largestDecodedAsset==8&&out.assets==1);
    allocation_test::failNext=true;
    const auto noAllocation=inspectDrumBundleGeometry(valid,out,error);
    const auto factoryNotCalled=allocation_test::failNext;allocation_test::failNext=false;
    CHECK(noAllocation&&factoryNotCalled);
    auto corruptDigest=valid;corruptDigest.back()^=1;
    CHECK(inspectDrumBundleGeometry(corruptDigest,out,error));
    DrumBundlePtr decoded;CHECK(!decodeDrumBundle(corruptDigest,decoded,error));
    const auto reject=[&](const Bytes& bad){out=sentinel;error=DrumError::none;
        CHECK(!inspectDrumBundleGeometry(bad,out,error));CHECK(out==sentinel);CHECK(error!=DrumError::none);};
    for(std::size_t n=0;n<valid.size();++n)reject(Bytes(valid.begin(),valid.begin()+n));
    const auto record=std::size_t(24+get(valid,8)+get(valid,12));
    for(const auto [offset,value]:std::array<std::pair<std::size_t,std::uint32_t>,13>{{
        {0,0},{20,1},{8,1048577},{12,65537},{16,65},{record,0xffffffffu},
        {record+4,0},{record+12,44100},{record+16,0},{record+16,480001},
        {record+20,0},{record+20,3},{record,55}}}){
        auto bad=valid;put(bad,offset,value);reject(bad);
    }
    auto trailing=valid;trailing.push_back(0);reject(trailing);
    auto count=valid;put(count,16,0);reject(count);
}
void boundedFuzz()
{
    begin("bounded mutation fuzz preserves rejected destinations");
    auto doc = document(); DrumBundlePtr old; DrumError e; CHECK(makeDrumBundle(doc, {}, {}, old, e));
    std::uint32_t state = 0x20261023;
    auto random = [&] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    for (unsigned i = 0; i < 256; ++i)
    {
        auto b = goldenBundle(); b[random() % b.size()] ^= static_cast<std::uint8_t>(1u << (random() % 8)); if (i % 2 == 0) rehash(b);
        auto dest = old;
        if (!decodeDrumBundle(b, dest, e)) { CHECK(e != DrumError::none); CHECK(dest == old); }
        else { juce::MemoryBlock encoded; CHECK(e == DrumError::none); CHECK(encodeDrumBundle(*dest, encoded, e)); CHECK(bytes(encoded) == b); }
    }
}
}
int main(int argc,char** argv) {
    const bool capacity=argc==2&&std::string_view(argv[1])=="--serialization-memory";
    if(argc!=1&&!capacity)return 2;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(capacity){std::cerr<<"BUNDLE_MEMORY_GATE_UNAVAILABLE\n";return 2;}
#endif
    try {
#if defined(_MSC_VER) && defined(_DEBUG)
        bundleHeap::calibrate();
        if(capacity)serializationWorkspace(64,480000,maxCompactDocumentBytes);
        else{serializationWorkspace(0,0,maxCompactDocumentBytes);serializationWorkspace(1,4096,0);}
#endif
        if(!capacity) {
            goldenAndOpaqueBytes();closureDedupAndOwnedRecall();metadataValidation();malformedDocuments();
            bundleAdmissionAndMalformedInput();fixedOccupancyAndAllocationFailure();geometryPreflight();boundedFuzz();
        }
    } catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";
    return failures==0?0:1;
}
