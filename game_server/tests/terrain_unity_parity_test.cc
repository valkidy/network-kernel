// The kernel compose against Compose Test Map's own output (implementation
// plan P1.5 / P1.6), stage by stage, on the seed 1 layout exported by
// ComposeParityExporter (unity-terrain-example claude/stamp-export 7a775cc).
//
// Unity runs in float and builds its terraces from the full 1254 px image; the
// kernel runs in integers from the 1.2 m export. So the stages are compared the
// way they can be:
//   stamps   given Unity's own ground, writing the stamps must reproduce
//            Unity's composed map to the centimetre (exact rule, exact data);
//   cones    given Unity's terraces, detail + cones must land close (the
//            detail noise is a port, 0.4 m amplitude);
//   terraces from the ground map, most samples on the same level and close;
//   all      the full kernel compose against Unity's final map.
// Then the golden hash: the kernel compose of this recipe, locked. The same
// number must come out on macOS arm64, Linux x64 and Windows x64.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "game_server/tests/terrain_test_data.h"

namespace {

namespace terrain = network_example::game_server::terrain;

struct Diff {
    int max_cm = 0;
    double mean_cm = 0;
    double within_5cm = 0;
    double within_30cm = 0;
    double p99_cm = 0;
};

Diff compare(const terrain::HeightGrid& a, const terrain::HeightGrid& b) {
    require(a.samples_x == b.samples_x && a.samples_z == b.samples_z);
    std::vector<int> d(a.cm.size());
    double sum = 0;
    std::size_t w5 = 0, w30 = 0;
    for (std::size_t k = 0; k < a.cm.size(); ++k) {
        d[k] = std::abs(a.cm[k] - b.cm[k]);
        sum += d[k];
        w5 += d[k] <= 5;
        w30 += d[k] <= 30;
    }
    Diff r;
    r.mean_cm = sum / static_cast<double>(d.size());
    r.within_5cm = static_cast<double>(w5) / static_cast<double>(d.size());
    r.within_30cm = static_cast<double>(w30) / static_cast<double>(d.size());
    std::vector<int> s = d;
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() * 99 / 100), s.end());
    r.p99_cm = s[s.size() * 99 / 100];
    r.max_cm = *std::max_element(d.begin(), d.end());
    return r;
}

void print(const char* what, const Diff& d) {
    std::printf("%-9s max %5d cm  mean %6.2f cm  p99 %5.0f cm  <=5 cm %6.2f%%  <=30 cm %6.2f%%\n",
        what, d.max_cm, d.mean_cm, d.p99_cm, 100 * d.within_5cm, 100 * d.within_30cm);
}

std::vector<std::int32_t> to_mm(const terrain::HeightGrid& g) {
    std::vector<std::int32_t> mm(g.cm.size());
    for (std::size_t k = 0; k < g.cm.size(); ++k) mm[k] = g.cm[k] * 10;
    return mm;
}

}  // namespace

int main() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    const terrain::TerrainRecipe recipe = terrain_test::parity_recipe(lib);
    const terrain::HeightGrid u_terrace = terrain_test::parity_stage("terrace_seed1.hf");
    const terrain::HeightGrid u_ground = terrain_test::parity_stage("ground_seed1.hf");
    const terrain::HeightGrid u_composed = terrain_test::parity_stage("composed_seed1.hf");
    std::string error;

    // stamps: exact rule on Unity's own ground.
    terrain::HeightGrid stamped = u_ground;
    require(terrain::write_stamps(recipe, lib, &stamped, &error));
    const Diff stamps = compare(stamped, u_composed);
    print("stamps", stamps);
    require(stamps.max_cm <= 1);

    // cones: Unity's terraces through the kernel's detail + cones.
    std::vector<std::int32_t> mm = to_mm(u_terrace);
    terrain::apply_detail_and_cones(recipe, lib, &mm);
    const Diff cones = compare(terrain::mm_to_cm_grid(recipe, mm), u_ground);
    print("cones", cones);

    // Stage dumps for a look outside the test (bazel's undeclared outputs).
    auto dump = [](const char* name, const terrain::HeightGrid& g) {
        const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
        if (dir == nullptr) return;
        const std::vector<std::uint8_t> b = terrain::write_nkhf(g, 0);
        std::FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "wb");
        if (f != nullptr) { std::fwrite(b.data(), 1, b.size(), f); std::fclose(f); }
    };
    dump("kernel_cones_on_unity_terrace.hf", terrain::mm_to_cm_grid(recipe, mm));

    // terraces and the whole compose.
    terrain::HeightGrid out;
    terrain::ComposeStages stages;
    require(terrain::compose(recipe, lib, &out, &error, &stages));
    const Diff terraces = compare(stages.terrace, u_terrace);
    print("terraces", terraces);
    const Diff all = compare(out, u_composed);
    print("all", all);

    // Measured 2026-11-04: cones max 1 cm / mean 0.09 cm; terraces mean 1.66 cm,
    // p99 15 cm, 99.92% within 30 cm, worst 114 cm (a level boundary one sample
    // over: Unity builds terraces from the 1254 px image, the kernel from the
    // 1.2 m export); all mean 1.31 cm, 99.93% within 30 cm.
    require(cones.max_cm <= 2);
    require(terraces.mean_cm <= 3.0 && terraces.within_30cm >= 0.995);
    require(all.mean_cm <= 3.0 && all.within_30cm >= 0.995);

    // Determinism within a run, then the golden hash.
    terrain::HeightGrid again;
    require(terrain::compose(recipe, lib, &again, &error));
    require(again.cm == out.cm);
    const std::uint64_t hash = terrain::height_grid_hash(out);
    std::printf("golden hash %016" PRIx64 "\n", hash);
    // Locked 2026-11-04 on macOS arm64. Any change here changes every map:
    // a deliberate compose change updates it; a platform that disagrees has a bug.
    require(hash == 0x1463eb70b65fa735ull);
    std::printf("terrain_unity_parity_test passed\n");
    return 0;
}
