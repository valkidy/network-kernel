// Server-side terrain generation (implementation plan P2.1-P2.5): layout
// invariants, the navmesh bake and checks on synthetic ground, the prototype's
// seeds 1-3 against Compose Test Map's result (fe57fb3: 1 navmesh island, every
// probe reached, 11-12 of 12 stamps), and the retry / fallback ladder.
// Top-down pictures of seeds 1-3 go to bazel's undeclared outputs.

#include <DetourNavMesh.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game_server/src/terrain/terrain_generate.h"
#include "game_server/tests/terrain_test_data.h"

namespace {

namespace terrain = network_example::game_server::terrain;

// ---------------------------------------------------------------- pictures

void png_rgb(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    // Stored (uncompressed) deflate blocks: no zlib needed.
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * w * 3, rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n)); z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n)); z.push_back(static_cast<std::uint8_t>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (n == 0) break;
    }
    std::uint32_t s1 = 1, s2 = 0;
    for (std::uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    const std::uint32_t ad = (s2 << 16) | s1;
    for (int k = 3; k >= 0; --k) z.push_back(static_cast<std::uint8_t>(ad >> (8 * k)));
    auto crc = [](const std::vector<std::uint8_t>& d) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::uint8_t b : d) { c ^= b; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
        return c ^ 0xFFFFFFFFu;
    };
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& d) {
        for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(d.size() >> (8 * k)));
        std::vector<std::uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        out.insert(out.end(), td.begin(), td.end());
        const std::uint32_t c = crc(td);
        for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(c >> (8 * k)));
    };
    std::vector<std::uint8_t> ihdr;
    for (std::uint32_t v : {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)})
        for (int k = 3; k >= 0; --k) ihdr.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f != nullptr) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}

// 2 px per metre, north up: shaded heights; red = probe not reached; magenta =
// seam break; white = stamp edges.
void picture(const std::string& name, const terrain::HeightGrid& g, const terrain::TerrainRecipe& r,
    const terrain::TerrainLibrary& lib, const terrain::NavCheck& c) {
    const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    if (dir == nullptr) return;
    const int n = c.probe_nx, W = n * 2, H = static_cast<int>(c.probe_state.size() / n) * 2;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(W) * H * 3);
    const auto [lo, hi] = std::minmax_element(g.cm.begin(), g.cm.end());
    for (int py = 0; py < H; ++py)
        for (int px = 0; px < W; ++px) {
            const double x = (px + 0.5) / 2, z = (H - 1 - py + 0.5) / 2;
            const int i = std::min(g.samples_x - 2, static_cast<int>(x / 0.3)), j = std::min(g.samples_z - 2, static_cast<int>(z / 0.3));
            const double h = g.at(i, j), dx = g.at(i + 1, j) - h, dz = g.at(i, j + 1) - h;
            double v = 50 + 170.0 * (h - *lo) / std::max(1, *hi - *lo) + std::clamp((dz - dx) * 0.6, -45.0, 45.0);
            std::uint8_t cr = 0, cg = 0, cb = 0;
            const std::uint8_t st = c.probe_state[static_cast<std::size_t>(static_cast<int>(z)) * n + static_cast<int>(x)];
            cr = cg = cb = static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
            if (st == 2) { cr = 230; cg = 50; cb = 40; }
            std::uint8_t* o = &rgb[(static_cast<std::size_t>(py) * W + px) * 3];
            o[0] = cr; o[1] = cg; o[2] = cb;
        }
    auto dot = [&](double x, double z, std::uint8_t r0, std::uint8_t g0, std::uint8_t b0, int rad) {
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                const int px = static_cast<int>(x * 2) + dx, py = H - 1 - static_cast<int>(z * 2) + dy;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                std::uint8_t* o = &rgb[(static_cast<std::size_t>(py) * W + px) * 3];
                o[0] = r0; o[1] = g0; o[2] = b0;
            }
    };
    for (const terrain::TerrainPlacement& p : r.placements) {
        const double e = lib.stamps[p.stamp].cells_x * 0.3, x0 = p.x_cells * 0.3, z0 = p.z_cells * 0.3;
        for (double t = 0; t <= e; t += 0.5) {
            dot(x0 + t, z0, 255, 255, 255, 0); dot(x0 + t, z0 + e, 255, 255, 255, 0);
            dot(x0, z0 + t, 255, 255, 255, 0); dot(x0 + e, z0 + t, 255, 255, 255, 0);
        }
    }
    for (std::size_t k = 0; k + 1 < c.seam_failures_xz.size(); k += 2) dot(c.seam_failures_xz[k], c.seam_failures_xz[k + 1], 255, 0, 255, 2);
    png_rgb(std::string(dir) + "/" + name, W, H, rgb);
}

// ---------------------------------------------------------------- cases

terrain::TerrainRecipe prototype_base(const terrain::TerrainLibrary& lib) {
    terrain::TerrainRecipe r = terrain_test::parity_recipe(lib);
    r.placements.clear();
    return r;
}

void layout_invariants() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    terrain::TerrainRecipe a = prototype_base(lib), b = a;
    terrain::LayoutParams lp;
    terrain::LayoutReport ra, rb;
    std::string error;
    require(terrain::layout_stamps(&a, lib, lp, &ra, &error));
    require(terrain::layout_stamps(&b, lib, lp, &rb, &error));
    require(a.placements.size() == b.placements.size());
    for (std::size_t k = 0; k < a.placements.size(); ++k) {
        const auto& p = a.placements[k];
        const auto& q = b.placements[k];
        require(p.stamp == q.stamp && p.x_cells == q.x_cells && p.z_cells == q.z_cells && p.base_cm == q.base_cm &&
            p.turns == q.turns && p.mirror == q.mirror && p.ring_mm == q.ring_mm);
        const std::int32_t e = lib.stamps[p.stamp].cells_x;
        require(p.x_cells % terrain::kCellsPerGrid == 0 && p.z_cells % terrain::kCellsPerGrid == 0);
        require(p.x_cells >= terrain::kCellsPerGrid && p.x_cells + e <= a.cells_x - terrain::kCellsPerGrid);
        require(p.base_cm % 150 == 0);
        require(p.ring_mm >= 4000 && p.ring_mm <= 24000);
        // Claims never overlap.
        for (std::size_t m = 0; m < k; ++m) {
            const auto& o = a.placements[m];
            const double eo = lib.stamps[o.stamp].cells_x * 0.3, ep = e * 0.3;
            const double px0 = p.x_cells * 0.3 - p.ring_mm / 1000.0, px1 = p.x_cells * 0.3 + ep + p.ring_mm / 1000.0;
            const double pz0 = p.z_cells * 0.3 - p.ring_mm / 1000.0, pz1 = p.z_cells * 0.3 + ep + p.ring_mm / 1000.0;
            const double ox0 = o.x_cells * 0.3 - o.ring_mm / 1000.0, ox1 = o.x_cells * 0.3 + eo + o.ring_mm / 1000.0;
            const double oz0 = o.z_cells * 0.3 - o.ring_mm / 1000.0, oz1 = o.z_cells * 0.3 + eo + o.ring_mm / 1000.0;
            require(!(px1 > ox0 && px0 < ox1 && pz1 > oz0 && pz0 < oz1));
        }
    }
    require(terrain::validate_recipe(a, lib, &error));
    terrain::TerrainRecipe c = prototype_base(lib);
    c.seed = 2;
    terrain::LayoutReport rc;
    require(terrain::layout_stamps(&c, lib, lp, &rc, &error));
    bool differs = c.placements.size() != a.placements.size();
    for (std::size_t k = 0; !differs && k < c.placements.size(); ++k) differs = c.placements[k].x_cells != a.placements[k].x_cells || c.placements[k].z_cells != a.placements[k].z_cells;
    require(differs);
    // Openness of the Depression: a round basin, open on every side.
    for (double o : terrain::stamp_openness(lib.stamps[3])) require(o > 0.4);
}

terrain::HeightGrid synthetic(int cells, int (*cm)(int i, int j)) {
    terrain::HeightGrid g;
    g.samples_x = g.samples_z = cells + 1;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) g.cm.push_back(static_cast<std::int16_t>(cm(i, j)));
    return g;
}

int flat(int, int) { return 2000; }
int wall(int i, int) { return i < 160 ? 2000 : 2500; }                      // a 5 m cliff at 48 m
int riser(int i, int) {                                                       // 1.2 m over 2.26 m (28 deg)
    const double x = i * 0.3 - 48.0;
    return 2000 + static_cast<int>(std::lround(120.0 * std::clamp(x / 2.26 + 0.5, 0.0, 1.0)));
}

terrain::NavCheck nav_on(const terrain::HeightGrid& g) {
    terrain::NavBakeStats st;
    std::string error;
    dtNavMesh* nav = terrain::bake_terrain_navmesh(g, terrain::NavBakeParams{}, &st, &error);
    if (nav == nullptr) std::fprintf(stderr, "%s\n", error.c_str());
    require(nav != nullptr);
    terrain::TerrainRecipe r;
    r.cells_x = g.samples_x - 1;
    r.cells_z = g.samples_z - 1;
    terrain::NavCheck c;
    require(terrain::check_terrain_navmesh(*nav, g, r, terrain::TerrainLibrary{}, &c, &error));
    dtFreeNavMesh(nav);
    return c;
}

void navmesh_on_synthetic_ground() {
    const terrain::NavCheck f = nav_on(synthetic(320, flat));     // 96 m: four tiles meeting at the centre
    std::printf("flat: islands %u, reach %u/%u\n", f.islands, f.probes_reached, f.probes_on_mesh);
    require(f.islands == 1 && f.probes_reached == f.probes_on_mesh && f.probes_on_mesh > 8000);
    const terrain::NavCheck w = nav_on(synthetic(320, wall));
    std::printf("wall: islands %u, reach %u/%u\n", w.islands, w.probes_reached, w.probes_on_mesh);
    require(w.islands == 2);
    const terrain::NavCheck s = nav_on(synthetic(320, riser));
    std::printf("riser: islands %u, reach %u/%u\n", s.islands, s.probes_reached, s.probes_on_mesh);
    require(s.islands == 1 && s.probes_reached == s.probes_on_mesh);
}

// The prototype's stamps and ground, seeds 1-10, with the real accept rules and
// retries: every seed must end in an accepted map that meets Compose Test Map's
// bar (fe57fb3: 11-12 of 12 stamps, one island, every probe reached, no seam
// break). The layout draws from its own PRNG, so seed N is not Unity's seed N;
// the first-try pass rate is reported.
void prototype_seeds() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    int first_try = 0, attempts = 0, single_island = 0;
    for (std::int32_t seed = 1; seed <= 10; ++seed) {
        terrain::GenerateRequest req;
        req.base = prototype_base(lib);
        req.seed = seed;
        req.accept.min_placed = 11;
        terrain::GenerateResult res;
        std::string error;
        if (!terrain::generate_terrain(req, lib, &res, &error)) std::fprintf(stderr, "seed %d: %s\n", seed, error.c_str());
        for (const auto& a : res.attempts) std::printf("seed %2d: %s\n", seed, terrain::format_attempt(a, lib).c_str());
        require(res.outcome == terrain::GenerateOutcome::kGenerated);
        const terrain::AttemptReport& a = res.attempts.back();
        // The accept rules: >= 99.5% reach and <= 400 m2 unreachable (a stray
        // ledge may be its own little island), no seam break, 11+ stamps.
        require(a.accepted && a.layout.placed >= 11 && a.check.seams_failed == 0);
        require(a.check.reach_fraction() >= 0.995 && a.check.unreachable_area_m2 <= 400.0);
        first_try += res.attempts.size() == 1;
        attempts += static_cast<int>(res.attempts.size());
        single_island += a.check.islands == 1;
        if (seed <= 3) picture("seed" + std::to_string(seed) + ".png", res.heights, res.recipe, lib, a.check);
        dtFreeNavMesh(res.navmesh);
    }
    std::printf("prototype seeds 1-10: all accepted; %d/10 on the first layout, %d layouts in all, %d/10 with a single island\n",
        first_try, attempts, single_island);
}

void retry_then_fallback() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    terrain::GenerateRequest req;
    req.base = prototype_base(lib);
    req.max_retries = 2;
    req.layout.stamp_count = 30;                  // far more than fit
    req.min_stamp_count = 20;                     // still too many
    req.fallback = terrain_test::parity_recipe(lib);
    req.fallback_hash = 0x1463eb70b65fa735ull;    // terrain_unity_parity_test's golden hash
    terrain::GenerateResult res;
    std::string error;
    require(terrain::generate_terrain(req, lib, &res, &error));
    for (const auto& a : res.attempts) std::printf("  %s\n", terrain::format_attempt(a, lib).c_str());
    require(res.outcome == terrain::GenerateOutcome::kFallback);
    require(res.attempts.size() == 5);            // 2 retry + 2 relax + fallback
    require(res.attempts[0].seed == 1 && res.attempts[1].seed != 1);
    require(res.hash == 0x1463eb70b65fa735ull);
    dtFreeNavMesh(res.navmesh);
    // A fallback that no longer composes to its recorded hash is refused.
    req.fallback_hash ^= 1;
    terrain::GenerateResult bad;
    require(!terrain::generate_terrain(req, lib, &bad, &error));
    require(bad.outcome == terrain::GenerateOutcome::kFailed);
    require(bad.attempts.back().reason.find("recorded") != std::string::npos);
    std::printf("fallback refused: %s\n", error.c_str());
}

}  // namespace

int main() {
    layout_invariants();
    navmesh_on_synthetic_ground();
    prototype_seeds();
    retry_then_fallback();
    std::printf("terrain_generate_test passed\n");
    return 0;
}
