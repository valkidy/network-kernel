// Measures what the two-pass beam query costs against what it saves.
//
// The single-pass version swept the beam's whole authored length with an
// all-hit collector, which cannot early out: it gathered every hit along the
// full length, sorted them, and only then discarded everything past the first
// blocker. The two-pass version finds the blocker with a shrinking closest-hit
// cast and then sweeps for actors only as far as the beam actually reaches.
//
// That is a trade, not a free win -- in open air it pays for two casts instead
// of one. These are the numbers that say whether it is worth it.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"

namespace ne = network_example;

namespace {

struct Scenario {
    const char* name;
    bool wall;
    float wall_distance;
    int actor_count;
};

ne::NetId add_actor(
    ne::World& world,
    ne::physics::PhysicsWorld& physics,
    const glm::vec3& position,
    std::uint32_t collider_id) {
    const ne::NetId net_id = world.spawn_enemy(position);
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    world.registry().get<ne::Health>(*entity) = ne::Health{10000, 10000};
    ne::physics::CollisionObjectDescriptor object;
    object.identity = ne::physics::CollisionObjectIdentity{
        net_id, collider_id, 0,
        ne::physics::CollisionObjectKind::kActorHitbox,
        ne::physics::CollisionLayer::kDamageable,
    };
    object.identity.gameplay_category = ne::kCollisionLayerHostileSide;
    object.shape.type = ne::physics::CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3{0.35f, 0.9f, 0.35f};
    object.position = position;
    std::string error;
    assert(physics.upsert_object(object, &error));
    return net_id;
}

void run(const Scenario& scenario, int ticks) {
    ne::physics::PhysicsWorld physics(ne::physics::PhysicsWorldConfig{0, true});
    ne::World world;
    world.set_collision_world(&physics);
    const ne::NetId shooter = world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});

    std::uint32_t collider_id = 500;
    for (int index = 0; index < scenario.actor_count; ++index) {
        // Spread along the beam, past the wall as well as before it.
        add_actor(
            world,
            physics,
            glm::vec3{1.0f + static_cast<float>(index) * 0.7f, 0.5f, 0.0f},
            collider_id++);
    }
    if (scenario.wall) {
        ne::physics::CollisionObjectDescriptor wall;
        wall.identity = ne::physics::CollisionObjectIdentity{
            900, 900, 0,
            ne::physics::CollisionObjectKind::kStaticObstacle,
            ne::physics::CollisionLayer::kStaticObstacle,
        };
        wall.shape.type = ne::physics::CollisionShapeType::kBox;
        wall.shape.half_extents = glm::vec3{0.25f, 3.0f, 3.0f};
        wall.position = glm::vec3{scenario.wall_distance, 0.5f, 0.0f};
        std::string error;
        assert(physics.upsert_object(wall, &error));
    }

    const ne::NetId beam_net_id =
        world.spawn_projectile(1, glm::vec3{0.0f, 0.5f, 0.0f}, glm::vec3{0.0f});
    const auto beam_entity = world.find_entity(beam_net_id);
    assert(beam_entity.has_value());
    world.registry().get<ne::ProjectileState>(*beam_entity).max_lifetime_ticks = 0;
    world.registry().emplace<ne::ProjectileBeamRuntime>(
        *beam_entity,
        ne::ProjectileBeamRuntime{
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            14.0f,
            0.3f,
            1,
            0,
            5,
            ne::kCollisionLayerHostileSide | ne::kCollisionLayerTerrain |
                ne::kCollisionLayerStaticObstacle,
        });

    physics.reset_query_stats();
    const auto started = std::chrono::steady_clock::now();
    for (int tick = 1; tick <= ticks; ++tick) {
        std::vector<KernelEvent> events;
        ne::simulate_beams(
            world,
            static_cast<std::uint32_t>(tick),
            1.0f / 30.0f,
            static_cast<std::uint64_t>(tick) * 33333ull,
            &events,
            nullptr);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const ne::physics::CollisionQueryStats stats = physics.query_stats();
    std::printf(
        "%-24s %8.2f us/tick  casts=%-5llu broadphase=%-6llu "
        "object_checks=%-6llu raw_hits=%llu\n",
        scenario.name,
        static_cast<double>(elapsed) / static_cast<double>(ticks),
        static_cast<unsigned long long>(stats.shape_cast_query_count),
        static_cast<unsigned long long>(stats.broadphase_layer_filter_checks),
        static_cast<unsigned long long>(stats.object_layer_filter_checks),
        static_cast<unsigned long long>(stats.raw_jolt_hits_collected));
}

}  // namespace

int main() {
    constexpr int kTicks = 20000;
    std::printf("beam occlusion, %d ticks each\n", kTicks);
    run({"open air, no actors", false, 0.0f, 0}, kTicks);
    run({"open air, 12 actors", false, 0.0f, 12}, kTicks);
    run({"wall at 1m, 12 actors", true, 1.0f, 12}, kTicks);
    run({"wall at 4m, 12 actors", true, 4.0f, 12}, kTicks);
    run({"wall at 13m, 12 actors", true, 13.0f, 12}, kTicks);
    return 0;
}
