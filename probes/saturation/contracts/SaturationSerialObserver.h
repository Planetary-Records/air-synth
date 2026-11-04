#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Private generated observer only. No arithmetic or numerical-test substitution.
namespace airack::probe::serial_observer {
using Frame = std::array<double, 2>;
struct Record {
    int partition, frame, slot, stage, order, channel, lane, subsample, call, kind, selector, object, flags, ramp;
    std::array<std::uint64_t, 18> words {};
    unsigned count = 0;
};
inline std::vector<Record> records;
inline constexpr std::size_t recordLimit = 2000000;
inline int partition = 0, currentFrame = -1, slot = -1, lane = -1, subsample = -1;
inline bool failed = false, resourcesRecorded = false;
inline const char* expectedSource = nullptr;
inline void compiledSource(const char* source) noexcept {
    if (expectedSource == nullptr || std::strcmp(source,expectedSource) != 0) failed = true;
}
inline std::array<const void*, 8> candidates {};
inline std::array<const void*, 4> lanes {};
inline std::array<const void*, 28> filters {};
inline Frame downValue {};
inline double shapeArgument = 0;
inline std::array<double,4> exactArguments {};
using CallKey = std::tuple<int,int,int,int,int>;
inline std::map<CallKey, int> calls;
inline bool active() noexcept { return partition == 1 && currentFrame <= 510; }
inline void emit(int stage, std::initializer_list<double> values, int channel = -1,
                 int kind = 0, int selector = 0, int object = -1, int flags = 0, int ramp = 0) noexcept {
    if (!active() || failed) return;
    try {
        if (records.size() >= recordLimit || values.size() > 18) { failed = true; return; }
        Record r { partition,currentFrame,slot,stage,int(records.size()),channel,lane,subsample,
            calls[{stage,channel,lane,subsample,object}]++,kind,selector,object,flags,ramp };
        for (const auto value : values) {
            if (!std::isfinite(value)) { failed = true; return; }
            r.words[r.count++] = std::bit_cast<std::uint64_t>(value);
        }
        records.push_back(r);
    } catch (...) { failed = true; }
}
inline void start(int value) {
    partition = value; currentFrame = -1; slot = lane = subsample = -1; calls.clear();
    if (value == 1) { records.reserve(recordLimit); resourcesRecorded = false; }
}
template<class Bank> void bank(Bank& value) noexcept {
    for (std::size_t n = 0; n < 8; ++n) candidates[n] = &value[n];
}
inline void candidate(const void* object) noexcept {
    if (!active()) return;
    int next = -1;
    for (int n = 0; n < 8; ++n) if (candidates[n] == object) next = n;
    if (next < 0) { failed = true; return; }
    if (slot != next) calls.clear();
    slot = next; lane = subsample = -1;
}
inline void frame(int value) noexcept {
    currentFrame = value; slot = lane = subsample = -1; calls.clear(); emit(0, {});
}
inline void event(int eventSlot, int operation, bool bypassed) noexcept {
    slot = eventSlot; lane = subsample = -1; calls.clear();
    emit(1, {}, -1, bypassed ? 1 : 0, operation);
}
inline void point(int sample) noexcept { lane = -1; subsample = sample; }
inline void frameValue(int stage, Frame value) noexcept {
    lane = subsample = -1; emit(stage, {value[0],value[1]});
}
template<class State> void bind(State& s) noexcept {
    for (int n = 0; n < 4; ++n) lanes[n] = &s.lanes[n];
    filters[0] = &s.preCut; filters[1] = &s.postCut;
    for (int n = 0; n < 4; ++n) { filters[2+2*n] = &s.lanes[n].lowShelf; filters[3+2*n] = &s.lanes[n].highShelf; }
    const auto network = [](auto& net, int offset) {
        for (int n = 0; n < 2; ++n) {
            filters[offset+n] = &net.low.low[n]; filters[offset+2+n] = &net.low.high[n];
            filters[offset+4+n] = &net.high.low[n]; filters[offset+6+n] = &net.high.high[n];
        }
        filters[offset+8] = &net.lowCompensation;
    };
    network(s.wetNetwork,10); network(s.dryNetwork,19);
}
template<class Coefficients> void coefficient(const Coefficients& c, int object) noexcept {
    emit(5, {c.b0,c.b1,c.b2,c.a1,c.a2}, -1, 0, 0, object);
}
template<class State> void state(const State& s) noexcept {
    if (!active()) return;
    lane = subsample = -1;
    const int flags = int(s.useWideNetwork) | (int(s.current.preLowCutEnabled)<<1)
        | (int(s.current.postHighCutEnabled)<<2) | (int(s.current.driveCompensation)<<3);
    emit(3, {s.rate,s.highRate,s.dcAlpha,s.sagAttack,s.sagRelease,s.inputGain,s.outputGain,
        s.targetLogHz[0],s.targetLogHz[1],s.targetLogHz[2],s.targetLogHz[3],
        s.current.inputTrimDb,s.current.outputTrimDb,s.current.mix,s.current.lowCrossoverHz,
        s.current.highCrossoverHz,s.current.preLowCutHz,s.current.postHighCutHz},
        -1,int(s.current.mode),0,-1,flags,int(s.ramp));
    emit(16,{s.target.inputTrimDb,s.target.outputTrimDb,s.target.mix,s.target.lowCrossoverHz,
        s.target.highCrossoverHz,s.target.preLowCutHz,s.target.postHighCutHz},-1,int(s.target.mode),0,-1,
        int(s.target.preLowCutEnabled)|(int(s.target.postHighCutEnabled)<<1)|(int(s.target.driveCompensation)<<2));
    coefficient(s.pre,0); coefficient(s.post,1); coefficient(s.network.lowLP,2);
    coefficient(s.network.lowHP,3); coefficient(s.network.highLP,4); coefficient(s.network.highHP,5);
    coefficient(s.network.highAP,6);
    for (int n = 0; n < 4; ++n) {
        const auto& c = s.current.lanes[n]; const auto& t = s.target.lanes[n];
        lane = n;
        emit(4,{c.driveDb,c.preTiltDb,c.mix,c.trimDb,t.driveDb,t.preTiltDb,t.mix,t.trimDb},
            -1,int(c.character),int(t.character),-1,int(c.bypassed)|(int(t.bypassed)<<1));
        emit(6,{s.driveGain[n],s.driveAmount[n],s.wetGain[n]});
        coefficient(s.shelfLow[n],7+2*n); coefficient(s.shelfHigh[n],8+2*n);
    }
    lane = -1;
}
template<class State> void ready(const State& s, double logTwo, double sqrtTwo, double bias, double scale, double pi, double gainLogScale) noexcept {
    emit(2,{logTwo,sqrtTwo,bias,scale,pi,gainLogScale}); state(s);
    if (resourcesRecorded || !active()) return;
    resourcesRecorded = true;
    int object = 0;
    for (const auto& view : s.coefficients->manifest) {
        subsample = int(view.values.size()); emit(15,{},-1,0,0,object);
        int n = 0;
        for (const auto value : view.values) { subsample = n++; emit(7,{value},-1,0,0,object); }
        ++object;
    }
    subsample = -1;
}
inline int filterId(const void* object) noexcept {
    for (int n = 0; n < 28; ++n) if (filters[n] == object) return n;
    failed = active(); return -1;
}
template<class C> void biquadIn(const void* object, Frame x, const C& c, Frame z1, Frame z2) noexcept {
    if (!active()) return;
    const auto id = filterId(object);
    for (int ch = 0; ch < 2; ++ch) emit(20,{x[ch],c.b0,c.b1,c.b2,c.a1,c.a2,z1[ch],z2[ch]},ch,0,0,id);
}
inline void biquadOut(const void* object, Frame y, Frame z1, Frame z2) noexcept {
    if (!active()) return;
    const auto id = filterId(object);
    for (int ch = 0; ch < 2; ++ch) emit(21,{y[ch],z1[ch],z2[ch]},ch,0,0,id);
}
inline void pre(Frame input, Frame wet) noexcept { emit(10,{input[0],input[1],wet[0],wet[1]}); }
template<class State> void wideState(const State& s, int stage) noexcept {
    const auto visit = [stage](const auto& f, int id) {
        emit(stage,{f.z1[0],f.z1[1],f.z1[2],f.z1[3],f.z2[0],f.z2[1],f.z2[2],f.z2[3]},-1,0,0,id);
    };
    for (int n = 0; n < 2; ++n) {
        visit(s.wideNetwork.lowLP[n],n); visit(s.wideNetwork.lowHP[n],2+n);
        visit(s.wideNetwork.highLP[n],4+n); visit(s.wideNetwork.highHP[n],6+n);
    }
    visit(s.wideNetwork.lowCompensation,8);
}
template<class Block, class State> void up(const Block& wet, const Block& dry, const State& s) noexcept {
    if (!active()) return;
    for (int n = 0; n < 4; ++n) { subsample = n; emit(11,{wet[n][0],wet[n][1],dry[n][0],dry[n][1]}); }
    subsample = -1;
    if (s.useWideNetwork && int(s.current.mode) == 1) wideState(s,12);
}
template<class Block, class State> void wideAfter(const Block& bands, const State& s) noexcept {
    if (!active() || !s.useWideNetwork || int(s.current.mode) != 1) return;
    wideState(s,13);
    for (int n = 0; n < 4; ++n) for (int band = 0; band < 3; ++band) {
        subsample = n; const auto& q = bands[n][band]; emit(14,{q[0],q[1],q[2],q[3]},-1,0,0,band);
    }
    subsample = -1;
}
inline void lanePoint(const void* object, int sample) noexcept {
    if (!active()) return;
    lane = -1; for (int n = 0; n < 4; ++n) if (lanes[n] == object) lane = n;
    if (lane < 0) failed = true;
    subsample = sample;
}
inline void laneInput(const void*, int ch, int character, double x, double h, double u,
    double px, double ph, double pu, double primitive, double difference, double dc, double envelope,
    int priorKind, bool bypassed, bool tilted) noexcept {
    emit(30,{x,h,u,px,ph,pu,primitive,difference,dc,envelope},ch,priorKind,character,-1,int(bypassed)|(int(tilted)<<1));
}
inline void shapeInput(double x) noexcept { shapeArgument = x; }
inline void exactInput(double x, double p, double bias, double normalizer) noexcept {
    exactArguments = {x,p,bias,normalizer};
}
inline void midpoint(int ch, int character, double u, double p, double shaped) noexcept {
    emit(31,{u,p,shapeArgument,shaped},ch,0,character);
}
inline void primitive(int ch, int character, int kind, int object, int cached, double x, double value) noexcept {
    emit(32,{x,value},ch,kind,character,object,cached);
}
inline void laneQuotient(int ch, int character, int kind, int previousKind, double u, double p,
    double current, double prior, double delta, double shaped) noexcept {
    if (kind == 1) emit(33,{exactArguments[0],exactArguments[1],exactArguments[2],exactArguments[3],shaped},ch,kind,character,-1,previousKind);
    else emit(34,{current,prior,delta,shaped},ch,kind,character,-1,previousKind);
}
inline void laneMix(int ch, int character, double d, double t, double amount, double difference,
                    double gain, double mix, double result) noexcept {
    emit(35,{d,t,amount,difference,gain,mix,result},ch,0,character);
}
inline void mix(Frame wet, Frame dry, Frame output) noexcept {
    for (int ch = 0; ch < 2; ++ch) emit(40,{wet[ch],dry[ch],output[ch]},ch);
}
inline void downInput(Frame value) noexcept { subsample = -1; downValue = value; emit(41,{value[0],value[1]}); }
inline void downOutput(Frame value, double gain) noexcept {
    for (int ch = 0; ch < 2; ++ch) emit(42,{downValue[ch],gain,value[ch]},ch);
}
inline double gainResult(int stage, unsigned factor, double db, double value) noexcept {
    emit(stage,{db,value},-1,0,0,-1,int(factor)); return value;
}
inline void fresh(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (!path.is_absolute() || fs::exists(fs::symlink_status(path)) || path.lexically_normal() != path)
        throw std::runtime_error("output: fresh canonical absolute directory required");
    for (auto p = path.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        const auto status = fs::symlink_status(p);
        if (fs::is_symlink(status) || (fs::exists(status) && !fs::is_directory(status)))
            throw std::runtime_error("output: plain parent directory required");
#if defined(_WIN32)
        const auto attributes = GetFileAttributesW(p.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("output: redirected parent directory refused");
#endif
    }
}
inline void write(const std::filesystem::path& path) {
    if (failed || records.empty()) throw std::runtime_error("observer: invalid/nonfinite/overflowed trace");
    std::ofstream out(path,std::ios::binary);
    for (const auto& r : records) {
        out << "{\"partition\":" << r.partition << ",\"frame\":" << r.frame << ",\"slot\":" << r.slot
            << ",\"stage\":" << r.stage << ",\"order\":" << r.order << ",\"channel\":" << r.channel
            << ",\"lane\":" << r.lane << ",\"subsample\":" << r.subsample << ",\"call\":" << r.call
            << ",\"kind\":" << r.kind << ",\"selector\":" << r.selector << ",\"object\":" << r.object
            << ",\"flags\":" << r.flags << ",\"ramp\":" << r.ramp << ",\"words\":[";
        for (unsigned n = 0; n < r.count; ++n) {
            if (n) out << ',';
            out << '"' << std::hex << std::setfill('0') << std::setw(16) << r.words[n] << '"' << std::dec;
        }
        out << "]}\n";
    }
    out.close();
    if (!out.good()) throw std::runtime_error("observer: trace write failed");
}
}

// Original fixture code is in an unnamed namespace; this preserves its short hook spelling.
namespace serial_observer = airack::probe::serial_observer;
