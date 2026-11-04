#pragma once
#include "GenerationRequest.h"
#include "CompanionClient.h"
#include <optional>
#include <string>

namespace airack
{
struct AiMusicalContext
{
    std::string role, key, scale;
    std::optional<double> auditionBpm;
    bool operator==(const AiMusicalContext&) const = default;
};
struct AiRequestOptions
{
    std::string instruction;
    juce::String model;
    ai::ProposalPolicy policy;
};
enum class AiProposalPhase { idle, pending, proposed, stale, failed };
struct AiWorkflowStatus
{
    companion::ClientStatus connection;
    // refining: the request that runs or waits for the group is a Refine.
    bool accountPending=false,applyPending=false,generationWaiting=false,refining=false;
    juce::String message;
};
}
