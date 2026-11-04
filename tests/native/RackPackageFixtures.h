#pragma once
#include "RackPackage.h"
namespace airack::test {
RackPackageManifest packageFixture(const juce::String& id="ember-reed");
juce::File writePackageZip(const juce::File& directory,const RackPackageManifest&,const juce::String& replacementEntry={});
std::vector<RackDesign> historicalRackFixtures();
juce::MemoryBlock packageFixtureDesign();
juce::MemoryBlock packageFixturePng();
juce::String packageFixtureJson(const RackPackageManifest&);
}
