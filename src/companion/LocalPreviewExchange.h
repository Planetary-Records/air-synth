#pragma once
#include "LocalPreview.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace airack::companion
{
// The existing IO owner services this mailbox while the one backend worker waits.
class LocalPreviewExchange
{
public:
    LocalPreviewExchange(std::array<std::uint64_t,3>,std::chrono::steady_clock::time_point);
    ai::PreviewReceipt evaluate(const ai::PreviewEvaluate&,const ai::CancelCheck&);
    bool takeEvaluation(ai::PreviewEvaluate&);
    bool acceptReceipt(const ai::PreviewReceipt&);
    void cancel() noexcept;
    bool lastReceipt(ai::PreviewReceipt&) const;
private:
    const std::array<std::uint64_t,3> tickets;
    const std::chrono::steady_clock::time_point deadline;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::optional<ai::PreviewEvaluate> offered;
    std::optional<ai::PreviewReceipt> receipt,last;
    std::size_t next=0;
    bool taken=false,cancelled=false;
};
}
