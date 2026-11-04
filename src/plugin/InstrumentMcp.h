#pragma once
#include "McpEndpoint.h"
#include "InstrumentProposal.h"
#include "DrumOwnership.h"
namespace airack {
struct McpView {juce::Uuid instance,epoch;bool enabled=true,connected=false,clientsAvailable=false;unsigned clients=0;std::uint64_t requests=0;juce::String label,status,lastEdit;};
struct McpState {
    struct Preview;
    std::shared_ptr<Preview> preview;
    McpState();~McpState();
    mcp::Endpoint endpoint;
    struct PendingReply {mcp::Reply reply;mcp::Caller caller;};
    // Endpoint::take reserves its maximum result bytes until publication or lifecycle retirement.
    std::optional<PendingReply> pendingReply;
    std::array<std::optional<PendingReply>,16> priorityReplies;
    std::atomic_bool admission{true};
    std::optional<mcp::ServiceOptions> options;
    struct Binding {
        juce::Uuid token=juce::Uuid::null(),epoch=juce::Uuid::null();
        std::uint64_t edit=0,context=0,structure=0,reset=0,source=0;
        juce::String content;
        RackInventoryPtr inventory;
    } snapshot;
    struct Cursor {juce::Uuid id=juce::Uuid::null(),snapshot=juce::Uuid::null();juce::String kind;std::size_t next=0;};
    std::optional<Cursor> cursor;
    struct Stage {
        juce::Uuid id=juce::Uuid::null();mcp::Caller caller;std::uint64_t touched=0,serial=0;
        Binding binding;ProposalCommitInput input;
        std::unique_ptr<ProjectDocument> candidate;
        std::shared_ptr<const drums::DrumOwnership::McpImage> image;
        juce::var diff;
    };
    std::shared_ptr<Stage> stage;
    struct Receipt {
        juce::Uuid edit=juce::Uuid::null();mcp::Caller caller;
        std::uint64_t touched=0,historyId=0,revision=0;bool changed=false;
    };
    std::array<std::optional<Receipt>,64> receipts;
    std::size_t nextReceipt=0;
    juce::String lastEdit;
    std::uint64_t lastEditHistory=0,lastEditRevision=0;
};
}
