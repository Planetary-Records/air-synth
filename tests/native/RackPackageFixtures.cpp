#include "RackPackageFixtures.h"
#include <juce_cryptography/juce_cryptography.h>
#include <set>
#include <stdexcept>

namespace airack::test {
namespace {
juce::var object(std::initializer_list<std::pair<juce::Identifier,juce::var>> fields) {
    auto* result=new juce::DynamicObject; for(const auto& [key,value]:fields)result->setProperty(key,value); return result;
}
juce::File fixtures() { return juce::File(RACKSYNTH_PACKAGE_FIXTURE_DIR); }
}
juce::MemoryBlock packageFixtureDesign() {
    juce::MemoryBlock bytes;
    if(!fixtures().getChildFile("current/c83dab6a1c4e48c183c480d9212ca601-r1.json").loadFileAsData(bytes))
        throw std::runtime_error("missing frozen package recipe");
    return bytes;
}
juce::MemoryBlock packageFixturePng() {
    // A valid 1x1 fixture PNG; never a release screenshot.
    constexpr unsigned char png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,96,96,96,96,0,0,0,5,0,1,165,246,69,64,0,0,0,0,73,69,78,68,174,66,96,130};
    return juce::MemoryBlock(png,sizeof(png));
}
RackPackageManifest packageFixture(const juce::String& id) {
    const auto bytes=packageFixtureDesign(),png=packageFixturePng(); RackDesign design;
    if(!decodeRackDesign(bytes.getData(),bytes.getSize(),design,RackValidationMode::instanceRecall))throw std::runtime_error("invalid frozen recipe");
    RackPackageManifest result; result.ref={id,"1.0.0",{}};
    result.metadata={design.id,design.revision,juce::SHA256(bytes).toHexString(),design.name,"Fixture Maker","Native fixture","Package boundary fixture",design.role,{"analog","warm"}};
    result.designPath="rack.json";result.screenshotPath="screenshot.png";
    result.files={{"rack.json",juce::SHA256(bytes).toHexString(),"design",bytes.getSize()},
        {"screenshot.png",juce::SHA256(png).toHexString(),"png",png.getSize()}};
    juce::Array<juce::var> modules; std::set<std::pair<int,int>> seen;
    for(const auto& unit:design.units)if(seen.emplace(int(unit.module),int(unit.algorithmRevision)).second)
        modules.add(object({{"module",int(unit.module)},{"algorithmRevision",int(unit.algorithmRevision)}}));
    result.capabilities=object({{"airMin","0.1.0"},{"modules",modules},{"nativeEditor",""}});
    return result;
}
// Independent test serialization: it never calls the manifest parser/encoder under test.
juce::String packageFixtureJson(const RackPackageManifest& m) {
    juce::Array<juce::var> tags,files;for(const auto& tag:m.metadata.tags)tags.add(tag);
    for(const auto& file:m.files)files.add(object({{"path",file.path},{"sha256",file.sha256},{"kind",file.kind},{"bytes",juce::int64(file.bytes)}}));
    return juce::JSON::toString(object({{"schema",1},{"id",m.ref.id},{"version",m.ref.version},
        {"metadata",object({{"design",m.metadata.design.toDashedString()},{"revision",juce::String(std::to_string(m.metadata.revision))},
            {"canonical_sha256",m.metadata.canonicalSha256},{"name",m.metadata.name},{"maker",m.metadata.maker},
            {"purpose",m.metadata.purpose},{"description",m.metadata.description},{"role",m.metadata.role==RackRole::synth?"synth":"fx"},{"tags",tags}})},
        {"design",m.designPath},{"skin",m.skinPath},{"screenshot",m.screenshotPath},{"files",files},{"capabilities",m.capabilities}}),true);
}
juce::File writePackageZip(const juce::File& directory,const RackPackageManifest& m,const juce::String& replacementEntry) {
    if(directory.createDirectory().failed())throw std::runtime_error("fixture directory");
    const auto output=directory.getChildFile(m.ref.id+"-"+m.ref.version+".zip");output.deleteFile();
    juce::ZipFile::Builder zip;const auto manifest=packageFixtureJson(m);const juce::Time stamp(1980,0,1,0,0);
    zip.addEntry(new juce::MemoryInputStream(manifest.toRawUTF8(),manifest.getNumBytesAsUTF8(),true),0,"package.json",stamp);
    bool first=true;
    for(const auto& file:m.files){const auto bytes=file.kind=="design"?packageFixtureDesign():packageFixturePng();
        zip.addEntry(new juce::MemoryInputStream(bytes,true),0,first&&replacementEntry.isNotEmpty()?replacementEntry:file.path,stamp);first=false;}
    auto stream=output.createOutputStream();if(!stream||!zip.writeToStream(*stream,nullptr))throw std::runtime_error("fixture zip");
    return output;
}
std::vector<RackDesign> historicalRackFixtures() {
    std::vector<RackDesign> result;auto files=fixtures().getChildFile("historical").findChildFiles(juce::File::findFiles,false,"*.json");files.sort();
    for(const auto& file:files){juce::MemoryBlock bytes;RackDesign design;
        if(!file.loadFileAsData(bytes)||!decodeRackDesign(bytes.getData(),bytes.getSize(),design,RackValidationMode::instanceRecall))throw std::runtime_error("historical fixture");
        result.push_back(std::move(design));}
    return result;
}
}
