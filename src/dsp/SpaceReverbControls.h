#pragma once
#include "SpaceReverbModel.h"
#include <complex>
#include <cstddef>
#include <span>
namespace airack::space {
enum class PostShape {bell,lowShelf,highShelf,lowCut,highCut};
enum class CutSlope {db12,db24};
struct PostBand {
    bool active=false,bypassed=false;
    PostShape shape=PostShape::bell;
    double frequencyHz=1000,gainDb=0,bellQ=1,shelfQ=1;
    CutSlope slope=CutSlope::db12;
    bool operator==(const PostBand&) const = default;
};
struct Settings {
    DecayCurve decay;
    double size=.5,predelayMs=20,character=.25,distance=.5,width=1,mix=.25;
    double duckRangeDb=0,duckThresholdDb=-24,duckAttackMs=10,duckReleaseMs=250;
    std::array<PostBand,6> post {};
    bool operator==(const Settings&) const = default;
};
bool valid(const Settings&) noexcept;
struct PostCoefficients {
    double b0=1,b1=0,b2=0,a1=0,a2=0;
    bool operator==(const PostCoefficients&) const = default;
};
struct PreparedPostEq {
    std::array<PostCoefficients,12> stages {};
    std::array<std::size_t,6> starts {},counts {};
    std::array<double,6> effectiveHz {};
    std::size_t count=0;
    double sampleRate=0;
    bool operator==(const PreparedPostEq&) const = default;
};
// Off-audio, transactional preparation. All retained settings are validated.
bool preparePostEq(const Settings&,double sampleRate,PreparedPostEq&) noexcept;
std::optional<std::complex<double>> postEqResponse(const PreparedPostEq&,double frequencyHz,
    std::optional<std::size_t> band=std::nullopt) noexcept;
class PostEq {
public:
    bool prepare(const PreparedPostEq&) noexcept;
    void reset() noexcept;
    std::array<double,2> tick(std::array<double,2>) noexcept;
    // Prepared blocks <=512 frames; invalid readiness/length preserves state/input.
    bool processBlock(std::span<std::array<double,2>>) noexcept;
    bool operator==(const PostEq&) const = default;
private:
    struct History {double z1=0,z2=0;bool operator==(const History&) const = default;};
    PreparedPostEq prepared_;
    std::array<std::array<History,12>,2> history_ {};
    bool ready_=false;
};
struct PreparedDuck {
    double attack=0,release=0,thresholdDb=-24,rangeDb=0,sampleRate=0;
    bool operator==(const PreparedDuck&) const = default;
};
bool prepareDuck(const Settings&,double sampleRate,PreparedDuck&) noexcept;
class DuckDetector {
public:
    bool prepare(const PreparedDuck&) noexcept;
    void reset() noexcept;
    double tick(std::array<double,2> input) noexcept;
    double envelope() const noexcept {return envelope_;}
    double attenuationDb() const noexcept {return attenuationDb_;}
    bool operator==(const DuckDetector&) const = default;
private:
    PreparedDuck prepared_;
    double envelope_=0,attenuationDb_=0;
    bool ready_=false;
};
}
