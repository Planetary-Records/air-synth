#include "RackPackage.h"
#include "StrictJson.h"
#include "DrumBundle.h"
#include "SoundDocument.h"
#include "SoundState.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace airack {
namespace {
using juce::String; using juce::var;
namespace limits=rackPackageLimits;
bool fail(RackLibraryStatus value,RackLibraryStatus* status) { if(status)*status=value;return false; }
bool ready(RackLibraryStatus* status) { if(status)*status=RackLibraryStatus::ready;return true; }
bool closed(const var& value,std::initializer_list<const char*> fields) {
    const auto* object=value.getDynamicObject();if(!object||object->getProperties().size()!=int(fields.size()))return false;
    return std::all_of(fields.begin(),fields.end(),[&](const auto* field){return object->hasProperty(field);});
}
var object(std::initializer_list<std::pair<juce::Identifier,var>> fields) {
    auto* result=new juce::DynamicObject;for(const auto& [key,value]:fields)result->setProperty(key,value);return result;
}
bool quantity(const var& value,std::uint64_t ceiling,std::uint64_t& result) {
    if(!value.isInt()&&!value.isInt64())return false;
    const auto signedValue=juce::int64(value);if(signedValue<0||std::uint64_t(signedValue)>ceiling)return false;
    result=std::uint64_t(signedValue);return true;
}
bool decimal(const String& value,std::uint64_t& result) {
    const auto text=value.toStdString();if(text.empty()||text[0]=='0')return false;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    return parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size();
}
bool humanText(const var& value,std::size_t maximum,bool empty=false) {
    if(!value.isString())return false;const auto text=value.toString();
    if((!empty&&text.trim().isEmpty())||text.getNumBytesAsUTF8()>maximum)return false;
    for(auto c:text)if(c==0||(c<32&&c!='\n'&&c!='\t'))return false;
    return true;
}
bool hash(const String& text) {
    return validRackPackageHash(text);
}
bool token(const String& text,std::size_t maximum) {
    if(text.isEmpty()||text.getNumBytesAsUTF8()>maximum||text[0]=='-'||text.endsWithChar('-'))return false;
    for(auto c:text)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))return false;return true;
}
bool version(const String& text,std::array<unsigned,3>& result) {
    if(!validRackPackageVersion(text))return false;
    const auto parts=juce::StringArray::fromTokens(text,".","");
    for(int i=0;i<3;++i)result[std::size_t(i)]=unsigned(parts[i].getIntValue());
    return true;
}
bool privateComponent(const String& component) {
    const auto value=component.toLowerCase();
    return value=="package.json"||value=="source.json"||value=="rack-source.json"||value.startsWith("prompt")
        ||value.startsWith("private")||value.contains("credential")||value==".env";
}
std::uint64_t fileLimit(const String& kind) {
    if(kind=="design")return limits::designBytes;if(kind=="skin")return limits::skinBytes;
    if(kind=="png")return limits::pngBytes;if(kind=="notice")return limits::skinBytes;
    if(kind=="preset")return maximumSoundStateBytes;
    if(kind=="drum-bundle")return drums::maxDrumBundleBytes;
    // Owned SampleAsset blobs retain the existing ten-second stereo limit.
    if(kind=="audio")return 20+480000ull*2*4+32;
    return 0;
}
bool fileKindPath(const RackPackageFile& file) {
    const auto extension=file.path.fromLastOccurrenceOf(".",true,false).toLowerCase();
    return ((file.kind=="design"||file.kind=="skin")&&extension==".json")||(file.kind=="png"&&extension==".png")
        ||(file.kind=="notice"&&(extension==".txt"||extension==".md"))
        ||(file.kind=="preset"&&extension==".soundstate")||(file.kind=="audio"&&extension==".airasset")
        ||(file.kind=="drum-bundle"&&extension==".airdrums");
}
bool capabilities(const var& value,RackLibraryStatus* status) {
    if(!closed(value,{"airMin","modules","nativeEditor"})||!value["airMin"].isString()||!value["nativeEditor"].isString())return fail(RackLibraryStatus::invalidData,status);
    std::array<unsigned,3> minimum{};if(!version(value["airMin"].toString(),minimum))return fail(RackLibraryStatus::invalidData,status);
    if(minimum>std::array<unsigned,3>{0,1,0})return fail(RackLibraryStatus::incompatible,status);
    const auto editor=value["nativeEditor"].toString();if(editor.isNotEmpty()&&editor!="space_reverb")return fail(RackLibraryStatus::incompatible,status);
    const auto* modules=value["modules"].getArray();if(!modules||modules->isEmpty()||modules->size()>9)return fail(RackLibraryStatus::invalidData,status);
    std::set<std::pair<unsigned,unsigned>> seen;bool space=false;
    for(const auto& module:*modules){std::uint64_t m=0,a=0;
        if(!closed(module,{"module","algorithmRevision"})||!quantity(module["module"],255,m)||!quantity(module["algorithmRevision"],255,a))return fail(RackLibraryStatus::invalidData,status);
        if(m>unsigned(NativeModule::saturation)||!supportsAlgorithm(NativeModule(m),AlgorithmRevision(a)))return fail(RackLibraryStatus::incompatible,status);
        if(!seen.emplace(unsigned(m),unsigned(a)).second)return fail(RackLibraryStatus::invalidData,status);space|=m==unsigned(NativeModule::spaceReverb);
    }
    if(editor=="space_reverb"&&!space)return fail(RackLibraryStatus::invalidData,status);return true;
}
var manifestObject(const RackPackageManifest& m) {
    juce::Array<var> tags,files;for(const auto& tag:m.metadata.tags)tags.add(tag);
    for(const auto& f:m.files)files.add(object({{"path",f.path},{"sha256",f.sha256},{"kind",f.kind},{"bytes",juce::int64(f.bytes)}}));
    return object({{"schema",1},{"id",m.ref.id},{"version",m.ref.version},
        {"metadata",object({{"design",m.metadata.design.toDashedString()},{"revision",String(std::to_string(m.metadata.revision))},
            {"canonical_sha256",m.metadata.canonicalSha256},{"name",m.metadata.name},{"maker",m.metadata.maker},
            {"purpose",m.metadata.purpose},{"description",m.metadata.description},{"role",m.metadata.role==RackRole::synth?"synth":"fx"},{"tags",tags}})},
        {"design",m.designPath},{"skin",m.skinPath},{"screenshot",m.screenshotPath},{"files",files},{"capabilities",m.capabilities}});
}
bool parseManifest(const var& v,RackPackageManifest& m,RackLibraryStatus* status) {
    std::uint64_t schema=0;
    if(!closed(v,{"schema","id","version","metadata","design","skin","screenshot","files","capabilities"})
        ||!quantity(v["schema"],1,schema)||schema!=1||!v["id"].isString()||!validRackPackageId(v["id"].toString())
        ||!v["version"].isString()||!validRackPackageVersion(v["version"].toString()))return fail(RackLibraryStatus::invalidData,status);
    m.ref={v["id"].toString(),v["version"].toString(),{}};const auto meta=v["metadata"];
    if(!closed(meta,{"design","revision","canonical_sha256","name","maker","purpose","description","role","tags"})
        ||!meta["design"].isString()||!meta["revision"].isString()||!meta["canonical_sha256"].isString()
        ||!hash(meta["canonical_sha256"].toString())||!decimal(meta["revision"].toString(),m.metadata.revision)
        ||!humanText(meta["name"],limits::nameBytes)||!humanText(meta["maker"],limits::makerBytes)
        ||!humanText(meta["purpose"],limits::purposeBytes)||!humanText(meta["description"],limits::descriptionBytes,true)
        ||!meta["role"].isString()||(meta["role"].toString()!="synth"&&meta["role"].toString()!="fx"))return fail(RackLibraryStatus::invalidData,status);
    m.metadata.design=juce::Uuid(meta["design"].toString());
    if(m.metadata.design.isNull()||m.metadata.design.toDashedString()!=meta["design"].toString())return fail(RackLibraryStatus::invalidData,status);
    m.metadata.canonicalSha256=meta["canonical_sha256"].toString();m.metadata.name=meta["name"].toString();m.metadata.maker=meta["maker"].toString();
    m.metadata.purpose=meta["purpose"].toString();m.metadata.description=meta["description"].toString();m.metadata.role=meta["role"].toString()=="synth"?RackRole::synth:RackRole::fx;
    const auto* tags=meta["tags"].getArray();if(!tags||tags->size()>int(limits::tagCount))return fail(RackLibraryStatus::countLimit,status);
    std::set<String> tagIds;
    for(const auto& tag:*tags)if(!tag.isString()||!validRackTagId(tag.toString())||!tagIds.insert(tag.toString()).second)return fail(RackLibraryStatus::invalidData,status);else m.metadata.tags.add(tag.toString());
    for(const auto* field:{"design","skin","screenshot"})if(!v[field].isString())return fail(RackLibraryStatus::invalidData,status);
    m.designPath=v["design"].toString();m.skinPath=v["skin"].toString();m.screenshotPath=v["screenshot"].toString();
    if(!validRackPackagePath(m.designPath)||!validRackPackagePath(m.screenshotPath)||(m.skinPath.isNotEmpty()&&!validRackPackagePath(m.skinPath)))return fail(RackLibraryStatus::invalidData,status);
    const auto* files=v["files"].getArray();if(!files||files->isEmpty()||files->size()+1>int(limits::fileCount))return fail(RackLibraryStatus::countLimit,status);
    std::set<String> names;std::uint64_t expanded=0;int designs=0,skins=0;
    for(const auto& value:*files){RackPackageFile file;
        if(!closed(value,{"path","sha256","kind","bytes"})||!value["path"].isString()||!value["sha256"].isString()||!value["kind"].isString())return fail(RackLibraryStatus::invalidData,status);
        file.path=value["path"].toString();file.sha256=value["sha256"].toString();file.kind=value["kind"].toString();const auto ceiling=fileLimit(file.kind);
        if(!validRackPackagePath(file.path)||!hash(file.sha256)||ceiling==0||!fileKindPath(file))return fail(RackLibraryStatus::invalidData,status);
        if(!quantity(value["bytes"],ceiling,file.bytes)||file.bytes==0||file.bytes>limits::expandedBytes-expanded)return fail(RackLibraryStatus::byteLimit,status);
        expanded+=file.bytes;if(!names.insert(file.path.toLowerCase()).second)return fail(RackLibraryStatus::invalidData,status);
        designs+=file.kind=="design";skins+=file.kind=="skin";m.files.push_back(std::move(file));
    }
    for(const auto& path:names){auto end=path.indexOfChar('/');while(end>=0){if(names.count(path.substring(0,end)))return fail(RackLibraryStatus::invalidData,status);end=path.indexOfChar(end+1,'/');}}
    const auto matching=[&](const String& path,const String& kind){return std::count_if(m.files.begin(),m.files.end(),[&](const auto& f){return f.path==path&&f.kind==kind;})==1;};
    if(designs!=1||skins!=(m.skinPath.isEmpty()?0:1)||!matching(m.designPath,"design")||!matching(m.screenshotPath,"png")
        ||(m.skinPath.isNotEmpty()&&!matching(m.skinPath,"skin")))return fail(RackLibraryStatus::invalidData,status);
    const auto recipe=std::find_if(m.files.begin(),m.files.end(),[&](const auto& f){return f.path==m.designPath;});
    if(recipe->sha256!=m.metadata.canonicalSha256)return fail(RackLibraryStatus::identityConflict,status);
    if(!capabilities(v["capabilities"],status))return false;m.capabilities=v["capabilities"].clone();return true;
}
}
bool validRackTagId(const String& text){return token(text,limits::tagBytes);}
bool validRackPackagePath(const String& text) {
    if(text.isEmpty()||text.getNumBytesAsUTF8()>limits::pathBytes||text.startsWithChar('/')||text.endsWithChar('/'))return false;
    for(auto c:text)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='-'||c=='_'||c=='.'))return false;
    const auto parts=juce::StringArray::fromTokens(text,"/","");
    for(const auto& part:parts){if(part.isEmpty()||part.startsWithChar('.')||part.endsWithChar('.')||privateComponent(part))return false;
        const auto base=part.upToFirstOccurrenceOf(".",false,false).toUpperCase();
        if(base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"
            ||(base.length()==4&&(base.startsWith("COM")||base.startsWith("LPT"))&&base[3]>='0'&&base[3]<='9'))return false;}
    return true;
}
bool decodeRackPackageManifest(std::string_view text,RackPackageManifest& out,RackLibraryStatus* status) {
    if(text.size()>limits::manifestBytes)return fail(RackLibraryStatus::byteLimit,status);
    var value;RackPackageManifest candidate;
    if(!ai::parseStrictJson(text,value,limits::manifestBytes))return fail(RackLibraryStatus::invalidData,status);
    if(!parseManifest(value,candidate,status))return false;
    std::uint64_t expanded=text.size();for(const auto& file:candidate.files){if(file.bytes>limits::expandedBytes-expanded)return fail(RackLibraryStatus::byteLimit,status);expanded+=file.bytes;}
    out=std::move(candidate);return ready(status);
}
bool encodeRackPackageManifest(const RackPackageManifest& m,String& out,RackLibraryStatus* status) {
    if(m.metadata.role!=RackRole::synth&&m.metadata.role!=RackRole::fx)return fail(RackLibraryStatus::invalidData,status);
    if(m.ref.sha256.isNotEmpty()&&!hash(m.ref.sha256))return fail(RackLibraryStatus::invalidData,status);
    const auto text=juce::JSON::toString(manifestObject(m),true);RackPackageManifest checked;
    if(!decodeRackPackageManifest(text.toStdString(),checked,status))return false;out=text;return ready(status);
}
bool validateRackTagDictionary(std::string_view text,std::vector<RackTag>& out,RackLibraryStatus* status) {
    var value;std::uint64_t schema=0;if(!ai::parseStrictJson(text,value,limits::catalogBytes)||!closed(value,{"schema","tags"})
        ||!quantity(value["schema"],1,schema)||schema!=1)return fail(RackLibraryStatus::invalidData,status);
    const auto* tags=value["tags"].getArray();if(!tags||tags->size()>int(limits::tagDictionaryCount))return fail(RackLibraryStatus::countLimit,status);
    std::set<String> ids;std::vector<RackTag> candidate;
    for(const auto& tag:*tags){if(!closed(tag,{"id","label"})||!tag["id"].isString()||!validRackTagId(tag["id"].toString())
        ||!humanText(tag["label"],limits::tagLabelBytes)||!ids.insert(tag["id"].toString()).second)return fail(RackLibraryStatus::invalidData,status);
        candidate.push_back({tag["id"].toString(),tag["label"].toString()});}
    out=std::move(candidate);return ready(status);
}
bool validateRackPackageDesign(const RackPackageManifest& m,std::string_view text,RackDesign& out,RackLibraryStatus* status) {
    if(text.empty()||text.size()>limits::designBytes)return fail(RackLibraryStatus::byteLimit,status);
    RackDesign candidate;if(!decodeRackDesign(text.data(),text.size(),candidate,RackValidationMode::instanceRecall))return fail(RackLibraryStatus::incompatible,status);
    juce::MemoryBlock canonical;if(!encodeRackDesign(candidate,canonical,RackValidationMode::instanceRecall))return fail(RackLibraryStatus::invalidData,status);
    if(candidate.id!=m.metadata.design||candidate.revision!=m.metadata.revision||candidate.role!=m.metadata.role||candidate.name!=m.metadata.name
        ||juce::SHA256(canonical).toHexString()!=m.metadata.canonicalSha256||canonical.getSize()!=text.size()
        ||std::memcmp(canonical.getData(),text.data(),text.size())!=0)return fail(RackLibraryStatus::identityConflict,status);
    if(!capabilities(m.capabilities,status))return false;
    std::set<std::pair<int,int>> expected,declared;
    for(const auto& unit:candidate.units)expected.emplace(int(unit.module),int(unit.algorithmRevision));
    for(const auto& module:*m.capabilities["modules"].getArray())declared.emplace(int(module["module"]),int(module["algorithmRevision"]));
    if(expected!=declared)return fail(RackLibraryStatus::incompatible,status);out=std::move(candidate);return ready(status);
}
bool inspectRackPackagePng(std::span<const std::byte> bytes,std::uint32_t& width,std::uint32_t& height,RackLibraryStatus* status) {
    if(bytes.size()>limits::pngBytes)return fail(RackLibraryStatus::byteLimit,status);
    constexpr unsigned char signature[]{137,80,78,71,13,10,26,10};const auto* p=reinterpret_cast<const unsigned char*>(bytes.data());
    if(bytes.size()<33||std::memcmp(p,signature,8)!=0||p[8]!=0||p[9]!=0||p[10]!=0||p[11]!=13||std::memcmp(p+12,"IHDR",4)!=0)return fail(RackLibraryStatus::invalidData,status);
    const auto u32=[&](std::size_t at){return (std::uint32_t(p[at])<<24)|(std::uint32_t(p[at+1])<<16)|(std::uint32_t(p[at+2])<<8)|std::uint32_t(p[at+3]);};
    const auto w=u32(16),h=u32(20);
    if(w==0||h==0||w>limits::pngEdge||h>limits::pngEdge||std::uint64_t(w)*h>limits::pngPixels)return fail(RackLibraryStatus::byteLimit,status);
    const auto depth=p[24],colour=p[25];
    if(!((colour==0&&(depth==1||depth==2||depth==4||depth==8||depth==16))||(colour==2&&(depth==8||depth==16))
        ||(colour==3&&(depth==1||depth==2||depth==4||depth==8))||((colour==4||colour==6)&&(depth==8||depth==16)))
        ||p[26]!=0||p[27]!=0||p[28]>1)return fail(RackLibraryStatus::invalidData,status);
    std::uint32_t crc=0xffffffffu;for(std::size_t i=12;i<29;++i){crc^=p[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&std::uint32_t(-int(crc&1)));}
    if((crc^0xffffffffu)!=u32(29))return fail(RackLibraryStatus::invalidData,status);width=w;height=h;return ready(status);
}
bool validateRackPackageFileBytes(const RackPackageFile& file,std::span<const std::byte> bytes,RackLibraryStatus* status,std::uint64_t* decodedPngBytes) {
    if(!validRackPackagePath(file.path)||!fileKindPath(file)||!hash(file.sha256)||fileLimit(file.kind)==0)return fail(RackLibraryStatus::invalidData,status);
    if(bytes.size()!=file.bytes||bytes.empty()||bytes.size()>fileLimit(file.kind))return fail(RackLibraryStatus::byteLimit,status);
    if(juce::SHA256(bytes.data(),bytes.size()).toHexString()!=file.sha256)return fail(RackLibraryStatus::identityConflict,status);
    if(file.kind=="png"){std::uint32_t w=0,h=0;if(!inspectRackPackagePng(bytes,w,h,status))return false;const auto decoded=std::uint64_t(w)*h*4;
        if(decodedPngBytes){if(*decodedPngBytes>limits::decodedSkinBytes||decoded>limits::decodedSkinBytes-*decodedPngBytes)return fail(RackLibraryStatus::byteLimit,status);*decodedPngBytes+=decoded;}}
    else if(file.kind=="design"||file.kind=="skin"){var value;if(!ai::parseStrictJson({reinterpret_cast<const char*>(bytes.data()),bytes.size()},value,std::size_t(fileLimit(file.kind)))||!value.isObject())return fail(RackLibraryStatus::invalidData,status);}
    else if(file.kind=="notice"){if(!juce::CharPointer_UTF8::isValidString(reinterpret_cast<const char*>(bytes.data()),int(bytes.size())))return fail(RackLibraryStatus::invalidData,status);}
    else if(file.kind=="preset"){SoundDocument sound;if(!decodeSoundState(bytes.data(),bytes.size(),sound))return fail(RackLibraryStatus::invalidData,status);}
    else if(file.kind=="audio"){drums::AssetPtr asset;drums::AssetError error;if(!drums::decodeSampleAsset({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},asset,error))return fail(RackLibraryStatus::invalidData,status);}
    else if(file.kind=="drum-bundle"){drums::DrumBundlePtr bundle;drums::DrumError error;if(!drums::decodeDrumBundle({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},bundle,error))return fail(RackLibraryStatus::invalidData,status);}
    return ready(status);
}
}
