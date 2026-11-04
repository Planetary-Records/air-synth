#include "SaturationHistoryTrace.h"
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace airack::probe::history_trace {
namespace {
thread_local Session* active = nullptr;
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
void write(std::ofstream& stream, const double* values, std::size_t count) {
    for (std::size_t n = 0; n < count; ++n) require(std::isfinite(values[n]), "nonfinite trace record");
    stream.write(reinterpret_cast<const char*>(values), std::streamsize(count * sizeof(double)));
}
struct Sample {
    std::array<double, 18> values {};
    bool current = false;
    CurrentLaneInfo info {};
};
struct EndpointDepth {
    unsigned mode = 0, preCount = 0, upCount = 0, postCount = 0;
    bool current = false, prefixReused = false;
    std::array<const void*, 4> lanes {};
    std::array<unsigned, 4> laneCount {};
    std::array<double, 19> native {};
    std::array<std::array<double, 4>, 64> up {};
    std::array<std::array<double, 9>, 64> post {};
    std::array<std::array<std::array<Sample, 2>, 64>, 4> samples {};
};
struct ObservedFrame { unsigned count = 0; bool invalid = false; std::array<EndpointDepth, 2> endpoints; };
std::vector<ObservedFrame> baseline; // Observations only, never Candidate state or expected nonlinear audio.
struct Metric {
    std::uint64_t count = 0, nonzero = 0;
    double maximum = 0;
    std::array<unsigned, 3> first {}, where {};
    std::vector<double> left, right, firstLeft, firstRight;
    void add(double a, double b, unsigned frame, unsigned n, unsigned ch,
             std::span<const double> allA, std::span<const double> allB) {
        require(std::isfinite(a) && std::isfinite(b), "nonfinite stage sample"); ++count;
        const double difference = std::abs(a - b);
        if (difference != 0 && nonzero++ == 0) {
            first = { frame, n, ch }; firstLeft.assign(allA.begin(), allA.end()); firstRight.assign(allB.begin(), allB.end());
        }
        if (difference > maximum) {
            maximum = difference; where = { frame, n, ch };
            left.assign(allA.begin(), allA.end()); right.assign(allB.begin(), allB.end());
        }
    }
};
void values(std::ostream& stream, std::span<const double> values) {
    stream << '[';
    for (std::size_t n = 0; n < values.size(); ++n) {
        if (n) stream << ',';
        stream << '"' << std::hexfloat << values[n] << std::defaultfloat << '"';
    }
    stream << ']';
}
}
struct Depth {
    ObservedFrame observed;
    struct Cell {
        const void* network = nullptr;
        const void* identity = nullptr;
        std::array<double, 5> coefficients {};
        Frame input {}, before1 {}, before2 {}, output {}, after1 {}, after2 {};
    };
    std::array<Cell, 64 * 2 * 9> cells {};
    std::array<const void*, 2> networks {};
    std::array<std::array<const void*, 9>, 2> cellIds {};
    std::array<const void*, 9> scopeIds {};
    const void* scopeNetwork = nullptr;
    unsigned cellCount = 0, sourceMode = 0, cellEndpoints = 0, cellFrame = 0, cellFactor = 0, cellRate = 0;
    bool cellsEnabled = false, cellInvalid = false, cellPending = false;
    std::filesystem::path cellStem;
    std::filesystem::path path;
    unsigned history, windowFrames;
    std::map<std::string, Metric> metrics;
    std::array<std::uint64_t, 3> branchCounts {}, cacheUsed {};
    std::uint64_t cacheChecks = 0, cacheMismatch = 0, primitiveChecks = 0, primitiveMismatch = 0;
    bool hasCurrent = false, hasLegacy = false;
    std::array<std::array<std::uint64_t, 3>, 5> routeBranchCounts {}, omittedCounts {};
    std::uint64_t primitiveOmitted = 0, storageMismatch = 0, predecessorMismatch = 0, routeMismatch = 0;
    std::uint64_t directEndpoints = 0, reusedPrefixEndpoints = 0;
    void analyzeCurrent(const EndpointDepth& e, unsigned lane, unsigned n, unsigned ch, unsigned factor) {
        const auto& sample = e.samples[lane][n][ch]; const auto& v = sample.values;
        const auto info = sample.info;
        const unsigned kind = unsigned(v[11]), route = unsigned(info.route);
        require(sample.current && route < 5 && unsigned(info.priorSource) < 4, "current lane metadata");
        for (double value : v) require(std::isfinite(value), "nonfinite current lane observation");
        ++routeBranchCounts[route][kind];
        const bool midpoint = std::abs(v[4]) < 1e-6 * (1 + std::max(std::abs(v[2]), std::abs(v[3])));
        const unsigned selected = midpoint ? 0u : route == 2 ? 1u
            : std::abs(v[4]) < .0001 * (1 + std::max(std::abs(v[2]), std::abs(v[3]))) ? 1u : 2u;
        if ((factor == 64) != (route == 2) || kind != selected || v[4] != v[2] - v[3]
            || ((route == 3 || route == 4) && kind != 2) || (route != 4 && info.temporalFrame != 0)) ++routeMismatch;
        require(routeMismatch == 0, "current route/factor/selector");
        if (kind == 0) {
            require(!info.primitivesMaterialized && info.priorSource == PriorSource::unused,
                    "current midpoint availability");
            if (v[5] != 0 || v[6] != 0) ++storageMismatch;
        } else if (!info.primitivesMaterialized) {
            require(route < 2 && kind == 1 && info.priorSource == PriorSource::unused,
                    "current optimized scalar exact omission");
            ++primitiveOmitted; ++omittedCounts[route][kind];
            if (v[5] != 0 || v[6] != 0 || (unsigned(v[10]) == 1 && v[8] != 0)) ++storageMismatch;
        } else {
            const auto character = SaturationCharacter(unsigned(v[12]));
            const auto evaluate = [&](double x) { return kind == 1 ? primitive(character, x) : preparedPrimitive(character, x); };
            ++primitiveChecks;
            if (evaluate(v[2]) != v[5]) ++primitiveMismatch;
            if (info.priorSource == PriorSource::memberCache) {
                ++cacheUsed[kind]; ++cacheChecks;
                if (unsigned(v[10]) != kind || v[8] != v[6] || evaluate(v[3]) != v[6]
                    || unsigned(v[9]) != unsigned(v[12])) ++cacheMismatch;
            } else if (info.priorSource == PriorSource::evaluated) {
                if (unsigned(v[10]) == kind || evaluate(v[3]) != v[6]) ++cacheMismatch;
            } else require(info.priorSource == PriorSource::predecessorVector && route == 4,
                           "current actual prior source");
        }
        if (route == 4) {
            if (info.temporalFrame != n % 4 || (n % 4 == 0) == (info.priorSource == PriorSource::predecessorVector))
                ++predecessorMismatch;
            if (n % 4) {
                const auto& prior = e.samples[lane][n-1][ch];
                const auto& initial = e.samples[lane][n - n % 4][ch].values;
                if (!prior.current || prior.info.route != CurrentRoute::temporal4
                    || prior.info.temporalFrame + 1 != info.temporalFrame
                    || v[6] != prior.values[5] || v[3] != prior.values[2] || v[13] != prior.values[0]
                    || v[14] != prior.values[1] || v[8] != initial[8] || v[9] != initial[9] || v[10] != initial[10])
                    ++predecessorMismatch;
            }
        } else if (info.priorSource == PriorSource::predecessorVector) ++predecessorMismatch;
        require(storageMismatch == 0, "current omitted primitive storage");
        require(predecessorMismatch == 0, "current vector predecessor/member snapshot");
        require(primitiveMismatch == 0, "current materialized primitive");
        require(cacheMismatch == 0, "current actual member cache/prior");
    }
    Depth(const std::filesystem::path& stem, unsigned history, unsigned windowFrames)
        : cellStem(stem), path(stem.string() + ".stages.json"), history(history), windowFrames(windowFrames) {
        if (history == 0) { baseline.clear(); baseline.resize(windowFrames); }
        else require(baseline.size() == windowFrames, "baseline stage extent");
    }
    void analyze(unsigned frame, unsigned relative, unsigned factor, unsigned expectedEndpoints, unsigned phase) {
        require(!observed.invalid && observed.count == expectedEndpoints, "deep endpoint extent/order");
        for (unsigned ep = 0; ep < expectedEndpoints; ++ep) {
            const auto& e = observed.endpoints[ep];
            hasCurrent = hasCurrent || e.current; hasLegacy = hasLegacy || !e.current;
            require(!(hasCurrent && hasLegacy), "mixed current/legacy observer session");
            if (e.current) {
                require(!e.prefixReused || (factor == 4 && ep == 1 && observed.endpoints[0].current
                    && !observed.endpoints[0].prefixReused), "current prefix endpoint order/factor");
                if (e.prefixReused) ++reusedPrefixEndpoints; else ++directEndpoints;
            }
            require(e.preCount == 1 && e.upCount == factor && e.postCount == factor, "deep pre/up/post extent/order");
            for (unsigned lane = 0; lane < 4; ++lane) {
                require(e.laneCount[lane] == ((e.mode == 0 ? lane == 0 : lane != 0) ? factor * 2 : 0),
                        "deep lane/channel/sample extent/order");
                for (unsigned n = 0; n < e.laneCount[lane] / 2; ++n) for (unsigned ch = 0; ch < 2; ++ch) {
                    const auto& v = e.samples[lane][n][ch].values;
                    const auto kind = unsigned(v[11]); ++branchCounts[kind];
                    if (e.current) { analyzeCurrent(e, lane, n, ch, factor); continue; }
                    require(!e.samples[lane][n][ch].current, "legacy lane interpretation");
                    if (kind) {
                        const auto character = SaturationCharacter(unsigned(v[12]));
                        const auto evaluate = [&](double x) { return kind == 1 ? primitive(character, x) : preparedPrimitive(character, x); };
                        ++primitiveChecks;
                        if (evaluate(v[2]) != v[5]) ++primitiveMismatch;
                        if (unsigned(v[10]) == kind) {
                            ++cacheUsed[kind]; ++cacheChecks;
                            if (v[8] != v[6] || evaluate(v[3]) != v[6] || unsigned(v[9]) != unsigned(v[12])) ++cacheMismatch;
                        }
                    }
                }
            }
        }
        if (history == 0) { baseline[relative] = observed; return; }
        const auto& ref = baseline[relative];
        require(ref.count == observed.count, "paired stage endpoint count");
        auto add = [&](const std::string& name, double a, double b, unsigned n, unsigned ch,
                       std::span<const double> av, std::span<const double> bv) {
            metrics[std::to_string(phase) + '/' + name].add(a, b, frame, n, ch, av, bv);
        };
        for (unsigned ep = 0; ep < expectedEndpoints; ++ep) {
            const auto& a = ref.endpoints[ep]; const auto& b = observed.endpoints[ep];
            require(a.mode == b.mode, "paired stage mode");
            const auto prefix = std::to_string(ep) + '/';
            const char* nativeNames[] { "input", "pre_z1", "pre_z2", "post_initial_z1", "post_initial_z2", "pre_filtered", "pre_after_z1", "pre_after_z2" };
            for (unsigned k = 0; k < 8; ++k) for (unsigned ch = 0; ch < 2; ++ch)
                add(prefix + nativeNames[k], a.native[k * 2 + ch], b.native[k * 2 + ch], 0, ch, a.native, b.native);
            for (unsigned n = 0; n < factor; ++n) {
                for (unsigned k = 0; k < 4; ++k)
                    add(prefix + (k < 2 ? "up_wet" : "up_dry"), a.up[n][k], b.up[n][k], n, k % 2, a.up[n], b.up[n]);
                const char* postNames[] { "post_before", "post_filtered", "post_z1", "post_z2" };
                for (unsigned k = 0; k < 8; ++k)
                    add(prefix + postNames[k / 2], a.post[n][k], b.post[n][k], n, k % 2, a.post[n], b.post[n]);
                for (unsigned lane = 0; lane < 4; ++lane) if (a.laneCount[lane]) for (unsigned ch = 0; ch < 2; ++ch) {
                    const auto& av = a.samples[lane][n][ch].values; const auto& bv = b.samples[lane][n][ch].values;
                    const auto label = prefix + "lane" + std::to_string(lane) + '/';
                    const std::pair<unsigned, const char*> fields[] { {0,"x"}, {1,"h"}, {2,"u"}, {3,"previous_u"}, {7,"shaped"}, {16,"dc"}, {17,"envelope"} };
                    for (const auto& [index, name] : fields) add(label + name, av[index], bv[index], n, ch, av, bv);
                }
            }
        }
    }
    void finishCells() {
        if (!cellsEnabled) return;
        require(!cellInvalid && !cellPending && scopeNetwork == nullptr && cellEndpoints == 1
            && cellCount == (cellFactor == 64 && sourceMode == 1 ? 1152u : 0u), "cell observer completion/extent");
        std::ofstream csv(cellStem.string() + ".cells.csv"), meta(cellStem.string() + ".cells.json");
        csv.exceptions(std::ios::badbit | std::ios::failbit); meta.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "frame,network,oversample,cell,network_id,cell_id,b0,b1,b2,a1,a2,x_l,x_r,z1_l,z1_r,z2_l,z2_r,y_l,y_r,next_z1_l,next_z1_r,next_z2_l,next_z2_r\n";
        for (unsigned k = 0; k < cellCount; ++k) {
            const auto& c = cells[k];
            csv << cellFrame << ',' << (k / 9) % 2 << ',' << k / 18 << ',' << k % 9 << ",0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(c.network) << ",0x" << reinterpret_cast<std::uintptr_t>(c.identity) << std::dec;
            const auto scalar = [&](double v) { require(std::isfinite(v), "nonfinite cell record"); csv << ',' << std::hexfloat << v << std::defaultfloat; };
            for (double v : c.coefficients) scalar(v);
            for (const auto& pair : { c.input, c.before1, c.before2, c.output, c.after1, c.after2 }) for (double v : pair) scalar(v);
            csv << '\n';
        }
        meta << "{\"schema\":\"saturation-crossover-cells-v1\",\"status\":\"complete\",\"factor\":" << cellFactor
             << ",\"rate\":" << cellRate << ",\"history\":" << history << ",\"frame\":" << cellFrame
             << ",\"source_mode\":" << sourceMode << ",\"endpoints\":" << cellEndpoints << ",\"records\":" << cellCount
             << ",\"cell_order\":[\"lowLP1\",\"lowHP1\",\"lowLP2\",\"lowHP2\",\"highLP1\",\"highHP1\",\"highLP2\",\"highHP2\",\"highAP\"]}\n";
        csv.close(); meta.close();
    }
    void finish() {
        finishCells();
        std::ofstream file(path); file.exceptions(std::ios::badbit | std::ios::failbit);
        file << std::setprecision(17) << "{\"history\":" << history << ",\"window_frames\":" << windowFrames
             << ",\"branch_counts\":[" << branchCounts[0] << ',' << branchCounts[1] << ',' << branchCounts[2]
             << "],\"cache_used\":[" << cacheUsed[0] << ',' << cacheUsed[1] << ',' << cacheUsed[2]
             << "],\"cache_checks\":" << cacheChecks << ",\"cache_mismatch\":" << cacheMismatch
             << ",\"primitive_checks\":" << primitiveChecks << ",\"primitive_mismatch\":" << primitiveMismatch
             << ",\"sample_fields\":[\"x\",\"h\",\"u\",\"previous_u\",\"delta\",\"current_primitive\",\"prior_primitive\",\"shaped\",\"cached_primitive\",\"cache_character\",\"cache_kind\",\"kind\",\"character\",\"previous_x\",\"previous_h\",\"previous_difference\",\"dc\",\"envelope\"],\"metrics\":{";
        bool first = true;
        for (const auto& [name, m] : metrics) {
            file << (first ? "" : ",") << '"' << name << "\":{\"samples\":" << m.count << ",\"nonzero\":" << m.nonzero
                 << ",\"max\":" << m.maximum << ",\"first\":[" << m.first[0] << ',' << m.first[1] << ',' << m.first[2]
                 << "],\"where\":[" << m.where[0] << ',' << m.where[1] << ',' << m.where[2] << "],\"left\":";
            values(file, m.left); file << ",\"right\":"; values(file, m.right);
            file << ",\"first_left\":"; values(file, m.firstLeft); file << ",\"first_right\":"; values(file, m.firstRight);
            file << '}'; first = false;
        }
        file << '}';
        if (hasCurrent) {
            file << ",\"current_observer\":{\"schema\":\"saturation-current-observer-v1\",\"route_branch_counts\":";
            const auto matrix = [&](const auto& counts) {
                file << '[';
                for (unsigned r = 0; r < counts.size(); ++r)
                    file << (r ? ",[" : "[") << counts[r][0] << ',' << counts[r][1] << ',' << counts[r][2] << ']';
                file << ']';
            };
            matrix(routeBranchCounts); file << ",\"omitted_counts\":"; matrix(omittedCounts);
            file << ",\"primitive_omitted\":" << primitiveOmitted << ",\"storage_mismatch\":" << storageMismatch
                 << ",\"predecessor_mismatch\":" << predecessorMismatch << ",\"route_mismatch\":" << routeMismatch
                 << ",\"direct_endpoints\":" << directEndpoints << ",\"reused_prefix_endpoints\":" << reusedPrefixEndpoints << '}';
        }
        file << "}\n"; file.close();
    }
};
Session::Session(const std::filesystem::path& stem, unsigned f, unsigned rate, unsigned start, unsigned history)
    : factor(f), fadeLength(unsigned(std::ceil(.020 * rate))), returnStart(start),
      from(f, saturationCoefficientManifest()), to(from),
      endpointFile(stem.string() + ".endpoints.f64", std::ios::binary),
      boundaryFile(stem.string() + ".boundary.f64", std::ios::binary),
      depth(std::make_unique<Depth>(stem, history, 64 + fadeLength + 128)) {
    endpointFile.exceptions(std::ios::badbit | std::ios::failbit);
    boundaryFile.exceptions(std::ios::badbit | std::ios::failbit);
    depth->cellFactor = f; depth->cellRate = rate; depth->cellFrame = start - 64;
}
Session::~Session() = default;
void Session::begin(unsigned nextFrame, bool structuralChange) {
    require(active == nullptr, "overlapping observer frame");
    frame = nextFrame; count = 0; overflow = false;
    if (frame + 64 >= returnStart) depth->observed = {};
    if (structuralChange) {
        require(!fading, "overlapping structural event");
        // Clone only the independent LTI model, before the first new endpoint group.
        to = from; fadeFrame = 0; fading = true;
    }
    active = this;
}
void Session::end(Frame actual) {
    active = nullptr;
    require(!overflow && count == factor * (fading ? 2u : 1u), "observer endpoint count/order/extent");
    if (frame + 64 >= returnStart) depth->analyze(frame, frame + 64 - returnStart, factor,
        fading ? 2u : 1u, frame < returnStart ? 0 : frame < returnStart + fadeLength ? 1 : 2);
    const auto endpoint = [&](unsigned offset, control_law::ReturnOracle& oracle) {
        std::array<Frame, 64> values {};
        for (unsigned n = 0; n < factor; ++n) for (unsigned ch = 0; ch < 2; ++ch)
            values[n][ch] = dry[offset + n][ch] + 1. * (wet[offset + n][ch] - dry[offset + n][ch]);
        return oracle.process(values, factor);
    };
    const auto a = endpoint(0, from);
    const auto b = fading ? endpoint(factor, to) : Frame {};
    const double weight = fading ? double(fadeFrame) / (fadeLength - 1) : 0;
    Frame linear = a;
    if (fading) for (unsigned ch = 0; ch < 2; ++ch) linear[ch] += weight * (b[ch] - linear[ch]);
    const double row[] { double(frame), double(count / factor), weight, a[0], a[1], b[0], b[1],
                         linear[0], linear[1], actual[0], actual[1] };
    write(endpointFile, row, std::size(row));
    if (frame + 64 >= returnStart) {
        const double header[] { double(frame), double(count) }; write(boundaryFile, header, 2);
        for (unsigned n = 0; n < count; ++n) {
            const double values[] { wet[n][0], wet[n][1], dry[n][0], dry[n][1] };
            write(boundaryFile, values, 4);
        }
    }
    if (fading && ++fadeFrame == fadeLength) { from = to; fading = false; }
}
void Session::finish() {
    require(active == nullptr && !fading, "unfinished trace");
    endpointFile.flush(); endpointFile.close(); boundaryFile.flush(); boundaryFile.close();
    depth->finish();
}
void selfTest() {
    Session* saved = active; active = nullptr; require(saved == nullptr, "selftest active observer");
    // Observer group storage has exact fixed capacity, including two 64x endpoints.
    require(std::tuple_size_v<decltype(Session::wet)> == 128, "observer capacity");
}
namespace {
EndpointDepth* current() noexcept {
    if (!active || active->frame + 64 < active->returnStart) return nullptr;
    auto& frame = active->depth->observed;
    if (frame.count == 0 || frame.count > 2) { frame.invalid = true; return nullptr; }
    return &frame.endpoints[frame.count - 1];
}
}
namespace {
Depth* cellDepth() noexcept {
    if (!active || active->factor != 64 || active->frame + 64 != active->returnStart) return nullptr;
    auto* d = active->depth.get();
    return d->sourceMode == 1 ? d : nullptr;
}
}
void cells_endpoint(unsigned mode) noexcept {
    if (!active) return;
    auto& d = *active->depth; d.cellsEnabled = true;
    if (active->frame + 64 != active->returnStart) return;
    if (mode > 1 || ++d.cellEndpoints != 1 || active->fading) { d.cellInvalid = true; return; }
    d.sourceMode = mode;
}
void network(const void* identity, std::array<const void*, 9> ids) noexcept {
    auto* d = cellDepth(); if (!d) return;
    if (!identity || d->cellEndpoints != 1 || d->scopeNetwork || d->cellPending || d->cellCount >= d->cells.size() || d->cellCount % 9) {
        d->cellInvalid = true; return;
    }
    const auto n = (d->cellCount / 9) % 2;
    if (d->cellCount < 18) { d->networks[n] = identity; d->cellIds[n] = ids; }
    if (d->networks[n] != identity || d->cellIds[n] != ids || (n == 1 && d->networks[0] == identity)) { d->cellInvalid = true; return; }
    for (unsigned k = 0; k < ids.size(); ++k) {
        if (!ids[k]) { d->cellInvalid = true; return; }
        for (unsigned j = 0; j < k; ++j) if (ids[j] == ids[k]) { d->cellInvalid = true; return; }
    }
    d->scopeNetwork = identity; d->scopeIds = ids;
}
void cell_before(const void* identity, std::array<double, 5> coefficients, Frame input, Frame z1, Frame z2) noexcept {
    auto* d = cellDepth(); if (!d || !d->scopeNetwork) return;
    if (d->cellPending || d->cellCount >= d->cells.size() || d->scopeIds[d->cellCount % 9] != identity) { d->cellInvalid = true; return; }
    auto& c = d->cells[d->cellCount]; c.network = d->scopeNetwork; c.identity = identity;
    c.coefficients = coefficients; c.input = input; c.before1 = z1; c.before2 = z2; d->cellPending = true;
}
void cell_after(const void* identity, Frame output, Frame z1, Frame z2) noexcept {
    auto* d = cellDepth(); if (!d || !d->scopeNetwork) return;
    if (!d->cellPending || d->cellCount >= d->cells.size() || d->cells[d->cellCount].identity != identity) { d->cellInvalid = true; return; }
    auto& c = d->cells[d->cellCount++]; c.output = output; c.after1 = z1; c.after2 = z2; d->cellPending = false;
    if (d->cellCount % 9 == 0) d->scopeNetwork = nullptr;
}
void endpoint(unsigned mode, std::array<const void*, 4> lanes, Frame input, Frame preZ1, Frame preZ2,
              Frame postZ1, Frame postZ2) noexcept {
    if (!active || active->frame + 64 < active->returnStart) return;
    auto& frame = active->depth->observed;
    if (frame.count == 2) { frame.invalid = true; return; }
    auto& e = frame.endpoints[frame.count++]; e.mode = mode; e.lanes = lanes;
    const Frame fields[] { input, preZ1, preZ2, postZ1, postZ2 };
    for (unsigned k = 0; k < 5; ++k) for (unsigned ch = 0; ch < 2; ++ch) e.native[k * 2 + ch] = fields[k][ch];
}
void current_endpoint(unsigned mode, std::array<const void*, 4> lanes, Frame input, Frame preZ1, Frame preZ2,
                      Frame postZ1, Frame postZ2, bool prefixReused) noexcept {
    endpoint(mode, lanes, input, preZ1, preZ2, postZ1, postZ2);
    if (auto* e = current()) { e->current = true; e->prefixReused = prefixReused; }
}
void pre(Frame filtered, Frame z1, Frame z2, bool enabled) noexcept {
    if (auto* e = current()) {
        ++e->preCount; const Frame fields[] { filtered, z1, z2 };
        for (unsigned k = 0; k < 3; ++k) for (unsigned ch = 0; ch < 2; ++ch) e->native[10 + k * 2 + ch] = fields[k][ch];
        e->native[16] = enabled ? 1 : 0;
    }
}
void up(const Frame* wet, const Frame* dry, unsigned factor) noexcept {
    if (auto* e = current()) {
        if (factor != active->factor || e->upCount != 0) { active->depth->observed.invalid = true; return; }
        e->upCount = factor;
        for (unsigned n = 0; n < factor; ++n) e->up[n] = { wet[n][0], wet[n][1], dry[n][0], dry[n][1] };
    }
}
void post(unsigned n, Frame before, Frame filtered, Frame z1, Frame z2, bool enabled) noexcept {
    if (auto* e = current()) {
        if (n != e->postCount || n >= active->factor) { active->depth->observed.invalid = true; return; }
        ++e->postCount;
        e->post[n] = { before[0], before[1], filtered[0], filtered[1], z1[0], z1[1], z2[0], z2[1], enabled ? 1. : 0. };
    }
}
void lane(const void* identity, unsigned ch, unsigned character, double x, double h, double u, double previous,
          double delta, double currentPrimitive, double priorPrimitive, double shaped, double cachePrimitive,
          unsigned cacheKind, unsigned kind, unsigned cacheCharacter, double previousX, double previousH,
          double previousDifference, double dc, double envelope) noexcept {
    if (auto* e = current()) {
        unsigned lane = 0; while (lane < 4 && e->lanes[lane] != identity) ++lane;
        if (lane == 4 || ch != e->laneCount[lane] % 2 || e->laneCount[lane] >= active->factor * 2 || kind > 2) {
            active->depth->observed.invalid = true; return;
        }
        const unsigned n = e->laneCount[lane]++ / 2;
        e->samples[lane][n][ch].values = { x, h, u, previous, delta, currentPrimitive, priorPrimitive, shaped,
            cachePrimitive, double(cacheCharacter), double(cacheKind), double(kind), double(character), previousX,
            previousH, previousDifference, dc, envelope };
    }
}
void current_lane(const void* identity, unsigned ch, unsigned character, double x, double h, double u, double previous,
                  double delta, double currentPrimitive, double priorPrimitive, double shaped, double cachePrimitive,
                  unsigned cacheKind, unsigned kind, unsigned cacheCharacter, double previousX, double previousH,
                  double previousDifference, double dc, double envelope, CurrentLaneInfo info) noexcept {
    auto* e = current(); if (!e) return;
    unsigned index = 0; while (index < 4 && e->lanes[index] != identity) ++index;
    if (index == 4 || e->laneCount[index] >= active->factor * 2) { active->depth->observed.invalid = true; return; }
    const auto count = e->laneCount[index];
    lane(identity, ch, character, x, h, u, previous, delta, currentPrimitive, priorPrimitive, shaped, cachePrimitive,
         cacheKind, kind, cacheCharacter, previousX, previousH, previousDifference, dc, envelope);
    if (e->laneCount[index] != count + 1) return;
    auto& sample = e->samples[index][count / 2][ch]; sample.current = true; sample.info = info;
}

}
namespace airack::probe {
void saturationReferenceTrace(Candidate::Frame wet, Candidate::Frame matchedDry) noexcept {
    auto* trace = history_trace::active;
    if (!trace) return;
    if (trace->count == trace->wet.size()) { trace->overflow = true; return; }
    trace->wet[trace->count] = wet; trace->dry[trace->count++] = matchedDry;
}
}
