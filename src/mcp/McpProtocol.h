#pragma once
#include <juce_core/juce_core.h>
#include <string_view>
#include <optional>
#include <atomic>
#include <memory>

namespace airack::mcp
{
inline constexpr std::size_t maximumRequestBytes = 1048576, maximumResultBytes = 8388608;
inline constexpr auto protocolVersion = "2025-11-25";
enum class Tool { listInstances, getState, getCatalog, prepareEdit, getEdit,
    applyEdit, discardEdit, renderPreview, getJob, cancelJob,
    playPreview, stopPreview, undoEdit, redoEdit, resourceList, resourceRead };
inline bool priorityTool(Tool t) noexcept {return t==Tool::cancelJob||t==Tool::stopPreview;}
enum class Status { ready, busy, stale, invalidInput, unsupported, unauthorized,
    unavailable, instanceGone, expired, capacity, hostControlUnavailable,
    audioUnavailable, cancelled, failed };
struct RpcMessage { juce::var id; juce::String method; juce::var params; bool notification=false; };
struct RpcError { int code=0; juce::String message; };
struct ToolCall { Tool tool=Tool::listInstances; juce::Uuid instance{juce::Uuid::null()}; juce::var arguments; };
struct ToolResult { Status status=Status::unavailable; juce::var data;
    std::shared_ptr<const void> resourceLifetime;
    std::shared_ptr<const std::atomic_bool> resourceRevoked;
};
bool decodeRpc(std::string_view, RpcMessage&, RpcError&);
bool decodeIpcCall(const juce::var&, ToolCall&, RpcError&);
bool previewResourceTarget(const juce::var&,juce::Uuid& instance,juce::Uuid& epoch,juce::Uuid& resource);
bool decodeToolCall(const juce::var& params, ToolCall&, RpcError&);
const char* toolName(Tool) noexcept;
juce::var toolDefinitions();
juce::String encodeJson(const juce::var&);
std::optional<std::size_t> jsonSize(const juce::var&,std::size_t cap);
// A resource with empty text was refused; non-resources retain the generic encoder.
struct BinaryJsonEncoding { bool resource=false; juce::String text; };
BinaryJsonEncoding encodeBinaryJson(const juce::var&,std::size_t cap);
juce::String rpcSuccess(const juce::var& id, const juce::var& result);
juce::String rpcFailure(const juce::var& id, const RpcError&);
juce::var toolResult(const ToolResult&);
bool validRpcId(const juce::var&);
juce::Uuid newRuntimeId();
namespace ipc {std::optional<juce::String> environment(const juce::String&);}
}
