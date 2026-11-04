#pragma once
#include "RackExamples.h"
#include <stdexcept>

// Historical test inputs are data fixtures, never linked shipping maker registrations.
namespace airack::test {
inline const std::vector<RackDesign>& frozenCurrentRackDesigns() {
    static const auto definitions=[] {
        const auto root=juce::File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("fixtures/rack-library/v1");
        const auto manifest=juce::JSON::parse(root.getChildFile("manifest.json"));std::vector<RackDesign> result;
        if(const auto* rows=manifest["current"].getArray())for(const auto& row:*rows){
            juce::MemoryBlock bytes;RackDesign design;
            if(!root.getChildFile(row["path"].toString()).loadFileAsData(bytes)||!decodeRackDesign(bytes.getData(),bytes.getSize(),design,RackValidationMode::instanceRecall))throw std::runtime_error("frozen current rack fixture");
            result.push_back(std::move(design));
        }
        if(result.size()!=28)throw std::runtime_error("frozen28 rack fixture count");return result;
    }();return definitions;
}
inline std::array<RackDesign,6> frozenExampleRackDesigns(){const auto& d=frozenCurrentRackDesigns();return {d[0],d[1],d[2],d[3],d[4],d[5]};}
inline std::vector<RackDesign> frozenFactoryRackDesigns(){const auto& d=frozenCurrentRackDesigns();return {d.begin()+6,d.end()-1};}
inline std::vector<RackDesign> frozenLibraryRackDesigns(){return frozenCurrentRackDesigns();}
inline RackDesign frozenTileArrayRackDesign(){return frozenCurrentRackDesigns().back();}
inline juce::MemoryBlock frozenMakerSkinFile(const juce::String& skin,const juce::String& path){
    const auto root=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("resources/maker-skins/"+skin);
    juce::MemoryBlock bytes;root.getChildFile(path=="elements.json"?skin+"-elements.json":path).loadFileAsData(bytes);return bytes;
}
}
