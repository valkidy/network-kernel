// Timings for the server's terrain generation at 216 / 504 / 1008 m
// (implementation plan P2.6). Run with -c opt: a dbg build is ~27x slower
// (server-perf-baseline-and-build-config).
//
//   bazel run -c opt //game_server:terrain_generate_bench
//
// The bigger maps mirror-tile ground_0001 and scale the stamp count with area.

#include <DetourNavMesh.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "game_server/src/terrain/terrain_generate.h"
#include "game_server/tests/terrain_test_data.h"

namespace terrain = network_example::game_server::terrain;

int main() {
    terrain::TerrainLibrary lib = terrain_test::load_library();
    const terrain::GroundMapAsset src = lib.ground_maps[0];   // 181 x 181, 216 m
    std::printf("hardware threads: %u\n", std::thread::hardware_concurrency());
    for (const int size_m : {216, 504, 1008}) {
        const int n = size_m * 10 / 12 + 1;   // 1.2 m samples
        terrain::GroundMapAsset g;
        g.name = "mirror";
        g.samples_x = g.samples_z = n;
        auto fold = [&](int k) { k %= 2 * (src.samples_x - 1); return k > src.samples_x - 1 ? 2 * (src.samples_x - 1) - k : k; };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) g.gray.push_back(src.gray[static_cast<std::size_t>(fold(j)) * src.samples_x + fold(i)]);
        lib.ground_maps = {g};
        terrain::GenerateRequest req;
        req.base = terrain_test::parity_recipe(lib);
        req.base.placements.clear();
        req.base.cells_x = req.base.cells_z = size_m * 10 / 3;
        req.layout.stamp_count = static_cast<int>(12.0 * size_m * size_m / (216.0 * 216.0) + 0.5);
        req.max_retries = 1;
        req.accept.min_placed = 1;
        req.accept.min_reach_fraction = 0;
        req.accept.max_unreachable_m2 = 1e12;
        req.accept.max_seam_failures = 1u << 30;
        {
            // Compose stage by stage on one layout (what a client pays).
            terrain::TerrainRecipe r = req.base;
            r.seed = 1;
            terrain::LayoutReport lr;
            std::string e;
            auto t0 = std::chrono::steady_clock::now();
            if (!terrain::layout_stamps(&r, lib, req.layout, &lr, &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
            auto t1 = std::chrono::steady_clock::now();
            std::vector<std::int32_t> mm;
            if (!terrain::build_terrace_mm(r, lib, &mm, &e)) return 1;
            auto t2 = std::chrono::steady_clock::now();
            terrain::apply_detail_and_cones(r, lib, &mm);
            auto t3 = std::chrono::steady_clock::now();
            terrain::HeightGrid g = terrain::mm_to_cm_grid(r, mm);
            if (!terrain::write_stamps(r, lib, &g, &e)) return 1;
            auto t4 = std::chrono::steady_clock::now();
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            std::uint64_t ph = 1469598103934665603ull;
            for (const auto& q : r.placements) for (std::int64_t v : {std::int64_t(q.stamp), std::int64_t(q.x_cells), std::int64_t(q.z_cells), std::int64_t(q.base_cm), std::int64_t(q.ring_mm), std::int64_t(q.turns), std::int64_t(q.mirror)}) { ph ^= static_cast<std::uint64_t>(v); ph *= 1099511628211ull; }
            std::printf("%4d m placements %016llx\n", size_m, static_cast<unsigned long long>(ph));
            std::printf("%4d m stages: layout %.0f ms (%d/%d placed) | terrace %.0f ms | detail+cones %.0f ms | stamps %.0f ms | hash %016llx\n",
                size_m, ms(t0, t1), lr.placed, lr.picked, ms(t1, t2), ms(t2, t3), ms(t3, t4),
                static_cast<unsigned long long>(terrain::height_grid_hash(g)));
        }
        for (const std::uint32_t threads : {0u}) {
            req.nav.threads = threads;
            terrain::GenerateResult res;
            std::string error;
            if (!terrain::generate_terrain(req, lib, &res, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
            const terrain::AttemptReport& a = res.attempts.front();
            std::printf("%4d m, %s: %s\n", size_m, threads == 1 ? "1 thread " : "all threads", terrain::format_attempt(a, lib).c_str());
            dtFreeNavMesh(res.navmesh);
        }
    }
    return 0;
}
