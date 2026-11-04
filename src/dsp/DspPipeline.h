#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace airack::detail
{
// Three prepared stages borrowed by one render owner. Jobs and their payload
// stay valid until finish(); creation, retirement and preparation are non-RT.
class DspPipeline final
{
public:
    using Work = void (*)(void*, unsigned) noexcept;
    using Entry = void (*)(DspPipeline*, unsigned) noexcept;
    using Launch = std::thread (*)(Entry, DspPipeline*, unsigned);
    DspPipeline() = default;
    ~DspPipeline();
    DspPipeline(const DspPipeline&) = delete;
    DspPipeline& operator=(const DspPipeline&) = delete;
    // The launch seam permits deterministic resource-failure verification.
    bool prepare(void* context, Work, Launch = launchThread) noexcept;
    bool available() const noexcept { return started == 3; }
    unsigned workerCount() const noexcept { return started; }
    void begin() noexcept;
    void finish() const noexcept;
    // Only after all DSP payload is complete: cancel unstarted stages and fence running owners.
    unsigned finishUnstarted() noexcept;
    void publish(unsigned stage, unsigned chunks) noexcept;
    void await(unsigned stage, unsigned chunks) const noexcept;
    static bool renderingOnThisThread() noexcept { return rendering; }
    static std::uint32_t schedulingTaskOnThisThread() noexcept { return schedulingTask; }

private:
    struct alignas(64) Counter { std::atomic<std::uint64_t> value{0}; };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
    static_assert(std::atomic<bool>::is_always_lock_free);
    static std::thread launchThread(Entry, DspPipeline*, unsigned);
    static void entry(DspPipeline*, unsigned) noexcept;
    void stop() noexcept;
    void wake() noexcept;
    std::array<Counter, 3> ready, completed;
    alignas(64) std::atomic<std::uint64_t> requested{0};
    std::atomic<bool> stopping{false};
    std::array<std::thread, 3> threads;
#if defined(_WIN32)
    std::array<std::uintptr_t, 3> wakeSignals{};
#endif
    std::array<std::atomic<int>,3> startup{};
    void* context = nullptr;
    Work work = nullptr;
    std::uint64_t current = 0;
    unsigned started = 0;
    bool priming = false;
    std::uint32_t floatingPointMode = 0;
    std::uint32_t sharedTask = 0;
    inline static thread_local bool rendering = false;
    inline static thread_local std::uint32_t schedulingTask = 0;
};
}
