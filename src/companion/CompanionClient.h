#pragma once
#include "CompanionLaunch.h"
#include <chrono>
#include <functional>
#include <variant>

namespace airack::companion
{
inline constexpr std::uint64_t clientStartupTimeoutMs = 5000;
inline constexpr std::uint64_t clientOperationTimeoutMs = 360000;
inline constexpr std::uint64_t clientGroupOperationTimeoutMs = 5000;
inline constexpr unsigned clientPollBudget = 4;
using ClientClock = std::function<std::chrono::steady_clock::time_point()>;
struct ClientOptions { LaunchOptions launch; bool allowLaunch=true,negotiateSpectrum=true,negotiateOwnedPreview=true; };
enum class ClientPhase { closed, connecting, ready, failed };
struct ClientStatus
{
    ClientPhase phase = ClientPhase::closed;
    RuntimeStatus status = RuntimeStatus::unavailable;
    juce::String errorCode;
};
using ClientGenerationReply = std::variant<ai::GenerationResult, ControlMessage, ai::LocalGenerationResult>;
namespace detail { struct CompanionClientTestAccess; }
// All methods belong to one caller-owned control/message thread, never audio.
// Replies carry captured authority only; a consumer must recheck local revisions.
class CompanionClient
{
public:
    explicit CompanionClient(juce::Uuid runtime = juce::Uuid(), ClientClock clock = {});
    ~CompanionClient();
    CompanionClient(const CompanionClient&) = delete;
    CompanionClient& operator=(const CompanionClient&) = delete;
    CompanionClient(CompanionClient&&) = delete;
    CompanionClient& operator=(CompanionClient&&) = delete;
    juce::Uuid runtime() const noexcept;
    juce::Uuid session() const noexcept;
    PipePeer peer() const noexcept;
    ClientStatus status() const;
    std::uint32_t capabilities() const noexcept;
    bool start(const ClientOptions& = {});
    void poll();
    bool generate(const ai::GenerationRequest&);
    bool generateLocal(const ai::LocalGenerationRequest&);
    bool generateGroupLocal(const ai::LocalGroupGenerationCommand&);
    bool takePreviewEvaluation(ai::PreviewEvaluate&);
    bool submitPreviewReceipt(const ai::PreviewReceipt&);
    bool groupLocal(const GroupCommand&,const ai::PreviewSampleMap&);
    bool group(const GroupCommand&);
    bool generateGroup(const GroupGenerationCommand&);
    bool account(const AccountCommand&);
    void cancelGeneration() noexcept;
    void cancelAccount() noexcept;
    bool takeGeneration(ClientGenerationReply&);
    bool takeAccount(AccountReply&);
    bool takeGroup(GroupReply&);
    void close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend struct detail::CompanionClientTestAccess;
};
namespace detail
{
struct CompanionClientTestAccess
{
    static bool hasStartedWrite(const CompanionClient&) noexcept;
    static bool hasGroupReply(const CompanionClient&) noexcept;
    static bool hasAccountReply(const CompanionClient&) noexcept;
    static void withholdCompletion(CompanionClient&,bool);
    static void* borrowProcessHandle(const CompanionClient&) noexcept;
};
}
}
