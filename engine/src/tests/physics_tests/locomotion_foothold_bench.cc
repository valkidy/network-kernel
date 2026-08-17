// Measures what a leg's foothold sampling actually costs against real terrain.
//
// The locomotion pass fires one downward ray per authored candidate offset per
// leg per tick (monster_sim_actor: 5 candidates x 4 legs = 20 rays), so moving
// the leg solve onto the presenting client means the client pays this on top of
// the server. Whether that is affordable is the open question; this reports the
// number instead of guessing it.
//
// Ray parameters mirror the real query in KernelEngine::update_legged_locomotion:
// origin is the foot's home stance raised by foothold_query_start_height_meters,
// direction is -Y, distance is foothold_query_distance_meters, and the filter
// accepts terrain plus static obstacles.
//
// Reported cost is wall-clock per ray on this host, which is what the decision
// needs; it is deliberately not asserted against a threshold, since a timing
// bound would be flaky in CI and would not survive a different machine.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "physics/public/physics_world.h"

namespace {

using network_example::physics::CollisionHit;
using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::PhysicsWorld;
using network_example::physics::PhysicsWorldConfig;
using network_example::physics::RayCastRequest;

// monster_sim_actor.yaml: 5 foothold candidates, 4 legs.
constexpr int kCandidatesPerLeg = 5;
constexpr int kLegs = 4;
constexpr int kRaysPerActorTick = kCandidatesPerLeg * kLegs;
constexpr float kQueryStartHeightMeters = 30.0f;
constexpr float kQueryDistanceMeters = 90.0f;

// Deliberately not assert(): this benchmark is meant to be run under -c opt,
// where NDEBUG makes assert() discard its argument unevaluated. Anything with a
// side effect (loading the scene, most of all) has to be checked for real, or
// the benchmark silently measures raycasts against an empty world.
void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "locomotion foothold bench: %s\n", what);
        std::abort();
    }
}

std::vector<std::uint8_t> read_bytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open terrain mesh");
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

RayCastRequest foothold_ray(const glm::vec3& stance) {
    RayCastRequest ray{};
    ray.origin = stance + glm::vec3(0.0f, kQueryStartHeightMeters, 0.0f);
    ray.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    ray.max_distance = kQueryDistanceMeters;
    ray.filter.collision_mask =
        network_example::physics::collision_layer_bit(CollisionLayer::kTerrain) |
        network_example::physics::collision_layer_bit(
            CollisionLayer::kStaticObstacle);
    ray.filter.object_kind_mask =
        (1u << static_cast<std::uint32_t>(CollisionObjectKind::kTerrain)) |
        (1u << static_cast<std::uint32_t>(CollisionObjectKind::kStaticObstacle));
    return ray;
}

// One actor-tick of foothold sampling: four legs splayed around the body, each
// fanning out over its authored candidate offsets.
int sample_actor_tick(PhysicsWorld* world, const glm::vec3& body, int* out_hits) {
    // Leg stances for a rig whose feet sit roughly +/-10 m from the body.
    const glm::vec3 leg_offsets[kLegs] = {
        glm::vec3(9.0f, 0.0f, 10.0f),
        glm::vec3(-9.0f, 0.0f, 10.0f),
        glm::vec3(9.0f, 0.0f, -10.0f),
        glm::vec3(-9.0f, 0.0f, -10.0f),
    };
    const glm::vec3 candidates[kCandidatesPerLeg] = {
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.2f, 0.0f, 0.0f),
        glm::vec3(-0.2f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 0.2f),
        glm::vec3(0.0f, 0.0f, -0.2f),
    };
    int rays = 0;
    for (const glm::vec3& leg : leg_offsets) {
        for (const glm::vec3& candidate : candidates) {
            CollisionHit hit{};
            if (world->ray_cast_closest(
                    foothold_ray(body + leg + candidate),
                    &hit)) {
                ++*out_hits;
            }
            ++rays;
        }
    }
    return rays;
}

// A crowd of legged rigs, as per-bone colliders in the world the foothold rays
// are fired through. 15 actors is one snapshot packet's worth and 12 boxes is
// the biped's rig, so this is the realistic worst case: 180 bodies standing in
// exactly the space the rays traverse.
//
// The point is that they cost nothing. Limbs occupy a broad phase layer of
// their own and the foothold filter does not name it, so the ray skips the
// whole subtree on one ShouldCollide -- but that is a claim about a filter, and
// the only honest way to check it is to put the bodies there and measure.
constexpr int kLimbActors = 15;
constexpr int kLimbsPerActor = 12;

void populate_limbs(PhysicsWorld* world) {
    std::string error;
    std::uint32_t collider_id = 1000;
    for (int actor = 0; actor < kLimbActors; ++actor) {
        // Spread across the same strip the body walks, at leg height, so the
        // boxes genuinely overlap the rays rather than sitting off to one side.
        const float x = -20.0f + static_cast<float>(actor) * 2.6f;
        for (int limb = 0; limb < kLimbsPerActor; ++limb) {
            network_example::physics::CollisionObjectDescriptor object{};
            object.identity.entity_net_id =
                static_cast<std::uint32_t>(actor) + 1u;
            object.identity.collider_id = ++collider_id;
            object.identity.kind = CollisionObjectKind::kActorLimb;
            object.identity.layer = CollisionLayer::kActorLimb;
            object.shape.type = network_example::physics::CollisionShapeType::kBox;
            // GEO_Leg0_Lower's real half extents on the quadruped rig.
            object.shape.half_extents = glm::vec3(0.375f, 4.75f, 0.375f);
            object.position = glm::vec3(
                x + static_cast<float>(limb % 4) * 0.6f,
                6.0f,
                static_cast<float>(limb / 4) * 9.0f - 9.0f);
            require(
                world->upsert_object(object, &error),
                "failed to add a limb collider");
        }
    }
    world->optimize_broad_phase();
}

}  // namespace

int main(int argc, char** argv) {
    require(argc >= 2, "expected a terrain mesh path argument");
    const std::vector<std::uint8_t> terrain = read_bytes(argv[1]);

    PhysicsWorld world(PhysicsWorldConfig{0});
    require(world.valid(), "physics world failed to initialize");
    CollisionObjectIdentity terrain_identity{};
    terrain_identity.collider_id = 1;
    terrain_identity.kind = CollisionObjectKind::kTerrain;
    terrain_identity.layer = CollisionLayer::kTerrain;
    std::string error;
    require(
        world.load_static_scene(terrain, terrain_identity, &error),
        "failed to load terrain static scene");

    // Walk the body across the terrain so the rays keep hitting fresh broadphase
    // nodes, rather than measuring one cache-warm spot forever. undulating.joltmesh
    // covers roughly x/z in [-50, 50] x [-30, 30]; stay inside it so every ray
    // does the full traversal-and-hit work a real foothold query does.
    constexpr int kWarmupTicks = 200;
    constexpr int kMeasuredTicks = 2000;
    const auto body_at = [](int tick) {
        const float sweep = static_cast<float>(tick % 400) * 0.1f;
        return glm::vec3(sweep - 20.0f, 10.0f, 0.0f);
    };

    int hits = 0;
    for (int tick = 0; tick < kWarmupTicks; ++tick) {
        sample_actor_tick(&world, body_at(tick), &hits);
    }

    hits = 0;
    int rays = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < kMeasuredTicks; ++tick) {
        rays += sample_actor_tick(&world, body_at(tick), &hits);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double total_us =
        std::chrono::duration<double, std::micro>(elapsed).count();

    require(rays == kMeasuredTicks * kRaysPerActorTick, "ray count mismatch");
    // The terrain has to actually be under the rays, or this measures the cost
    // of a broadphase miss and reports it as the cost of a foothold query.
    require(hits == rays, "rays missed the terrain; the walk left the mesh");

    const double per_ray_us = total_us / static_cast<double>(rays);
    const double per_actor_tick_us = per_ray_us * kRaysPerActorTick;
    std::printf(
        "locomotion foothold cost: rays=%d hits=%d total_us=%.1f "
        "per_ray_us=%.4f per_actor_tick_us=%.3f\n",
        rays,
        hits,
        total_us,
        per_ray_us,
        per_actor_tick_us);
    // Budget context: one 30 Hz tick is 33333 us. Report how many actors' worth
    // of foothold sampling fits in 1% of a tick, which is the shape of the
    // question when deciding whether the client can afford to run the leg solve
    // for every visible actor.
    std::printf(
        "locomotion foothold budget: actors_per_1pct_of_30hz_tick=%.1f "
        "actors_per_1pct_of_60fps_frame=%.1f\n",
        333.33 / per_actor_tick_us,
        166.67 / per_actor_tick_us);

    // Same walk again, with a crowd of per-bone limb colliders in the world.
    populate_limbs(&world);

    // First, that they are really there and really invisible: a ray that names
    // the limb layer finds one, and the foothold ray still hits only terrain.
    RayCastRequest limb_probe = foothold_ray(glm::vec3(-20.0f, 6.0f, -9.0f));
    limb_probe.filter.collision_mask =
        network_example::physics::collision_layer_bit(CollisionLayer::kActorLimb);
    limb_probe.filter.object_kind_mask =
        1u << static_cast<std::uint32_t>(CollisionObjectKind::kActorLimb);
    CollisionHit limb_hit{};
    require(
        world.ray_cast_closest(limb_probe, &limb_hit),
        "the limb crowd is not where the rays go");
    require(
        limb_hit.identity.kind == CollisionObjectKind::kActorLimb,
        "limb probe hit something that is not a limb");

    int limb_hits = 0;
    for (int tick = 0; tick < kWarmupTicks; ++tick) {
        sample_actor_tick(&world, body_at(tick), &limb_hits);
    }
    limb_hits = 0;
    int limb_rays = 0;
    const auto limb_start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < kMeasuredTicks; ++tick) {
        limb_rays += sample_actor_tick(&world, body_at(tick), &limb_hits);
    }
    const double limb_total_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - limb_start).count();
    // Not a timing assertion -- this one is about results. The foothold query
    // must still find terrain under every ray and nothing else, however many
    // legs are standing in the way.
    require(limb_hits == limb_rays, "limb colliders changed the foothold hits");

    const double limb_per_ray_us =
        limb_total_us / static_cast<double>(limb_rays);
    std::printf(
        "locomotion foothold with limbs: bodies=%d per_ray_us=%.4f "
        "ratio_vs_bare=%.3f\n",
        kLimbActors * kLimbsPerActor,
        limb_per_ray_us,
        limb_per_ray_us / per_ray_us);
    return 0;
}
