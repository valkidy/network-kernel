// Timings for terrain generation at 216 m up to the 1440 m target and a long
// map of the same area (implementation plan P2.6 / P6). Run with -c opt: a dbg
// build is ~27x slower (server-perf-baseline-and-build-config).
//
//   bazel run -c opt //game_server:terrain_generate_bench
//
// Bigger maps mirror-tile ground_0001 and scale the stamp count with area.
// Per size: the client's cost (compose stages, collision artifact, loading it
// into a physics world), then one whole server attempt (layout, compose, bake,
// checks). The heights / placements hashes catch an optimization that changed
// a result.

#include <DetourNavMesh.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "game_server/src/terrain/terrain_generate.h"
#include "game_server/src/terrain/terrain_physics.h"
#include "game_server/tests/terrain_test_data.h"
#include "kernel/public/kernel_types.h"
#include "physics/public/physics_world.h"

namespace terrain = network_example::game_server::terrain;

namespace {

double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

int main() {
    terrain::TerrainLibrary lib = terrain_test::load_library();
    const terrain::GroundMapAsset src = lib.ground_maps[0];   // 181 x 181, 216 m
    std::printf("hardware threads: %u, static scene limit %u MB\n", std::thread::hardware_concurrency(),
        KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES >> 20);
    struct Size { int x, z; };
    for (const Size size : {Size{216, 216}, Size{504, 504}, Size{1008, 1008}, Size{1440, 1440}, Size{960, 2160}}) {
        terrain::GroundMapAsset g;
        g.name = "mirror";
        g.samples_x = size.x * 10 / 12 + 1;
        g.samples_z = size.z * 10 / 12 + 1;
        auto fold = [&](int k) { k %= 2 * (src.samples_x - 1); return k > src.samples_x - 1 ? 2 * (src.samples_x - 1) - k : k; };
        for (int j = 0; j < g.samples_z; ++j)
            for (int i = 0; i < g.samples_x; ++i) g.gray.push_back(src.gray[static_cast<std::size_t>(fold(j)) * src.samples_x + fold(i)]);
        lib.ground_maps = {g};
        terrain::GenerateRequest req;
        req.base = terrain_test::parity_recipe(lib);
        req.base.placements.clear();
        req.base.cells_x = size.x * 10 / 3;
        req.base.cells_z = size.z * 10 / 3;
        req.layout.stamp_count = static_cast<int>(12.0 * size.x * size.z / (216.0 * 216.0) + 0.5);
        const std::string name = std::to_string(size.x) + "x" + std::to_string(size.z);

        // The client's cost.
        terrain::TerrainRecipe r = req.base;
        r.seed = 1;
        terrain::LayoutReport lr;
        std::string e;
        const auto t0 = std::chrono::steady_clock::now();
        if (!terrain::layout_stamps(&r, lib, req.layout, &lr, &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
        const auto t1 = std::chrono::steady_clock::now();
        std::vector<std::int32_t> mm;
        if (!terrain::build_terrace_mm(r, lib, &mm, &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
        const auto t2 = std::chrono::steady_clock::now();
        terrain::apply_detail_and_cones(r, lib, &mm);
        const auto t3 = std::chrono::steady_clock::now();
        terrain::HeightGrid h = terrain::mm_to_cm_grid(r, mm);
        if (!terrain::write_stamps(r, lib, &h, &e)) return 1;
        const auto t4 = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> artifact;
        terrain::TerrainArtifactStats st;
        if (!terrain::build_static_scene_artifact(h, terrain::TerrainArtifactOptions{}, &artifact, &st, &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
        const auto t5 = std::chrono::steady_clock::now();
        bool loaded = false;
        {
            network_example::physics::PhysicsWorld world;
            network_example::physics::CollisionObjectIdentity id;
            id.collider_id = 1;
            id.scene_id = 1;
            id.kind = network_example::physics::CollisionObjectKind::kTerrain;
            id.layer = network_example::physics::CollisionLayer::kTerrain;
            loaded = world.valid() && world.load_static_scene(artifact, id, &e);
        }
        const auto t6 = std::chrono::steady_clock::now();
        std::uint64_t ph = 1469598103934665603ull;
        for (const auto& q : r.placements)
            for (std::int64_t v : {std::int64_t(q.stamp), std::int64_t(q.x_cells), std::int64_t(q.z_cells), std::int64_t(q.base_cm), std::int64_t(q.ring_mm), std::int64_t(q.turns), std::int64_t(q.mirror)}) {
                ph ^= static_cast<std::uint64_t>(v);
                ph *= 1099511628211ull;
            }
        std::printf("%9s client: compose %.0f ms (terrace %.0f, detail+cones %.0f, stamps %.0f), artifact %.1f MB in %.0f ms (%u tiles, %s the limit), physics load %.0f ms %s | heights %016llx placements %016llx (%d/%d, layout %.0f ms)\n",
            name.c_str(), ms(t1, t4), ms(t1, t2), ms(t2, t3), ms(t3, t4), artifact.size() / 1048576.0, ms(t4, t5), st.tiles,
            artifact.size() <= KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES ? "within" : "OVER", ms(t5, t6), loaded ? "ok" : e.c_str(),
            static_cast<unsigned long long>(terrain::height_grid_hash(h)), static_cast<unsigned long long>(ph), lr.placed, lr.picked, ms(t0, t1));

        // One whole server attempt.
        req.max_retries = 1;
        req.accept.min_placed = 1;
        req.accept.min_reach_fraction = 0;
        req.accept.max_unreachable_m2 = 1e12;
        req.accept.max_seam_failures = 1u << 30;
        terrain::GenerateResult res;
        if (!terrain::generate_terrain(req, lib, &res, &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
        std::printf("%9s server: %s\n", name.c_str(), terrain::format_attempt(res.attempts.front(), lib).c_str());
        dtFreeNavMesh(res.navmesh);
    }
    return 0;
}
