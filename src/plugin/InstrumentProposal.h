#pragma once
#include "ProposalDocument.h"
#include "McpProtocol.h"
#include "RackLibraryClient.h"
namespace airack {
struct ProposalCommitInput {
    ai::RequestTag tag;
    std::uint64_t structuralGeneration=0,resetGeneration=0;
    ai::ProposalPolicy policy;ai::ProposalEdits edits;
    std::shared_ptr<const ai::CatalogAuthority> authority;
    juce::String reviewedName; // External staging keeps the effective name separate from the strictly validated edits.
    RackInventoryPtr inventory;
    RackLibraryPlacementPreparationPtr placementPreparation;
    std::vector<RackPackageRef> packageRefs;
    std::shared_ptr<const RackLibraryPlacementView> placement;
};
juce::String resolveProposalName(const juce::String&,const std::vector<juce::String>&);
struct ProposalCommitResult {
    mcp::Status status=mcp::Status::unavailable;
    std::uint64_t editRevision=0,historyId=0;
    bool changed=false;
};
}
