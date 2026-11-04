#pragma once
#include "ProjectDocument.h"
#include "RackLibrary.h"
#include "RackInventory.h"

namespace airack::ai
{
enum class CatalogRendering : std::uint8_t { companionPcm,ownedLocal };
struct CatalogAuthority;
bool catalogFor(const ProjectDocument&,CatalogRendering,juce::var&);
bool catalogSliceFor(const ProjectDocument&,CatalogRendering,std::size_t first,std::size_t count,juce::var&);
bool catalogInventoryFor(const RackCatalogSnapshot&,CatalogRendering,juce::var&);
bool catalogInventoryFor(const RackInventorySnapshot&,CatalogRendering,juce::var&);
bool providerCatalogFor(const ProjectDocument&,CatalogRendering,juce::var&);
bool providerCatalogFor(const ProjectDocument&,const RackCatalogSnapshot&,CatalogRendering,juce::var&);
bool providerCatalogFor(const ProjectDocument&,const RackInventorySnapshot&,CatalogRendering,juce::var&);
bool providerCatalogFor(const ProjectDocument&,const CatalogAuthority&,CatalogRendering,juce::var&);
bool catalogFor(const ProjectDocument&, juce::var& output);
bool catalogInventoryFor(const RackCatalogSnapshot&, juce::var& output);
bool providerCatalogFor(const ProjectDocument&, juce::var& output);
bool providerCatalogFor(const ProjectDocument&, const RackCatalogSnapshot&, juce::var& output);
}
