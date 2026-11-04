#include "FrozenRackExamples.h"
#include "RackPackageFixtures.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "DrumBundle.h"
#include "SoundDocument.h"
#include "SoundState.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <set>

using namespace airack;
namespace {
int checks=0,failures=0;
void check(bool result,const char* expression,int line){++checks;if(!result){++failures;std::cerr<<line<<": "<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
std::string utf8(const juce::String& s){return s.toStdString();}
std::span<const std::byte> span(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
bool decode(const juce::var& v,RackPackageManifest& out,RackLibraryStatus* status=nullptr){return decodeRackPackageManifest(utf8(juce::JSON::toString(v,true)),out,status);}
juce::var fixtureObject(){return juce::JSON::parse(test::packageFixtureJson(test::packageFixture()));}
void manifests(){
    const auto good=test::packageFixture();auto out=good;const auto json=utf8(test::packageFixtureJson(good));RackLibraryStatus status=RackLibraryStatus::unavailable;
    CHECK(decodeRackPackageManifest(json,out,&status));CHECK(status==RackLibraryStatus::ready);CHECK(out.ref.id==good.ref.id&&out.metadata.design==good.metadata.design);
    juce::String encoded="unchanged";CHECK(encodeRackPackageManifest(good,encoded));CHECK(decodeRackPackageManifest(utf8(encoded),out));
    for(const auto& bad:{std::string("{\"schema\":1,\"schema\":1}"),json+"x",std::string("\xff",1),std::string(65537,' ')}){
        out=good;CHECK(!decodeRackPackageManifest(bad,out));CHECK(out.ref==good.ref&&out.metadata.name==good.metadata.name&&out.files.size()==good.files.size());}
    for(const auto* field:{"schema","id","version","metadata","design","skin","screenshot","files","capabilities"}){
        auto v=fixtureObject();v.getDynamicObject()->removeProperty(field);CHECK(!decode(v,out));}
    {auto v=fixtureObject();v.getDynamicObject()->setProperty("command","run");CHECK(!decode(v,out));}
    for(const auto* kind:{"dll","executable","script","unknown"}){auto v=fixtureObject();(*v["files"].getArray())[1].getDynamicObject()->setProperty("kind",kind);CHECK(!decode(v,out));}
    for(const auto* path:{"../outside","/outside","C:/private","skin/CON.png","skin/com1.txt","skin/face.png:stream","skin\\face.png","a//b.png","a/./b.png","a/b.","a/b ","skin/é.png","package.json","SOURCE.json","skin/prompt.txt"})CHECK(!validRackPackagePath(juce::String::fromUTF8(path)));
    CHECK(validRackPackagePath("skin/parts/face-1.png"));CHECK(validRackPackagePath(juce::String::repeatedString("a",156)+".png"));CHECK(!validRackPackagePath(juce::String::repeatedString("a",157)+".png"));
    CHECK(validRackPackageId("ember-reed"));CHECK(!validRackPackageId("Ember Reed"));CHECK(validRackPackageId(juce::String::repeatedString("a",64)));CHECK(!validRackPackageId(juce::String::repeatedString("a",65)));
    for(const auto* version:{"1","1.0","1.0.0-beta","1.0.0+build","01.0.0","1.-1.0","65536.0.0"})CHECK(!validRackPackageVersion(version));
    CHECK(validRackPackageVersion("65535.65535.65535"));CHECK(validRackTagId("saturator"));CHECK(!validRackTagId("Saturator"));CHECK(!validRackTagId("saturation "));
    for(const auto* tag:{"Analog","","saturator ","bad/tag"}){auto v=fixtureObject();v["metadata"]["tags"].getArray()->set(0,tag);CHECK(!decode(v,out));}
    {auto v=fixtureObject();v["metadata"]["tags"].getArray()->add("analog");CHECK(!decode(v,out));}
    {auto v=fixtureObject();auto* tags=v["metadata"]["tags"].getArray();tags->clear();for(int i=0;i<9;++i)tags->add("tag-"+juce::String(i));CHECK(!decode(v,out));}
    for(const auto& pair:std::vector<std::pair<const char*,int>>{{"name",128},{"maker",128},{"purpose",512},{"description",4096}}){
        auto v=fixtureObject();v["metadata"].getDynamicObject()->setProperty(pair.first,juce::String::repeatedString("a",pair.second));CHECK(decode(v,out));
        v["metadata"].getDynamicObject()->setProperty(pair.first,juce::String::repeatedString("a",pair.second+1));CHECK(!decode(v,out));}
    {auto v=fixtureObject();v.getDynamicObject()->setProperty("screenshot","");CHECK(!decode(v,out));}
    {auto v=fixtureObject();v["metadata"].getDynamicObject()->setProperty("revision","18446744073709551616");CHECK(!decode(v,out));}
    {auto v=fixtureObject();v["metadata"].getDynamicObject()->setProperty("design","bad-uuid");CHECK(!decode(v,out));}
    {auto v=fixtureObject();v["capabilities"].getDynamicObject()->setProperty("airMin","99.0.0");CHECK(!decode(v,out,&status));CHECK(status==RackLibraryStatus::incompatible);}
    for(const auto* editor:{"dll","maker-v1","future"}){auto v=fixtureObject();v["capabilities"].getDynamicObject()->setProperty("nativeEditor",editor);CHECK(!decode(v,out));}
    for(const auto* field:{"module","algorithmRevision"}){auto v=fixtureObject();(*v["capabilities"]["modules"].getArray())[0].getDynamicObject()->setProperty(field,255);CHECK(!decode(v,out));}
    {auto v=fixtureObject();v["files"].getArray()->add((*v["files"].getArray())[0]);CHECK(!decode(v,out));}
    {auto v=fixtureObject();(*v["files"].getArray())[0].getDynamicObject()->setProperty("bytes",65537);CHECK(!decode(v,out));}
    {auto v=fixtureObject();(*v["files"].getArray())[1].getDynamicObject()->setProperty("bytes",juce::int64(rackPackageLimits::pngBytes+1));CHECK(!decode(v,out));}
    {auto v=fixtureObject();(*v["files"].getArray())[1].getDynamicObject()->setProperty("bytes",-1);CHECK(!decode(v,out));}
    {auto v=fixtureObject();(*v["files"].getArray())[1].getDynamicObject()->setProperty("bytes",1.5);CHECK(!decode(v,out));}
    std::vector<RackTag> tags{{"kept","Kept"}};
    CHECK(validateRackTagDictionary("{\"schema\":1,\"tags\":[{\"id\":\"drums\",\"label\":\"Drums\"}]}",tags));CHECK(tags.size()==1&&tags[0].id=="drums");
    CHECK(!validateRackTagDictionary("{\"schema\":1,\"tags\":[{\"id\":\"Drums\",\"label\":\"Drums\"}]}",tags));CHECK(tags[0].id=="drums");
}
void content(){
    auto m=test::packageFixture();const auto bytes=test::packageFixtureDesign();const auto raw=std::string_view(static_cast<const char*>(bytes.getData()),bytes.getSize());RackDesign out;
    CHECK(validateRackPackageDesign(m,raw,out));CHECK(out.id==m.metadata.design);const auto kept=out;
    for(int i=0;i<3;++i){auto bad=m;if(i==0)bad.metadata.design=juce::Uuid();if(i==1)++bad.metadata.revision;if(i==2)bad.metadata.canonicalSha256=juce::String::repeatedString("0",64);CHECK(!validateRackPackageDesign(bad,raw,out));CHECK(out==kept);}
    for(const auto* field:{"module","algorithm_revision"}){auto v=juce::JSON::parse(juce::String::fromUTF8(raw.data(),int(raw.size())));(*v["units"].getArray())[0].getDynamicObject()->setProperty(field,field==std::string("module")?juce::var("unknown"):juce::var(255));const auto text=utf8(juce::JSON::toString(v,true));auto bad=m;bad.metadata.canonicalSha256=juce::SHA256(text.data(),text.size()).toHexString();CHECK(!validateRackPackageDesign(bad,text,out));}
    {auto v=juce::JSON::parse(juce::String::fromUTF8(raw.data(),int(raw.size())));(*(*v["units"].getArray())[0]["defaults"].getArray())[0].getDynamicObject()->setProperty("key","unknown");const auto text=utf8(juce::JSON::toString(v,true));auto bad=m;bad.metadata.canonicalSha256=juce::SHA256(text.data(),text.size()).toHexString();CHECK(!validateRackPackageDesign(bad,text,out));}
    auto png=test::packageFixturePng();std::uint32_t w=99,h=88;CHECK(inspectRackPackagePng(span(png),w,h));CHECK(w==1&&h==1);CHECK(validateRackPackageFileBytes(m.files[1],span(png)));
    static_cast<unsigned char*>(png.getData())[16]=1;w=99;h=88;CHECK(!inspectRackPackagePng(span(png),w,h));CHECK(w==99&&h==88);CHECK(!validateRackPackageFileBytes(m.files[1],span(png)));
    const auto diagnostic=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",juce::String(RACKSYNTH_PACKAGE_SOURCE_DIR)+"/build/rack-library-native/task1");
    const auto directory=juce::File(diagnostic).getNonexistentChildFile("air-package-test",{},false);
    const auto zip=test::writePackageZip(directory,m);juce::MemoryBlock first,second;CHECK(zip.loadFileAsData(first));CHECK(test::writePackageZip(directory,m).loadFileAsData(second));CHECK(first==second);
    {juce::ZipFile archive(zip);CHECK(archive.getNumEntries()==3);CHECK(archive.getEntry(1)->filename=="rack.json");}
    {juce::ZipFile malformed(test::writePackageZip(directory,m,"../outside"));CHECK(malformed.getEntry(1)->filename=="../outside");}
    directory.deleteRecursively();
}
void boundaries(){
    auto m=test::packageFixture();RackPackageManifest out;
    auto text=utf8(test::packageFixtureJson(m));text.append(65536-text.size(),' ');CHECK(decodeRackPackageManifest(text,out));text.push_back(' ');CHECK(!decodeRackPackageManifest(text,out));
    for(const auto& [kind,extension,limit]:std::vector<std::tuple<juce::String,juce::String,std::uint64_t>>{
        {"skin","json",rackPackageLimits::skinBytes},{"notice","txt",rackPackageLimits::skinBytes},
        {"preset","soundstate",maximumSoundStateBytes},{"audio","airasset",20+480000ull*2*4+32},{"drum-bundle","airdrums",drums::maxDrumBundleBytes}}){
        auto candidate=m;const auto path="data/fixture."+extension;candidate.files.push_back({path,juce::String::repeatedString("a",64),kind,limit});if(kind=="skin")candidate.skinPath=path;
        CHECK(decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));candidate.files.back().bytes++;CHECK(!decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));}
    auto counted=m;for(int i=0;i<253;++i)counted.files.push_back({"notices/n"+juce::String(i)+".txt",juce::String::repeatedString("a",64),"notice",1});
    CHECK(decodeRackPackageManifest(utf8(test::packageFixtureJson(counted)),out));counted.files.push_back({"notices/extra.txt",juce::String::repeatedString("a",64),"notice",1});CHECK(!decodeRackPackageManifest(utf8(test::packageFixtureJson(counted)),out));
    auto aggregate=m;aggregate.files.push_back({"data/one.airdrums",juce::String::repeatedString("a",64),"drum-bundle",drums::maxDrumBundleBytes});
    aggregate.files.push_back({"data/two.airdrums",juce::String::repeatedString("a",64),"drum-bundle",100000000});
    for(int i=0;i<3;++i){std::uint64_t used=utf8(test::packageFixtureJson(aggregate)).size();for(std::size_t j=0;j+1<aggregate.files.size();++j)used+=aggregate.files[j].bytes;aggregate.files.back().bytes=rackPackageLimits::expandedBytes-used;}
    CHECK(decodeRackPackageManifest(utf8(test::packageFixtureJson(aggregate)),out));aggregate.files.back().bytes++;CHECK(!decodeRackPackageManifest(utf8(test::packageFixtureJson(aggregate)),out));
    {auto candidate=m;candidate.metadata.tags.clear();for(int i=0;i<8;++i)candidate.metadata.tags.add("tag-"+juce::String(i));CHECK(decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));candidate.metadata.tags.add("tag-8");CHECK(!decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));}
    CHECK(validRackTagId(juce::String::repeatedString("a",32)));CHECK(!validRackTagId(juce::String::repeatedString("a",33)));
    {auto candidate=m;candidate.metadata.name=juce::String::repeatedString(juce::String::charToString(0x03c0),64);CHECK(decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));candidate.metadata.name+=juce::String::charToString(0x03c0);CHECK(!decodeRackPackageManifest(utf8(test::packageFixtureJson(candidate)),out));}
    juce::Array<juce::var> tags;for(int i=0;i<128;++i){auto* row=new juce::DynamicObject;row->setProperty("id","tag-"+juce::String(i));row->setProperty("label",juce::String::repeatedString("a",64));tags.add(juce::var(row));}
    auto* dictionary=new juce::DynamicObject;dictionary->setProperty("schema",1);dictionary->setProperty("tags",tags);juce::var dict(dictionary);std::vector<RackTag> result;
    CHECK(validateRackTagDictionary(utf8(juce::JSON::toString(dict,true)),result));CHECK(result.size()==128);
    dict["tags"].getArray()->add(tags[0].clone());CHECK(!validateRackTagDictionary(utf8(juce::JSON::toString(dict,true)),result));CHECK(result.size()==128);
    const auto image=[](std::uint32_t w,std::uint32_t h){auto bytes=test::packageFixturePng();auto* p=static_cast<unsigned char*>(bytes.getData());
        const auto put=[&](int at,std::uint32_t n){for(int i=0;i<4;++i)p[at+i]=static_cast<unsigned char>(n>>(24-i*8));};put(16,w);put(20,h);
        std::uint32_t crc=0xffffffffu;for(int i=12;i<29;++i){crc^=p[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&std::uint32_t(-int(crc&1)));}put(29,crc^0xffffffffu);return bytes;};
    std::uint32_t width=0,height=0;CHECK(inspectRackPackagePng(span(image(229,5496)),width,height));CHECK(width==229&&height==5496);
    CHECK(inspectRackPackagePng(span(image(8192,2048)),width,height));CHECK(!inspectRackPackagePng(span(image(8193,1)),width,height));CHECK(!inspectRackPackagePng(span(image(8192,2049)),width,height));CHECK(!inspectRackPackagePng(span(image(0,1)),width,height));
    auto png=test::packageFixturePng();std::uint64_t decoded=rackPackageLimits::decodedSkinBytes-4;CHECK(validateRackPackageFileBytes(m.files[1],span(png),nullptr,&decoded));CHECK(decoded==rackPackageLimits::decodedSkinBytes);CHECK(!validateRackPackageFileBytes(m.files[1],span(png),nullptr,&decoded));CHECK(decoded==rackPackageLimits::decodedSkinBytes);
    juce::MemoryBlock sound;CHECK(encodeSoundState(newInstanceSound(),sound));RackPackageFile preset{"presets/default.soundstate",juce::SHA256(sound).toHexString(),"preset",sound.getSize()};CHECK(validateRackPackageFileBytes(preset,span(sound)));
    drums::AssetPtr sample;drums::AssetError error;const std::array<float,2> samples{0.f,.5f};CHECK(drums::makeSampleAsset(samples,1,sample,error));juce::MemoryBlock audio;CHECK(drums::encodeSampleAsset(*sample,audio,error));RackPackageFile owned{"audio/sample.airasset",juce::SHA256(audio).toHexString(),"audio",audio.getSize()};CHECK(validateRackPackageFileBytes(owned,span(audio)));
    const juce::String invalid="not an owned bundle";juce::MemoryBlock blob(invalid.toRawUTF8(),invalid.getNumBytesAsUTF8());RackPackageFile bundle{"audio/default.airdrums",juce::SHA256(blob).toHexString(),"drum-bundle",blob.getSize()};CHECK(!validateRackPackageFileBytes(bundle,span(blob)));
    auto invalidManifest=m;invalidManifest.files[1].kind="executable";juce::String kept="kept";CHECK(!encodeRackPackageManifest(invalidManifest,kept));CHECK(kept=="kept");
}
void frozen(bool cutover=false){
    const auto root=juce::File(RACKSYNTH_PACKAGE_FIXTURE_DIR);const auto manifest=juce::JSON::parse(root.getChildFile("manifest.json"));
    const auto current=airack::test::frozenLibraryRackDesigns();CHECK(current.size()==28);CHECK(manifest["current"].size()==28);CHECK(manifest["packages"].size()==11);
    for(const auto& row:*manifest["current"].getArray()){
        juce::MemoryBlock frozen;CHECK(root.getChildFile(row["path"].toString()).loadFileAsData(frozen));CHECK(juce::SHA256(frozen).toHexString()==row["sha256"].toString());
        const auto it=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==juce::Uuid(row["design"].toString());});CHECK(it!=current.end());if(it==current.end())continue;
        juce::MemoryBlock actual;CHECK(encodeRackDesign(*it,actual,RackValidationMode::instanceRecall));CHECK(actual==frozen);
    }
    const auto historical=test::historicalRackFixtures();CHECK(!historical.empty());
    const auto repo=juce::File(RACKSYNTH_PACKAGE_SOURCE_DIR);
    if(cutover){
        CHECK(factoryRackDesigns().size()==17&&libraryRackDesigns().size()==17);
        for(const auto& retained:factoryRackDesigns()){const auto old=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==retained.id;});CHECK(old!=current.end()&&*old==retained);}
        const auto legacy=juce::JSON::parse(repo.getChildFile("racks/legacy-skins.json"));CHECK(legacy["entries"].size()==11);
        for(const auto& row:*legacy["entries"].getArray()){
            const auto id=row["id"].toString();const auto source=repo.getChildFile("racks/"+id);RackPackageManifest package;
            CHECK(decodeRackPackageManifest(source.getChildFile("package.json").loadFileAsString().toStdString(),package));
            const RackPackageRef exact{id,row["version"].toString(),row["sha256"].toString()};
            juce::MemoryBlock canonical;CHECK(source.getChildFile("rack.json").loadFileAsData(canonical));RackDesign definition;CHECK(decodeRackDesign(canonical.getData(),canonical.getSize(),definition,RackValidationMode::instanceRecall));
            CHECK(legacyRackPackageRef(definition)==exact);CHECK(juce::SHA256(canonical).toHexString()==row["canonical_sha256"].toString());
            CHECK(package.metadata.canonicalSha256==row["canonical_sha256"].toString()&&package.metadata.design==definition.id&&package.metadata.revision==definition.revision);
            const auto frozen=std::find_if(current.begin(),current.end(),[&](const auto& d){return d.id==definition.id;});CHECK(frozen!=current.end()&&*frozen==definition);
            juce::FileInputStream archive(repo.getChildFile("build/rack-library-native/releases/"+id+"-"+exact.version+".airrack"));CHECK(archive.openedOk());CHECK(juce::SHA256(archive).toHexString()==exact.sha256);
        }
    }
    if(!cutover)for(const auto& row:*manifest["historical"].getArray()){juce::MemoryBlock copy,original;CHECK(root.getChildFile(row["path"].toString()).loadFileAsData(copy));CHECK(repo.getChildFile(row["source"].toString()).loadFileAsData(original));CHECK(copy==original);CHECK(juce::SHA256(copy).toHexString()==row["sha256"].toString());}
    // These paths are the original producer bytes, retained independently of current rack metadata and checkout line endings.
    const std::set<juce::String> historicalInputs{
        "src/racks/RackLibrary.cpp",
        "src/state/ParameterRegistry.cpp",
        "src/dsp/DspCompatibility.h",
        "resources/maker-skins/band-color/SOURCE.json",
        "resources/maker-skins/character-drive/SOURCE.json",
        "resources/maker-skins/ember-reed/SOURCE.json",
        "resources/maker-skins/glass-current/SOURCE.json",
        "resources/maker-skins/lumen-field/SOURCE.json",
        "resources/maker-skins/night-dial/SOURCE.json",
        "resources/maker-skins/oxide-return/SOURCE.json",
        "resources/maker-skins/small-hours/SOURCE.json",
        "resources/maker-skins/thread-press/SOURCE.json",
        "resources/maker-skins/tile-array/SOURCE.json",
        "resources/maker-skins/tile-array/tile-array-elements.json",
    };
    const auto historicalFile=[&](const juce::String& path){return historicalInputs.count(path)?root.getChildFile("historical-byte-inputs").getChildFile(path):repo.getChildFile(path);};
    for(const auto& row:*manifest["sources"].getArray()){
        // Task4 adds metadata validators; retain the real original producer bytes and canonical-output checks above.
        const auto path=row["path"].toString()=="src/racks/RackDesign.cpp"?"tests/fixtures/rack-package-recall/RackDesign.cpp.frozen":row["path"].toString()=="src/racks/RackExamples.cpp"?"tests/fixtures/rack-library-product/v1/RackExamples.cpp.frozen":row["path"].toString();
        juce::MemoryBlock bytes;CHECK(historicalFile(path).loadFileAsData(bytes));CHECK(juce::SHA256(bytes).toHexString()==row["sha256"].toString());
    }
    if(!cutover)for(const auto& row:*manifest["assets"].getArray()){juce::MemoryBlock bytes;CHECK(historicalFile(row["path"].toString()).loadFileAsData(bytes));CHECK(bytes.getSize()==std::size_t(juce::int64(row["bytes"])));CHECK(juce::SHA256(bytes).toHexString()==row["sha256"].toString());}
}
}
int main(int argc,char** argv){const bool cutover=argc==2&&juce::String(argv[1])=="--cutover";if(argc!=1&&!cutover)return 2;try{manifests();content();boundaries();frozen(cutover);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
