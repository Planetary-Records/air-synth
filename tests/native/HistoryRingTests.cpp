#include "HistoryRing.h"
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
int checks = 0, failures = 0;
void exact(double actual, double expected, const char* description)
{
    ++checks;
    if (std::bit_cast<std::uint64_t>(actual) != std::bit_cast<std::uint64_t>(expected))
    {
        if (failures < 12) std::cerr << description << ": expected " << expected << ", got " << actual << '\n';
        ++failures;
    }
}

// Independent chronological storage: no circular indices, head or validity counter.
// A missing older sample is the positive zero supplied by freshly cleared history.
double chronological(const std::vector<double>& history, std::size_t capacity, double delay)
{
    if (delay < 1) delay = 1;
    if (delay > double(capacity)) delay = double(capacity);
    const auto age = static_cast<std::size_t>(std::floor(delay));
    const auto recent = age <= history.size() ? history[history.size()-age] : 0.0;
    const auto older = age < history.size() ? history[history.size()-age-1] : 0.0;
    return recent+(older-recent)*(delay-std::floor(delay));
}

void checkHistory(const airack::detail::HistoryRing& ring, const std::vector<double>& history,
                  std::size_t capacity)
{
    for (double delay : {-100.0, -0.0, .5, 1.0, 1.1, 1.25, 1.5, 1.75,
                         double(capacity)-.5, double(capacity), double(capacity)+.5,
                         1e9, std::numeric_limits<double>::infinity()})
        exact(ring.read(delay), chronological(history,capacity,delay), "chronological/clamped read");
    for (std::size_t age = 1; age <= capacity; ++age)
    {
        for (double fraction : {0.0, .125, .5, .875})
            exact(ring.read(double(age)+fraction), chronological(history,capacity,double(age)+fraction),
                  "every integer/fractional age");
        for (double direction : {0.0, std::numeric_limits<double>::infinity()})
        {
            const double boundary = std::nextafter(double(age),direction);
            exact(ring.read(boundary),chronological(history,capacity,boundary),"adjacent delay boundary");
        }
    }
}

void literalTiming()
{
    airack::detail::HistoryRing ring;
    ring.clear(); ring.clear(); // Safe before storage exists; preparation supplies the storage.
    ring.prepare(4);
    exact(ring.read(1),0.0,"prepared silence");
    ring.write(8);
    exact(ring.read(1),8,"read-before-write age one");
    exact(ring.read(1.25),6,"fractional tap crosses unwritten history");
    exact(ring.read(2),0.0,"unwritten integer tap");
    ring.write(-4);
    exact(ring.read(1.5),2,"two written interpolation taps");
    ring.write(16); ring.write(32);
    exact(ring.read(4),8,"maximum delay");
    exact(ring.read(100),8,"upper delay clamp");
    exact(ring.read(-100),32,"lower delay clamp");
    ring.clear(); ring.clear();
    exact(ring.read(1.5),0.0,"repeated clear invalidates both taps");
    ring.write(2);
    exact(ring.read(1.5),1,"partial refill after clear");
    exact(ring.read(4),0.0,"maximum remains invalid after partial refill");
}

void wrapsAndReprepare()
{
    airack::detail::HistoryRing ring;
    // Reuse the same ring while shrinking and growing its prepared capacity.
    for (std::size_t capacity : {1u, 2u, 7u, 19u, 3u})
    {
        ring.prepare(capacity);
        std::vector<double> history;
        checkHistory(ring,history,capacity);
        for (int reset = 0; reset < 4; ++reset)
        {
            // More than three full physical wraps, including the two spare cells.
            for (std::size_t i = 0; i < 4*(capacity+2)+3; ++i)
            {
                const double value = i%5 == 0 ? double(std::nextafter(1.0f,2.0f))
                    : i%5 == 1 ? std::nextafter(-1.0,0.0)
                    : (double((i*37+reset*11)%113)-56)/16;
                ring.write(value); history.push_back(value);
                checkHistory(ring,history,capacity);
            }
            ring.clear(); history.clear();
            checkHistory(ring,history,capacity);
            ring.clear();
            checkHistory(ring,history,capacity);
        }
    }
}

void staleSpecialValues()
{
    airack::detail::HistoryRing ring;
    constexpr std::size_t capacity = 9;
    ring.prepare(capacity);
    for (double stale : {std::numeric_limits<double>::quiet_NaN(), -0.0})
    {
        // Poison every physical cell, including interpolation spares, across wraps.
        for (std::size_t i = 0; i < 3*(capacity+2); ++i) ring.write(stale);
        ring.clear();
        std::vector<double> history;
        checkHistory(ring,history,capacity);
        for (std::size_t i = 0; i <= capacity+2; ++i)
        {
            ring.write(double(i+1)*.25); history.push_back(double(i+1)*.25);
            checkHistory(ring,history,capacity);
        }
    }
    // Signed zeros written after clear remain ordinary valid samples.
    ring.clear(); ring.write(0.0); ring.write(-0.0);
    exact(ring.read(1),0.0,"retain original signed-zero interpolation arithmetic");
    ring.clear(); ring.write(-0.0); ring.write(-0.0);
    exact(ring.read(1),0.0,"equal negative-zero taps use original arithmetic");
}
}

int main()
{
    literalTiming(); wrapsAndReprepare(); staleSpecialValues();
    std::cout << "History: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
