#pragma once
#include <array>
#include <cstdint>

namespace wide_direct_layout {
using Frame = std::array<double, 2>;
using Quad = std::array<double, 4>;
using InputBlock = std::array<Frame, 4>;
// b0,b1,b2,a1,a2; network order lowLP,lowHP,highLP,highHP,highAP.
using Coefficients = std::array<double, 5>;
using Network = std::array<Coefficients, 5>;
// lowLP[0,1],lowHP[0,1],highLP[0,1],highHP[0,1],lowCompensation;
// each state contributes z1[0..3], then z2[0..3].
using History = std::array<double, 72>;
struct LegacyResult { std::array<std::array<Quad, 3>, 4> bands; History history; };
struct DirectResult { std::array<InputBlock, 3> wet; std::array<std::uint64_t, 2> afterWet; std::array<std::array<Frame, 3>, 4> dry; std::array<std::uint64_t, 2> afterDry; History history; };

bool avxAdmitted() noexcept;
void* createOriginal();
void* createProposed();
void destroyOriginal(void*) noexcept;
void destroyProposed(void*) noexcept;
void clearOriginal(void*) noexcept;
void clearProposed(void*) noexcept;
void seedOriginal(void*, const History&) noexcept;
void seedProposed(void*, const History&) noexcept;
void runOriginal(void*, const Network&, const InputBlock& wet, const InputBlock& dry, LegacyResult&) noexcept;
void runProposed(void*, const Network&, const InputBlock& wet, const InputBlock& dry, DirectResult&) noexcept;
}