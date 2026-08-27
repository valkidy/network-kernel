// What a travelling area effect costs per tick, and where that cost actually
// sits.
//
// A travelling field pays for two queries. simulate_area_effects runs one
// sphere overlap of the whole radius, and it runs it every tick regardless of
// damage_interval_ticks -- the interval is checked per target after the query,
// not before it. simulate_projectiles adds a swept query of the field's own
// collider, but only for a field that authored a motion_collision_mask.
//
// The rows below are the same world and the same population, differing only in
// what the field authored and where it sits.
//
// Two things come out of them. The sweep is close to free -- it moves
// simulate_projectiles from ~2.0 to ~3.1 us/tick, and runs at all only for a
// field that authored a mask -- while simulate_area_effects costs two orders of
// magnitude more in the same ticks. And the driver of that cost is not how many
// targets the overlap finds but how deeply it sits inside them: a blast parked
// on top of an actor costs roughly nine times one parked between two, while
// finding fewer. A travelling field lands in between because it is only briefly
// co-located with anything.
//
// None of that is new here. It is what the overlap has always cost; the reason
// to write it down is that rocket_explosion spawns at its own impact point,
// which is to say on top of whoever was hit -- the expensive case, every time.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"
#include "simulation/public/simulation.h"

namespace ne = network_example;

namespace {

void add_actor(
    ne::World& world,
    ne::physics::PhysicsWorld& physics,
    const glm::vec3& position,
    std::uint32_t collider_id) {
    const ne::NetId net_id = world.spawn_enemy(position);
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    world.registry().get<ne::Health>(*entity) = ne::Health{60000, 60000};
    world.registry().get<ne::Hitbox>(*entity) =
        ne::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
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
}

void run(
    const char* name,
    float speed,
    std::uint32_t motion_collision_mask,
    int actors,
    int ticks,
    float origin_x = 0.0f) {
    ne::physics::PhysicsWorld physics(ne::physics::PhysicsWorldConfig{0, true});
    ne::World world;
    world.set_collision_world(&physics);

    // Spread along the path so the field keeps finding new targets as it goes,
    // which is the case a stationary blast never has.
    std::uint32_t collider_id = 900;
    for (int index = 0; index < actors; ++index) {
        add_actor(
            world,
            physics,
            glm::vec3{static_cast<float>(index) * 1.5f, 0.0f, 0.0f},
            collider_id++);
    }

    const glm::vec3 origin{origin_x, 0.5f, 0.0f};
    const glm::vec3 velocity{speed, 0.0f, 0.0f};
    const ne::NetId net_id = world.spawn_projectile(1, origin, velocity);
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    ne::ProjectileState& state =
        world.registry().get<ne::ProjectileState>(*entity);
    state.collision_mask = ne::kCollisionLayerHostileSide;
    state.max_lifetime_ticks = 0;
    state.spawn_position = origin;
    state.initial_velocity = velocity;
    state.previous_position = origin;
    // The field's own body, which is what the sweep moves. Deliberately much
    // smaller than the radius it damages with: that asymmetry is the whole
    // reason the two queries do not cost the same.
    state.has_collision_geometry = true;
    state.collision_geometry.shape_type = ne::ColliderShapeType::kSphere;
    state.collision_geometry.radius = 0.5f;
    world.registry().replace<ne::Hitbox>(
        *entity, ne::Hitbox{{0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}, 0});
    world.registry().emplace<ne::ProjectileAreaEffectRuntime>(
        *entity,
        ne::ProjectileAreaEffectRuntime{
            5.0f,
            20,
            // One damage tick in five. The overlap below still runs on all
            // five, which is the point this benchmark is here to show.
            5,
            0,
            7,
            ne::kCollisionLayerHostileSide,
            ne::ProjectileDamageFalloff::kNone,
            false,
            motion_collision_mask,
            {},
        });

    physics.optimize_broad_phase();
    ne::DamagePipeline pipeline;
    std::int64_t projectiles_us = 0;
    std::int64_t area_effects_us = 0;
    const auto tick_once = [&](int tick, bool measure) {
        std::vector<KernelEvent> events;
        const auto server_tick = static_cast<std::uint32_t>(tick);
        auto mark = std::chrono::steady_clock::now();
        ne::simulate_projectiles(world, 1.0f / 30.0f, server_tick, &events);
        const auto after_projectiles = std::chrono::steady_clock::now();
        ne::simulate_area_effects(world, server_tick, &events, &pipeline);
        const auto after_area = std::chrono::steady_clock::now();
        if (measure) {
            projectiles_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    after_projectiles - mark)
                    .count();
            area_effects_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    after_area - after_projectiles)
                    .count();
        }
    };

    // Jolt builds its tree lazily and the first pass through this loop also
    // pays for every allocation it will ever make, so an unwarmed first row
    // reads several times slower than the same work does afterwards. Warm, then
    // reset both clocks and counters.
    for (int tick = 1; tick <= ticks; ++tick) {
        tick_once(tick, false);
    }

    physics.reset_query_stats();
    const auto started = std::chrono::steady_clock::now();
    for (int tick = ticks + 1; tick <= ticks * 2; ++tick) {
        tick_once(tick, true);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const ne::physics::CollisionQueryStats stats = physics.query_stats();
    std::printf(
        "%-34s %7.2f us/tick (projectiles %6.2f + area %6.2f)  "
        "overlaps=%-5llu shape_casts=%-5llu "
        "objects_filtered=%-6llu hits=%llu\n",
        name,
        static_cast<double>(elapsed) / static_cast<double>(ticks),
        static_cast<double>(projectiles_us) / static_cast<double>(ticks),
        static_cast<double>(area_effects_us) / static_cast<double>(ticks),
        static_cast<unsigned long long>(stats.overlap_query_count),
        static_cast<unsigned long long>(stats.shape_cast_query_count),
        static_cast<unsigned long long>(stats.object_layer_filter_checks),
        static_cast<unsigned long long>(stats.final_hits_accepted));
}

}  // namespace

int main() {
    constexpr int kActors = 64;
    constexpr int kTicks = 90;
    std::printf("area effect motion, %d actors, %d ticks\n", kActors, kTicks);
    run("still blast", 0.0f, 0u, kActors, kTicks);
    run("travelling, crosses walls", 6.0f, 0u, kActors, kTicks);
    run("travelling, world stops it", 6.0f,
        KERNEL_COLLISION_LAYER_TERRAIN | KERNEL_COLLISION_LAYER_STATIC_OBSTACLE,
        kActors, kTicks);
    // Same row as the first, run last: if the two disagree, the number is
    // measuring position in this list rather than the config.
    run("still blast (again)", 0.0f, 0u, kActors, kTicks);
    // Same still blast, parked where the travelling one ends up. If this reads
    // like the travelling rows rather than like the still ones, what the first
    // row measures is where it sits, not that it is standing still.
    run("still blast, parked at x=18", 0.0f, 0u, kActors, kTicks, 18.0f);
    // Actors sit every 1.5 m from the origin, so both still rows above are
    // parked exactly on top of one. This one sits between two instead.
    run("still blast, between actors", 0.0f, 0u, kActors, kTicks, 0.75f);
    return 0;
}
