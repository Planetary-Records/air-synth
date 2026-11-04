#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace airack::detail
{
// Read before write gives exact integer delay timing. Two spare frames keep
// both interpolation taps bounded, including maximum supported delay times.
class HistoryRing
{
public:
    void prepare(std::size_t capacity) { data.assign(capacity+2,0.0); clear(); }
    void clear() noexcept { head = 0; written = 0; }
    double read(double delay) const noexcept
    {
        delay = std::clamp(delay,1.0,double(data.size()-2));
        const auto integral = static_cast<std::size_t>(delay);
        const auto a = (head+data.size()-integral)%data.size(), b = (a+data.size()-1)%data.size();
        // Invalid taps contribute positive zero without touching stale storage.
        const double recent = integral <= written ? data[a] : 0.0;
        const double older = integral+1 <= written ? data[b] : 0.0;
        return recent+(older-recent)*(delay-double(integral));
    }
    // Windowed-sinc read through a tap table such as SincTapTable. Lengths up to
    // the prepared capacity less Taps::points-2 frames are in range; lengths
    // within Taps::lead frames read linearly, where the kernel would need samples
    // not yet written.
    template <class Taps>
    double read(double delay, const Taps& taps) const noexcept
    {
        constexpr auto points = std::size_t(Taps::points), lead = std::size_t(Taps::lead);
        delay = std::clamp(delay,1.0,double(data.size()-points));
        const auto integral = static_cast<std::size_t>(delay);
        if (integral <= lead) return read(delay);
        const double position = (delay-double(integral))*Taps::phases;
        const int phase = std::min(static_cast<int>(position),Taps::phases-1);
        const double blend = position-phase;
        const double* lo = taps.row(phase);
        const double* hi = lo+points;
        // Tap j reads the sample written youngest+j writes ago.
        const auto youngest = integral-lead;
        const auto start = (head+data.size()-youngest)%data.size();
        double sum = 0.0;
        for (std::size_t j = 0; j < points; ++j)
        {
            const auto index = start >= j ? start-j : start+data.size()-j;
            if (youngest+j <= written) sum += (lo[j]+blend*(hi[j]-lo[j]))*data[index];
        }
        return sum;
    }
    void write(double x) noexcept
    {
        data[head] = x;
        if (++head == data.size()) head = 0;
        if (written < data.size()) ++written;
    }
private:
    std::vector<double> data;
    std::size_t head = 0, written = 0;
};
}
