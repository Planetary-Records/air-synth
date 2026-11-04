#pragma once
#if defined(AIRACK_MODAL_STAGE_DIAGNOSTIC)
#include <array>
#include <chrono>
#include <cstdint>
namespace airack::space::probe {
inline constexpr std::size_t modalServiceStageCount=10;
struct ModalServiceStageSample {std::array<std::uint64_t,modalServiceStageCount> ns{},calls{};};
inline thread_local ModalServiceStageSample* modalServiceStageSample=nullptr;
template<class Fn> decltype(auto) modalServiceStage(unsigned index,Fn&& fn) noexcept {
    if(!modalServiceStageSample)return fn();
    struct Record {unsigned index;std::chrono::steady_clock::time_point start;~Record(){auto& s=*modalServiceStageSample;s.ns[index]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());++s.calls[index];}} record{index,std::chrono::steady_clock::now()};
    return fn();
}
}
#define AIRACK_MODAL_STAGE(index,expression) modalServiceStage(index,[&]() noexcept -> decltype(auto) {return (expression);})
#else
#define AIRACK_MODAL_STAGE(index,expression) (expression)
#endif
