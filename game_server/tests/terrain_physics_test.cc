// The composed heightfield as a kernel static scene (implementation plan
// P1.7 / P1.8): the artifact loads through PhysicsWorld::load_static_scene, its
// surface matches the int16 heights (including on tile seams), a character
// capsule walks across tile seams without catching, and how big the artifact
// gets (T5: the 16 MB KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game_server/src/terrain/terrain_physics.h"
#include "game_server/tests/terrain_test_data.h"
#include "physics/public/physics_world.h"

namespace {

namespace terrain = network_example::game_server::terrain;
namespace physics = network_example::physics;

physics::CollisionObjectIdentity terrain_identity() {
    physics::CollisionObjectIdentity id;
    id.collider_id = 1;
    id.scene_id = 1;
    id.kind = physics::CollisionObjectKind::kTerrain;
    id.layer = physics::CollisionLayer::kTerrain;
    return id;
}

std::vector<std::uint8_t> artifact_for(const terrain::HeightGrid& g, const terrain::TerrainArtifactOptions& o,
    terrain::TerrainArtifactStats* stats = nullptr) {
    std::vector<std::uint8_t> a;
    std::string error;
    if (!terrain::build_static_scene_artifact(g, o, &a, stats, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        require(false);
    }
    return a;
}

bool height_at(const physics::PhysicsWorld& w, float x, float z, float* y) {
    physics::RayCastRequest r;
    r.origin = glm::vec3(x, 500.0f, z);
    r.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    r.max_distance = 1000.0f;
    physics::CollisionHit hit;
    if (!w.ray_cast_closest(r, &hit)) return false;
    *y = hit.position.y;
    return true;
}

// Every stride-th sample, plus every sample on a tile seam: the surface must be
// there, and at the sample height.
float surface_error(const terrain::HeightGrid& g, const terrain::TerrainArtifactOptions& o, int stride, int* seam_samples) {
    physics::PhysicsWorld world;
    require(world.valid());
    std::string error;
    require(world.load_static_scene(artifact_for(g, o), terrain_identity(), &error));
    world.optimize_broad_phase();
    const float ox = terrain::terrain_world_origin_x(g), oz = terrain::terrain_world_origin_z(g);
    float worst = 0;
    *seam_samples = 0;
    for (int j = 0; j < g.samples_z; ++j) {
        for (int i = 0; i < g.samples_x; ++i) {
            const bool seam = (i % o.tile_cells == 0) || (j % o.tile_cells == 0);
            if (!seam && (i % stride != 0 || j % stride != 0)) continue;
            // Nudge in from the outer map edge so the ray hits inside the field.
            // A ray straight through a vertex can graze past every triangle that
            // shares it; aim a few mm off the sample (and in from the map edge).
            const float x = ox + 0.3f * std::clamp(static_cast<float>(i) + 0.011f, 0.011f, g.samples_x - 1.011f);
            const float z = oz + 0.3f * std::clamp(static_cast<float>(j) + 0.007f, 0.007f, g.samples_z - 1.007f);
            float y = 0;
            if (!height_at(world, x, z, &y)) {
                std::fprintf(stderr, "no surface at sample (%d, %d) world (%.3f, %.3f) seam=%d\n", i, j, x, z, seam ? 1 : 0);
                require(false);
            }
            // 3 mm off the sample: allow the steepest neighbouring rise over that.
            const int i1 = std::min(i + 1, g.samples_x - 1), j1 = std::min(j + 1, g.samples_z - 1);
            const float slope = std::max({std::abs(g.at(i1, j) - g.at(i, j)), std::abs(g.at(i, j1) - g.at(i, j)),
                std::abs(g.at(i1, j1) - g.at(i, j))}) / 100.0f / 0.3f;
            const float want = g.at(i, j) / 100.0f;
            worst = std::max(worst, std::fabs(y - want) - slope * 0.0045f);
            if (seam) ++*seam_samples;
        }
    }
    return worst;
}

terrain::HeightGrid synthetic(int cells, float (*h)(float x, float z)) {
    terrain::HeightGrid g;
    g.samples_x = g.samples_z = cells + 1;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            g.cm.push_back(static_cast<std::int16_t>(std::lround(h(i * 0.3f, j * 0.3f) * 100.0f)));
    return g;
}

struct Walk {
    float travelled = 0;
    float expected = 0;
    int grounded_ticks = 0;
    int ticks = 0;
    float slowest_tick = 1e9f;   // horizontal metres in one tick
};

// A player-sized capsule pushed at 5 m/s from `from` toward `to`, 30 Hz.
Walk walk(const terrain::HeightGrid& g, glm::vec2 from, glm::vec2 to, int ticks) {
    physics::PhysicsWorld world;
    require(world.valid());
    std::string error;
    require(world.load_static_scene(artifact_for(g, terrain::TerrainArtifactOptions{}), terrain_identity(), &error));
    world.optimize_broad_phase();
    physics::CharacterDescriptor c;
    c.character_id = 77;
    c.shape.type = physics::CollisionShapeType::kCapsule;
    c.shape.radius = 0.4f;
    c.shape.capsule_half_height = 0.5f;
    c.shape.local_center = glm::vec3(0.0f, 0.9f, 0.0f);
    require(world.upsert_character(c, &error));
    float y = 0;
    require(height_at(world, from.x, from.y, &y));
    glm::vec3 pos(from.x, y + 0.05f, from.y);
    glm::vec3 vel(0.0f);
    const glm::vec2 dir = glm::normalize(to - from);
    const float dt = 1.0f / 30.0f;
    Walk w;
    for (int t = 0; t < ticks; ++t) {
        physics::CharacterMoveRequest m;
        m.character_id = 77;
        m.current_position = pos;
        // CharacterVirtual moves by the velocity it is given; gravity is the
        // caller's to integrate (as the kernel's movement does).
        m.linear_velocity = glm::vec3(dir.x * 5.0f, vel.y - 9.81f * dt, dir.y * 5.0f);
        m.delta_seconds = dt;
        physics::CharacterMoveResult r;
        require(world.move_character(m, &r, &error));
        const float step = glm::length(glm::vec2(r.position.x - pos.x, r.position.z - pos.z));
        if (t > 5) w.slowest_tick = std::min(w.slowest_tick, step);
        if (t > 5 && step < 0.8f * 5.0f / 30.0f && std::getenv("TERRAIN_WALK_TRACE") != nullptr)
            std::printf("  slow tick %d: (%.2f, %.2f, %.2f) moved %.3f state %d vel (%.2f, %.2f, %.2f)\n", t, r.position.x, r.position.y, r.position.z, step,
                static_cast<int>(r.ground_state), r.linear_velocity.x, r.linear_velocity.y, r.linear_velocity.z);
        w.travelled += step;
        if (r.ground_state == physics::CharacterGroundState::kGrounded) ++w.grounded_ticks;
        pos = r.position;
        vel = r.linear_velocity;
        // Standing on ground cancels the fall (the kernel's movement does the same).
        if (r.ground_state == physics::CharacterGroundState::kGrounded) vel.y = 0.0f;
        ++w.ticks;
    }
    w.expected = 5.0f * dt * static_cast<float>(ticks);
    return w;
}

float flat(float, float) { return 20.0f; }
float ramp(float x, float) { return 10.0f + x * std::tan(10.0f * static_cast<float>(terrain_test::kPi) / 180.0f); }
float rolling(float x, float z) { return 20.0f + 1.5f * std::sin(x / 7.0f) * std::cos(z / 9.0f); }

void check_walk(const char* name, const terrain::HeightGrid& g, glm::vec2 a, glm::vec2 b) {
    const Walk w = walk(g, a, b, 360);
    std::printf("walk %-8s travelled %.2f of %.2f m, grounded %d/%d ticks, slowest tick %.3f m (of %.3f)\n",
        name, w.travelled, w.expected, w.grounded_ticks, w.ticks, w.slowest_tick, 5.0f / 30.0f);
    require(w.travelled >= 0.95f * w.expected);
    require(w.slowest_tick >= 0.8f * 5.0f / 30.0f);
    require(w.grounded_ticks >= w.ticks - 10);
}

}  // namespace

int main() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    const terrain::TerrainRecipe recipe = terrain_test::parity_recipe(lib);
    terrain::HeightGrid map;
    std::string error;
    require(terrain::compose(recipe, lib, &map, &error));

    // Surface vs the int16 heights, for a few Jolt block sizes / precisions.
    struct Variant { std::uint32_t block, bits; float bound; };
    for (const Variant v : {Variant{4, 8, 0.05f}, Variant{8, 8, 0.05f}, Variant{8, 12, 0.01f}, Variant{4, 16, 0.01f}}) {
        terrain::TerrainArtifactOptions o;
        o.block_size = v.block;
        o.bits_per_sample = v.bits;
        terrain::TerrainArtifactStats st;
        const std::size_t bytes = artifact_for(map, o, &st).size();
        int seams = 0;
        const float err = surface_error(map, o, 3, &seams);
        std::printf("216 m, block %u, %2u bits/sample: %u tiles, artifact %zu bytes (%.2f B/sample), worst |surface - height| %.3f m (%d seam samples + every 3rd)\n",
            v.block, v.bits, st.tiles, bytes, static_cast<double>(bytes) / map.cm.size(), err, seams);
        require(err <= v.bound);
    }

    // T5: size on a larger map (the 216 m map repeated), extrapolated to 1440 m.
    terrain::HeightGrid big;
    big.samples_x = big.samples_z = 3361;    // 1008 m
    big.cm.resize(static_cast<std::size_t>(3361) * 3361);
    for (int j = 0; j < 3361; ++j)
        for (int i = 0; i < 3361; ++i)
            big.cm[static_cast<std::size_t>(j) * 3361 + i] = map.at(i % 720, j % 720);
    for (std::uint32_t block : {2u, 4u, 8u}) {
        terrain::TerrainArtifactOptions o;
        o.block_size = block;
        const std::uint32_t bits = o.bits_per_sample;
        const std::size_t bytes = artifact_for(big, o).size();
        const double per = static_cast<double>(bytes) / big.cm.size();
        std::printf("1008 m, block %u, %2u bits/sample: artifact %.1f MB (%.2f B/sample) -> 1440 m about %.1f MB (limit 16 MB)\n",
            block, bits, bytes / 1048576.0, per, per * 4801.0 * 4801.0 / 1048576.0);
    }

    // Walking across tile seams (48 m tiles; on a 96 m map the seam is at world 0).
    check_walk("flat", synthetic(320, flat), {-20.0f, 3.0f}, {20.0f, 3.0f});
    check_walk("corner", synthetic(320, flat), {-14.0f, -14.0f}, {14.0f, 14.0f});
    check_walk("ramp", synthetic(320, ramp), {-20.0f, 1.0f}, {20.0f, 1.0f});
    check_walk("rolling", synthetic(320, rolling), {-20.0f, -12.0f}, {20.0f, 12.0f});
    std::printf("terrain_physics_test passed\n");
    return 0;
}
