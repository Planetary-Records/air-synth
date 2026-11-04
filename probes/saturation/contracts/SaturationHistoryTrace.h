#pragma once
#include "SaturationReturnOracle.h"
#include <filesystem>
#include <fstream>
#include <memory>

namespace airack::probe::history_trace {
using Frame = Candidate::Frame;
void endpoint(unsigned mode, std::array<const void*, 4> lanes, Frame input, Frame preZ1, Frame preZ2,
              Frame postZ1, Frame postZ2) noexcept;
void pre(Frame filtered, Frame z1, Frame z2, bool enabled) noexcept;
void up(const Frame* wet, const Frame* dry, unsigned factor) noexcept;
void post(unsigned n, Frame before, Frame filtered, Frame z1, Frame z2, bool enabled) noexcept;
void lane(const void* identity, unsigned ch, unsigned character, double x, double h, double u, double previous,
          double delta, double currentPrimitive, double priorPrimitive, double shaped, double cachePrimitive,
          unsigned cacheKind, unsigned kind, unsigned cacheCharacter, double previousX, double previousH,
          double previousDifference, double dc, double envelope) noexcept;
// Current observations distinguish source-omitted primitives and vector operands from member caches.
enum class CurrentRoute : unsigned { scalar4, scalarFour, scalarGeneric, stereoPair, temporal4 };
enum class PriorSource : unsigned { unused, memberCache, evaluated, predecessorVector };
struct CurrentLaneInfo {
    CurrentRoute route;
    PriorSource priorSource;
    bool primitivesMaterialized;
    unsigned temporalFrame = 0;
};
void current_endpoint(unsigned mode, std::array<const void*, 4> lanes, Frame input, Frame preZ1, Frame preZ2,
                      Frame postZ1, Frame postZ2, bool prefixReused) noexcept;
void current_lane(const void* identity, unsigned ch, unsigned character, double x, double h, double u, double previous,
                  double delta, double currentPrimitive, double priorPrimitive, double shaped, double cachePrimitive,
                  unsigned cacheKind, unsigned kind, unsigned cacheCharacter, double previousX, double previousH,
                  double previousDifference, double dc, double envelope, CurrentLaneInfo info) noexcept;
// Current-source generated hooks only. Historical generation never calls these.
void cells_endpoint(unsigned mode) noexcept;
void network(const void* identity, std::array<const void*, 9> cells) noexcept;
void cell_before(const void* identity, std::array<double, 5> coefficients, Frame input, Frame z1, Frame z2) noexcept;
void cell_after(const void* identity, Frame output, Frame z1, Frame z2) noexcept;
struct Depth;
// Buffers are fixed before processing. The existing observer only copies values.
struct Session {
    using Frame = Candidate::Frame;
    std::array<Frame, 128> wet {}, dry {};
    unsigned count = 0, factor, fadeLength, frame = 0, returnStart, fadeFrame = 0;
    bool overflow = false, fading = false;
    control_law::ReturnOracle from, to;
    std::ofstream endpointFile, boundaryFile;
    std::unique_ptr<Depth> depth;
    Session(const std::filesystem::path& stem, unsigned factor, unsigned rate, unsigned returnStart, unsigned history);
    ~Session();
    void begin(unsigned frame, bool structuralChange);
    void end(Frame actual);
    void finish();
};
void selfTest();
}
