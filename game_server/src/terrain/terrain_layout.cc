#include "game_server/src/terrain/terrain_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace network_example::game_server::terrain {
namespace {

constexpr double kCellM = kCellMm / 1000.0;

// Bilinear height (m) on a mm grid at map metres, clamped to the map.
struct GroundSampler {
    const std::vector<std::int32_t>* mm = nullptr;
    std::int32_t nx = 0, nz = 0;

    double at(double x, double z) const {
        const double fx = std::clamp(x / kCellM, 0.0, static_cast<double>(nx - 1));
        const double fz = std::clamp(z / kCellM, 0.0, static_cast<double>(nz - 1));
        const std::int32_t x0 = std::min(static_cast<std::int32_t>(fx), nx - 2);
        const std::int32_t z0 = std::min(static_cast<std::int32_t>(fz), nz - 2);
        const double tx = fx - x0, tz = fz - z0;
        auto h = [&](std::int32_t i, std::int32_t j) {
            return (*mm)[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)] / 1000.0;
        };
        const double a = h(x0, z0) + (h(x0 + 1, z0) - h(x0, z0)) * tx;
        const double b = h(x0, z0 + 1) + (h(x0 + 1, z0 + 1) - h(x0, z0 + 1)) * tx;
        return a + (b - a) * tz;
    }
};

struct Rect {
    double x0, z0, x1, z1;
    bool overlaps(const Rect& o) const { return x1 > o.x0 && x0 < o.x1 && z1 > o.z0 && z0 < o.z1; }
};

double stamp_height_m(const StampAsset& s, double lx, double lz) {
    const double e = s.cells_x * kCellM;
    const double fx = std::clamp(lx / e, 0.0, 1.0) * s.cells_x, fz = std::clamp(lz / e, 0.0, 1.0) * s.cells_z;
    const std::int32_t x0 = std::min(static_cast<std::int32_t>(fx), s.cells_x - 1);
    const std::int32_t z0 = std::min(static_cast<std::int32_t>(fz), s.cells_z - 1);
    const double tx = fx - x0, tz = fz - z0;
    const double a = s.at(x0, z0) + (s.at(x0 + 1, z0) - s.at(x0, z0)) * tx;
    const double b = s.at(x0, z0 + 1) + (s.at(x0 + 1, z0 + 1) - s.at(x0, z0 + 1)) * tx;
    return (a + (b - a) * tz) / 100.0;
}

// TerrainMapLayers.CrossesCliff, painted-map branch.
bool crosses_cliff(const GroundSampler& g, const Rect& claim, const LayoutParams& p) {
    const double st = p.cliff_probe_m;
    for (double a = claim.x0; a <= claim.x1; a += st) {
        for (double b = claim.z0; b <= claim.z1; b += st) {
            const double h0 = g.at(a, b);
            if (std::fabs(g.at(a + st, b) - h0) > p.cliff_rise_m || std::fabs(g.at(a, b + st) - h0) > p.cliff_rise_m) {
                return true;
            }
        }
    }
    return false;
}

// C#'s Mathf.Round rounds half to even.
double round_half_even(double v) { return std::nearbyint(v); }

}  // namespace

std::uint64_t LayoutRng::next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double LayoutRng::next_double() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

std::uint32_t LayoutRng::next_below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }

std::vector<double> stamp_openness(const StampAsset& s) {
    const double e = s.cells_x * kCellM;
    std::vector<double> open(4, 0.0);
    for (int side = 0; side < 4; ++side) {
        int n = 0, ok = 0;
        for (double t = 2.5; t <= e - 2.5 + 1e-9; t += 0.5) {
            double lx = 0, lz = 0;
            switch (side) {
            case 0: lx = t; lz = 3; break;
            case 1: lx = e - 3; lz = t; break;
            case 2: lx = t; lz = e - 3; break;
            default: lx = 3; lz = t; break;
            }
            ++n;
            if (std::fabs(stamp_height_m(s, lx, lz)) < 0.6) ++ok;
        }
        open[side] = n > 0 ? static_cast<double>(ok) / n : 0.0;
    }
    return open;
}

bool layout_stamps(TerrainRecipe* r, const TerrainLibrary& lib, const LayoutParams& p, LayoutReport* report, std::string* error) {
    *report = LayoutReport{};
    r->placements.clear();
    const double tan_ring = std::tan(p.ring_slope_deg * 3.14159265358979323846 / 180.0);
    r->ring_tan_q16 = static_cast<std::int64_t>(std::llround(tan_ring * 65536.0));
    std::vector<std::int32_t> terrace;
    if (!build_terrace_mm(*r, lib, &terrace, error)) {
        return false;
    }
    const GroundSampler g{&terrace, r->cells_x + 1, r->cells_z + 1};
    const double size_x = r->cells_x * kCellM, size_z = r->cells_z * kCellM;
    const double grid = kGridMm / 1000.0;

    std::vector<std::uint32_t> pool = p.stamp_pool;
    if (pool.empty()) {
        for (std::uint32_t k = 0; k < lib.stamps.size(); ++k) pool.push_back(k);
    }
    for (std::uint32_t k : pool) {
        if (k >= lib.stamps.size() || lib.stamps[k].cells_x != lib.stamps[k].cells_z) {
            *error = "layout stamp pool names a missing or non-square stamp";
            return false;
        }
    }
    if (pool.empty()) {
        return true;
    }
    LayoutRng rng(static_cast<std::uint64_t>(static_cast<std::uint32_t>(r->seed)) * 0x100000001b3ull + 0x5eedull);
    std::vector<std::uint32_t> picks;
    for (std::int32_t k = 0; k < p.stamp_count; ++k) picks.push_back(pool[rng.next_below(static_cast<std::uint32_t>(pool.size()))]);
    std::stable_sort(picks.begin(), picks.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (lib.stamps[a].cells_x != lib.stamps[b].cells_x) return lib.stamps[a].cells_x > lib.stamps[b].cells_x;
        return lib.stamps[a].name < lib.stamps[b].name;
    });
    report->picked = static_cast<std::int32_t>(picks.size());

    std::vector<Rect> reserved;
    for (const std::uint32_t m : picks) {
        const StampAsset& s = lib.stamps[m];
        const double e = s.cells_x * kCellM;
        double lowest = 0;
        for (const std::int16_t v : s.cm) lowest = std::min(lowest, v / 100.0);
        const std::vector<double> open = stamp_openness(s);
        bool found = false;
        double best_score = std::numeric_limits<double>::max();
        double bx = 0, bz = 0, bbase = 0, bring = 0;
        for (double x = grid; x + e <= size_x - grid + 1e-9; x += grid) {
            for (double z = grid; z + e <= size_z - grid + 1e-9; z += grid) {
                const double a = p.around_m;
                double lo = std::numeric_limits<double>::max(), hi = -lo;
                for (double t = -a; t <= e + a + 1e-9; t += 2.0) {
                    for (const double h : {g.at(x + t, z - a), g.at(x + t, z + e + a), g.at(x - a, z + t), g.at(x + e + a, z + t)}) {
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                    }
                }
                double base = round_half_even((lo + hi) / 2.0 / p.base_step_m) * p.base_step_m;
                const double min_base = p.min_floor_m - lowest;
                if (base < min_base) base = std::ceil(min_base / p.base_step_m) * p.base_step_m;
                const double delta = std::max(hi - base, base - lo);
                const double ring = std::max(p.min_ring_m, delta / tan_ring + 1.0);
                if (ring > p.max_ring_m) { ++report->rejected_ring; continue; }
                const Rect claim{x - ring, z - ring, x + e + ring, z + e + ring};
                bool overlap = false;
                for (const Rect& q : reserved) overlap = overlap || q.overlaps(claim);
                if (overlap) { ++report->rejected_overlap; continue; }
                if (crosses_cliff(g, claim, p)) { ++report->rejected_cliff; continue; }
                const double score = ring + rng.next_double() * p.ring_noise_m;
                if (score < best_score) {
                    best_score = score;
                    found = true;
                    bx = x; bz = z; bbase = base; bring = ring;
                }
            }
        }
        if (!found) {
            report->unplaced.push_back(s.name);
            continue;
        }
        // Turn: world side w (S, E, N, W) gets stamp side (w - turns) & 3, E and W swapped when mirrored.
        double access[4];
        for (int w = 0; w < 4; ++w) {
            double sum = 0;
            int n = 0;
            for (double t = 0; t <= e + 1e-9; t += 2.0) {
                double px = 0, pz = 0;
                switch (w) {
                case 0: px = bx + t; pz = bz - bring; break;
                case 1: px = bx + e + bring; pz = bz + t; break;
                case 2: px = bx + t; pz = bz + e + bring; break;
                default: px = bx - bring; pz = bz + t; break;
                }
                sum += std::fabs(g.at(px, pz) - bbase);
                ++n;
            }
            access[w] = std::exp(-sum / n / 2.0);
        }
        double best_fit = -std::numeric_limits<double>::max();
        std::uint8_t turns = 0;
        bool mirror = false;
        for (int mi = 0; mi < 2; ++mi) {
            for (int t = 0; t < 4; ++t) {
                double fit = rng.next_double() * 0.01;
                for (int w = 0; w < 4; ++w) {
                    int side = (w - t + 4) & 3;
                    if (mi == 1 && (side & 1) == 1) side ^= 2;
                    fit += open[static_cast<std::size_t>(side)] * access[w];
                }
                if (fit > best_fit) { best_fit = fit; turns = static_cast<std::uint8_t>(t); mirror = mi == 1; }
            }
        }
        TerrainPlacement tp;
        tp.stamp = m;
        tp.x_cells = static_cast<std::int32_t>(std::llround(bx / kCellM));
        tp.z_cells = static_cast<std::int32_t>(std::llround(bz / kCellM));
        tp.base_cm = static_cast<std::int32_t>(std::llround(bbase * 100.0));
        tp.ring_mm = static_cast<std::int32_t>(std::llround(bring * 1000.0));
        tp.turns = turns;
        tp.mirror = mirror;
        r->placements.push_back(tp);
        reserved.push_back(Rect{bx - bring, bz - bring, bx + e + bring, bz + e + bring});
    }
    report->placed = static_cast<std::int32_t>(r->placements.size());
    return true;
}

}  // namespace network_example::game_server::terrain
