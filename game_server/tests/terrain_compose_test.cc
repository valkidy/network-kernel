// Unit checks for the terrain compose core (implementation plan P1.1-P1.5):
// integer helpers, asset formats, noise, the turn rule, terrace risers, cones
// and stamps, on small synthetic inputs.

#include <algorithm>
#include <cstdio>
#include <set>

#include "game_server/tests/terrain_test_data.h"

namespace {

namespace terrain = network_example::game_server::terrain;
using terrain::kQ16;

void integer_helpers() {
    require(terrain::floor_div(7, 2) == 3);
    require(terrain::floor_div(-7, 2) == -4);
    require(terrain::floor_div(-8, 2) == -4);
    require(terrain::round_div(5, 10) == 1);
    require(terrain::round_div(-5, 10) == 0);
    require(terrain::round_div(-6, 10) == -1);
    require(terrain::isqrt64(0) == 0);
    require(terrain::isqrt64(15) == 3);
    require(terrain::isqrt64(16) == 4);
    require(terrain::isqrt64(1ull << 62) == (1ull << 31));
    require(terrain::smoothstep_q16(0) == 0);
    require(terrain::smoothstep_q16(kQ16) == kQ16);
    require(terrain::smoothstep_q16(kQ16 / 2) == kQ16 / 2);
    require(terrain::inverse_lerp_q16(10, 20, 15) == kQ16 / 2);
    // FNV-1a 64 of "a" (published test vector).
    terrain::Fnv64 h;
    h.bytes("a", 1);
    require(h.value() == 0xaf63dc4c8601ec8cull);
}

void asset_formats() {
    terrain::HeightGrid g;
    g.samples_x = 41;
    g.samples_z = 81;
    g.cm.assign(41 * 81, 0);
    g.cm[41 * 40 + 20] = -1234;
    std::string error;
    terrain::StampAsset s;
    require(terrain::parse_stamp(terrain::write_nkhf(g, 1), "rect", &s, &error));
    require(s.cells_x == 40 && s.cells_z == 80 && s.at(20, 40) == -1234);
    // Not a stamp: flags clear; a border sample off 0; not a 12 m multiple.
    require(!terrain::parse_stamp(terrain::write_nkhf(g, 0), "x", &s, &error));
    g.cm[0] = 5;
    require(!terrain::parse_stamp(terrain::write_nkhf(g, 1), "x", &s, &error));
    require(error.find("border") != std::string::npos);
    g.cm[0] = 0;
    terrain::HeightGrid odd = g;
    odd.samples_x = 42;
    odd.cm.assign(42 * 81, 0);
    require(!terrain::parse_stamp(terrain::write_nkhf(odd, 1), "x", &s, &error));
    // Truncated.
    std::vector<std::uint8_t> b = terrain::write_nkhf(g, 1);
    b.pop_back();
    require(!terrain::parse_stamp(b, "x", &s, &error));

    terrain::GroundMapAsset m;
    m.samples_x = 21;
    m.samples_z = 11;
    m.gray.assign(21 * 11, 7);
    m.gray[5] = 65535;
    terrain::GroundMapAsset back;
    require(terrain::parse_ground_map(terrain::write_nkgm(m), "m", &back, &error));
    require(back.samples_x == 21 && back.samples_z == 11 && back.gray == m.gray);
    m.samples_x = 22;
    m.gray.assign(22 * 11, 0);
    require(!terrain::parse_ground_map(terrain::write_nkgm(m), "m", &back, &error));
}

void noise_is_bounded_varied_and_fixed() {
    std::int64_t lo = kQ16, hi = -kQ16, sum = 0;
    std::set<std::int64_t> seen;
    for (int k = 0; k < 4000; ++k) {
        const std::int64_t x = (k % 63) * 1777 - 40000, z = (k / 63) * 2311 - 30000;
        const std::int64_t n = terrain::gradient_noise_q16(x, z, 12000, 7);
        require(n == terrain::gradient_noise_q16(x, z, 12000, 7));
        lo = std::min(lo, n);
        hi = std::max(hi, n);
        sum += n;
        seen.insert(n);
    }
    require(lo >= -kQ16 && hi <= kQ16);
    require(hi - lo > kQ16);                 // spans more than half the range
    require(seen.size() > 3000);
    require(std::llabs(sum / 4000) < kQ16 / 8);
    // A different seed is a different field.
    require(terrain::gradient_noise_q16(5000, 5000, 12000, 7) != terrain::gradient_noise_q16(5000, 5000, 12000, 8));
    // Locked values: a change here changes every map (and must be deliberate).
    require(terrain::gradient_noise_q16(0, 0, 12000, 1) == -45973);
    require(terrain::gradient_noise_q16(123456, -654321, 25000, 809) == -33514);
    require(terrain::gradient_noise_q16(-1, 98765, 6000, 304) == -5087);
}

void turn_rule_matches_compose_test_map() {
    // TerrainMapComposer.Compose, e = 10, output (u, v) = (2, 3).
    std::int32_t lx = 0, lz = 0;
    terrain::turn_source(2, 3, 10, 0, false, &lx, &lz); require(lx == 2 && lz == 3);
    terrain::turn_source(2, 3, 10, 1, false, &lx, &lz); require(lx == 3 && lz == 8);
    terrain::turn_source(2, 3, 10, 2, false, &lx, &lz); require(lx == 8 && lz == 7);
    terrain::turn_source(2, 3, 10, 3, false, &lx, &lz); require(lx == 7 && lz == 2);
    terrain::turn_source(2, 3, 10, 1, true, &lx, &lz); require(lx == 7 && lz == 8);
    // Every one of the 8 is a bijection on the square.
    for (std::uint8_t t = 0; t < 4; ++t) {
        for (int m = 0; m < 2; ++m) {
            std::set<int> hit;
            for (int v = 0; v <= 10; ++v)
                for (int u = 0; u <= 10; ++u) {
                    terrain::turn_source(u, v, 10, t, m == 1, &lx, &lz);
                    require(lx >= 0 && lx <= 10 && lz >= 0 && lz <= 10);
                    hit.insert(lz * 11 + lx);
                }
            require(hit.size() == 121);
        }
    }
}

// A 48 m map whose ground map is level 5 west of x = 24 m and level 6 east of it.
terrain::TerrainLibrary step_library() {
    terrain::TerrainLibrary lib;
    terrain::GroundMapAsset g;
    g.samples_x = g.samples_z = 41;
    for (int j = 0; j < 41; ++j)
        for (int i = 0; i < 41; ++i)
            g.gray.push_back(static_cast<std::uint16_t>(i < 20 ? 65535 * 5 / 19 : 65535 * 6 / 19));
    lib.ground_maps.push_back(g);
    return lib;
}

terrain::TerrainRecipe step_recipe() {
    terrain::TerrainRecipe r;
    r.seed = 3;
    r.cells_x = r.cells_z = 160;
    return r;
}

// Width (m) and steepest slope (deg x 10) of the riser along row 80.
void riser_profile(const terrain::TerrainRecipe& r, const terrain::TerrainLibrary& lib, double* width_m, double* steepest) {
    std::vector<std::int32_t> mm;
    std::string error;
    require(terrain::build_terrace_mm(r, lib, &mm, &error));
    const std::int32_t lo = r.terrace.black_mm + 5 * r.terrace.level_height_mm;
    const std::int32_t hi = lo + r.terrace.level_height_mm;
    int first = -1, last = -1;
    *steepest = 0;
    for (int i = 0; i < 161; ++i) {
        const std::int32_t h = mm[80 * 161 + i];
        require(h >= lo && h <= hi);
        if (h != lo && h != hi) {
            if (first < 0) first = i;
            last = i;
        }
        if (i > 0) {
            const double rise = h - mm[80 * 161 + i - 1];
            *steepest = std::max(*steepest, std::atan(rise / 300.0) * 180.0 / M_PI);
        }
    }
    // A 0.3 m cliff can fall between two samples: then no sample is mid-riser.
    *width_m = first < 0 ? 0.3 : (last - first + 2) * 0.3;
}

void terrace_riser_is_a_walkable_slope_or_a_cliff() {
    terrain::TerrainLibrary lib = step_library();
    terrain::TerrainRecipe r = step_recipe();
    r.terrace.cliff_share_q16 = 0;
    double width = 0, steepest = 0;
    riser_profile(r, lib, &width, &steepest);
    std::printf("riser, no cliffs: %.2f m wide, steepest %.1f deg\n", width, steepest);
    // 1.2 m over the 2.257 m run is 28 deg; one sample of quantization either side.
    require(width >= 1.9 && width <= 2.9);
    require(steepest >= 25.0 && steepest <= 31.0);
    r.terrace.cliff_share_q16 = kQ16;   // every riser a cliff
    riser_profile(r, lib, &width, &steepest);
    std::printf("riser, all cliffs: %.2f m wide, steepest %.1f deg\n", width, steepest);
    require(width <= 0.9);
    require(steepest >= 60.0);
}

void orientation_turns_the_whole_ground_map() {
    terrain::TerrainLibrary lib = step_library();
    terrain::TerrainRecipe r = step_recipe();
    std::vector<std::int32_t> a, b;
    std::string error;
    require(terrain::build_terrace_mm(r, lib, &a, &error));
    r.ground_orientation = 1;     // a quarter turn: the step now runs east-west
    require(terrain::build_terrace_mm(r, lib, &b, &error));
    require(a != b);
    // The step is along x before and along z after: west / east before, south / north after.
    require(a[80 * 161 + 10] < a[80 * 161 + 150]);
    require(b[10 * 161 + 80] != b[150 * 161 + 80]);
    require(b[10 * 161 + 10] == b[10 * 161 + 150]);
    require(b[150 * 161 + 10] == b[150 * 161 + 150]);
    r.ground_orientation = 9;
    require(!terrain::build_terrace_mm(r, lib, &b, &error));
}

void cones_cap_the_ground_around_a_stamp() {
    terrain::TerrainLibrary lib;
    terrain::GroundMapAsset g;
    g.samples_x = g.samples_z = 41;
    g.gray.assign(41 * 41, 65535 * 10 / 19);    // flat at level 10 = 24 m
    lib.ground_maps.push_back(g);
    terrain::HeightGrid st;
    st.samples_x = st.samples_z = 81;
    st.cm.assign(81 * 81, 0);
    terrain::StampAsset s;
    std::string error;
    require(terrain::parse_stamp(terrain::write_nkhf(st, 1), "flat24", &s, &error));
    lib.stamps.push_back(s);
    terrain::TerrainRecipe r = step_recipe();
    r.detail.amplitude_mm = 0;
    terrain::TerrainPlacement p;
    p.x_cells = p.z_cells = 40;      // 12 m .. 36 m
    p.base_cm = 1800;                // 6 m below the ground
    p.ring_mm = 11000;
    r.placements.push_back(p);
    terrain::HeightGrid out;
    require(terrain::compose(r, lib, &out, &error));
    // On the footprint: the base. Out at distance d: base + d * tan30 until it meets 24 m.
    require(out.at(60, 60) == 1800);
    double worst = 0;
    for (int i = 120; i < 160; ++i) {
        const double d = (i - 120) * 0.3;
        const double want = std::min(24.0, 18.0 + d * std::tan(M_PI / 6)) * 100.0;
        worst = std::max(worst, std::abs(out.at(i, 60) - want));
    }
    std::printf("cone: worst |height - (base + d tan30)| = %.1f cm\n", worst);
    require(worst <= 1.0);
    require(out.at(160, 60) == 2400);
}

void stamps_land_at_base_with_the_turn_rule() {
    terrain::TerrainLibrary lib = terrain_test::load_library();
    terrain::TerrainRecipe r;
    r.cells_x = r.cells_z = 720;
    terrain::TerrainPlacement p;
    p.stamp = 2;                         // L_concave, asymmetric
    p.x_cells = 40;
    p.z_cells = 80;
    p.base_cm = 2000;
    p.turns = 1;
    p.mirror = true;
    r.placements.push_back(p);
    terrain::HeightGrid g;
    g.samples_x = g.samples_z = 721;
    g.cm.assign(721 * 721, 777);
    std::string error;
    require(terrain::write_stamps(r, lib, &g, &error));
    const terrain::StampAsset& s = lib.stamps[2];
    for (int v = 0; v <= 80; v += 7)
        for (int u = 0; u <= 80; u += 5) {
            std::int32_t lx = 0, lz = 0;
            terrain::turn_source(u, v, 80, 1, true, &lx, &lz);
            require(g.at(40 + u, 80 + v) == 2000 + s.at(lx, lz));
        }
    require(g.at(39, 80) == 777 && g.at(121, 80) == 777);
}

void recipe_validation() {
    terrain::TerrainLibrary lib = terrain_test::load_library();
    terrain::TerrainRecipe r = terrain_test::parity_recipe(lib);
    std::string error;
    require(terrain::validate_recipe(r, lib, &error));
    terrain::TerrainRecipe bad = r;
    bad.cells_x += 1;
    require(!terrain::validate_recipe(bad, lib, &error));
    bad = r;
    bad.placements[0].x_cells = r.cells_x - 10;
    require(!terrain::validate_recipe(bad, lib, &error));
    bad = r;
    bad.placements[0].stamp = 99;
    require(!terrain::validate_recipe(bad, lib, &error));
    bad = r;
    bad.cells_x = bad.cells_z = 760;     // ground map is for 216 m
    require(!terrain::validate_recipe(bad, lib, &error));
}

}  // namespace

int main() {
    integer_helpers();
    asset_formats();
    noise_is_bounded_varied_and_fixed();
    turn_rule_matches_compose_test_map();
    terrace_riser_is_a_walkable_slope_or_a_cliff();
    orientation_turns_the_whole_ground_map();
    cones_cap_the_ground_around_a_stamp();
    stamps_land_at_base_with_the_turn_rule();
    recipe_validation();
    std::printf("terrain_compose_test passed\n");
    return 0;
}
