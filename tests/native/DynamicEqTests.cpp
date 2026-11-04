#include "DynamicEq.h"
#if !defined(_DEBUG)
// Release-only private-state oracle, as in RoutedSynthEngineTests.
#include "../../src/dsp/DynamicEq.cpp"
#endif
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <new>
#include <numbers>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace { thread_local bool countAllocations = false; thread_local std::size_t allocations = 0, frees = 0, allocatedBytes = 0; }
void* operator new(std::size_t size)
{
    if (countAllocations) { ++allocations; allocatedBytes += size; }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { if (countAllocations && memory) ++frees; std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (countAllocations) { ++allocations; allocatedBytes += size; }
    if (auto* memory = _aligned_malloc(size ? size : 1, std::size_t(alignment))) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* memory, std::align_val_t) noexcept { if (countAllocations && memory) ++frees; _aligned_free(memory); }
void operator delete[](void* memory, std::align_val_t alignment) noexcept { ::operator delete(memory, alignment); }
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(memory, alignment); }
void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(memory, alignment); }

namespace
{
using namespace airack;
int checks = 0, failures = 0;
void check(bool good, const char* expression, int line)
{
    ++checks;
    if (!good) { ++failures; std::cout << "FAIL " << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void preparedResponseMatchesTheOriginal()
{
    allocations=frees=allocatedBytes=0;countAllocations=true;
    PreparedDynamicEqResponse prepared;countAllocations=false;
    CHECK(allocations==1&&allocatedBytes<=8192);
    CHECK(!prepared.energy(1000));
    DynamicEqSettings settings;settings.bands[0].active=true;settings.bands[0].gainDb=7;settings.bands[0].rangeDb=-12;
    const auto ready=prepared.prepare(settings,48000);CHECK(ready);if(!ready)return;
    const auto compare=[&](const DynamicEqSettings& s,double rate,const std::array<float,8>& movement,int points){
        const bool good=prepared.prepare(s,rate,movement);CHECK(good);if(!good)return;
        for(int i=0;i<=points;++i){
            const double hz=i==0?0:i==points?rate*.5:20*std::pow(std::min(1000.,rate*.5/20),double(i)/points);
            const auto expected=dynamicEqEnergyResponse(s,rate,hz,movement),actual=prepared.energy(hz);
            CHECK(bool(actual)==bool(expected));
            if(actual&&expected)CHECK(std::bit_cast<std::uint64_t>(*actual)==std::bit_cast<std::uint64_t>(*expected));
        }
    };
    for(int shape=0;shape<9;++shape)for(int slope=0;slope<6;++slope)for(int placement=0;placement<5;++placement){
        auto s=settings;auto& b=s.bands[0];b.shape=DynamicEqShape(shape);b.slope=DynamicEqSlope(slope);b.placement=DynamicEqPlacement(placement);
        for(float movement:{0.f,-3.f,-12.f,24.f}){std::array<float,8> values{};values[0]=movement;compare(s,48000,values,8);}
    }
    for(double rate:{8000.,44100.,48000.,96000.,192000.,1536000.})for(float scale:{0.f,.5f,2.f}){
        auto s=settings;s.gainScale=scale;s.outputGainDb=-3;
        for(std::size_t n=0;n<8;++n){auto& b=s.bands[n];b.active=true;b.shape=DynamicEqShape(n%9);b.slope=DynamicEqSlope(n%6);b.placement=DynamicEqPlacement(n%5);b.frequencyHz=float(20*std::pow(1000.,double(n)/7));b.q=n%2?.25f:18.f;b.gainDb=n%2?-24.f:24.f;b.rangeDb=n%2?24.f:-24.f;}
        for(float amount:{0.f,.5f,1.f}){std::array<float,8> movement{};for(std::size_t n=0;n<8;++n)movement[n]=amount*s.bands[n].rangeDb;compare(s,rate,movement,32);}
    }
    settings.autoGain=true;for(double rate:{48000.,192000.})for(float movement:{0.f,-6.f}){std::array<float,8> values{};values[0]=movement;compare(settings,rate,values,8);}
    CHECK(!prepared.energy(-1)&&!prepared.energy(std::numeric_limits<double>::infinity())&&!prepared.energy(192001));
    auto invalid=settings;invalid.bands[0].q=0;CHECK(!prepared.prepare(invalid,48000));CHECK(!prepared.energy(1000));
    CHECK(prepared.prepare(settings,48000));CHECK(prepared.energy(1000));
    CHECK(!prepared.prepare(settings,0));CHECK(!prepared.energy(1000));
    std::array<float,8> bad{};bad[7]=std::numeric_limits<float>::quiet_NaN();CHECK(!prepared.prepare(settings,48000,bad));CHECK(!prepared.energy(1000));
    CHECK(prepared.prepare(settings,48000));(void)prepared.energy(1000);
    allocations=frees=0;countAllocations=true;const bool repeated=prepared.prepare(settings,48000);const auto value=prepared.energy(1000);countAllocations=false;
    CHECK(repeated&&value);CHECK(allocations==0&&frees==0);
}
void profilePreparedResponse(bool usePrepared)
{
    DynamicEqSettings settings;std::array<double,421> frequencies{},values{};
    for(std::size_t i=0;i<8;++i){auto& b=settings.bands[i];b.active=true;b.frequencyHz=float(60*std::pow(120.,double(i)/7));b.gainDb=i%2?-6.f:7.f;b.rangeDb=i%2?12.f:-12.f;b.q=.8f;b.placement=DynamicEqPlacement::mid;}
    for(std::size_t i=0;i<frequencies.size();++i)frequencies[i]=20*std::exp(std::log(1000.)*double(i)/420);
    PreparedDynamicEqResponse prepared;
    auto volatile legacy=&dynamicEqEnergyResponse;
    auto volatile evaluate=&PreparedDynamicEqResponse::energy;
    std::array<double,64> elapsed{};std::uint64_t hash=14695981039346656037ull;
    for(int step=0;step<72;++step){
        std::array<float,8> movement{};for(std::size_t i=0;i<8;++i)movement[i]=float((step+int(i))%32+1)*settings.bands[i].rangeDb/64;
        const auto start=std::chrono::steady_clock::now();bool ready=true;
        if(usePrepared){ready=prepared.prepare(settings,192000,movement);for(std::size_t i=0;i<values.size();++i)values[i]=(prepared.*evaluate)(frequencies[i]).value_or(std::numeric_limits<double>::quiet_NaN());}
        else for(std::size_t i=0;i<values.size();++i)values[i]=legacy(settings,192000,frequencies[i],movement).value_or(std::numeric_limits<double>::quiet_NaN());
        const auto end=std::chrono::steady_clock::now();CHECK(ready);CHECK(std::all_of(values.begin(),values.end(),[](double value){return std::isfinite(value);}));
        if(step<8)continue;
        elapsed[std::size_t(step-8)]=std::chrono::duration<double,std::nano>(end-start).count();
        for(double value:values){const auto bits=std::bit_cast<std::uint64_t>(value);for(int byte=0;byte<8;++byte)hash=(hash^((bits>>(byte*8))&255))*1099511628211ull;}
    }
    std::sort(elapsed.begin(),elapsed.end());double total=0;for(double value:elapsed)total+=value;
    std::cout.precision(17);std::cout<<"RESPONSE_PROFILE prepared="<<int(usePrepared)<<" curves=64 points=421 mean_ns="<<total/elapsed.size()<<" p99_ns="<<elapsed.back()<<" checksum="<<hash<<'\n';
}
DynamicEqSettings band(DynamicEqShape shape = DynamicEqShape::bell, float gain = 9)
{
    DynamicEqSettings s;
    s.bands[0].active = true; s.bands[0].shape = shape;
    s.bands[0].gainDb = gain; s.bands[0].q = float(std::sqrt(.5));
    return s;
}
double measuredGainDb(DynamicEq& eq, double rate, double hz)
{
    double input = 0, output = 0;
    for (int n = 0; n < int(rate * .5); ++n) {
        const auto x = .01 * std::sin(2 * std::numbers::pi * hz * n / rate);
        const auto y = eq.process({x, x});
        if (n >= int(rate * .4)) { input += x*x; output += y[0]*y[0]; }
    }
    return 10 * std::log10(std::max(output / input, 1e-30));
}
void gainCase(DynamicEqSettings s, double rate, double frequency, double expected, double tolerance = .15)
{
    DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(rate));
    const auto observed = measuredGainDb(eq, rate, frequency);
    std::cout << "GAIN rate=" << rate << " shape=" << int(s.bands[0].shape)
              << " hz=" << frequency << " observed=" << observed << " expected=" << expected << '\n';
    CHECK(std::abs(observed - expected) < tolerance);
    const auto response = dynamicEqResponse(s, rate, frequency); CHECK(response.has_value());
    if (response) CHECK(std::abs(20 * std::log10(std::max(std::abs(*response), 1e-15)) - observed) < .15);
}
void transparentAndAdmission()
{
    std::cout << "SECTION transparent_and_transactional_admission\n";
    DynamicEq eq; const DynamicEq::Frame sample {.125, -.375};
    CHECK(eq.process(sample) == sample); CHECK(eq.prepare(48000));
    for (int n = 0; n < 1000; ++n) {
        const DynamicEq::Frame x {std::sin(n * .019), std::cos(n * .031) * .25}; CHECK(eq.process(x) == x);
    }
    CHECK(eq.tailSeconds() == 0);
    const auto baseline = eq.settings();
    for (int bad = 0; bad < 13; ++bad) {
        auto invalid = baseline; auto& b = invalid.bands[7];
        switch (bad) {
            case 0: b.frequencyHz = std::numeric_limits<float>::infinity(); break;
            case 1: b.q = .249f; break;
            case 2: b.q = 18.01f; break;
            case 3: b.gainDb = 24.01f; break;
            case 4: b.thresholdDb = -60.1f; break;
            case 5: b.rangeDb = -24.1f; break;
            case 6: b.attackMs = 0; break;
            case 7: b.releaseMs = 2001; break;
            case 8: b.shape = DynamicEqShape(99); break;
            case 9: b.slope = DynamicEqSlope(99); break;
            case 10: invalid.outputGainDb = 12.01f; break;
            case 11: b.frequencyHz = 19.9f; break;
            case 12: b.thresholdDb = std::numeric_limits<float>::quiet_NaN(); break;
        }
        CHECK(!validateDynamicEq(invalid)); CHECK(!eq.setSettings(invalid));
        CHECK(eq.settings() == baseline); CHECK(eq.process(sample) == sample);
    }
    for (const double rate : {0., -48000., 7999., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK(!eq.prepare(rate)); CHECK(eq.process(sample) == sample);
    }
    CHECK(!dynamicEqResponse(baseline, 0, 1000)); CHECK(!dynamicEqResponse(baseline, 48000, -1));
}
void autoGainUsesTheSettingsAndHasNeutralDefault()
{
    std::cout << "SECTION settings_based_gain_compensation\n";
    DynamicEqSettings s;
    CHECK(dynamicEqAutoGainDb(s) == 0.f);
    s.bands[0].active = true;
    s.bands[0].shape = DynamicEqShape::lowShelf;
    s.bands[0].frequencyHz = float(std::sqrt(20. * 20000.));
    s.bands[0].gainDb = 12;
    s.autoGain = true;
    CHECK(std::abs(dynamicEqAutoGainDb(s) + 6.f) < .001f);
    s.gainScale = 0;
    CHECK(std::abs(dynamicEqAutoGainDb(s)) < .001f);
    s.gainScale = 1;
    s.bands[0].shape = DynamicEqShape::highShelf;
    CHECK(std::abs(dynamicEqAutoGainDb(s) + 6.f) < .001f);
    s.bands[0].shape = DynamicEqShape::bell; s.bands[0].q = 1;
    CHECK(std::abs(dynamicEqAutoGainDb(s) + 1.67190112199983) < 1e-6);
    s.bands[0].q = .25f;
    CHECK(std::abs(dynamicEqAutoGainDb(s) + 5.01570336599949) < 1e-6);
    s.bands[0].q = 18;
    CHECK(std::abs(dynamicEqAutoGainDb(s) + .09649747792424712) < 1e-7);
    s.bands[0].shape = DynamicEqShape::lowShelf;
    s.bands[0].frequencyHz = 20000;
    CHECK(dynamicEqAutoGainDb(s) == -12);
    s.bands[0].frequencyHz = 20;
    CHECK(dynamicEqAutoGainDb(s) == 0);
    s.bands[0].shape = DynamicEqShape::highShelf;
    CHECK(dynamicEqAutoGainDb(s) == -12);
    s.bands[0].gainDb = -24; s.gainScale = 2;
    CHECK(dynamicEqAutoGainDb(s) == 12);
    for (auto& b : s.bands) { b = s.bands[0]; b.gainDb = 24; }
    CHECK(dynamicEqAutoGainDb(s) == -12);
    for (auto& b : s.bands) b.bypassed = true;
    CHECK(dynamicEqAutoGainDb(s) == 0);
    s.bands[0].bypassed = false; s.bands[0].active = false;
    CHECK(dynamicEqAutoGainDb(s) == 0);
    s.bands[0].active = true;
    for (auto shape : {DynamicEqShape::lowCut, DynamicEqShape::highCut, DynamicEqShape::notch}) {
        s.bands[0].shape = shape;
        CHECK(dynamicEqAutoGainDb(s) == 0);
    }
    s = band(DynamicEqShape::bell, 12); s.autoGain = true; s.bands[0].q = 1;
    const auto before = dynamicEqAutoGainDb(s);
    s.outputGainDb = 12; s.bands[0].rangeDb = -24; s.bands[0].thresholdDb = -60;
    s.bands[0].attackMs = 200; s.bands[0].releaseMs = 2000;
    CHECK(dynamicEqAutoGainDb(s) == before);
    s.autoGain = false;
    CHECK(dynamicEqAutoGainDb(s) == 0);
    DynamicEq eq; CHECK(eq.prepare(48000)); const auto accepted = eq.settings();
    for (float value : {-.01f, 2.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        s = {}; s.gainScale = value;
        CHECK(!validateDynamicEq(s)); CHECK(!eq.setSettings(s)); CHECK(eq.settings() == accepted);
        CHECK(std::isnan(dynamicEqAutoGainDb(s)));
        CHECK(!dynamicEqResponse(s, 48000, 1000));
        CHECK(!dynamicEqBandResponse(s.bands[0], 48000, 1000, 0, value));
    }
}
void scaledGainSignalProof()
{
    std::cout << "SECTION gain_scale_independent_sine_and_dynamic_equivalence\n";
    for (double rate : {48000., 96000.})
        for (auto shape : {DynamicEqShape::bell, DynamicEqShape::lowShelf, DynamicEqShape::highShelf})
            for (float gain : {-18.f, 18.f}) for (float scale : {0.f, 1.f, 2.f}) {
                auto s = band(shape, gain); s.gainScale = scale; s.outputGainDb = 3;
                s.bands[0].frequencyHz = shape == DynamicEqShape::lowShelf ? 16000.f : shape == DynamicEqShape::highShelf ? 80.f : 1000.f;
                const auto testHz = shape == DynamicEqShape::lowShelf ? 64. : shape == DynamicEqShape::highShelf ? 6000. : 1000.;
                gainCase(s, rate, testHz, std::clamp(gain*scale, -24.f, 24.f) + 3, .15);
                s.autoGain = true;
                gainCase(s, rate, testHz, std::clamp(gain*scale, -24.f, 24.f) + 3 + dynamicEqAutoGainDb(s), .15);
            }
    // Scaling the stored static endpoint is equivalent to editing that gain.
    // Dynamics range/timing, cuts/notches and output trim retain their values.
    for (auto shape : {DynamicEqShape::bell, DynamicEqShape::lowShelf, DynamicEqShape::highShelf,
                       DynamicEqShape::lowCut, DynamicEqShape::highCut, DynamicEqShape::notch})
        for (float gain : {-18.f, 18.f}) for (float scale : {0.f, .5f, 2.f}) {
            auto s = band(shape, gain); s.gainScale = scale; s.bands[0].rangeDb = gain > 0 ? -12.f : 12.f;
            s.bands[0].thresholdDb = -42; s.outputGainDb = -4;
            auto equivalent = s; equivalent.gainScale = 1;
            if (shape <= DynamicEqShape::highShelf) equivalent.bands[0].gainDb = std::clamp(gain*scale, -24.f, 24.f);
            DynamicEq scaled, edited;
            CHECK(scaled.setSettings(s) && edited.setSettings(equivalent));
            CHECK(scaled.prepare(48000) && edited.prepare(48000));
            bool exact = true;
            for (int n = 0; n < 4096; ++n) {
                const DynamicEq::Frame input {n == 0 ? .01 : .03*std::sin(n*.13), -.02*std::cos(n*.23)};
                const auto a = scaled.process(input), b = edited.process(input);
                exact = exact && a == b;
            }
            CHECK(exact); CHECK(scaled.dynamicGainsDb() == edited.dynamicGainsDb());
            CHECK(scaled.settings() == s);
            for (double hz : {20., 1000., 20000.}) {
                const auto a = dynamicEqBandResponse(s.bands[0], 48000, hz, 2, scale);
                const auto b = dynamicEqBandResponse(equivalent.bands[0], 48000, hz, 2);
                CHECK(a && b && *a == *b);
            }
            CHECK(dynamicEqTailSeconds(s, 48000) == dynamicEqTailSeconds(equivalent, 48000));
        }
}
void compensationRamps()
{
    std::cout << "SECTION compensation_five_ms_enable_disable_and_reset\n";
    for (double rate : {8000., 44100., 96000.}) {
        auto s = band(DynamicEqShape::lowShelf, 12); s.bands[0].frequencyHz = float(std::sqrt(20.*20000.));
        DynamicEq compensated, reference;
        CHECK(compensated.setSettings(s) && reference.setSettings(s));
        CHECK(compensated.prepare(rate) && reference.prepare(rate));
        const DynamicEq::Frame input {.01, -.007};
        for (int n = 0; n < 4096; ++n) { CHECK(compensated.process(input) == reference.process(input)); }
        const int count = int(std::ceil(rate*.005));
        const auto target = std::exp(-6.*.1151292546497022842);
        s.autoGain = true; CHECK(compensated.setSettings(s));
        double error = 0;
        for (int n = 0; n < count; ++n) {
            if (n == count/2) CHECK(compensated.setSettings(s));
            const auto a = compensated.process(input), b = reference.process(input);
            const auto expected = 1. + (target-1.)*double(n+1)/count;
            for (std::size_t c = 0; c < 2; ++c) error = std::max(error, std::abs(a[c]-b[c]*expected));
        }
        CHECK(error < 1e-8);
        s.autoGain = false; CHECK(compensated.setSettings(s)); error = 0;
        bool unity = false;
        for (int n = 0; n < count; ++n) {
            const auto a = compensated.process(input), b = reference.process(input);
            const auto expected = target + (1.-target)*double(n+1)/count;
            for (std::size_t c = 0; c < 2; ++c) error = std::max(error, std::abs(a[c]-b[c]*expected));
            if (n == count-1) unity = a == b;
        }
        CHECK(error < 1e-8); CHECK(unity);
        s.autoGain = true; CHECK(compensated.setSettings(s));
        for (int n = 0; n < count/2; ++n) (void)compensated.process(input);
        compensated.reset(); reference.reset();
        const auto a = compensated.process(input), b = reference.process(input);
        CHECK(std::abs(a[0]-b[0]*target) < 1e-8);
        std::cout << "COMPENSATION_RAMP rate=" << rate << " samples=" << count << " max_error=" << error << '\n';
    }
}
void gainBlockAndHeapProof()
{
    std::cout << "SECTION gain_controls_frame_block_partition_and_heap\n";
    for (double rate : {48000., 96000.}) {
        DynamicEq frameEq, blockEq; CHECK(frameEq.prepare(rate) && blockEq.prepare(rate));
        DynamicEqSettings s;
        for (std::size_t i = 0; i < 8; ++i) { auto& b=s.bands[i]; b.active=true; b.shape=DynamicEqShape(i%3); b.frequencyHz=80+900*float(i); b.gainDb=i%2?8.f:-7.f; b.rangeDb=i%2?-12.f:12.f; b.thresholdDb=-36; }
        bool exact=true, accepted=true, finite=true;
        std::array<DynamicEq::Frame,513> actual{}, expected{};
        allocations=0; frees=0; countAllocations=true;
        for (int edit=0; edit<80; ++edit) {
            s.gainScale=std::array{0.f,.5f,1.f,2.f}[std::size_t(edit)%4]; s.autoGain=edit%3!=0;
            s.bands[std::size_t(edit)%8].frequencyHz=40+float((edit*701)%19000);
            s.bands[std::size_t(edit)%8].q=.25f+float(edit%18); s.outputGainDb=float(edit%8)-4;
            accepted=frameEq.setSettings(s)&&blockEq.setSettings(s)&&accepted;
            if (edit%17==0) { frameEq.reset(); blockEq.reset(); }
            const int count=std::array{1,7,64,511,513}[std::size_t(edit)%5];
            for (int n=0; n<count; ++n) { actual[std::size_t(n)]={n==0?.01:.003*std::sin((edit*513+n)*.19),.002*std::cos((edit*513+n)*.17)}; expected[std::size_t(n)]=frameEq.process(actual[std::size_t(n)]); }
            blockEq.processBlock(std::span(actual).first(std::size_t(count)));
            for (int n=0; n<count; ++n) for (std::size_t c=0; c<2; ++c) {
                exact=std::bit_cast<std::uint64_t>(actual[std::size_t(n)][c])==std::bit_cast<std::uint64_t>(expected[std::size_t(n)][c])&&exact;
                finite=std::isfinite(actual[std::size_t(n)][c])&&finite;
            }
            exact=frameEq.dynamicGainsDb()==blockEq.dynamicGainsDb()&&frameEq.tailSeconds()==blockEq.tailSeconds()&&exact;
        }
        countAllocations=false;
        CHECK(accepted); CHECK(finite); CHECK(exact); CHECK(allocations==0); CHECK(frees==0);
        std::cout << "GAIN_HEAP rate=" << rate << " allocations=" << allocations << " frees=" << frees << '\n';
    }
}
void staticShapes()
{
    std::cout << "SECTION independent_static_audio_measurements\n";
    for (const double rate : {44100., 48000., 96000.}) {
        gainCase(band(DynamicEqShape::bell, 9), rate, 1000, 9);
        gainCase(band(DynamicEqShape::bell, -9), rate, 1000, -9);
        auto low = band(DynamicEqShape::lowShelf, 9); low.bands[0].frequencyHz = 2000;
        gainCase(low, rate, 31.25, 9);
        auto high = band(DynamicEqShape::highShelf, -9); high.bands[0].frequencyHz = 200;
        gainCase(high, rate, 6000, -9);
        for (auto shape : {DynamicEqShape::lowCut, DynamicEqShape::highCut}) {
            for (auto slope : {DynamicEqSlope::db12, DynamicEqSlope::db24}) {
                auto s = band(shape); s.bands[0].slope = slope;
                gainCase(s, rate, 1000, -3.010299956639812);
                DynamicEq a, b; CHECK(a.setSettings(s)); CHECK(a.prepare(rate)); CHECK(b.setSettings(s)); CHECK(b.prepare(rate));
                if (shape == DynamicEqShape::lowCut) {
                    const auto first = measuredGainDb(a, rate, 31.25), second = measuredGainDb(b, rate, 62.5);
                    const auto want = slope == DynamicEqSlope::db12 ? 12.041199826559248 : 24.082399653118497;
                    CHECK(std::abs(second - first - want) < .4);
                } else {
                    const auto response = dynamicEqBandResponse(s.bands[0], rate, rate * .499);
                    CHECK(response && std::abs(*response) < (slope == DynamicEqSlope::db12 ? .001 : .000001));
                }
            }
        }
        DynamicEq notch; const auto s = band(DynamicEqShape::notch); CHECK(notch.setSettings(s)); CHECK(notch.prepare(rate));
        CHECK(measuredGainDb(notch, rate, 1000) < -50);
        gainCase(band(DynamicEqShape::bell, 0), rate, 1000, 0, 1e-8);
    }
}
void identitiesAndStereo()
{
    std::cout << "SECTION eight_bands_stereo_trim_and_bypass\n";
    DynamicEqSettings all; for (auto& b : all.bands) { b.active = true; b.gainDb = 2; } all.outputGainDb = 3;
    gainCase(all, 48000, 1000, 19);
    auto s = band(); DynamicEq leftOnly; CHECK(leftOnly.setSettings(s)); CHECK(leftOnly.prepare(48000));
    double leaked = 0;
    for (int n = 0; n < 16000; ++n) {
        const auto y = leftOnly.process({.01 * std::sin(n * .13), 0}); leaked = std::max(leaked, std::abs(y[1]));
    }
    CHECK(leaked == 0);
    s.bands[0].bypassed = true; CHECK(leftOnly.setSettings(s)); CHECK(std::abs(measuredGainDb(leftOnly, 48000, 1000)) < 1e-8);
    s.bands[0].active = false; s.bands[0].bypassed = false;
    CHECK(leftOnly.setSettings(s)); CHECK(std::abs(measuredGainDb(leftOnly, 48000, 1000)) < 1e-8);
    auto capped = band(DynamicEqShape::lowShelf); capped.bands[0].q = 18;
    auto effective = capped; effective.bands[0].q = 4;
    DynamicEq a, b; CHECK(a.setSettings(capped)); CHECK(a.prepare(48000)); CHECK(b.setSettings(effective)); CHECK(b.prepare(48000));
    double error = 0;
    for (int n = 0; n < 10000; ++n) {
        const DynamicEq::Frame x {.001 * std::sin(n * .13), .002 * std::cos(n * .19)};
        const auto ya = a.process(x), yb = b.process(x); error = std::max({error, std::abs(ya[0]-yb[0]), std::abs(ya[1]-yb[1])});
    }
    CHECK(error == 0);
}
void resetAndPartitions()
{
    std::cout << "SECTION reset_reprepare_and_repeated_publication\n";
    auto s = band(); DynamicEq once, repeated;
    CHECK(once.setSettings(s)); CHECK(once.prepare(48000)); CHECK(repeated.setSettings(s)); CHECK(repeated.prepare(48000));
    double error = 0;
    for (int n = 0; n < 8192; ++n) {
        if (n == 2048) { s.bands[0].gainDb = -9; CHECK(once.setSettings(s)); }
        if (n % 64 == 0) CHECK(repeated.setSettings(s));
        const DynamicEq::Frame x {.01 * std::sin(n * .13), .02 * std::cos(n * .07)};
        const auto a = once.process(x), b = repeated.process(x); error = std::max({error, std::abs(a[0]-b[0]), std::abs(a[1]-b[1])});
    }
    CHECK(error == 0); once.reset(); DynamicEq fresh; CHECK(fresh.setSettings(s)); CHECK(fresh.prepare(48000));
    for (int n = 0; n < 2000; ++n) { const DynamicEq::Frame x {std::sin(n*.17), std::cos(n*.11)}; CHECK(once.process(x) == fresh.process(x)); }
    CHECK(once.prepare(96000)); CHECK(fresh.prepare(96000));
    for (int n = 0; n < 2000; ++n) { const DynamicEq::Frame x {std::sin(n*.13), std::cos(n*.09)}; CHECK(once.process(x) == fresh.process(x)); }
    CHECK(!once.prepare(0));
    for (int n = 0; n < 2000; ++n) { const DynamicEq::Frame x {std::sin(n*.07), std::cos(n*.03)}; CHECK(once.process(x) == fresh.process(x)); }
}
void tailBound()
{
    std::cout << "SECTION low_frequency_high_Q_tail\n";
    auto s = band(DynamicEqShape::notch); s.bands[0].frequencyHz = 20; s.bands[0].q = 18;
    DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000)); const auto tail = eq.tailSeconds();
    CHECK(tail > 0 && tail < 120); if (!(tail > 0 && tail < 120)) return;
    const int count = int(std::ceil((tail + .05) * 48000)); double last = 0;
    for (int n = 0; n < count; ++n) {
        const auto y = eq.process({n == 0 ? 1. : 0., 0}); CHECK(std::isfinite(y[0]) && std::isfinite(y[1]));
        if (n >= count-256) last = std::max(last, std::abs(y[0]));
    }
    std::cout << "TAIL seconds=" << tail << " final_peak=" << last << '\n'; CHECK(last < 1e-6);
}
void dynamicLaw()
{
    std::cout << "SECTION dynamic_gain_law\n";
    CHECK(dynamicEqTargetGainDb(-40, -24, -12) == 0);
    CHECK(dynamicEqTargetGainDb(-12, -24, -12) == -9);
    CHECK(dynamicEqTargetGainDb(-12, -24, 12) == 9);
    CHECK(dynamicEqTargetGainDb(0, -24, -12) == -12);
    CHECK(dynamicEqTargetGainDb(0, -24, 12) == 12);
    CHECK(std::abs(dynamicEqTargetGainDb(-24, -24, -12) + .5625f) < 1e-6);
    CHECK(dynamicEqTargetGainDb(-27, -24, -12) == 0);
}
float toneDetector(DynamicEqSettings s, double hz, bool rightOnly = false)
{
    DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000));
    double sum = 0; int count = 0;
    for (int n = 0; n < 48000; ++n) {
        const auto x = .4 * std::sin(2 * std::numbers::pi * hz * n / 48000);
        const auto y = eq.process(rightOnly ? DynamicEq::Frame {0,x} : DynamicEq::Frame {x,0});
        CHECK(std::isfinite(y[0]) && std::isfinite(y[1]));
        CHECK(y[rightOnly ? 0 : 1] == 0);
        if (n > 40000) { sum += eq.dynamicGainsDb()[0]; ++count; }
    }
    return float(sum/count);
}
void linkedDetectors()
{
    std::cout << "SECTION linked_selective_independent_detectors\n";
    auto s = band(DynamicEqShape::bell, 0); auto& b = s.bands[0];
    b.rangeDb = -12; b.thresholdDb = -30; b.q = 18;
    const auto center = toneDetector(s, 1000), distant = toneDetector(s, 100);
    const auto right = toneDetector(s, 1000, true);
    std::cout << "DETECT center=" << center << " distant=" << distant << " right=" << right << '\n';
    CHECK(center < -6); CHECK(distant > -.1); CHECK(center == right);
    for (auto shape : {DynamicEqShape::lowShelf, DynamicEqShape::highShelf}) {
        b.shape = shape; b.q = 1;
        const auto low = toneDetector(s, 100), high = toneDetector(s, 10000);
        CHECK(shape == DynamicEqShape::lowShelf ? low < -6 && high > -.1 : high < -6 && low > -.1);
    }
    b.shape = DynamicEqShape::bell; b.q = 18;
    s.bands[1] = b; s.bands[1].frequencyHz = 8000;
    DynamicEq independent; CHECK(independent.setSettings(s)); CHECK(independent.prepare(48000));
    for (int n = 0; n < 48000; ++n) independent.process({.4*std::sin(n*2*std::numbers::pi*1000/48000),0});
    CHECK(independent.dynamicGainsDb()[0] < -6); CHECK(independent.dynamicGainsDb()[1] > -.1);
    for (auto shape : {DynamicEqShape::lowCut, DynamicEqShape::highCut, DynamicEqShape::notch}) {
        s = band(shape); s.bands[0].rangeDb = -24;
        CHECK(toneDetector(s, 1000) == 0);
    }
    s = band(); CHECK(toneDetector(s, 1000) == 0);
    s.bands[0].rangeDb = -24; s.bands[0].bypassed = true; CHECK(toneDetector(s, 1000) == 0);
}
void dynamicTimingAndAudio()
{
    std::cout << "SECTION measured_dynamic_time_constants_and_audio\n";
    for (const float direction : {-1.f, 1.f}) {
        auto s = band(DynamicEqShape::lowShelf, 0); auto& b = s.bands[0];
        b.frequencyHz = 20000; b.rangeDb = direction*12; b.thresholdDb = -24; b.attackMs = 100; b.releaseMs = 200;
        DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000));
        const double target = direction*9, input = std::pow(10., -12./20.);
        for (int n = 0; n < 4800; ++n) eq.process({input,input*.5});
        const auto attack = eq.dynamicGainsDb()[0];
        std::cout << "TIMING direction=" << direction << " attack=" << attack << " ideal=" << target*(1-std::exp(-1.)) << '\n';
        CHECK(std::abs(attack-target*(1-std::exp(-1.))) < .025);
        DynamicEq::Frame output {};
        for (int n = 0; n < 60000; ++n) output = eq.process({input,input*.5});
        const auto settled = eq.dynamicGainsDb()[0];
        CHECK(std::abs(settled-target) < .001);
        CHECK(std::abs(20*std::log10(output[0]/input)-target) < .002);
        CHECK(std::abs(output[1]/output[0]-.5) < 1e-12);
        for (int n = 0; n < 9600; ++n) eq.process({0,0});
        const auto release = eq.dynamicGainsDb()[0];
        std::cout << "TIMING release=" << release << " ideal=" << settled*std::exp(-1.) << '\n';
        CHECK(std::abs(release-settled*std::exp(-1.)) < .025);
    }
    for (float gain : {-24.f, -9.f, 0.f, 9.f, 24.f}) for (float range : {-24.f, 24.f}) {
        auto s = band(DynamicEqShape::lowShelf, gain); auto& b = s.bands[0]; b.frequencyHz = 20000;
        b.rangeDb = range; b.thresholdDb = -60; b.attackMs = .1f;
        DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000));
        DynamicEq::Frame y {}; for (int n = 0; n < 24000; ++n) y = eq.process({.25,.125});
        const auto expected = std::clamp(gain+range,-24.f,24.f);
        CHECK(std::abs(gain+eq.dynamicGainsDb()[0]-expected) < .001);
        CHECK(std::abs(20*std::log10(y[0]/.25)-expected) < .002);
    }
}
void dynamicResponseAndPartitions()
{
    std::cout << "SECTION complex_dynamic_response_and_partition_invariance\n";
    for (auto shape : {DynamicEqShape::bell,DynamicEqShape::lowShelf,DynamicEqShape::highShelf})
        for (float gain : {-18.f,0.f,18.f}) for (float range : {-24.f,24.f}) {
            auto s = band(shape,gain); s.bands[0].rangeDb = range;
            const auto endpoint = std::clamp(gain+range,-24.f,24.f);
            auto first = s.bands[0], last = first; first.rangeDb = last.rangeDb = 0; last.gainDb = endpoint;
            for (int step = 0; step <= 10; ++step) for (double hz : {20.,100.,1000.,8000.,20000.}) {
                const auto wanted = gain+(endpoint-gain)*step/10.f;
                const auto a = std::pow(10.,gain/20.), z = std::pow(10.,endpoint/20.);
                const auto mix = std::abs(z-a) < 1e-12 ? 0 : (std::pow(10.,wanted/20.)-a)/(z-a);
                const auto h0 = dynamicEqBandResponse(first,48000,hz), h1 = dynamicEqBandResponse(last,48000,hz);
                const auto h = dynamicEqBandResponse(s.bands[0],48000,hz,wanted-gain);
                CHECK(h && h0 && h1);
                if (h && h0 && h1) CHECK(std::abs(*h-(*h0+mix*(*h1-*h0))) < 1e-11);
            }
        }
    auto s = band(); s.bands[0].rangeDb = -18;
    DynamicEq a,b; CHECK(a.setSettings(s)); CHECK(b.setSettings(s)); CHECK(a.prepare(48000)); CHECK(b.prepare(48000));
    double error = 0;
    for (int n = 0; n < 24000; ++n) {
        if (n == 4096 || n == 8192 || n == 12288) {
            s.bands[0].rangeDb *= -1; s.bands[0].frequencyHz = n == 8192 ? 20.f : 20000.f;
            s.bands[0].shape = n == 12288 ? DynamicEqShape::highShelf : DynamicEqShape::bell;
            CHECK(a.setSettings(s)); CHECK(b.setSettings(s));
        }
        if (n % 64 == 0 || n % 511 == 0) CHECK(b.setSettings(s));
        const DynamicEq::Frame x {.01*std::sin(n*.13),.03*std::cos(n*.07)};
        const auto ya = a.process(x), yb = b.process(x);
        error = std::max({error,std::abs(ya[0]-yb[0]),std::abs(ya[1]-yb[1])});
        CHECK(a.dynamicGainsDb() == b.dynamicGainsDb()); CHECK(std::isfinite(ya[0]) && std::isfinite(ya[1]));
    }
    CHECK(error == 0);
}
void effectiveMovement()
{
    std::cout << "SECTION effective_dynamic_movement_under_range_and_bypass_automation\n";
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];
    b.rangeDb=12;b.thresholdDb=-60;b.attackMs=.1f;b.releaseMs=2000;
    DynamicEq eq;CHECK(eq.setSettings(s));CHECK(eq.prepare(48000));
    int frame=0;
    const auto run=[&](int samples){for(int i=0;i<samples;++i,++frame){const auto x=.5*std::sin(2*std::numbers::pi*1000*frame/48000);eq.process({x,x});}};
    run(12000);CHECK(eq.dynamicGainsDb()[0]>11.9f);
    b.rangeDb=0;CHECK(eq.setSettings(s));run(960);
    std::cout << "METER zero_range=" << eq.dynamicGainsDb()[0] << '\n';CHECK(eq.dynamicGainsDb()[0]==0);
    b.rangeDb=2;CHECK(eq.setSettings(s));run(960);
    std::cout << "METER small_range=" << eq.dynamicGainsDb()[0] << '\n';CHECK(eq.dynamicGainsDb()[0]>=0&&eq.dynamicGainsDb()[0]<=2.001f);
    b.rangeDb=-6;CHECK(eq.setSettings(s));run(960);
    std::cout << "METER opposite_range=" << eq.dynamicGainsDb()[0] << '\n';CHECK(eq.dynamicGainsDb()[0]>=-6.001f&&eq.dynamicGainsDb()[0]<=0);
    b.rangeDb=12;CHECK(eq.setSettings(s));eq.reset();frame=0;run(12000);
    const auto before=eq.dynamicGainsDb()[0];b.bypassed=true;CHECK(eq.setSettings(s));run(1);
    const auto fading=eq.dynamicGainsDb()[0];std::cout << "METER bypass_fading=" << fading << '\n';CHECK(fading>0&&fading<before);
    run(960);CHECK(eq.dynamicGainsDb()[0]==0);
    b.bypassed=false;b.gainDb=23;b.rangeDb=24;CHECK(eq.setSettings(s));eq.reset();run(12000);
    CHECK(eq.dynamicGainsDb()[0]>=0&&eq.dynamicGainsDb()[0]<=1.001f);
    b.gainDb=-23;b.rangeDb=-24;CHECK(eq.setSettings(s));eq.reset();run(12000);
    CHECK(eq.dynamicGainsDb()[0]<=0&&eq.dynamicGainsDb()[0]>=-1.001f);
    b.active=false;CHECK(eq.setSettings(s));run(960);CHECK(eq.dynamicGainsDb()[0]==0);
    allocations=0;countAllocations=true;for(int i=0;i<1000;++i)(void)eq.dynamicGainsDb();countAllocations=false;CHECK(allocations==0);
}
void auditionProof()
{
    std::cout << "SECTION temporary_audition_isolation_and_restoration\n";
    for (double rate : {44100.,48000.,96000.,192000.}) {
        auto s=band(DynamicEqShape::bell,9);s.bands[0].q=4;
        DynamicEq eq,normal;CHECK(eq.setSettings(s)&&normal.setSettings(s));CHECK(eq.prepare(rate)&&normal.prepare(rate));
        CHECK(!eq.setAudition(8));CHECK(!eq.auditioning());
        CHECK(eq.setAudition(0));
        const auto centre=measuredGainDb(eq,rate,1000);CHECK(eq.auditioning());
        CHECK(std::abs(centre)<.15);
        const auto off=measuredGainDb(eq,rate,8000);CHECK(off < -20);
        CHECK(eq.setAudition(std::nullopt));
        eq.reset();normal.reset();CHECK(!eq.auditioning());
        double restored=0;
        CHECK(eq.setAudition(0));
        for(int n=0;n<int(rate*.1);++n){const DynamicEq::Frame x{.01*std::sin(n*.13),.02*std::cos(n*.07)};
            (void)normal.process(x);(void)eq.process(x);}
        CHECK(eq.setAudition(std::nullopt));
        for(int n=0;n<int(rate*.02);++n){const DynamicEq::Frame x{.01*std::sin(n*.17),.02*std::cos(n*.11)};
            const auto a=eq.process(x),b=normal.process(x);if(n>int(rate*.006))restored=std::max({restored,std::abs(a[0]-b[0]),std::abs(a[1]-b[1])});}
        CHECK(restored==0);CHECK(!eq.auditioning());
        CHECK(eq.setAudition(0));s.bands[0].active=false;CHECK(eq.setSettings(s));
        for(int n=0;n<int(rate*.02);++n)(void)eq.process({.01,.02});CHECK(!eq.auditioning());
        s.bands[0].active=true;s.bands[0].frequencyHz=20;s.bands[0].q=18;CHECK(eq.setSettings(s));eq.reset();CHECK(eq.setAudition(0));
        double peak=0,last=0;
        allocations=0;countAllocations=true;
        for(int n=0;n<int(rate*8);++n){const auto y=eq.process({n==0?1.:0.,0.});peak=std::max(peak,std::abs(y[0]));if(n>=int(rate*7.99))last=std::max(last,std::abs(y[0]));if(y[1]!=0||!std::isfinite(y[0]))++failures;}
        countAllocations=false;CHECK(allocations==0);CHECK(peak<2);CHECK(last<1e-10);
        eq.reset();CHECK(!eq.auditioning());
        std::cout<<"AUDITION rate="<<rate<<" centre_db="<<centre<<" off_db="<<off<<" restored_error="<<restored<<" tail="<<last<<'\n';
    }
}
void auditionAutomation()
{
    std::cout<<"SECTION audition_selection_and_frequency_Q_automation\n";
    for(double rate:{8000.,48000.,192000.}){
        DynamicEq eq;DynamicEqSettings s;for(auto& b:s.bands)b.active=true;
        CHECK(eq.setSettings(s)&&eq.prepare(rate));double peak=0,jump=0;DynamicEq::Frame previous{};bool finite=true,accepted=true;
        allocations=0;countAllocations=true;
        for(int n=0;n<48000;++n){
            if(n%64==0){auto& b=s.bands[(n/64)%8];b.frequencyHz=n%128?20:20000;b.q=n%128?18:.25f;
                accepted=eq.setSettings(s)&&accepted;accepted=eq.setAudition((n/64)%11<8?std::optional<std::size_t>((n/64)%8):std::nullopt)&&accepted;}
            const auto y=eq.process({.001*std::sin(n*.13),.002*std::cos(n*.07)});
            finite=finite&&std::isfinite(y[0])&&std::isfinite(y[1]);peak=std::max({peak,std::abs(y[0]),std::abs(y[1])});jump=std::max({jump,std::abs(y[0]-previous[0]),std::abs(y[1]-previous[1])});previous=y;
        }
        countAllocations=false;CHECK(accepted&&finite);CHECK(allocations==0);CHECK(peak<2&&jump<.1);
        CHECK(eq.setAudition(std::nullopt));for(int n=0;n<int(rate*.02);++n)(void)eq.process({0,0});CHECK(!eq.auditioning());
        std::cout<<"AUDITION_AUTOMATION rate="<<rate<<" peak="<<peak<<" jump="<<jump<<" allocations="<<allocations<<'\n';
    }
}
void allocationProof()
{
    std::cout << "SECTION prepared_allocation_and_capacity\n";
    std::array<DynamicEq,8> eqs;
    DynamicEqSettings s;
    for (std::size_t i = 0; i < 8; ++i) {
        auto& b = s.bands[i]; b.active = true; b.frequencyHz = 40.f*float(std::pow(2.,i)); b.rangeDb = -6;
    }
    allocations = 0; countAllocations = true;
    for (auto& eq : eqs) { CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000)); }
    double peak = 0;
    for (int n = 0; n < 24000; ++n) {
        if (n % 64 == 0 || n % 512 == 0 || n % 139 == 0) {
            auto& b = s.bands[(n/64)%8]; b.shape = DynamicEqShape((n/64)%6);
            b.bypassed = n%128 == 0; b.frequencyHz = n%128 == 0 ? 20.f : 20000.f;
            b.rangeDb = n%128 == 0 ? 6.f : -6.f;
            for (auto& eq : eqs) CHECK(eq.setSettings(s));
        }
        if (n == 12000) for (auto& eq : eqs) { eq.reset(); CHECK(eq.prepare(96000)); }
        for (auto& eq : eqs) {
            const auto y = eq.process({.001*std::sin(n*.17),.002*std::cos(n*.11)});
            CHECK(std::isfinite(y[0]) && std::isfinite(y[1])); peak = std::max({peak,std::abs(y[0]),std::abs(y[1])});
        }
    }
    countAllocations = false;
    std::cout << "ALLOCATIONS count=" << allocations << " peak=" << peak << '\n'; CHECK(allocations == 0);
}
void transitionTails()
{
    std::cout << "SECTION tails_during_parameter_and_bypass_fades\n";
    auto s = band(DynamicEqShape::bell,24); s.bands[0].frequencyHz = 20; s.bands[0].q = 18;
    DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(48000));
    eq.process({1,0}); const auto ringing = eq.tailSeconds(); CHECK(ringing > 1);
    s.bands[0].gainDb = 0; CHECK(eq.setSettings(s)); CHECK(eq.tailSeconds() > .005);
    for (int i = 0; i < 1000; ++i) eq.process({0,0}); CHECK(eq.tailSeconds() == 0);
    s.bands[0].gainDb = 24; CHECK(eq.setSettings(s)); eq.reset(); eq.process({1,0});
    s.bands[0].bypassed = true; CHECK(eq.setSettings(s)); CHECK(eq.tailSeconds() > .005);
    for (int i = 0; i < 1000; ++i) eq.process({0,0}); CHECK(eq.tailSeconds() == 0);
    s.bands[0].bypassed = false; s.bands[0].gainDb = 0; CHECK(eq.setSettings(s)); eq.reset();
    s.bands[0].gainDb = 9; CHECK(eq.setSettings(s));
    s.bands[0].gainDb = 0; CHECK(eq.setSettings(s));
    for (int i = 0; i < 1000; ++i) eq.process({0,0}); CHECK(eq.tailSeconds() == 0);
}
void numericalStress()
{
    std::cout << "SECTION boundary_response_and_automation_stress\n";
    double responsePeak = 0, audioPeak = 0, jumpPeak = 0;
    for (const double rate : {44100.,48000.,96000.}) {
        for (int shape = 0; shape < 6; ++shape) for (float hz : {20.f,20000.f})
            for (float q : {.25f,18.f}) for (float gain : {-24.f,24.f}) for (float range : {-24.f,24.f}) {
                auto s = band(DynamicEqShape(shape),gain); auto& b = s.bands[0]; b.frequencyHz = hz; b.q = q; b.rangeDb = range;
                for (int step = 0; step <= 4; ++step) for (int point = 0; point <= 64; ++point) {
                    const auto f = rate*.499*point/64.;
                    const auto dynamic = (std::clamp(gain+range,-24.f,24.f)-gain)*step/4;
                    const auto h = dynamicEqBandResponse(b,rate,f,dynamic);
                    CHECK(h && std::isfinite(h->real()) && std::isfinite(h->imag()));
                    if (h) responsePeak = std::max(responsePeak,std::abs(*h));
                }
                // Actual audio at the admitted corner independently verifies
                // static response; dynamic audio/timing is measured above.
                b.rangeDb = 0; DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(rate));
                const auto f = std::min(double(hz),.45*rate);
                const auto measured = measuredGainDb(eq,rate,f);
                const auto h = dynamicEqResponse(s,rate,f); CHECK(h.has_value());
                // Low 20 Hz/high-Q filters may need their reported tail to
                // settle; center-notch cancellation is checked separately.
                if (hz > 20 && shape != int(DynamicEqShape::notch) && h)
                    CHECK(std::abs(measured-20*std::log10(std::abs(*h))) < .2);
            }
        auto s = band(); DynamicEq eq; CHECK(eq.setSettings(s)); CHECK(eq.prepare(rate));
        DynamicEq::Frame previous {};
        for (int n = 0; n < int(rate); ++n) {
            if (n%64 == 0) {
                auto& b = s.bands[0]; const auto step = n/64;
                b.frequencyHz = step%2 ? 20.f : 20000.f; b.q = step%2 ? 18.f : .25f;
                b.gainDb = step%2 ? 24.f : -24.f; b.rangeDb = -b.gainDb;
                b.shape = DynamicEqShape((step/17)%6); b.slope = DynamicEqSlope((step/13)%2);
                b.active = step%31 != 0; b.bypassed = step%23 == 0; CHECK(eq.setSettings(s));
            }
            const auto y = eq.process({.001*std::sin(n*.17),.001*std::cos(n*.11)});
            CHECK(std::isfinite(y[0]) && std::isfinite(y[1]));
            audioPeak = std::max({audioPeak,std::abs(y[0]),std::abs(y[1])});
            jumpPeak = std::max({jumpPeak,std::abs(y[0]-previous[0]),std::abs(y[1]-previous[1])}); previous = y;
        }
    }
    std::cout << "STRESS response_peak=" << responsePeak << " audio_peak=" << audioPeak << " adjacent_jump=" << jumpPeak << '\n';
    CHECK(responsePeak < 4096); CHECK(audioPeak < 2); CHECK(jumpPeak < .1);
}
void profile(bool diagnose = false, bool gainEnabled = false, int placementMode = -1)
{
    std::cout << "SECTION release_eight_EQ_eight_dynamic_bands_profile\n";
    using Clock = std::chrono::steady_clock;
    const auto rates = diagnose ? std::array<double,3> {96000,48000,44100} : std::array<double,3> {44100,48000,96000};
    for (const double rate : rates) for (const int block : {64,512}) {
        std::array<DynamicEq,8> eqs; DynamicEqSettings s;
        for (std::size_t i = 0; i < 8; ++i) {
            auto& b = s.bands[i]; b.active = true; b.frequencyHz = 40.f*float(std::pow(2.,i));
            b.q = 2; b.thresholdDb = -60; b.rangeDb = i%2 ? 6.f : -6.f;
            if (gainEnabled) b.gainDb = 3+.25f*float(i);
            if(placementMode>=0)b.placement=placementMode==0?DynamicEqPlacement::mid:placementMode==1?DynamicEqPlacement::side:DynamicEqPlacement(i%5);
        }
        if (gainEnabled) { s.autoGain=true; s.gainScale=1.5f; CHECK(dynamicEqAutoGainDb(s)<-.1f); }
        for (auto& eq : eqs) { CHECK(eq.setSettings(s)); CHECK(eq.prepare(rate)); }
        const auto blocks = int(std::ceil(rate*2/block));
        std::vector<double> durations(std::size_t(blocks),0.);
        std::array<DynamicEq::Frame,512> input;
        std::array<std::array<DynamicEq::Frame,512>,8> rendered;
        double checksum = 0; bool finite = true; allocations = 0; frees = 0;
        std::array<int,64> cpuBlocks {}; std::array<double,64> cpuTime {};
        const auto started = Clock::now();
        for (int trial = 0; trial < blocks*2; ++trial) {
            const auto before = Clock::now(); countAllocations = true;
            if (gainEnabled && trial%8==0) {
                s.gainScale=.75f+.75f*float(.5+.5*std::sin(trial*.017));
                for (auto& eq : eqs) CHECK(eq.setSettings(s));
            }
            // Include source generation, all eight independent input copies,
            // the real block API and the original frame/slot checksum order.
            for (int frame = 0; frame < block; ++frame) {
                const auto n = trial*block+frame;
                input[std::size_t(frame)] = {.25*std::sin(n*.13),.5*std::cos(n*.07)};
            }
            for (std::size_t slot = 0; slot < eqs.size(); ++slot) {
                std::copy_n(input.begin(),block,rendered[slot].begin());
                eqs[slot].processBlock(std::span(rendered[slot]).first(std::size_t(block)));
            }
            for (int frame = 0; frame < block; ++frame) for (const auto& output : rendered) {
                const auto& y = output[std::size_t(frame)]; checksum += y[0]+y[1];
                finite = finite && std::isfinite(y[0]) && std::isfinite(y[1]);
            }
            countAllocations = false;
            const auto elapsed = std::chrono::duration<double,std::milli>(Clock::now()-before).count();
            if (trial >= blocks) durations[std::size_t(trial-blocks)] = elapsed;
#if defined(_WIN32)
            if (diagnose && trial >= blocks) {
                const auto cpu = GetCurrentProcessorNumber();
                if (cpu < cpuBlocks.size()) { ++cpuBlocks[cpu]; cpuTime[cpu] += elapsed; }
            }
#endif
        }
        const auto wall = std::chrono::duration<double>(Clock::now()-started).count();
        std::sort(durations.begin(),durations.end());
        const auto p50 = durations[durations.size()/2], p99 = durations[std::size_t((durations.size()-1)*.99)];
        const auto period = block/rate*1000;
        std::cout << "PROFILE rate=" << rate << " block=" << block << " p50_ms=" << p50 << " p99_ms=" << p99
                  << " api=block independent_eqs=8 staging_and_accumulation_timed=1 worst_ms=" << durations.back() << " period_ms=" << period << " wall_s=" << wall
                  << " allocations=" << allocations << " finite=" << finite << " checksum=" << checksum
                  << " placement_mode=" << placementMode << " auto_gain=" << gainEnabled << " gain_scale=" << s.gainScale << " frees=" << frees << '\n';
        if (diagnose) for (std::size_t cpu = 0; cpu < cpuBlocks.size(); ++cpu) if (cpuBlocks[cpu])
            std::cout << "CPU core=" << cpu << " blocks=" << cpuBlocks[cpu] << " mean_ms=" << cpuTime[cpu]/cpuBlocks[cpu] << '\n';
        for (const auto& eq : eqs) for (const auto gain : eq.dynamicGainsDb()) CHECK(std::abs(gain) > .1f);
        CHECK(finite); CHECK(allocations == 0); CHECK(p99 < period*.5);
        if (gainEnabled) { CHECK(frees==0); for (const auto& eq : eqs) CHECK(eq.settings().autoGain && eq.settings().gainScale==s.gainScale); }
    }
}
void blockEquivalence()
{
    std::cout << "SECTION exact_frame_and_band_block_processing\n";
    for(double rate : {48000.,96000.,192000.}) {
        DynamicEq frameEq, blockEq; DynamicEqSettings settings;
        std::array<DynamicEq::Frame,514> input{}, expected{};
        input[1]={.25,-.5}; blockEq.processBlock(std::span(input).subspan(1,1)); CHECK(input[1]==DynamicEq::Frame({.25,-.5}));
        CHECK(frameEq.prepare(rate)&&blockEq.prepare(rate));
        std::uint32_t random=1;
        for(int edit=0;edit<100;++edit) {
            for(std::size_t band=0;band<8;++band) {
                auto& b=settings.bands[band];b.active=(edit+band)%13!=0;b.bypassed=(edit+band)%17==0;
                b.shape=DynamicEqShape((edit/7+band)%6);b.slope=DynamicEqSlope((edit/3+band)%2);
                b.frequencyHz=40+float((edit*137+band*881)%19000);b.q=.25f+float((edit+band)%12);
                b.gainDb=float(edit%49)-24;b.rangeDb=band%2?12.f:-12.f;b.thresholdDb=-36;
                b.attackMs=edit%2?.1f:100.f;b.releaseMs=edit%3?10.f:500.f;
            }
            settings.outputGainDb=float(edit%9)-4;
            CHECK(frameEq.setSettings(settings)&&blockEq.setSettings(settings));
            CHECK(frameEq.setSettings(settings)&&blockEq.setSettings(settings));
            if(edit%19==0){frameEq.reset();blockEq.reset();}
            const auto audition=edit%11==0?std::optional<std::size_t>(1):std::nullopt;
            CHECK(frameEq.setAudition(audition)==blockEq.setAudition(audition));
            const int count=std::array{1,7,64,511,513}[std::size_t(edit)%5];
            for(int n=1;n<=count;++n){random=random*1664525u+1013904223u;input[std::size_t(n)]={double(random>>8)/8388608.-1.,.25*std::sin(double(edit*513+n)*.17)};expected[std::size_t(n)]=frameEq.process(input[std::size_t(n)]);}
            allocations=0;countAllocations=true;blockEq.processBlock(std::span(input).subspan(1,std::size_t(count)));blockEq.processBlock({});countAllocations=false;CHECK(allocations==0);
            for(int n=1;n<=count;++n)for(std::size_t channel=0;channel<2;++channel)CHECK(std::bit_cast<std::uint64_t>(input[std::size_t(n)][channel])==std::bit_cast<std::uint64_t>(expected[std::size_t(n)][channel]));
            CHECK(frameEq.dynamicGainsDb()==blockEq.dynamicGainsDb());CHECK(frameEq.tailSeconds()==blockEq.tailSeconds());
            if(edit%23==0){CHECK(!frameEq.prepare(0)&&!blockEq.prepare(0));}
        }
    }
}
void automationFingerprint()
{
    std::cout<<"SECTION exact_settled_and_moving_coefficient_publication_audio\n";
    constexpr std::array<std::uint64_t,3> expected {0x6259a1c23be25c83ull,0xd3fc7273e6cfb404ull,0xae20315179bf6279ull};
    std::size_t scene=0;
    for(double rate:{48000.,96000.,192000.}){
        DynamicEq eq;DynamicEqSettings initial;
        for(std::size_t i=0;i<8;++i){auto& b=initial.bands[i];b.active=true;b.shape=static_cast<DynamicEqShape>(i%3);b.frequencyHz=80+700*float(i);b.gainDb=i%2?3.f:-3.f;b.q=.7f+.13f*float(i);b.rangeDb=i%2?12.f:-12.f;b.thresholdDb=-30;}
        CHECK(eq.setSettings(initial)&&eq.prepare(rate));auto settings=initial;
        const int ramp=int(std::ceil(rate*.005));int sample=0;std::uint32_t random=1;std::uint64_t hash=14695981039346656037ull;std::vector<double> raw;raw.reserve(std::size_t(rate)*2);
        const auto run=[&](int frames){bool finite=true;for(int n=0;n<frames;++n){random=random*1664525u+1013904223u;const double noise=double(random>>8)/8388608.-1;
            const auto y=eq.process({.2*std::sin(.017*sample)+.03*noise,-.17*std::cos(.031*sample)+.02*noise});++sample;
            for(double value:y){finite=finite&&std::isfinite(value);raw.push_back(value);const auto bits=std::bit_cast<std::uint64_t>(value);for(int k=0;k<8;++k)hash=(hash^((bits>>(8*k))&255))*1099511628211ull;}}
            CHECK(finite);
        };
        run(int(rate*.012));for(std::size_t i=0;i<8;++i){auto& b=settings.bands[i];b.gainDb=i%2?-18.f:18.f;b.rangeDb=-b.rangeDb;b.frequencyHz=i==0?20.f:i==7?20000.f:650+1100*float(i);b.q=i==7?18.f:1.2f;}CHECK(eq.setSettings(settings));run(ramp-1);
        for(std::size_t i=0;i<8;++i){auto& b=settings.bands[i];b.shape=static_cast<DynamicEqShape>(i%6);b.slope=DynamicEqSlope::db24;b.bypassed=i==3;b.active=i!=5;}settings.outputGainDb=-4;CHECK(eq.setSettings(settings));run(ramp+1);
        CHECK(eq.setSettings(initial));run(ramp/2);settings=initial;settings.bands[0].gainDb=12;settings.bands[1].rangeDb=-20;CHECK(eq.setSettings(settings));run(2*ramp+1);
        for(int i=0;i<4;++i){CHECK(eq.setSettings(settings));run(37);}CHECK(!eq.prepare(7999));run(103);
        CHECK(eq.setAudition(0));run(2*ramp);CHECK(eq.setAudition(std::nullopt));run(2*ramp);eq.reset();run(137);
        CHECK(eq.prepare(rate==192000?48000:rate*2));run(1467);
        CHECK(hash==expected[scene++]);std::cout<<"AUTOMATION_FINGERPRINT rate="<<rate<<" frames="<<sample<<" fnv="<<std::hex<<hash<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_EQ_AUTOMATION_CHARACTERIZATION_DIR")){
            const std::filesystem::path folder(directory);const auto workspace=std::filesystem::current_path().string()+std::string(1,std::filesystem::path::preferred_separator);const bool owned=folder.is_absolute()&&folder.lexically_normal().string().starts_with(workspace);CHECK(owned);
            if(owned){std::filesystem::create_directories(folder);const auto file=folder/("automation-"+std::to_string(int(rate))+".f64");CHECK(!std::filesystem::exists(file));if(!std::filesystem::exists(file)){std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(raw.data()),std::streamsize(raw.size()*sizeof(double)));CHECK(out.good());}}
        }
    }
}

void placementAdmissionAndSideNull()
{
    std::cout << "SECTION placement_default_validation_matrix_and_pure_mid_null\n";
    auto settings = band(DynamicEqShape::bell, 12);
    CHECK(settings.bands[0].placement == DynamicEqPlacement::stereo);
    auto invalid = settings;
    invalid.bands[7].placement = static_cast<DynamicEqPlacement>(255);
    CHECK(!validateDynamicEq(invalid));
    CHECK(!dynamicEqMatrixResponse(invalid,48000,1000));
    settings.bands[0].placement = DynamicEqPlacement::side;
    DynamicEq eq;
    CHECK(eq.setSettings(settings)); CHECK(eq.prepare(48000));
    double error=0;
    for(int i=0;i<24000;++i) {
        const double x=.01*std::sin(2*std::numbers::pi*1000*i/48000.);
        const auto y=eq.process({x,x});
        if(i>1000)error=std::max({error,std::abs(y[0]-x),std::abs(y[1]-x)});
    }
    CHECK(error<1.e-12);
    const auto matrix=dynamicEqMatrixResponse(settings,48000,1000);
    CHECK(matrix.has_value());
    if(matrix){
        const auto gain=std::pow(10.,12./20.);
        CHECK(std::abs((*matrix)[0]-std::complex<double>((1.+gain)*.5,0))<1.e-10);
        CHECK(std::abs((*matrix)[1]-std::complex<double>((1.-gain)*.5,0))<1.e-10);
        CHECK(std::abs((*matrix)[2]-(*matrix)[1])<1.e-12);
        CHECK(std::abs((*matrix)[3]-(*matrix)[0])<1.e-12);
    }
}


std::array<std::complex<double>,2> referenceComponent(std::array<std::complex<double>,2> input,
    DynamicEqPlacement placement,std::complex<double> h)
{
    const auto l=input[0],r=input[1];
    switch(placement){
        case DynamicEqPlacement::stereo:return {h*l,h*r};
        case DynamicEqPlacement::left:return {h*l,r};
        case DynamicEqPlacement::right:return {l,h*r};
        case DynamicEqPlacement::mid:{const auto m=h*(l+r)*.5,side=(l-r)*.5;return {m+side,m-side};}
        case DynamicEqPlacement::side:{const auto m=(l+r)*.5,side=h*(l-r)*.5;return {m+side,m-side};}
    }
    return {};
}
void placementMatrixAndAudio()
{
    std::cout<<"SECTION placement_matrix_independent_component_tones_all_shapes_and_rates\n";
    for(double rate:{48000.,96000.})for(int mode=0;mode<5;++mode)for(int shape=0;shape<6;++shape){
        auto settings=band(DynamicEqShape(shape),9);settings.bands[0].placement=DynamicEqPlacement(mode);
        settings.bands[0].frequencyHz=1300;settings.bands[0].q=.8f;settings.outputGainDb=-3;settings.gainScale=1.5f;settings.autoGain=true;
        constexpr double hz=997;
        const auto h=dynamicEqBandResponse(settings.bands[0],rate,hz,0,settings.gainScale);
        const auto matrix=dynamicEqMatrixResponse(settings,rate,hz);CHECK(h&&matrix);if(!h||!matrix)continue;
        const auto gain=std::pow(10.,(settings.outputGainDb+dynamicEqAutoGainDb(settings))/20.);
        for(int column=0;column<2;++column){std::array<std::complex<double>,2> unit{};unit[column]=1.;const auto expected=referenceComponent(unit,settings.bands[0].placement,*h);
            CHECK(std::abs((*matrix)[column]-gain*expected[0])<1.e-6);CHECK(std::abs((*matrix)[2+column]-gain*expected[1])<1.e-6);}
        const std::array<std::complex<double>,2> source{{{.009,.004},{-.003,.001}}};
        auto expected=referenceComponent(source,settings.bands[0].placement,*h);for(auto& v:expected)v*=gain;
        DynamicEq eq;CHECK(eq.setSettings(settings)&&eq.prepare(rate));double error=0;
        const auto count=int(rate*.15);for(int n=0;n<count;++n){const auto phasor=std::polar(1.,2*std::numbers::pi*hz*n/rate);
            const auto y=eq.process({(source[0]*phasor).real(),(source[1]*phasor).real()});
            if(n>count-1000)for(int ch=0;ch<2;++ch)error=std::max(error,std::abs(y[ch]-(expected[ch]*phasor).real()));}
        CHECK(error<2.e-8);
    }
    DynamicEqSettings empty;CHECK(!dynamicEqMatrixResponse(empty,0,1000));CHECK(!dynamicEqMatrixResponse(empty,48000,24001));CHECK(!dynamicEqMatrixResponse(empty,48000,std::numeric_limits<double>::quiet_NaN()));
}
void placementOrderAndNullChannels()
{
    std::cout<<"SECTION placement_physical_band_order_noncommutativity_and_exact_untouched_channels\n";
    DynamicEqSettings settings;for(int i=0;i<3;++i){auto& b=settings.bands[i];b.active=true;b.placement=std::array{DynamicEqPlacement::left,DynamicEqPlacement::mid,DynamicEqPlacement::side}[i];b.frequencyHz=float(700+1300*i);b.gainDb=float(6-7*i);b.q=.8f;}
    const auto matrix=dynamicEqMatrixResponse(settings,48000,997);CHECK(matrix);if(matrix){
        for(int column=0;column<2;++column){std::array<std::complex<double>,2> expected{};expected[column]=1.;
            for(const auto& b:settings.bands)if(b.active){const auto h=dynamicEqBandResponse(b,48000,997);CHECK(h);if(h)expected=referenceComponent(expected,b.placement,*h);}
            CHECK(std::abs((*matrix)[column]-expected[0])<1.e-12);CHECK(std::abs((*matrix)[2+column]-expected[1])<1.e-12);}
        auto reversed=settings;std::reverse(reversed.bands.begin(),reversed.bands.end());const auto other=dynamicEqMatrixResponse(reversed,48000,997);CHECK(other);if(other)CHECK(std::abs((*other)[1]-(*matrix)[1])>.001);
        const std::array<std::complex<double>,2> source{{{.01,.003},{-.004,.007}}};auto expected=source;
        for(const auto& b:settings.bands)if(b.active){const auto h=dynamicEqBandResponse(b,48000,997);CHECK(h);if(h)expected=referenceComponent(expected,b.placement,*h);}
        DynamicEq eq;CHECK(eq.setSettings(settings)&&eq.prepare(48000));double error=0;
        for(int n=0;n<12000;++n){const auto phasor=std::polar(1.,2*std::numbers::pi*997*n/48000.);const auto y=eq.process({(source[0]*phasor).real(),(source[1]*phasor).real()});if(n>6000)for(int ch=0;ch<2;++ch)error=std::max(error,std::abs(y[ch]-(expected[ch]*phasor).real()));}CHECK(error<1.e-10);
    }
    for(const auto placement:{DynamicEqPlacement::left,DynamicEqPlacement::right}){
        auto s=band();s.bands[0].placement=placement;DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));bool exact=true;
        for(int n=0;n<2048;++n){const double x=.01*std::sin(n*.13);const double untouched=n%2?-0.:.003;
            const DynamicEq::Frame input=placement==DynamicEqPlacement::left?DynamicEq::Frame{x,untouched}:DynamicEq::Frame{untouched,x};const auto output=eq.process(input);const auto ch=placement==DynamicEqPlacement::left?1:0;exact=exact&&std::bit_cast<std::uint64_t>(output[ch])==std::bit_cast<std::uint64_t>(input[ch]);}
        CHECK(exact);
    }
    for(int mode=0;mode<5;++mode)for(const bool bypass:{false,true}){auto s=band();s.bands[0].placement=DynamicEqPlacement(mode);s.bands[0].active=bypass;s.bands[0].bypassed=bypass;DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));bool exact=true;
        for(int n=0;n<512;++n){DynamicEq::Frame input{std::sin(n*.13)*.1,std::cos(n*.07)*.1};exact=exact&&eq.process(input)==input;}CHECK(exact);}
}
DynamicEq::Frame componentTone(DynamicEqPlacement placement,bool selected,double x)
{
    switch(placement){
        case DynamicEqPlacement::left:return selected?DynamicEq::Frame{x,0}:DynamicEq::Frame{0,x};
        case DynamicEqPlacement::right:return selected?DynamicEq::Frame{0,x}:DynamicEq::Frame{x,0};
        case DynamicEqPlacement::mid:return selected?DynamicEq::Frame{x,x}:DynamicEq::Frame{x,-x};
        case DynamicEqPlacement::side:return selected?DynamicEq::Frame{x,-x}:DynamicEq::Frame{x,x};
        default:return {x,x};
    }
}
void placementDetectorAndAudition()
{
    std::cout<<"SECTION placement_selected_component_detector_compression_expansion_and_audition\n";
    for(int mode=1;mode<5;++mode)for(float range:{-9.f,9.f})for(bool selected:{false,true}){
        auto s=band(DynamicEqShape::bell,0);s.bands[0].placement=DynamicEqPlacement(mode);s.bands[0].rangeDb=range;s.bands[0].thresholdDb=-45;s.bands[0].attackMs=1;
        DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));float movement=0;
        for(int n=0;n<12000;++n){eq.process(componentTone(s.bands[0].placement,selected,.04*std::sin(2*std::numbers::pi*1000*n/48000.)));if(n>6000)movement=std::max(movement,std::abs(eq.dynamicGainsDb()[0]));}
        CHECK(selected?movement>1:movement==0);
    }
    for(int mode=1;mode<5;++mode){auto s=band();s.bands[0].placement=DynamicEqPlacement(mode);DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000)&&eq.setAudition(0));double peak=0;
        for(int n=0;n<6000;++n){const auto y=eq.process(componentTone(s.bands[0].placement,false,.04*std::sin(n*.13)));if(n>1000)peak=std::max({peak,std::abs(y[0]),std::abs(y[1])});}CHECK(peak<1.e-12);
    }
}
#if !defined(_DEBUG)
void extendedBlockPathIsExact()
{
    std::cout<<"SECTION extension_block_admission_exact_audio_and_physical_histories\n";
    const auto same=[](double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);};
    for(double rate:{8000.,48000.,96000.,192000.})for(int mode=0;mode<5;++mode)
    for(int kind=0;kind<11;++kind)for(float gain:{-12.f,12.f})for(float frequency:{20.f,1300.f,20000.f}){
        auto settings=band(kind<3?DynamicEqShape(6+kind):kind%2?DynamicEqShape::lowCut:DynamicEqShape::highCut,gain);
        auto& b=settings.bands[0];b.placement=DynamicEqPlacement(mode);b.frequencyHz=frequency;b.q=.9f;b.rangeDb=-9;
        if(kind>=3)b.slope=std::array{DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96}[std::size_t((kind-3)/2)];
        auto scalar=std::make_unique<BandState>(),blocked=std::make_unique<BandState>();
        scalar->install(b,rate,240,true);blocked->install(b,rate,240,true);
        alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
        for(int packet=0;packet<12;++packet){
            const auto count=std::array{1,2,7,16,31,32}[packet%6];std::array<DynamicEq::Frame,32> data{},expected{};
            for(int n=0;n<count;++n){const int t=packet*32+n;auto input=DynamicEq::Frame{.031*std::sin(.13*t),.047*std::cos(.071*t)};
                if(t%97==0)input={0.,-0.};else if(t%101==0)input={std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min()};
                data[n]=input;lanes[0]=input[0];lanes[1]=input[1];const auto output=scalar->process(StereoRegister::fromRawArray(lanes.data()),b,rate,240);
                output.copyToRawArray(lanes.data());expected[n]={lanes[0],lanes[1]};
            }
            allocations=frees=0;countAllocations=true;const bool used=blocked->processBlock(std::span(data).first(count),rate);countAllocations=false;
            CHECK(used);CHECK(allocations==0&&frees==0);if(!used)break;
            for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)CHECK(same(data[n][ch],expected[n][ch]));
            for(std::size_t pair=0;pair<4;++pair)for(std::size_t stage=0;stage<2;++stage){
                const auto& a=blocked->filters.extended.pairs[pair].history[stage];const auto& x=scalar->filters.extended.pairs[pair].history[stage];
                const std::array actual{a.x1,a.x2,a.y1,a.y2},old{x.x1,x.x2,x.y1,x.y2};
                for(std::size_t i=0;i<4;++i)for(std::size_t lane=0;lane<StereoRegister::size();++lane)CHECK(same(actual[i].get(lane),old[i].get(lane)));
            }
        }
    }
}
void componentBlockPathIsExact()
{
    std::cout<<"SECTION component_block_path_admission_and_exact_scalar_state\n";
    const auto same=[](double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);};
    const auto history=[&](const auto& a,const auto& b){
        CHECK(a.left==b.left&&a.stages==b.stages&&a.transparent==b.transparent);
        for(std::size_t stage=0;stage<a.history.size();++stage){
            const auto& x=a.history[stage];const auto& y=b.history[stage];
            const std::array old{x.x1,x.x2,x.y1,x.y2},next{y.x1,y.x2,y.y1,y.y2};
            for(std::size_t i=0;i<old.size();++i)for(std::size_t lane=0;lane<StereoRegister::size();++lane)
                CHECK(same(old[i].get(lane),next[i].get(lane)));
        }
    };
    for(double rate:{8000.,48000.,96000.,192000.})for(int mode=1;mode<5;++mode)
    for(auto shape:{DynamicEqShape::bell,DynamicEqShape::lowShelf,DynamicEqShape::highShelf})
    for(float gain:{-24.f,-6.f,0.f,6.f,24.f})for(float range:{-9.f,9.f}){
        auto settings=band(shape,gain);auto& b=settings.bands[0];b.placement=DynamicEqPlacement(mode);
        b.frequencyHz=1300;b.q=.9f;b.rangeDb=range;b.thresholdDb=-40;b.attackMs=.3f;b.releaseMs=37;
        CHECK(validateDynamicEq(settings));
        auto scalar=std::make_unique<BandState>(),blocked=std::make_unique<BandState>();
        scalar->install(b,rate,int(std::ceil(rate*.005)),true);blocked->install(b,rate,int(std::ceil(rate*.005)),true);
        alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
        for(int packet=0;packet<24;++packet){
            const auto count=std::array{1,2,7,16,31,32}[packet%6];std::array<DynamicEq::Frame,32> data{},expected{};
            for(int n=0;n<count;++n){const auto t=packet*32+n;auto input=DynamicEq::Frame{.031*std::sin(.13*t),.047*std::cos(.071*t)};
                if(t%97==0)input={0.,-0.};else if(t%101==0)input={std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min()};
                else if(t%211==0)input[1]=input[0];else if(t%223==0)input[1]=-input[0];
                data[n]=input;lanes[0]=input[0];lanes[1]=input[1];const auto output=scalar->process(StereoRegister::fromRawArray(lanes.data()),b,rate,int(std::ceil(rate*.005)));
                output.copyToRawArray(lanes.data());expected[n]={lanes[0],lanes[1]};
            }
            allocations=frees=0;countAllocations=true;const bool used=blocked->processBlock(std::span(data).first(count),rate);countAllocations=false;
            CHECK(used);CHECK(allocations==0&&frees==0);if(!used)break;
            for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)CHECK(same(data[n][ch],expected[n][ch]));
            CHECK(same(blocked->dynamicGain,scalar->dynamicGain));CHECK(same(blocked->previousDynamicGain,scalar->previousDynamicGain));CHECK(same(blocked->cachedMix,scalar->cachedMix));
            history(blocked->filters.legacy.filter,scalar->filters.legacy.filter);history(blocked->filters.legacy.endpoint,scalar->filters.legacy.endpoint);history(blocked->filters.legacy.detector,scalar->filters.legacy.detector);
        }
    }
}
void detectorBlockPathIsExact()
{
    std::cout<<"SECTION detector_block_admission_exact_audio_learning_and_physical_histories\n";
    const auto same=[](double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);};
    const auto history=[&](const Filter& a,const Filter& b){
        CHECK(a.left==b.left&&a.stages==b.stages&&a.transparent==b.transparent);
        for(std::size_t stage=0;stage<2;++stage){const auto& x=a.history[stage];const auto& y=b.history[stage];
            const std::array before{x.x1,x.x2,x.y1,x.y2},after{y.x1,y.x2,y.y1,y.y2};
            for(std::size_t n=0;n<4;++n)for(std::size_t lane=0;lane<StereoRegister::size();++lane)CHECK(same(before[n].get(lane),after[n].get(lane)));}
    };
    for(double rate:{48000.,192000.})for(int source=0;source<4;++source)for(int flags=0;flags<4;++flags)for(int placement=0;placement<5;++placement)
    for(auto shape:{DynamicEqShape::bell,DynamicEqShape::lowShelf,DynamicEqShape::highShelf})for(float range:{-9.f,9.f}){
        auto settings=band(shape,source%2?6.f:-6.f);auto& b=settings.bands[0];b.frequencyHz=1300;b.q=.9f;b.rangeDb=range;b.thresholdDb=-40;b.attackMs=.3f;b.releaseMs=37;
        b.detectorSource=DynamicEqDetectorSource(source);b.autoThreshold=(flags&1)!=0;b.adaptiveTiming=(flags&2)!=0;b.placement=DynamicEqPlacement(placement);b.detectorLowHz=80;b.detectorHighHz=5000;CHECK(validateDynamicEq(settings));
        auto scalar=std::make_unique<BandState>(),blocked=std::make_unique<BandState>();scalar->install(b,rate,240,true);blocked->install(b,rate,240,true);
        alignas(StereoRegister::SIMDRegisterSize) std::array<double,StereoRegister::size()> lanes{};
        for(int packet=0;packet<12;++packet){
            const auto count=std::array{1,7,16,32,31,2}[packet%6];std::array<DynamicEq::Frame,32> data{},external{},expected{};
            for(int n=0;n<count;++n){const auto t=packet*32+n;data[n]={.031*std::sin(.13*t),.047*std::cos(.071*t)};external[n]={.017*std::cos(.037*t),.023*std::sin(.121*t)};
                if(t%97==0)data[n]={0.,-0.};if(packet%3==2)external[n]={};
                lanes[0]=external[n][0];lanes[1]=external[n][1];const auto trigger=StereoRegister::fromRawArray(lanes.data());lanes[0]=data[n][0];lanes[1]=data[n][1];
                const auto output=scalar->process(StereoRegister::fromRawArray(lanes.data()),b,rate,240,trigger);output.copyToRawArray(lanes.data());expected[n]={lanes[0],lanes[1]};
            }
            allocations=frees=0;countAllocations=true;const bool used=blocked->processBlock(std::span(data).first(count),rate,std::span<const DynamicEq::Frame>(external).first(count));countAllocations=false;
            CHECK(used);CHECK(allocations==0&&frees==0);if(!used)break;
            for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch)CHECK(same(data[n][ch],expected[n][ch]));
            CHECK(same(blocked->dynamicGain,scalar->dynamicGain));CHECK(same(blocked->previousDynamicGain,scalar->previousDynamicGain));CHECK(same(blocked->cachedMix,scalar->cachedMix));CHECK(same(blocked->learnedPower,scalar->learnedPower));
            history(blocked->filters.legacy.filter,scalar->filters.legacy.filter);history(blocked->filters.legacy.endpoint,scalar->filters.legacy.endpoint);history(blocked->filters.legacy.detector,scalar->filters.legacy.detector);
        }
    }
}
#endif

void placementBlocksTransitionsAndHeap()
{
    std::cout<<"SECTION placement_frame_block_exact_partitions_changes_reset_and_heap\n";
    for(double rate:{8000.,48000.,96000.})for(int mode=1;mode<5;++mode){auto s=band();s.bands[0].placement=DynamicEqPlacement(mode);s.bands[0].rangeDb=-8;
        s.bands[1]=s.bands[0];s.bands[1].placement=DynamicEqPlacement((mode+1)%5);s.bands[1].frequencyHz=1800;s.bands[1].rangeDb=6;
        DynamicEq frames,blocks;CHECK(frames.setSettings(s)&&blocks.setSettings(s)&&frames.prepare(rate)&&blocks.prepare(rate));bool exact=true,finite=true;std::array<DynamicEq::Frame,64> data{};
        allocations=frees=0;countAllocations=true;
        for(int packet=0;packet<90;++packet){if(packet==19||packet==43){s.bands[0].placement=DynamicEqPlacement(packet==19?(mode+2)%5:mode);frames.setSettings(s);blocks.setSettings(s);}if(packet==61){frames.reset();blocks.reset();}
            const auto count=std::array{1,7,31,64}[packet%4];std::array<DynamicEq::Frame,64> expected{};
            for(int n=0;n<count;++n){const auto t=packet*64+n;data[n]={.02*std::sin(t*.13),.03*std::cos(t*.07)};expected[n]=frames.process(data[n]);}
            blocks.processBlock(std::span(data).first(count));for(int n=0;n<count;++n)for(int ch=0;ch<2;++ch){finite=finite&&std::isfinite(data[n][ch]);exact=exact&&std::bit_cast<std::uint64_t>(data[n][ch])==std::bit_cast<std::uint64_t>(expected[n][ch]);}}
        countAllocations=false;CHECK(exact);CHECK(finite);CHECK(allocations==0&&frees==0);
    }
}


void placementDynamicMatrixAndModeTails()
{
    std::cout<<"SECTION placement_dynamic_matrix_coordinates_and_mode_history_isolation\n";
    for(int mode=0;mode<5;++mode)for(float range:{-9.f,9.f}){
        auto s=band(DynamicEqShape::bell,0);s.bands[0].placement=DynamicEqPlacement(mode);s.bands[0].rangeDb=range;
        std::array<float,8> movement{};movement[0]=range*.6f;
        const auto matrix=dynamicEqMatrixResponse(s,48000,997,movement);const auto h=dynamicEqBandResponse(s.bands[0],48000,997,movement[0]);CHECK(matrix&&h);
        if(matrix&&h)for(int column=0;column<2;++column){std::array<std::complex<double>,2> unit{};unit[column]=1.;const auto expected=referenceComponent(unit,s.bands[0].placement,*h);CHECK(std::abs((*matrix)[column]-expected[0])<1.e-12);CHECK(std::abs((*matrix)[2+column]-expected[1])<1.e-12);}
        movement[3]=std::numeric_limits<float>::quiet_NaN();CHECK(!dynamicEqMatrixResponse(s,48000,997,movement));
    }
    for(const auto first:{DynamicEqPlacement::left,DynamicEqPlacement::mid}){
        auto s=band();s.bands[0].placement=first;DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));
        for(int n=0;n<6000;++n)eq.process(componentTone(first,true,.04*std::sin(n*.13)));
        s.bands[0].placement=first==DynamicEqPlacement::left?DynamicEqPlacement::right:DynamicEqPlacement::side;CHECK(eq.setSettings(s));double leak=0;
        for(int n=0;n<3000;++n){const auto input=componentTone(s.bands[0].placement,false,.04*std::sin(n*.13));const auto y=eq.process(input);if(n>1000)leak=std::max({leak,std::abs(y[0]-input[0]),std::abs(y[1]-input[1])});}CHECK(leak<1.e-12);
    }
    auto s=band();s.bands[0].placement=DynamicEqPlacement::mid;DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000)&&eq.setAudition(0));
    for(int n=0;n<3000;++n)eq.process({.03*std::sin(n*.13),.03*std::sin(n*.13)});
    s.bands[0].placement=DynamicEqPlacement::side;CHECK(eq.setSettings(s));double leak=0;
    for(int n=0;n<3000;++n){const double x=.03*std::sin(n*.13);const auto y=eq.process({x,x});if(n>1000)leak=std::max({leak,std::abs(y[0]),std::abs(y[1])});}CHECK(leak<1.e-12);
}


void placementEnergyResponse()
{
    std::cout<<"SECTION placement_stereo_energy_response_and_exact_stereo_scalar_path\n";
    for(double rate:{48000.,96000.})for(int mode=0;mode<5;++mode){auto s=band(DynamicEqShape::bell,9);s.bands[0].placement=DynamicEqPlacement(mode);s.bands[0].rangeDb=-9;
        for(const float movement:{0.f,-5.f}){std::array<float,8> dynamics{};dynamics[0]=movement;
            const auto energy=dynamicEqEnergyResponse(s,rate,1000,dynamics);const auto matrix=dynamicEqMatrixResponse(s,rate,1000,dynamics);CHECK(energy&&matrix);if(energy&&matrix){double expected=0;for(const auto& entry:*matrix)expected+=std::norm(entry);expected=std::sqrt(expected*.5);CHECK(std::abs(*energy-expected)<1.e-12);}
            if(mode==0){const auto scalar=dynamicEqResponse(s,rate,1000,dynamics);CHECK(scalar&&energy);if(scalar&&energy)CHECK(std::bit_cast<std::uint64_t>(*energy)==std::bit_cast<std::uint64_t>(std::abs(*scalar)));}
        }
    }
    auto side=band(DynamicEqShape::bell,12);side.bands[0].placement=DynamicEqPlacement::side;
    const auto energy=dynamicEqEnergyResponse(side,48000,1000);CHECK(energy);if(energy)CHECK(std::abs(*energy-std::sqrt((std::pow(10.,12./10.)+1)*.5))<1.e-10);
    CHECK(!dynamicEqEnergyResponse(side,0,1000));CHECK(!dynamicEqEnergyResponse(side,48000,24001));std::array<float,8> invalid{};invalid[2]=std::numeric_limits<float>::quiet_NaN();CHECK(!dynamicEqEnergyResponse(side,48000,1000,invalid));
}

std::complex<double> extensionReference(const DynamicEqBand& b,double rate,double hz)
{
    using Complex=std::complex<double>;
    const auto at=[&](double center){return Complex(0,std::tan(std::numbers::pi*hz/rate)/std::tan(std::numbers::pi*center/rate));};
    const auto shelf=[](double gain,double q,Complex s,bool high){
        const double a=std::pow(10.,gain/40.),damping=std::sqrt(a)/q;
        return high ? a*(1.+damping*s+a*s*s)/(a+damping*s+s*s)
                    : a*(a+damping*s+s*s)/(1.+damping*s+a*s*s);
    };
    const auto s=at(std::min(double(b.frequencyHz),rate*.45));
    if(b.shape==DynamicEqShape::bandPass)return (s/double(b.q))/(s*s+s/double(b.q)+1.);
    if(b.shape==DynamicEqShape::tiltShelf)return shelf(-b.gainDb*.5,std::min(double(b.q),4.),s,false)*shelf(b.gainDb*.5,std::min(double(b.q),4.),s,true);
    if(b.shape==DynamicEqShape::flatTilt){
        Complex result=std::pow(10.,-b.gainDb/40.);
        for(double octave:{-2.5,-1.5,-.5,.5,1.5,2.5})
            result*=shelf(b.gainDb/6.,std::sqrt(.5),at(std::min(double(b.frequencyHz)*std::exp2(octave),rate*.49)),true);
        return result;
    }
    const int order=b.slope==DynamicEqSlope::db6?1:b.slope==DynamicEqSlope::db36?6:b.slope==DynamicEqSlope::db48?8:16;
    if(order==1)return b.shape==DynamicEqShape::lowCut?s/(1.+s):1./(1.+s);
    Complex result=1.;
    for(int k=0;k<order/2;++k){
        const double damping=2.*std::cos((2.*k+1.)*std::numbers::pi/(2.*order));
        result*=(b.shape==DynamicEqShape::lowCut?s*s:Complex(1.))/(s*s+damping*s+1.);
    }
    return result;
}
void extendedShapesAndSlopes()
{
    std::cout<<"SECTION extensions_independent_bilinear_transfer_audio_partitions_heap\n";
    for(double rate:{8000.,44100.,48000.,96000.})for(float frequency:{20.f,1000.f,20000.f})
        for(float quality:{.25f,float(std::sqrt(.5)),18.f})for(float gain:{-24.f,0.f,24.f})
            for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt}){
                auto settings=band(shape,gain);auto& b=settings.bands[0];b.frequencyHz=frequency;b.q=quality;
                CHECK(validateDynamicEq(settings));
                for(double hz:{10.,100.,1000.,rate*.49}){
                    const auto expected=extensionReference(b,rate,hz);const auto actual=dynamicEqBandResponse(b,rate,hz);CHECK(actual);
                    if(actual)CHECK(std::abs(*actual-expected)<2.e-7*std::max(1.,std::abs(expected)));
                }
            }
    for(double rate:{8000.,44100.,48000.,96000.})for(float frequency:{20.f,1000.f,20000.f})
        for(auto slope:{DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96})
            for(auto shape:{DynamicEqShape::lowCut,DynamicEqShape::highCut}){
                auto settings=band(shape,0);auto& b=settings.bands[0];b.slope=slope;b.frequencyHz=frequency;CHECK(validateDynamicEq(settings));
                for(double hz:{10.,100.,1000.,rate*.49}){
                    const auto expected=extensionReference(b,rate,hz);const auto actual=dynamicEqBandResponse(b,rate,hz);CHECK(actual);
                    if(actual)CHECK(std::abs(*actual-expected)<2.e-7*std::max(1.,std::abs(expected)));
                }
            }
    for(double rate:{44100.,48000.,96000.})
        for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt,DynamicEqShape::lowCut,DynamicEqShape::highCut}){
            auto settings=band(shape,12);if(shape==DynamicEqShape::lowCut||shape==DynamicEqShape::highCut)settings.bands[0].slope=DynamicEqSlope::db96;
            const double hz=shape==DynamicEqShape::bandPass?1000.:shape==DynamicEqShape::highCut?2000.:500.;
            const auto expected=20*std::log10(std::abs(extensionReference(settings.bands[0],rate,hz)));
            gainCase(settings,rate,hz,expected,.15);
        }
    for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt,DynamicEqShape::lowCut,DynamicEqShape::highCut}){
        auto settings=band(shape,12);if(shape==DynamicEqShape::lowCut||shape==DynamicEqShape::highCut)settings.bands[0].slope=DynamicEqSlope::db96;
        DynamicEq frame,block;CHECK(frame.setSettings(settings)&&block.setSettings(settings));CHECK(frame.prepare(48000)&&block.prepare(48000));
        std::array<DynamicEq::Frame,31> input{},expected{};bool exact=true,finite=true;
        allocations=frees=0;countAllocations=true;
        for(int packet=0;packet<100;++packet){
            for(int n=0;n<31;++n){const int t=packet*31+n;input[n]={.02*std::sin(.13*t),.03*std::cos(.07*t)};expected[n]=frame.process(input[n]);}
            block.processBlock(input);
            for(int n=0;n<31;++n)for(int ch=0;ch<2;++ch){finite&=std::isfinite(input[n][ch]);exact&=std::bit_cast<std::uint64_t>(input[n][ch])==std::bit_cast<std::uint64_t>(expected[n][ch]);}
        }
        countAllocations=false;CHECK(exact&&finite);CHECK(allocations==0&&frees==0);
    }
}

void extendedRuntimeContracts()
{
    std::cout<<"SECTION extension_component_audio_tail_transitions_and_prepared_memory\n";
    allocations=frees=allocatedBytes=0;countAllocations=true;
    {DynamicEq prepared;}
    countAllocations=false;CHECK(allocations==1&&frees==1&&allocatedBytes>0);
    CHECK(allocatedBytes<=16384);
    std::cout<<"EXTENSION_PREPARED bytes_per_eq="<<allocatedBytes<<" bytes_for_eight="<<8*allocatedBytes<<'\n';
#if !defined(_DEBUG)
    std::cout<<"EXTENSION_STORAGE band_state="<<sizeof(BandState)<<" shared_filter_storage="<<sizeof(ExtendedFilter)<<'\n';
#endif
    for(auto shape:{DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt})for(int mode=0;mode<5;++mode){
        auto settings=band(shape,0);settings.bands[0].placement=DynamicEqPlacement(mode);settings.bands[0].rangeDb=12;
        DynamicEq eq;CHECK(eq.setSettings(settings)&&eq.prepare(48000));bool exact=true;
        for(int n=0;n<2000;++n){const DynamicEq::Frame x=n%31?DynamicEq::Frame{.02*std::sin(.13*n),.03*std::cos(.07*n)}:DynamicEq::Frame{0.,-0.};const auto y=eq.process(x);
            for(int ch=0;ch<2;++ch)exact&=std::bit_cast<std::uint64_t>(x[ch])==std::bit_cast<std::uint64_t>(y[ch]);}
        CHECK(exact);
    }
    std::vector<DynamicEqSettings> configurations;
    for(auto shape:{DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt}){
        auto a=band(shape,0),b=band(shape,1.e-9f);DynamicEq zero,nearZero;
        CHECK(zero.setSettings(a)&&nearZero.setSettings(b)&&zero.prepare(48000)&&nearZero.prepare(48000));
        for(int n=0;n<1000;++n){zero.process({.01,-.02});nearZero.process({.01,-.02});}
        a.bands[0].gainDb=b.bands[0].gainDb=12;CHECK(zero.setSettings(a)&&nearZero.setSettings(b));double difference=0;
        for(int n=0;n<32;++n){const auto x=zero.process({.01,-.02}),y=nearZero.process({.01,-.02});for(int ch=0;ch<2;++ch)difference=std::max(difference,std::abs(x[ch]-y[ch]));}
        CHECK(difference<1.e-9);
        std::cout<<"ZERO_GAIN_EDIT shape="<<int(shape)<<" max_difference="<<difference<<'\n';
    }
    for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt})configurations.push_back(band(shape,12));
    for(auto slope:{DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96})
        for(auto shape:{DynamicEqShape::lowCut,DynamicEqShape::highCut}){auto s=band(shape,0);s.bands[0].slope=slope;configurations.push_back(s);}
    for(auto settings:configurations)for(int mode=0;mode<5;++mode){
        auto& b=settings.bands[0];b.placement=DynamicEqPlacement(mode);b.rangeDb=-7;b.thresholdDb=-60;
        DynamicEq eq;CHECK(eq.setSettings(settings)&&eq.prepare(48000));
        const auto declared=dynamicEqTailSeconds(settings,48000);CHECK(std::isfinite(declared)&&declared>0);
        CHECK(eq.tailSeconds()>=declared*.999999);
        using Complex=std::complex<double>;const std::array<Complex,2> input{1.,Complex(.3,.4)};
        const auto expected=referenceComponent(input,b.placement,extensionReference(b,48000,700));
        std::array<Complex,2> measured{};
        for(int n=0;n<9600;++n){
            const auto carrier=std::polar(1.,2*std::numbers::pi*700*n/48000.);
            const auto y=eq.process({.01*std::real(input[0]*carrier),.01*std::real(input[1]*carrier)});
            if(n>=4800)for(int ch=0;ch<2;++ch)measured[ch]+=y[ch]*std::conj(carrier)*(2./(.01*4800));
        }
        for(int ch=0;ch<2;++ch)CHECK(std::abs(measured[ch]-expected[ch])<1.e-7*std::max(1.,std::abs(expected[ch])));
        CHECK(eq.dynamicGainsDb()[0]==0);
    }
    auto settings=configurations.front();DynamicEq frame,block;
    CHECK(frame.setSettings(settings)&&block.setSettings(settings)&&frame.prepare(48000)&&block.prepare(48000));
    bool exact=true,finite=true;std::array<DynamicEq::Frame,31> data{},expected{};
    allocations=frees=0;countAllocations=true;
    for(int packet=0;packet<240;++packet){
        if(packet%17==0){settings=configurations[static_cast<std::size_t>(packet/17)%configurations.size()];settings.bands[0].placement=DynamicEqPlacement((packet/17)%5);settings.bands[0].bypassed=(packet/17)%3==0;settings.bands[0].q=packet%2?18.f:.25f;settings.bands[0].frequencyHz=packet%2?20:20000;settings.bands[0].gainDb=packet%2?-24:24;frame.setSettings(settings);block.setSettings(settings);}
        if(packet==121){frame.reset();block.reset();}
        const int size=std::array{1,7,16,31}[packet%4];
        for(int n=0;n<size;++n){const int t=packet*31+n;data[n]={.02*std::sin(.13*t),.03*std::cos(.07*t)};expected[n]=frame.process(data[n]);}
        block.processBlock(std::span(data).first(size));
        for(int n=0;n<size;++n)for(int ch=0;ch<2;++ch){exact&=std::bit_cast<std::uint64_t>(data[n][ch])==std::bit_cast<std::uint64_t>(expected[n][ch]);finite&=std::isfinite(data[n][ch]);}
    }
    countAllocations=false;CHECK(exact&&finite);CHECK(allocations==0&&frees==0);
}

void extensionStorageReuse()
{
    std::cout<<"SECTION legacy_extended_storage_reuse_reset_and_callback_heap\n";
    DynamicEq reused,reference;
    CHECK(reused.prepare(48000)&&reference.prepare(48000));
    bool exact=true,finite=true;
    allocations=frees=0;countAllocations=true;
    for(int cycle=0;cycle<24;++cycle){
        DynamicEqSettings next;
        for(std::size_t i=0;i<dynamicEqBandCount;++i){
            auto& b=next.bands[i];b.active=true;b.frequencyHz=100.f*float(i+1);b.gainDb=3;b.rangeDb=-6;
            const auto extension=(cycle/2+int(i))%5;
            b.shape=DynamicEqShape(cycle%2?(cycle/2+int(i))%6:extension<3?6+extension:extension);
            b.slope=cycle%2?DynamicEqSlope((cycle/2)%2):DynamicEqSlope::db96;
            b.placement=DynamicEqPlacement((cycle+int(i))%5);
        }
        CHECK(reused.setSettings(next)&&reference.setSettings(next));
        for(int n=0;n<600;++n){
            const auto y=reused.process({.02*std::sin(.13*n),.03*std::cos(.07*n)});
            finite&=std::isfinite(y[0])&&std::isfinite(y[1]);
        }
        reused.reset();reference.reset();
        std::array<DynamicEq::Frame,31> frames{},expected{};
        for(int packet=0;packet<20;++packet){
            for(std::size_t n=0;n<frames.size();++n){frames[n]={.02*std::sin(.13*(packet*31+n)),.03*std::cos(.07*(packet*31+n))};expected[n]=reference.process(frames[n]);}
            reused.processBlock(frames);
            for(std::size_t n=0;n<frames.size();++n)for(int ch=0;ch<2;++ch)
                exact&=std::bit_cast<std::uint64_t>(frames[n][ch])==std::bit_cast<std::uint64_t>(expected[n][ch]);
        }
    }
    countAllocations=false;CHECK(exact&&finite);CHECK(allocations==0&&frees==0);
}

struct ExtensionReferenceFilter
{
    using Digital=std::array<double,5>;
    std::array<Digital,8> sections{};
    std::array<std::array<std::array<double,2>,8>,2> states{};
    int count=0;double trim=1;
    void append(double rate,double center,std::array<double,3> numerator,std::array<double,3> denominator)
    {
        const double k=1/std::tan(std::numbers::pi*center/rate);
        const double a0=denominator[0]+denominator[1]*k+denominator[2]*k*k;
        auto& c=sections[static_cast<std::size_t>(count++)];
        if(denominator[2]==0){
            c={(numerator[0]+numerator[1]*k)/a0,(numerator[0]-numerator[1]*k)/a0,0,(denominator[0]-denominator[1]*k)/a0,0};
        }else{
            c={(numerator[0]+numerator[1]*k+numerator[2]*k*k)/a0,(2*numerator[0]-2*numerator[2]*k*k)/a0,(numerator[0]-numerator[1]*k+numerator[2]*k*k)/a0,
                (2*denominator[0]-2*denominator[2]*k*k)/a0,(denominator[0]-denominator[1]*k+denominator[2]*k*k)/a0};
        }
    }
    static ExtensionReferenceFilter make(const DynamicEqBand& b,double rate)
    {
        ExtensionReferenceFilter result;const double fc=std::min(double(b.frequencyHz),rate*.45);
        const auto shelf=[&](double gain,double center,double q,bool high){
            const double a=std::pow(10.,gain/40.),d=std::sqrt(a)/q;
            if(high)result.append(rate,center,{a,a*d,a*a},{a,d,1});
            else result.append(rate,center,{a*a,a*d,a},{1,d,a});
        };
        if(b.shape==DynamicEqShape::bandPass)result.append(rate,fc,{0,1./b.q,0},{1,1./b.q,1});
        else if(b.shape==DynamicEqShape::tiltShelf){shelf(-b.gainDb*.5,fc,std::min(double(b.q),4.),false);shelf(b.gainDb*.5,fc,std::min(double(b.q),4.),true);}
        else if(b.shape==DynamicEqShape::flatTilt){
            for(double octave:{-2.5,-1.5,-.5,.5,1.5,2.5})shelf(b.gainDb/6.,std::min(double(b.frequencyHz)*std::exp2(octave),rate*.49),std::sqrt(.5),true);
            result.trim=std::pow(10.,-b.gainDb/40.);
        }else{
            const bool high=b.shape==DynamicEqShape::lowCut;
            if(b.slope==DynamicEqSlope::db6)result.append(rate,fc,high?std::array{0.,1.,0.}:std::array{1.,0.,0.},{1,1,0});
            else {const int order=b.slope==DynamicEqSlope::db36?6:b.slope==DynamicEqSlope::db48?8:16;
                for(int i=0;i<order/2;++i)result.append(rate,fc,high?std::array{0.,0.,1.}:std::array{1.,0.,0.},{1,2*std::cos((2.*i+1)*std::numbers::pi/(2.*order)),1});}
        }
        return result;
    }
    double channel(double input,std::size_t channel)
    {
        for(int i=0;i<count;++i){const auto index=static_cast<std::size_t>(i);const auto& c=sections[index];auto& z=states[channel][index];
            const double y=c[0]*input+z[0];z[0]=c[1]*input-c[3]*y+z[1];z[1]=c[2]*input-c[4]*y;input=y;}
        return input*trim;
    }
    DynamicEq::Frame process(DynamicEq::Frame input,DynamicEqPlacement placement)
    {
        if(placement==DynamicEqPlacement::stereo)return {channel(input[0],0),channel(input[1],1)};
        const double selected=placement==DynamicEqPlacement::left?input[0]:placement==DynamicEqPlacement::right?input[1]:placement==DynamicEqPlacement::mid?(input[0]+input[1])*.5:(input[0]-input[1])*.5;
        const double delta=channel(selected,0)-selected;
        if(placement==DynamicEqPlacement::left)input[0]+=delta;
        else if(placement==DynamicEqPlacement::right)input[1]+=delta;
        else {input[0]+=delta;input[1]+=placement==DynamicEqPlacement::mid?delta:-delta;}
        return input;
    }
};
void extensionIndependentTransients()
{
    std::cout<<"SECTION extension_independent_analog_bilinear_DF2_transients_and_mixed_bands\n";
    std::vector<DynamicEqBand> types;
    for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt})types.push_back(band(shape).bands[0]);
    for(auto slope:{DynamicEqSlope::db6,DynamicEqSlope::db36,DynamicEqSlope::db48,DynamicEqSlope::db96})for(auto shape:{DynamicEqShape::lowCut,DynamicEqShape::highCut}){auto b=band(shape).bands[0];b.slope=slope;types.push_back(b);}
    std::size_t cases=0;double worst=0;
    for(double rate:{8000.,44100.,48000.,96000.})for(float center:{20.f,1000.f,20000.f})for(float q:{.25f,float(std::sqrt(.5)),18.f})for(float gain:{-24.f,0.f,24.f})
        for(auto b:types)for(int mode=0;mode<5;++mode){
            b.frequencyHz=center;b.q=q;b.gainDb=gain;b.rangeDb=-9;b.placement=DynamicEqPlacement(mode);
            DynamicEqSettings settings;settings.bands[0]=b;DynamicEq eq;CHECK(eq.setSettings(settings)&&eq.prepare(rate));auto reference=ExtensionReferenceFilter::make(b,rate);
            double error=0,peak=0;bool finite=true;allocations=frees=0;countAllocations=true;
            for(int n=0;n<512;++n){const DynamicEq::Frame x=n==0?DynamicEq::Frame{.01,-.02}:DynamicEq::Frame{.0003*std::sin(.13*n),.0004*std::cos(.07*n)};
                const auto y=eq.process(x),expected=reference.process(x,b.placement);
                for(int ch=0;ch<2;++ch){error=std::max(error,std::abs(y[ch]-expected[ch]));peak=std::max(peak,std::abs(expected[ch]));finite&=std::isfinite(y[ch])&&std::isfinite(expected[ch]);}}
            countAllocations=false;CHECK(finite&&error<2.e-8*std::max(1.,peak));CHECK(allocations==0&&frees==0);CHECK(eq.dynamicGainsDb()[0]==0);
            if(!finite||error>=2.e-8*std::max(1.,peak))std::cout<<"TRANSIENT_ERROR rate="<<rate<<" fc="<<center<<" q="<<q<<" gain="<<gain<<" shape="<<int(b.shape)<<" slope="<<int(b.slope)<<" mode="<<mode<<" error="<<error<<'\n';
            worst=std::max(worst,error);++cases;
        }
    CHECK(cases==5940);std::cout<<"EXTENSION_TRANSIENT cases="<<cases<<" frames="<<cases*512<<" max_abs_error="<<worst<<'\n';
    DynamicEqSettings mixed;std::array<ExtensionReferenceFilter,8> reference;
    for(std::size_t i=0;i<8;++i){mixed.bands[i]=types[std::array{0,1,2,3,5,7,9,10}[i]];auto& b=mixed.bands[i];b.placement=DynamicEqPlacement(i%5);b.frequencyHz=i<3?float(400+500*i):i==7?20000.f:20.f;b.gainDb=i%2?9.f:-9.f;reference[i]=ExtensionReferenceFilter::make(b,48000);}
    DynamicEq eq;CHECK(eq.setSettings(mixed)&&eq.prepare(48000));double error=0;
    for(int packet=0;packet<80;++packet){std::array<DynamicEq::Frame,31> frames{},expected{};const int size=std::array{1,7,16,31}[packet%4];
        for(int n=0;n<size;++n){const int t=packet*31+n;frames[n]={.01*std::sin(.13*t),.02*std::cos(.07*t)};expected[n]=frames[n];for(std::size_t i=0;i<8;++i)expected[n]=reference[i].process(expected[n],mixed.bands[i].placement);}
        eq.processBlock(std::span(frames).first(size));for(int n=0;n<size;++n)for(int ch=0;ch<2;++ch)error=std::max(error,std::abs(frames[n][ch]-expected[n][ch]));}
    CHECK(std::isfinite(error)&&error<2.e-8);std::cout<<"EXTENSION_MIXED max_abs_error="<<error<<'\n';
    for(double rate:{8000.,44100.,48000.,96000.})for(float center:{20.f,1000.f,20000.f})for(float gain:{-24.f,24.f}){
        auto s=band(DynamicEqShape::flatTilt,gain);s.bands[0].frequencyHz=center;double deviation=0,previous=gain>0?-100:100;
        for(int n=0;n<128;++n){const double hz=20*std::pow(std::min(20000.,rate*.49)/20,n/127.);const auto h=dynamicEqResponse(s,rate,hz);CHECK(h);
            if(h){const double db=20*std::log10(std::abs(*h)),ideal=std::clamp(double(gain)*std::log2(hz/center)/6.,-std::abs(double(gain))*.5,std::abs(double(gain))*.5);
                CHECK(std::isfinite(db)&&std::abs(db)<=std::abs(gain)*.5+1.e-6);CHECK(gain>0?db>=previous-1.e-7:db<=previous+1.e-7);previous=db;deviation=std::max(deviation,std::abs(db-ideal));}}
        std::cout<<"FLAT_TILT_APPROX rate="<<rate<<" fc="<<center<<" gain="<<gain<<" max_line_deviation_db="<<deviation<<'\n';
    }
}

void extensionGainControls()
{
    std::cout<<"SECTION extension_gain_scale_auto_gain_and_static_dynamics\n";
    for(auto shape:{DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt})for(float center:{20.f,1000.f,20000.f})for(float gain:{-12.f,12.f})for(float scale:{0.f,.5f,2.f}){
        auto s=band(shape,gain);s.bands[0].frequencyHz=center;s.bands[0].rangeDb=-9;s.gainScale=scale;s.autoGain=true;
        const auto before=s;auto effective=s.bands[0];effective.gainDb=std::clamp(gain*scale,-24.f,24.f);
        double mean=0;for(int n=0;n<4096;++n){const double hz=20*std::pow(1000.,(n+.5)/4096);double db=-effective.gainDb*.5;
            if(shape==DynamicEqShape::tiltShelf){if(hz>center)db+=effective.gainDb;}
            else for(double octave:{-2.5,-1.5,-.5,.5,1.5,2.5})if(hz>center*std::exp2(octave))db+=effective.gainDb/6.;
            mean+=db/4096;}
        CHECK(std::abs(dynamicEqAutoGainDb(s)-std::clamp(-mean,-12.,12.))<.01);
        for(double rate:{8000.,48000.,96000.}){
            const double hz=std::min(6000.,rate*.4);const auto h=dynamicEqBandResponse(s.bands[0],rate,hz,0,scale);CHECK(h);
            if(h)CHECK(std::abs(*h-extensionReference(effective,rate,hz))<1.e-10);
            for(int mode=0;mode<5;++mode){s.bands[0].placement=DynamicEqPlacement(mode);DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(rate));auto reference=ExtensionReferenceFilter::make(effective,rate);
                const double compensation=std::pow(10.,dynamicEqAutoGainDb(s)/20.);double error=0;bool unity=true;
                allocations=frees=0;countAllocations=true;
                for(int n=0;n<512;++n){const DynamicEq::Frame input=n==0?DynamicEq::Frame{0.,-0.}:DynamicEq::Frame{.005*std::sin(.13*n),.008*std::cos(.07*n)};
                    const auto output=eq.process(input);auto expected=reference.process(input,DynamicEqPlacement(mode));for(auto& v:expected)v*=compensation;
                    for(int ch=0;ch<2;++ch){error=std::max(error,std::abs(output[ch]-expected[ch]));if(scale==0)unity&=std::bit_cast<std::uint64_t>(output[ch])==std::bit_cast<std::uint64_t>(input[ch]);}}
                countAllocations=false;CHECK(error<1.e-8);CHECK(unity);CHECK(allocations==0&&frees==0);CHECK(eq.dynamicGainsDb()[0]==0);
            }
        }
        s.bands[0].placement=before.bands[0].placement;CHECK(s==before);
    }
    for(auto shape:{DynamicEqShape::bandPass,DynamicEqShape::lowCut,DynamicEqShape::highCut}){
        auto s=band(shape,12);s.bands[0].slope=DynamicEqSlope::db96;s.autoGain=true;s.gainScale=0;CHECK(dynamicEqAutoGainDb(s)==0);
        const auto a=dynamicEqResponse(s,48000,1000);s.gainScale=2;const auto b=dynamicEqResponse(s,48000,1000);CHECK(a&&b);if(a&&b)CHECK(*a==*b);
    }
}
}
namespace
{
void independentDetectorAdmission()
{
    std::cout<<"SECTION independent_detector_validation_and_atomic_refusal\n";
    DynamicEqSettings s;
    s.bands[0].detectorSource=DynamicEqDetectorSource::internalFree;
    s.bands[0].detectorLowHz=5000;s.bands[0].detectorHighHz=500;
    CHECK(!validateDynamicEq(s));
    DynamicEq eq;const auto before=eq.settings();CHECK(!eq.setSettings(s)&&eq.settings()==before);
    s.bands[0].detectorHighHz=8000;CHECK(validateDynamicEq(s));
    s.bands[0].detectorSource=static_cast<DynamicEqDetectorSource>(255);CHECK(!validateDynamicEq(s));
    s.bands[0].detectorSource=DynamicEqDetectorSource::internalBand;s.bands[0].detectorLowHz=std::numeric_limits<float>::quiet_NaN();CHECK(!validateDynamicEq(s));
}
struct DetectorToneResult {double gainDb=0;float movement=0;bool finite=true;};
DetectorToneResult detectorTone(DynamicEqDetectorSource source,double triggerHz,float movement)
{
    constexpr double rate=48000;constexpr int frames=48000;
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.frequencyHz=4000;b.q=4;b.rangeDb=movement;b.thresholdDb=-30;b.attackMs=1;b.releaseMs=100;
    b.detectorSource=source;b.detectorLowHz=60;b.detectorHighHz=140;
    DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(rate));
    const bool external=source==DynamicEqDetectorSource::externalBand||source==DynamicEqDetectorSource::externalFree;
    double sine=0,cosine=0;DetectorToneResult result;
    allocations=frees=0;countAllocations=true;
    for(int n=0;n<frames;++n){
        const auto phase=2*std::numbers::pi*4000*n/rate,trigger=.3*std::sin(2*std::numbers::pi*triggerHz*n/rate);
        const auto audio=.01*std::sin(phase)+(external?0:trigger);
        const auto out=eq.process({audio,audio},{external?trigger:0,external?trigger:0});
        result.finite&=std::isfinite(out[0])&&std::isfinite(out[1]);
        if(n>=frames/2){sine+=out[0]*std::sin(phase);cosine+=out[0]*std::cos(phase);}
    }
    countAllocations=false;CHECK(allocations==0&&frees==0);CHECK(result.finite);
    result.gainDb=20*std::log10(4*std::hypot(sine,cosine)/frames/.01);result.movement=eq.dynamicGainsDb()[0];return result;
}
void independentDetectorSignalProof()
{
    std::cout<<"SECTION free_detector_two_tone_compression_expansion_and_external_source\n";
    for(float rangeDb:{-12.f,12.f}){
        for(auto source:{DynamicEqDetectorSource::internalFree,DynamicEqDetectorSource::externalFree}){
            const auto hit=detectorTone(source,100,rangeDb),miss=detectorTone(source,8000,rangeDb);
            CHECK(rangeDb<0?hit.gainDb<-8:hit.gainDb>8);CHECK(rangeDb<0?hit.movement<-8:hit.movement>8);
            CHECK(std::abs(miss.gainDb)<.1&&std::abs(miss.movement)<.1);
        }
        const auto bandHit=detectorTone(DynamicEqDetectorSource::externalBand,4000,rangeDb);
        CHECK(rangeDb<0?bandHit.gainDb<-8:bandHit.gainDb>8);
    }
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.frequencyHz=4000;b.rangeDb=-12;b.thresholdDb=-30;b.attackMs=1;b.releaseMs=100;b.detectorSource=DynamicEqDetectorSource::externalBand;
    DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));
    for(int n=0;n<24000;++n){const auto x=.3*std::sin(2*std::numbers::pi*4000*n/48000);eq.process({x,x},{x,x});}
    CHECK(eq.dynamicGainsDb()[0]<-8);
    for(int n=0;n<48000;++n){const auto x=.3*std::sin(2*std::numbers::pi*4000*n/48000);eq.process({x,x});}
    CHECK(std::abs(eq.dynamicGainsDb()[0])<.01);
}
void detectorSpanAndDefaultContracts()
{
    std::cout<<"SECTION external_span_mismatch_atomicity_partition_and_exact_default\n";
    auto s=band(DynamicEqShape::bell,0);s.bands[0].rangeDb=-12;s.bands[0].detectorSource=DynamicEqDetectorSource::externalFree;s.bands[0].detectorLowHz=200;s.bands[0].detectorHighHz=2000;
    DynamicEq block,frame;CHECK(block.setSettings(s)&&frame.setSettings(s)&&block.prepare(48000)&&frame.prepare(48000));
    std::array<DynamicEq::Frame,63> input{},external{};
    for(std::size_t n=0;n<input.size();++n){const auto v=.01*std::sin(n*.17),e=.3*std::sin(n*.13);input[n]={v,-v};external[n]={e,e};}
    auto rendered=input;
    CHECK(!block.processBlock(rendered,std::span<const DynamicEq::Frame>(external).first(62)));
    CHECK(std::memcmp(rendered.data(),input.data(),sizeof(input))==0);
    CHECK(block.processBlock(rendered,external));
    auto reference=input;for(std::size_t n=0;n<input.size();++n)reference[n]=frame.process(input[n],external[n]);
    CHECK(std::memcmp(rendered.data(),reference.data(),sizeof(reference))==0);
    CHECK(block.dynamicGainsDb()==frame.dynamicGainsDb());
    CHECK(block.processBlock(std::span<DynamicEq::Frame>{},std::span<const DynamicEq::Frame>{}));
    s.bands[0].detectorSource=DynamicEqDetectorSource::internalBand;
    DynamicEq original,supplied;CHECK(original.setSettings(s)&&supplied.setSettings(s)&&original.prepare(48000)&&supplied.prepare(48000));
    for(int n=0;n<2048;++n){
        const DynamicEq::Frame audio{.3*std::sin(n*.17),.2*std::cos(n*.13)},sidechain{2*std::sin(n*.71),-2*std::cos(n*.31)};
        const auto a=original.process(audio),b=supplied.process(audio,sidechain);
        CHECK(std::bit_cast<std::uint64_t>(a[0])==std::bit_cast<std::uint64_t>(b[0]));CHECK(std::bit_cast<std::uint64_t>(a[1])==std::bit_cast<std::uint64_t>(b[1]));
    }
}
void automaticThresholdCalibration()
{
    std::cout<<"SECTION auto_threshold_rms_calibration_steps_silence_and_frozen_manual_learning\n";
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.frequencyHz=1000;b.rangeDb=12;b.thresholdDb=-37;
    DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));
    double referencePower=0;const auto powerCoefficient=std::exp(-1./(.5*48000));
    auto run=[&](double amplitude,int frames){for(int n=0;n<frames;++n){const auto x=amplitude*std::sin(2*std::numbers::pi*1000*n/48000);if(b.autoThreshold)referencePower=powerCoefficient*referencePower+(1-powerCoefficient)*x*x;eq.process({x,x});}};
    run(.25,48000);b.autoThreshold=true;CHECK(eq.setSettings(s));CHECK(std::abs(eq.detectorReadbacks()[0].thresholdDb+60)<1e-4);
    for(double amplitude:{.25,.5,.125}){
        run(amplitude,144000);const auto read=eq.detectorReadbacks()[0];
        const auto expected=std::clamp(10*std::log10(referencePower)+6.,-60.,0.);
        CHECK(std::abs(read.thresholdDb-expected)<.05);CHECK(read.attackMs==b.attackMs&&read.releaseMs==b.releaseMs);
        CHECK(eq.settings().bands[0].thresholdDb==-37);
    }
    const auto learned=eq.detectorReadbacks()[0].thresholdDb;b.autoThreshold=false;CHECK(eq.setSettings(s));CHECK(eq.detectorReadbacks()[0].thresholdDb==-37);
    run(.001,48000);b.autoThreshold=true;CHECK(eq.setSettings(s));CHECK(eq.detectorReadbacks()[0].thresholdDb==learned);
    run(0,48000);CHECK(std::abs(eq.dynamicGainsDb()[0])<.01);CHECK(eq.detectorReadbacks()[0].thresholdDb<=learned+.001f);
    eq.reset();CHECK(eq.detectorReadbacks()[0].thresholdDb==-60);run(2,144000);CHECK(eq.detectorReadbacks()[0].thresholdDb==0);
    b.detectorSource=DynamicEqDetectorSource::internalFree;CHECK(eq.setSettings(s));run(0,1024);CHECK(eq.detectorReadbacks()[0].thresholdDb==-60);
}
void adaptiveDetectorTiming()
{
    std::cout<<"SECTION adaptive_attack_step_release_law_and_manual_bases\n";
    auto s=band(DynamicEqShape::lowShelf,0);auto& b=s.bands[0];b.frequencyHz=4000;b.rangeDb=12;b.thresholdDb=-60;b.attackMs=10;b.releaseMs=100;b.adaptiveTiming=true;b.detectorSource=DynamicEqDetectorSource::externalBand;
    DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));
    CHECK(std::abs(eq.detectorReadbacks()[0].attackMs-5)<1e-5);
    for(int n=0;n<240;++n)eq.process({.01,.01},{1,1});
    CHECK(std::abs(eq.dynamicGainsDb()[0]-12*(1-std::exp(-1.)))<.05);
    for(int n=0;n<48000;++n)eq.process({.01,.01},{1,1});
    double expected=eq.dynamicGainsDb()[0];
    for(int n=0;n<4800;++n){const auto releaseMs=std::clamp(100*(1+std::min(std::abs(expected)/12,1.)),10.,2000.);expected*=std::exp(-1./(48000*releaseMs*.001));eq.process({.01,.01},{0,0});}
    CHECK(std::abs(eq.dynamicGainsDb()[0]-expected)<.05);
    CHECK(std::abs(eq.detectorReadbacks()[0].releaseMs-100*(1+std::min(std::abs(eq.dynamicGainsDb()[0])/12.f,1.f)))<.02);
    CHECK(eq.settings().bands[0].attackMs==10&&eq.settings().bands[0].releaseMs==100);
    for(float hz:{20.f,250.f,1000.f,20000.f})for(float attack:{.1f,10.f,200.f}){
        b.frequencyHz=hz;b.attackMs=attack;b.releaseMs=2000;CHECK(eq.setSettings(s));eq.reset();
        const auto read=eq.detectorReadbacks()[0];CHECK(std::abs(read.attackMs-std::clamp(attack*std::sqrt(1000.f/hz),.1f,200.f))<1e-4);CHECK(read.releaseMs==2000);
    }
}
void detectorAuditionUsesTheTriggerSource()
{
    std::cout<<"SECTION detector_audition_external_free_component_and_lifetime\n";
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.frequencyHz=4000;b.q=4;b.detectorSource=DynamicEqDetectorSource::externalFree;b.detectorLowHz=60;b.detectorHighHz=140;
    for(auto placement:{DynamicEqPlacement::stereo,DynamicEqPlacement::mid,DynamicEqPlacement::side}){
        b.placement=placement;DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000)&&eq.setDetectorAudition(0));
        double energy=0,high=0;bool finite=true;
        for(int n=0;n<48000;++n){const auto a=.1*std::sin(2*std::numbers::pi*4000*n/48000),e=.3*std::sin(2*std::numbers::pi*100*n/48000);const auto y=eq.process({a,a},{e,-e});finite&=std::isfinite(y[0])&&std::isfinite(y[1]);if(n>=24000){energy+=y[0]*y[0];high+=y[0]*std::sin(2*std::numbers::pi*4000*n/48000);}}
        CHECK(finite);CHECK(placement==DynamicEqPlacement::mid?energy<1e-12:energy>100);CHECK(std::abs(high/12000)<1e-5);
        CHECK(!eq.setDetectorAudition(8));eq.cancelAudition();CHECK(!eq.auditioning());
        const auto y=eq.process({.01,-.02},{1,1});CHECK(y[0]==.01&&y[1]==-.02);
    }
}
void detectorTransitionsAndExtremes()
{
    std::cout<<"SECTION detector_source_fade_external_block_and_extreme_partitions\n";
    auto s=band(DynamicEqShape::bell,0);s.bands[0].rangeDb=-12;s.bands[0].thresholdDb=-30;s.bands[0].attackMs=1;s.bands[0].detectorSource=DynamicEqDetectorSource::externalBand;
    DynamicEq block,frame;CHECK(block.setSettings(s)&&frame.setSettings(s)&&block.prepare(48000)&&frame.prepare(48000));
    for(int n=0;n<2048;++n){const auto a=.01*std::sin(n*.13),e=.3*std::sin(n*.13);block.process({a,a},{e,e});frame.process({a,a},{e,e});}
    s.bands[0].detectorSource=DynamicEqDetectorSource::internalBand;CHECK(block.setSettings(s)&&frame.setSettings(s));
    std::array<DynamicEq::Frame,256> a{},b{},external{};
    for(std::size_t n=0;n<a.size();++n){const auto x=.01*std::sin((n+2048)*.13),e=.3*std::sin((n+2048)*.13);a[n]=b[n]={x,x};external[n]={e,e};}
    CHECK(block.processBlock(a,external));for(std::size_t n=0;n<b.size();++n)b[n]=frame.process(b[n],external[n]);
    CHECK(std::memcmp(a.data(),b.data(),sizeof(a))==0);
    for(double rate:{8000.,44100.,96000.,1536000.})for(int mode=0;mode<2;++mode){
        DynamicEqSettings settings;settings.outputGainDb=-12;
        for(std::size_t i=0;i<dynamicEqBandCount;++i){auto& band=settings.bands[i];band.active=true;band.frequencyHz=i%2?20000.f:20.f;band.q=i%2?18.f:.25f;band.gainDb=i%2?6.f:-6.f;band.rangeDb=i%2?-4.f:4.f;band.thresholdDb=-40;band.autoThreshold=i%3==0;band.adaptiveTiming=i%2==0;band.placement=DynamicEqPlacement(i%5);band.detectorSource=mode?DynamicEqDetectorSource(i%4):DynamicEqDetectorSource(i%2);}
        DynamicEq partitioned,reference;CHECK(partitioned.setSettings(settings)&&reference.setSettings(settings)&&partitioned.prepare(rate)&&reference.prepare(rate));
        std::vector<DynamicEq::Frame> input(8192),sidechain(8192),rendered(8192),expected(8192);
        for(std::size_t n=0;n<input.size();++n){input[n]={.001*std::sin(n*.07),.002*std::cos(n*.11)};sidechain[n]={.02*std::sin(n*.037),.03*std::cos(n*.173)};}
        rendered=input;bool accepted=true,finite=true;
        allocations=frees=0;countAllocations=true;
        for(std::size_t phase=0;phase<4;++phase){
            for(std::size_t i=0;i<dynamicEqBandCount;++i)settings.bands[i].detectorSource=mode&&phase%2==0?DynamicEqDetectorSource((i+phase)%4):DynamicEqDetectorSource((i+phase)%2);
            accepted=partitioned.setSettings(settings)&&reference.setSettings(settings)&&accepted;
            if(phase==2){partitioned.reset();reference.reset();}
            const auto end=(phase+1)*2048;
            for(std::size_t n=phase*2048,chunk=0;n<end;++chunk){
                const auto length=std::min<std::size_t>(std::array<std::size_t,5>{1,17,31,64,127}[chunk%5],end-n);
                accepted=partitioned.processBlock(std::span<DynamicEq::Frame>(rendered).subspan(n,length),std::span<const DynamicEq::Frame>(sidechain).subspan(n,length))&&accepted;
                for(std::size_t i=n;i<n+length;++i){expected[i]=reference.process(input[i],sidechain[i]);finite=finite&&std::isfinite(rendered[i][0])&&std::isfinite(rendered[i][1]);}
                n+=length;
            }
        }
        countAllocations=false;CHECK(accepted&&finite);CHECK(allocations==0&&frees==0);
        CHECK(std::memcmp(rendered.data(),expected.data(),rendered.size()*sizeof(DynamicEq::Frame))==0);
        const auto read=partitioned.detectorReadbacks();for(const auto& band:read)CHECK(std::isfinite(band.thresholdDb)&&band.thresholdDb>=-60&&band.thresholdDb<=0&&band.attackMs>=.1f&&band.attackMs<=200&&band.releaseMs>=10&&band.releaseMs<=2000);
    }
}
void detectorPlacementCalibration()
{
    std::cout<<"SECTION external_rms_component_calibration_and_stereo_peak_linking\n";
    for(int mode=0;mode<6;++mode){
        auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.autoThreshold=true;b.frequencyHz=1000;b.detectorSource=DynamicEqDetectorSource::externalBand;b.placement=mode<5?DynamicEqPlacement(mode):DynamicEqPlacement::stereo;
        DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));bool unchanged=true;
        allocations=frees=0;countAllocations=true;
        for(int n=0;n<144000;++n){const auto phase=2*std::numbers::pi*1000*n/48000;const auto y=eq.process({.01,-.02},{.25*std::sin(phase),mode==5?.25*std::cos(phase):0});unchanged=unchanged&&y[0]==.01&&y[1]==-.02;}
        countAllocations=false;CHECK(allocations==0&&frees==0);CHECK(unchanged&&eq.dynamicGainsDb()[0]==0);
        const auto magnitude=mode==2?0.:mode==3||mode==4?.125/std::sqrt(2.):mode==5?.25*std::sqrt(.5+1/std::numbers::pi):.25/std::sqrt(2.);
        const auto threshold=magnitude==0?-60.:std::clamp(20*std::log10(magnitude)+6.,-60.,0.);
        CHECK(std::abs(eq.detectorReadbacks()[0].thresholdDb-threshold)<.05);
    }
}
void externalDetectorBypassIsDry()
{
    std::cout<<"SECTION external_detector_bypass_reset_and_resume\n";
    auto s=band(DynamicEqShape::bell,0);auto& b=s.bands[0];b.frequencyHz=4000;b.rangeDb=-12;b.thresholdDb=-30;b.adaptiveTiming=true;b.detectorSource=DynamicEqDetectorSource::externalFree;b.detectorLowHz=60;b.detectorHighHz=140;
    DynamicEq eq;CHECK(eq.setSettings(s)&&eq.prepare(48000));
    auto run=[&](int frames,bool checkDry){bool dry=true;for(int n=0;n<frames;++n){const auto a=.01*std::sin(n*.47),e=.3*std::sin(2*std::numbers::pi*100*n/48000);const DynamicEq::Frame input{a,-a};const auto y=eq.process(input,{e,e});if(n>=512&&checkDry)dry=dry&&y==input;}return dry;};
    run(48000,false);CHECK(eq.dynamicGainsDb()[0]<-8);
    b.bypassed=true;CHECK(eq.setSettings(s));CHECK(run(1024,true)&&eq.dynamicGainsDb()[0]==0);
    eq.reset();CHECK(run(1024,true)&&eq.dynamicGainsDb()[0]==0);
    b.bypassed=false;CHECK(eq.setSettings(s));run(48000,false);CHECK(eq.dynamicGainsDb()[0]<-8);
}
void staticShapesIgnoreDetectorControls()
{
    std::cout<<"SECTION static_shapes_ignore_retained_detector_controls\n";
    for(auto shape:{DynamicEqShape::lowCut,DynamicEqShape::highCut,DynamicEqShape::notch,DynamicEqShape::bandPass,DynamicEqShape::tiltShelf,DynamicEqShape::flatTilt}){
        auto s=band(shape,12);s.bands[0].rangeDb=-12;DynamicEq original,edited;CHECK(original.setSettings(s)&&edited.setSettings(s)&&original.prepare(48000)&&edited.prepare(48000));
        for(int n=0;n<1024;++n){const DynamicEq::Frame input{.01*std::sin(n*.07),.013*std::cos(n*.11)};original.process(input);edited.process(input);}
        auto& b=s.bands[0];b.detectorSource=DynamicEqDetectorSource::externalFree;b.autoThreshold=true;b.adaptiveTiming=true;b.detectorLowHz=200;b.detectorHighHz=2000;CHECK(edited.setSettings(s));bool exact=true;
        for(int n=1024;n<3072;++n){const DynamicEq::Frame input{.01*std::sin(n*.07),.013*std::cos(n*.11)};const auto a=original.process(input),b=edited.process(input,{.3,-.4});exact=exact&&std::memcmp(a.data(),b.data(),sizeof(a))==0;}
        CHECK(exact&&edited.dynamicGainsDb()[0]==0);
    }
}
void detectorProof()
{
    independentDetectorAdmission();independentDetectorSignalProof();detectorSpanAndDefaultContracts();automaticThresholdCalibration();adaptiveDetectorTiming();detectorAuditionUsesTheTriggerSource();detectorTransitionsAndExtremes();detectorPlacementCalibration();externalDetectorBypassIsDry();staticShapesIgnoreDetectorControls();
}
void detectorOverlappingBlocksKeepFrameOrder()
{
    std::cout<<"SECTION external_detector_overlapping_spans_preserve_frame_order\n";
    for(int offset:{-1,0,1})for(auto source:{DynamicEqDetectorSource::externalBand,DynamicEqDetectorSource::externalFree}){
        auto settings=band();auto& b=settings.bands[0];b.detectorSource=source;b.rangeDb=-9;b.autoThreshold=true;b.adaptiveTiming=true;b.detectorLowHz=80;b.detectorHighHz=4000;
        settings.bands[1]=b;settings.bands[1].frequencyHz=1800;settings.bands[1].rangeDb=9;settings.bands[1].placement=DynamicEqPlacement::side;
        DynamicEq blocked,scalar;CHECK(blocked.setSettings(settings)&&scalar.setSettings(settings)&&blocked.prepare(48000)&&scalar.prepare(48000));
        std::array<DynamicEq::Frame,258> actual{},expected{};for(std::size_t n=0;n<actual.size();++n)actual[n]={.03*std::sin(n*.13),.04*std::cos(n*.071)};expected=actual;
        const auto audioStart=offset<0?1u:0u,externalStart=offset>0?1u:0u;auto audio=std::span(actual).subspan(audioStart,257),reference=std::span(expected).subspan(audioStart,257);
        const auto external=std::span<const DynamicEq::Frame>(actual).subspan(externalStart,257),referenceExternal=std::span<const DynamicEq::Frame>(expected).subspan(externalStart,257);
        for(std::size_t n=0;n<reference.size();++n)reference[n]=scalar.process(reference[n],referenceExternal[n]);
        allocations=frees=0;countAllocations=true;const bool accepted=blocked.processBlock(audio,external);countAllocations=false;CHECK(accepted&&allocations==0&&frees==0);
        CHECK(std::memcmp(actual.data(),expected.data(),sizeof(actual))==0);CHECK(blocked.dynamicGainsDb()==scalar.dynamicGainsDb());
        const auto left=blocked.detectorReadbacks(),right=scalar.detectorReadbacks();CHECK(std::memcmp(left.data(),right.data(),sizeof(left))==0);
    }
}
}
int main(int argc, char** argv)
{
    const auto section = argc == 2 ? std::string_view(argv[1]) : std::string_view();
    if(section=="--profile-response-legacy"||section=="--profile-response-prepared"){profilePreparedResponse(section=="--profile-response-prepared");std::cout<<"Response profile: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(section.empty()||section=="--prepared-response"){preparedResponseMatchesTheOriginal();if(!section.empty()){std::cout<<"Prepared response: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}}
#if !defined(_DEBUG)
    if(section=="--detector-blocks"){detectorBlockPathIsExact();detectorOverlappingBlocksKeepFrameOrder();std::cout<<"EQ detector blocks: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
#endif
    if(section=="--detectors"){detectorProof();std::cout<<"EQ detectors: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if (argc > 2 || (!section.empty() && section != "--extension-gain" && section != "--extensions" && section != "--component-blocks" && section != "--placement" && section != "--profile-placement" && section != "--profile-gain" && section != "--gain" && section != "--block-equivalence" && section != "--automation-fingerprint" && section != "--audition" && section != "--static" && section != "--dynamics" && section != "--proof" && section != "--allocations" && section != "--profile" && section != "--profile-diagnose")) {
        std::cerr << "Usage: RackSynthDynamicEqTests [--static|--dynamics|--proof|--allocations|--profile]\n"; return 2;
    }
    if(section.empty()){detectorProof();detectorOverlappingBlocksKeepFrameOrder();}
#if !defined(_DEBUG)
    if(section.empty())detectorBlockPathIsExact();
    if(section.empty()||section=="--extensions")extendedBlockPathIsExact();
    if(section.empty()||section=="--placement"||section=="--component-blocks")componentBlockPathIsExact();
#else
    if(section=="--component-blocks"){std::cerr<<"Component block private-state oracle requires Release\n";return 2;}
#endif
    if (section.empty() || section == "--extensions") {extendedShapesAndSlopes();extendedRuntimeContracts();extensionStorageReuse();extensionIndependentTransients();}
    if (section.empty() || section == "--extension-gain") extensionGainControls();
    if (section.empty() || section == "--placement") {placementAdmissionAndSideNull();placementMatrixAndAudio();placementOrderAndNullChannels();placementDetectorAndAudition();placementBlocksTransitionsAndHeap();placementDynamicMatrixAndModeTails();placementEnergyResponse();}
    if (section.empty() || section == "--static") {
        transparentAndAdmission(); staticShapes(); identitiesAndStereo(); resetAndPartitions(); tailBound();
    }
    if (section.empty() || section == "--dynamics") {
        dynamicLaw(); linkedDetectors(); dynamicTimingAndAudio(); dynamicResponseAndPartitions(); effectiveMovement();
    }
    if (section.empty() || section == "--audition") {auditionProof();auditionAutomation();}
    if (section.empty() || section == "--proof" || section == "--allocations") allocationProof();
    if (section.empty() || section == "--proof") { transitionTails(); numericalStress(); }
    if (section == "--profile") profile();
    if (section == "--profile-diagnose") profile(true);
    if (section == "--profile-gain") profile(false, true);
    if (section == "--profile-placement") for(int mode=0;mode<3;++mode)profile(false,false,mode);
    if (section.empty() || section == "--block-equivalence") blockEquivalence();
    if (section.empty() || section == "--automation-fingerprint") automationFingerprint();
    if (section.empty() || section == "--gain") { autoGainUsesTheSettingsAndHasNeutralDefault(); scaledGainSignalProof(); compensationRamps(); gainBlockAndHeapProof(); }
    std::cout << "Dynamic EQ: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
