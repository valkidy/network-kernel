#include "game_server/src/terrain/terrain_compose.h"

#include <algorithm>

namespace network_example::game_server::terrain {
namespace {

// cos / sin of k / 256 turns, Q15 (32767 = 1.0). Generated once with
// round(cos(2 pi k / 256) * 32767); literals, so every platform reads the
// same values.
constexpr std::int16_t kDirs[256][2] = {
    {32767, 0}, {32757, 804}, {32728, 1608}, {32678, 2410}, {32609, 3212}, {32521, 4011}, {32412, 4808}, {32285, 5602},
    {32137, 6393}, {31971, 7179}, {31785, 7962}, {31580, 8739}, {31356, 9512}, {31113, 10278}, {30852, 11039}, {30571, 11793},
    {30273, 12539}, {29956, 13279}, {29621, 14010}, {29268, 14732}, {28898, 15446}, {28510, 16151}, {28105, 16846}, {27683, 17530},
    {27245, 18204}, {26790, 18868}, {26319, 19519}, {25832, 20159}, {25329, 20787}, {24811, 21403}, {24279, 22005}, {23731, 22594},
    {23170, 23170}, {22594, 23731}, {22005, 24279}, {21403, 24811}, {20787, 25329}, {20159, 25832}, {19519, 26319}, {18868, 26790},
    {18204, 27245}, {17530, 27683}, {16846, 28105}, {16151, 28510}, {15446, 28898}, {14732, 29268}, {14010, 29621}, {13279, 29956},
    {12539, 30273}, {11793, 30571}, {11039, 30852}, {10278, 31113}, {9512, 31356}, {8739, 31580}, {7962, 31785}, {7179, 31971},
    {6393, 32137}, {5602, 32285}, {4808, 32412}, {4011, 32521}, {3212, 32609}, {2410, 32678}, {1608, 32728}, {804, 32757},
    {0, 32767}, {-804, 32757}, {-1608, 32728}, {-2410, 32678}, {-3212, 32609}, {-4011, 32521}, {-4808, 32412}, {-5602, 32285},
    {-6393, 32137}, {-7179, 31971}, {-7962, 31785}, {-8739, 31580}, {-9512, 31356}, {-10278, 31113}, {-11039, 30852}, {-11793, 30571},
    {-12539, 30273}, {-13279, 29956}, {-14010, 29621}, {-14732, 29268}, {-15446, 28898}, {-16151, 28510}, {-16846, 28105}, {-17530, 27683},
    {-18204, 27245}, {-18868, 26790}, {-19519, 26319}, {-20159, 25832}, {-20787, 25329}, {-21403, 24811}, {-22005, 24279}, {-22594, 23731},
    {-23170, 23170}, {-23731, 22594}, {-24279, 22005}, {-24811, 21403}, {-25329, 20787}, {-25832, 20159}, {-26319, 19519}, {-26790, 18868},
    {-27245, 18204}, {-27683, 17530}, {-28105, 16846}, {-28510, 16151}, {-28898, 15446}, {-29268, 14732}, {-29621, 14010}, {-29956, 13279},
    {-30273, 12539}, {-30571, 11793}, {-30852, 11039}, {-31113, 10278}, {-31356, 9512}, {-31580, 8739}, {-31785, 7962}, {-31971, 7179},
    {-32137, 6393}, {-32285, 5602}, {-32412, 4808}, {-32521, 4011}, {-32609, 3212}, {-32678, 2410}, {-32728, 1608}, {-32757, 804},
    {-32767, 0}, {-32757, -804}, {-32728, -1608}, {-32678, -2410}, {-32609, -3212}, {-32521, -4011}, {-32412, -4808}, {-32285, -5602},
    {-32137, -6393}, {-31971, -7179}, {-31785, -7962}, {-31580, -8739}, {-31356, -9512}, {-31113, -10278}, {-30852, -11039}, {-30571, -11793},
    {-30273, -12539}, {-29956, -13279}, {-29621, -14010}, {-29268, -14732}, {-28898, -15446}, {-28510, -16151}, {-28105, -16846}, {-27683, -17530},
    {-27245, -18204}, {-26790, -18868}, {-26319, -19519}, {-25832, -20159}, {-25329, -20787}, {-24811, -21403}, {-24279, -22005}, {-23731, -22594},
    {-23170, -23170}, {-22594, -23731}, {-22005, -24279}, {-21403, -24811}, {-20787, -25329}, {-20159, -25832}, {-19519, -26319}, {-18868, -26790},
    {-18204, -27245}, {-17530, -27683}, {-16846, -28105}, {-16151, -28510}, {-15446, -28898}, {-14732, -29268}, {-14010, -29621}, {-13279, -29956},
    {-12539, -30273}, {-11793, -30571}, {-11039, -30852}, {-10278, -31113}, {-9512, -31356}, {-8739, -31580}, {-7962, -31785}, {-7179, -31971},
    {-6393, -32137}, {-5602, -32285}, {-4808, -32412}, {-4011, -32521}, {-3212, -32609}, {-2410, -32678}, {-1608, -32728}, {-804, -32757},
    {0, -32767}, {804, -32757}, {1608, -32728}, {2410, -32678}, {3212, -32609}, {4011, -32521}, {4808, -32412}, {5602, -32285},
    {6393, -32137}, {7179, -31971}, {7962, -31785}, {8739, -31580}, {9512, -31356}, {10278, -31113}, {11039, -30852}, {11793, -30571},
    {12539, -30273}, {13279, -29956}, {14010, -29621}, {14732, -29268}, {15446, -28898}, {16151, -28510}, {16846, -28105}, {17530, -27683},
    {18204, -27245}, {18868, -26790}, {19519, -26319}, {20159, -25832}, {20787, -25329}, {21403, -24811}, {22005, -24279}, {22594, -23731},
    {23170, -23170}, {23731, -22594}, {24279, -22005}, {24811, -21403}, {25329, -20787}, {25832, -20159}, {26319, -19519}, {26790, -18868},
    {27245, -18204}, {27683, -17530}, {28105, -16846}, {28510, -16151}, {28898, -15446}, {29268, -14732}, {29621, -14010}, {29956, -13279},
    {30273, -12539}, {30571, -11793}, {30852, -11039}, {31113, -10278}, {31356, -9512}, {31580, -8739}, {31785, -7962}, {31971, -7179},
    {32137, -6393}, {32285, -5602}, {32412, -4808}, {32521, -4011}, {32609, -3212}, {32678, -2410}, {32728, -1608}, {32757, -804},
};

constexpr std::int64_t kDirScale = 32767;
constexpr std::int32_t kHalfCellMm = kCellMm / 2;
constexpr std::int32_t kDiagMm = 424;            // 0.3 m * sqrt(2)
constexpr std::int32_t kFar = 1 << 29;
constexpr std::int64_t kConeReachMm = 40000;     // a cone stops mattering 40 m above or below its base
constexpr std::int64_t kDetailFadeMm = 4000;     // detail fades in over 4 m past a ring
constexpr std::int64_t kCliffWindowQ16 = 786;    // 0.012, the cliff share's soft edge

// TerrainMapLayers.Hash's integer part: the same constants, 24 bits kept.
std::uint32_t lattice_hash(std::int32_t x, std::int32_t z, std::int32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
        static_cast<std::uint32_t>(z) * 668265263u +
        static_cast<std::uint32_t>(seed) * 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h & 0xFFFFFFu;
}

std::int64_t fade_q16(std::int64_t t) {
    const std::int64_t a = floor_div((6 * t - 15 * kQ16) * t, kQ16) + 10 * kQ16;
    const std::int64_t t3 = t * t / kQ16 * t / kQ16;   // t in [0, 1): non-negative
    return a * t3 / kQ16;
}

std::int64_t lerp_q16(std::int64_t a, std::int64_t b, std::int64_t t) {
    return a + floor_div((b - a) * t, kQ16);
}

// Direction at t / 2^24 of a turn (t < 2^24), Q15: linear between the 256
// table entries, so the angle keeps all 24 bits of the hash. (Snapping to 256
// directions turned each seed's noise domain by up to 1.4 deg, which moved a
// point 18 lattice cells out by half a cell and decorrelated it from Unity's.)
void direction(std::uint32_t t, std::int64_t* c, std::int64_t* s) {
    const std::uint32_t k = (t >> 16) & 255u, n = (k + 1u) & 255u;
    const std::int64_t f = t & 0xFFFFu;
    *c = kDirs[k][0] + floor_div((kDirs[n][0] - kDirs[k][0]) * f, 65536);
    *s = kDirs[k][1] + floor_div((kDirs[n][1] - kDirs[k][1]) * f, 65536);
}

std::int64_t lattice_dot(std::int64_t cx, std::int64_t cz, std::int64_t dx, std::int64_t dz, std::int32_t seed) {
    // Unity's gradient angle is Hash * pi with Hash in [-1, 1]: the hash's
    // fraction of a turn, offset by half a turn.
    const std::uint32_t t = (lattice_hash(static_cast<std::int32_t>(cx), static_cast<std::int32_t>(cz), seed) + 0x800000u) & 0xFFFFFFu;
    std::int64_t c = 0, s = 0;
    direction(t, &c, &s);
    return floor_div(c * dx + s * dz, kDirScale);
}

std::int64_t dist_to_rect_mm(std::int64_t x, std::int64_t z, std::int64_t x0, std::int64_t z0, std::int64_t e) {
    const std::int64_t dx = std::max<std::int64_t>({x0 - x, 0, x - (x0 + e)});
    const std::int64_t dz = std::max<std::int64_t>({z0 - z, 0, z - (z0 + e)});
    return static_cast<std::int64_t>(isqrt64(static_cast<std::uint64_t>(dx * dx + dz * dz)));
}

std::int64_t cliff_t_q16(const TerrainRecipe& r, std::int64_t x, std::int64_t z) {
    const std::int64_t n = gradient_noise_q16(x, z, r.terrace.cliff_wavelength_mm, r.seed + 808);
    const std::int64_t v = (n + kQ16) / 2;   // n >= -kQ16: non-negative
    const std::int64_t share = r.terrace.cliff_share_q16;
    return smoothstep_q16(inverse_lerp_q16(kQ16 - share - kCliffWindowQ16, kQ16 - share + kCliffWindowQ16, v));
}

std::int64_t detail_mm(const TerrainRecipe& r, std::int64_t x, std::int64_t z) {
    const std::int64_t wl = r.detail.wavelength_mm;
    const std::int64_t a = gradient_noise_q16(x, z, wl, r.seed + 202);
    const std::int64_t b = gradient_noise_q16(x, z, wl / 2, r.seed + 303);
    return floor_div(r.detail.amplitude_mm * (2 * a + b), 2 * kQ16);
}

struct Box {
    std::int32_t i0, j0, i1, j1;  // inclusive
};

Box clip_box(std::int64_t x0, std::int64_t z0, std::int64_t x1, std::int64_t z1, std::int32_t nx, std::int32_t nz) {
    // Samples whose position lies in [x0, x1] x [z0, z1] (mm).
    return Box{
        static_cast<std::int32_t>(clamp64(floor_div(x0 + kCellMm - 1, kCellMm), 0, nx - 1)),
        static_cast<std::int32_t>(clamp64(floor_div(z0 + kCellMm - 1, kCellMm), 0, nz - 1)),
        static_cast<std::int32_t>(clamp64(floor_div(x1, kCellMm), 0, nx - 1)),
        static_cast<std::int32_t>(clamp64(floor_div(z1, kCellMm), 0, nz - 1)),
    };
}

// Two-pass chamfer inside box: distance in mm from each sample to the nearest
// source sample, measured to the source's edge (half a cell less), >= 0, capped.
// TerrainMapLayers.Chamfer with weights 300 / 424.
void chamfer(const std::vector<std::uint8_t>& is_source, std::int32_t nx, const Box& b,
    std::int32_t cap, std::vector<std::int32_t>* d) {
    const std::int32_t w = b.i1 - b.i0 + 1, h = b.j1 - b.j0 + 1;
    std::vector<std::int32_t>& v = *d;
    v.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), kFar);
    auto at = [&](std::int32_t x, std::int32_t y) -> std::int32_t& {
        return v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    };
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            if (is_source[static_cast<std::size_t>(b.j0 + y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(b.i0 + x)]) {
                at(x, y) = -kHalfCellMm;
            }
        }
    }
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            std::int32_t m = at(x, y);
            if (x > 0) m = std::min(m, at(x - 1, y) + kCellMm);
            if (y > 0) {
                m = std::min(m, at(x, y - 1) + kCellMm);
                if (x > 0) m = std::min(m, at(x - 1, y - 1) + kDiagMm);
                if (x < w - 1) m = std::min(m, at(x + 1, y - 1) + kDiagMm);
            }
            at(x, y) = m;
        }
    }
    for (std::int32_t y = h - 1; y >= 0; --y) {
        for (std::int32_t x = w - 1; x >= 0; --x) {
            std::int32_t m = at(x, y);
            if (x < w - 1) m = std::min(m, at(x + 1, y) + kCellMm);
            if (y < h - 1) {
                m = std::min(m, at(x, y + 1) + kCellMm);
                if (x < w - 1) m = std::min(m, at(x + 1, y + 1) + kDiagMm);
                if (x > 0) m = std::min(m, at(x - 1, y + 1) + kDiagMm);
            }
            at(x, y) = m;
        }
    }
    for (std::int32_t& m : v) {
        m = std::clamp(m, 0, cap);
    }
}

// Distance (capped) from every sample whose level is `target` to the nearest
// sample for which source_test(level) holds; written into out where level == target.
template <typename SourceTest>
void level_distance(const std::vector<std::uint8_t>& level, std::int32_t nx, std::int32_t nz, std::int32_t target,
    SourceTest source_test, std::int32_t cap, std::vector<std::int32_t>* out,
    std::vector<std::uint8_t>* scratch_source, std::vector<std::int32_t>* scratch_dist) {
    std::int32_t i0 = nx, j0 = nz, i1 = -1, j1 = -1;
    for (std::int32_t j = 0; j < nz; ++j) {
        for (std::int32_t i = 0; i < nx; ++i) {
            if (level[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)] == target) {
                i0 = std::min(i0, i); i1 = std::max(i1, i);
                j0 = std::min(j0, j); j1 = std::max(j1, j);
            }
        }
    }
    if (i1 < 0) {
        return;
    }
    const std::int32_t pad = cap / kCellMm + 2;
    const Box b{std::max(0, i0 - pad), std::max(0, j0 - pad), std::min(nx - 1, i1 + pad), std::min(nz - 1, j1 + pad)};
    std::vector<std::uint8_t>& src = *scratch_source;
    src.assign(level.size(), 0);
    for (std::int32_t j = b.j0; j <= b.j1; ++j) {
        for (std::int32_t i = b.i0; i <= b.i1; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i);
            src[k] = source_test(level[k]) ? 1 : 0;
        }
    }
    chamfer(src, nx, b, cap, scratch_dist);
    const std::int32_t w = b.i1 - b.i0 + 1;
    for (std::int32_t j = b.j0; j <= b.j1; ++j) {
        for (std::int32_t i = b.i0; i <= b.i1; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i);
            if (level[k] == target) {
                (*out)[k] = (*scratch_dist)[static_cast<std::size_t>(j - b.j0) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i - b.i0)];
            }
        }
    }
}

}  // namespace

std::int64_t gradient_noise_q16(std::int64_t x_mm, std::int64_t z_mm, std::int64_t wavelength_mm, std::int32_t seed) {
    // Unity turns each seed's domain by (Hash(seed, 7, 99) + 1) * pi: the hash's fraction of a turn.
    std::int64_t c = 0, s = 0;
    direction(lattice_hash(seed, 7, 99), &c, &s);
    // (turned position) / wavelength in Q16 = 2 * Q15 product / wavelength.
    const std::int64_t gx = floor_div(2 * (c * x_mm - s * z_mm), wavelength_mm) + 24248 * static_cast<std::int64_t>(seed);
    const std::int64_t gz = floor_div(2 * (s * x_mm + c * z_mm), wavelength_mm) - 46531 * static_cast<std::int64_t>(seed);
    const std::int64_t ix = floor_div(gx, kQ16), iz = floor_div(gz, kQ16);
    const std::int64_t fx = gx - ix * kQ16, fz = gz - iz * kQ16;
    const std::int64_t u = fade_q16(fx), v = fade_q16(fz);
    const std::int64_t n0 = lerp_q16(lattice_dot(ix, iz, fx, fz, seed), lattice_dot(ix + 1, iz, fx - kQ16, fz, seed), u);
    const std::int64_t n1 = lerp_q16(lattice_dot(ix, iz + 1, fx, fz - kQ16, seed), lattice_dot(ix + 1, iz + 1, fx - kQ16, fz - kQ16, seed), u);
    return clamp64(floor_div(lerp_q16(n0, n1, v) * 8, 5), -kQ16, kQ16);
}

void turn_source(std::int32_t u, std::int32_t v, std::int32_t e, std::uint8_t turns, bool mirror,
    std::int32_t* lx, std::int32_t* lz) {
    switch (turns & 3u) {
    case 1: *lx = v; *lz = e - u; break;
    case 2: *lx = e - u; *lz = e - v; break;
    case 3: *lx = e - v; *lz = u; break;
    default: *lx = u; *lz = v; break;
    }
    if (mirror) {
        *lx = e - *lx;
    }
}

bool validate_recipe(const TerrainRecipe& r, const TerrainLibrary& lib, std::string* error) {
    auto fail = [&](const std::string& m) { *error = m; return false; };
    if (r.version != 1) return fail("terrain recipe version is not 1");
    if (r.cells_x <= 0 || r.cells_z <= 0 || r.cells_x % kCellsPerGrid != 0 || r.cells_z % kCellsPerGrid != 0)
        return fail("terrain map edges are not a positive multiple of 12 m");
    if (r.cells_x > 16000 || r.cells_z > 16000) return fail("terrain map is larger than 4.8 km");
    if (r.ground_map >= lib.ground_maps.size()) return fail("terrain recipe names a missing ground map");
    const GroundMapAsset& g = lib.ground_maps[r.ground_map];
    const bool odd = (r.ground_orientation & 1u) != 0;
    if (r.ground_orientation > 7) return fail("ground orientation is not 0-7");
    if (odd && g.samples_x != g.samples_z) return fail("a quarter-turned ground map must be square");
    if (g.samples_x != r.cells_x / kCellsPerGroundSample + 1 || g.samples_z != r.cells_z / kCellsPerGroundSample + 1)
        return fail("ground map size does not match the map");
    const TerraceParams& t = r.terrace;
    if (t.levels < 1 || t.levels > 255 || t.level_height_mm <= 0 || t.riser_run_mm <= 0 || t.cliff_width_mm <= 0 ||
        t.cliff_wavelength_mm <= 0 || t.cliff_share_q16 < 0 || t.cliff_share_q16 > kQ16)
        return fail("terrace parameters out of range");
    if (r.detail.wavelength_mm < 2 || r.detail.amplitude_mm < 0) return fail("detail parameters out of range");
    if (r.ring_tan_q16 <= 0) return fail("ring slope must be positive");
    for (const TerrainPlacement& p : r.placements) {
        if (p.stamp >= lib.stamps.size()) return fail("terrain placement names a missing stamp");
        const StampAsset& s = lib.stamps[p.stamp];
        if (s.cells_x != s.cells_z) return fail("stamp " + s.name + " is not square (rectangles come later)");
        if (p.turns > 3) return fail("placement turns is not 0-3");
        if (p.x_cells < 0 || p.z_cells < 0 || p.x_cells + s.cells_x > r.cells_x || p.z_cells + s.cells_z > r.cells_z)
            return fail("placement of " + s.name + " leaves the map");
        if (p.base_cm < -30000 || p.base_cm > 30000 || p.ring_mm < 0) return fail("placement base or ring out of range");
    }
    return true;
}

bool build_terrace_mm(const TerrainRecipe& r, const TerrainLibrary& lib, std::vector<std::int32_t>* out, std::string* error) {
    if (!validate_recipe(r, lib, error)) {
        return false;
    }
    const GroundMapAsset& g = lib.ground_maps[r.ground_map];
    // Whole-map turn / mirror, on the 1.2 m samples.
    const std::int32_t gx = r.cells_x / kCellsPerGroundSample + 1, gz = r.cells_z / kCellsPerGroundSample + 1;
    std::vector<std::uint16_t> gray(static_cast<std::size_t>(gx) * static_cast<std::size_t>(gz));
    for (std::int32_t v = 0; v < gz; ++v) {
        for (std::int32_t u = 0; u < gx; ++u) {
            std::int32_t lx = u, lz = v;
            if (r.ground_orientation != 0) {
                turn_source(u, v, gx - 1, r.ground_orientation & 3u, (r.ground_orientation & 4u) != 0, &lx, &lz);
            }
            gray[static_cast<std::size_t>(v) * static_cast<std::size_t>(gx) + static_cast<std::size_t>(u)] =
                g.gray[static_cast<std::size_t>(lz) * static_cast<std::size_t>(g.samples_x) + static_cast<std::size_t>(lx)];
        }
    }
    // 0.3 m samples: exact bilinear in quarters, then terrace levels.
    const std::int32_t nx = r.cells_x + 1, nz = r.cells_z + 1;
    const std::size_t n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz);
    std::vector<std::uint8_t> level(n);
    const std::int64_t full = 16 * 65535;
    for (std::int32_t j = 0; j < nz; ++j) {
        const std::int32_t gj = j / kCellsPerGroundSample, fj = j % kCellsPerGroundSample;
        for (std::int32_t i = 0; i < nx; ++i) {
            const std::int32_t gi = i / kCellsPerGroundSample, fi = i % kCellsPerGroundSample;
            auto G = [&](std::int32_t a, std::int32_t b) -> std::int64_t {
                return gray[static_cast<std::size_t>(b) * static_cast<std::size_t>(gx) + static_cast<std::size_t>(a)];
            };
            std::int64_t sum = static_cast<std::int64_t>(4 - fi) * (4 - fj) * G(gi, gj);
            if (fi) sum += static_cast<std::int64_t>(fi) * (4 - fj) * G(gi + 1, gj);
            if (fj) sum += static_cast<std::int64_t>(4 - fi) * fj * G(gi, gj + 1);
            if (fi && fj) sum += static_cast<std::int64_t>(fi) * fj * G(gi + 1, gj + 1);
            level[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((sum * r.terrace.levels * 2 + full) / (2 * full));
        }
    }
    // Distances to the next level up and down. Past cap every formula below
    // gives what an infinite distance would, so the transforms stay local.
    const std::int32_t walk = r.terrace.riser_run_mm;
    const std::int32_t cap = std::max(4000, walk * 13 / 10 + 2 * kCellMm);
    std::vector<std::int32_t> up(n, cap), down(n, cap);
    std::vector<std::uint8_t> scratch_source;
    std::vector<std::int32_t> scratch_dist;
    for (std::int32_t k = 0; k < r.terrace.levels; ++k) {
        level_distance(level, nx, nz, k, [k](std::uint8_t l) { return l > k; }, cap, &up, &scratch_source, &scratch_dist);
        level_distance(level, nx, nz, k + 1, [k](std::uint8_t l) { return l <= k; }, cap, &down, &scratch_source, &scratch_dist);
    }
    // Heights (TerrainMapLayers.BuildTexture's formula).
    out->assign(n, 0);
    const std::int64_t cw = r.terrace.cliff_width_mm;
    for (std::int32_t j = 0; j < nz; ++j) {
        for (std::int32_t i = 0; i < nx; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i);
            const std::int64_t band = static_cast<std::int64_t>(up[k]) + down[k];
            const std::int64_t tight = kQ16 - smoothstep_q16(inverse_lerp_q16(walk * 105 / 100, walk * 130 / 100, band));
            const std::int64_t m = std::max(cliff_t_q16(r, static_cast<std::int64_t>(i) * kCellMm, static_cast<std::int64_t>(j) * kCellMm), tight);
            const std::int64_t wd = walk + floor_div((cw - walk) * m, kQ16);
            std::int64_t steps = static_cast<std::int64_t>(level[k]) * kQ16;
            const std::int64_t tu = kQ16 / 2 - floor_div(static_cast<std::int64_t>(up[k]) * kQ16, wd);
            const std::int64_t td = kQ16 / 2 - floor_div(static_cast<std::int64_t>(down[k]) * kQ16, wd);
            if (tu > 0) steps += tu;
            if (td > 0) steps -= td;
            (*out)[k] = static_cast<std::int32_t>(r.terrace.black_mm + floor_div(r.terrace.level_height_mm * steps, kQ16));
        }
    }
    return true;
}

void apply_detail_and_cones(const TerrainRecipe& r, const TerrainLibrary& lib, std::vector<std::int32_t>* mm) {
    const std::int32_t nx = r.cells_x + 1, nz = r.cells_z + 1;
    std::vector<std::int32_t>& h = *mm;
    // Detail weight: 0 inside a ring, fading in over 4 m past it.
    std::vector<std::int32_t> weight(h.size(), static_cast<std::int32_t>(kQ16));
    for (const TerrainPlacement& p : r.placements) {
        const std::int64_t e = static_cast<std::int64_t>(lib.stamps[p.stamp].cells_x) * kCellMm;
        const std::int64_t x0 = static_cast<std::int64_t>(p.x_cells) * kCellMm, z0 = static_cast<std::int64_t>(p.z_cells) * kCellMm;
        const std::int64_t reach = p.ring_mm + kDetailFadeMm;
        const Box b = clip_box(x0 - reach, z0 - reach, x0 + e + reach, z0 + e + reach, nx, nz);
        for (std::int32_t j = b.j0; j <= b.j1; ++j) {
            for (std::int32_t i = b.i0; i <= b.i1; ++i) {
                const std::int64_t d = dist_to_rect_mm(static_cast<std::int64_t>(i) * kCellMm, static_cast<std::int64_t>(j) * kCellMm, x0, z0, e);
                const std::int64_t w = clamp64(floor_div((d - p.ring_mm) * kQ16, kDetailFadeMm), 0, kQ16);
                std::int32_t& slot = weight[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)];
                slot = std::min(slot, static_cast<std::int32_t>(w));
            }
        }
    }
    for (std::int32_t j = 0; j < nz; ++j) {
        for (std::int32_t i = 0; i < nx; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i);
            if (weight[k] == 0) continue;
            h[k] += static_cast<std::int32_t>(floor_div(detail_mm(r, static_cast<std::int64_t>(i) * kCellMm, static_cast<std::int64_t>(j) * kCellMm) * weight[k], kQ16));
        }
    }
    // Cones, in placement order (each sample sees the stamps in recipe order).
    const std::int64_t reach = floor_div(kConeReachMm * kQ16 + r.ring_tan_q16 - 1, r.ring_tan_q16);
    for (const TerrainPlacement& p : r.placements) {
        const std::int64_t e = static_cast<std::int64_t>(lib.stamps[p.stamp].cells_x) * kCellMm;
        const std::int64_t x0 = static_cast<std::int64_t>(p.x_cells) * kCellMm, z0 = static_cast<std::int64_t>(p.z_cells) * kCellMm;
        const std::int64_t base = static_cast<std::int64_t>(p.base_cm) * 10;
        const Box b = clip_box(x0 - reach, z0 - reach, x0 + e + reach, z0 + e + reach, nx, nz);
        for (std::int32_t j = b.j0; j <= b.j1; ++j) {
            for (std::int32_t i = b.i0; i <= b.i1; ++i) {
                const std::int64_t d = dist_to_rect_mm(static_cast<std::int64_t>(i) * kCellMm, static_cast<std::int64_t>(j) * kCellMm, x0, z0, e);
                const std::int64_t a = floor_div(d * r.ring_tan_q16, kQ16);
                if (a >= kConeReachMm) continue;
                std::int32_t& v = h[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)];
                v = static_cast<std::int32_t>(base + clamp64(v - base, -a, a));
            }
        }
    }
}

HeightGrid mm_to_cm_grid(const TerrainRecipe& r, const std::vector<std::int32_t>& mm) {
    HeightGrid g;
    g.samples_x = r.cells_x + 1;
    g.samples_z = r.cells_z + 1;
    g.cm.resize(mm.size());
    for (std::size_t k = 0; k < mm.size(); ++k) {
        g.cm[k] = static_cast<std::int16_t>(clamp64(round_div(mm[k], 10), -32768, 32767));
    }
    return g;
}

bool write_stamps(const TerrainRecipe& r, const TerrainLibrary& lib, HeightGrid* grid, std::string* error) {
    if (grid->samples_x != r.cells_x + 1 || grid->samples_z != r.cells_z + 1) {
        *error = "heightfield does not match the recipe";
        return false;
    }
    for (const TerrainPlacement& p : r.placements) {
        const StampAsset& s = lib.stamps[p.stamp];
        const std::int32_t e = s.cells_x;
        for (std::int32_t v = 0; v <= e; ++v) {
            for (std::int32_t u = 0; u <= e; ++u) {
                std::int32_t lx = 0, lz = 0;
                turn_source(u, v, e, p.turns, p.mirror, &lx, &lz);
                grid->cm[static_cast<std::size_t>(p.z_cells + v) * static_cast<std::size_t>(grid->samples_x) + static_cast<std::size_t>(p.x_cells + u)] =
                    static_cast<std::int16_t>(clamp64(static_cast<std::int64_t>(p.base_cm) + s.at(lx, lz), -32768, 32767));
            }
        }
    }
    return true;
}

bool compose(const TerrainRecipe& r, const TerrainLibrary& lib, HeightGrid* out, std::string* error, ComposeStages* stages) {
    std::vector<std::int32_t> mm;
    if (!build_terrace_mm(r, lib, &mm, error)) {
        return false;
    }
    if (stages != nullptr) {
        stages->terrace = mm_to_cm_grid(r, mm);
    }
    apply_detail_and_cones(r, lib, &mm);
    *out = mm_to_cm_grid(r, mm);
    if (stages != nullptr) {
        stages->ground = *out;
    }
    return write_stamps(r, lib, out, error);
}

}  // namespace network_example::game_server::terrain
