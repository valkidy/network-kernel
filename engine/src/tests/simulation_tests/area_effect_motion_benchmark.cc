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
// The last three rows are the tornado's additions (2026-10-03, -c opt, numbers
// drift by about 2x between runs on the same machine, so compare rows within
// one run). Riding the ground costs two rays a tick -- the probe and the sweep
// -- and about 0.45 us on top of a straight-line field's ~0.15. A cylinder and
// a sphere of the same radius cost the same at the straight rows' height
// (2.8 vs 2.7 us of area); at the ground follower's 1 m the sphere happened to
// be ~3x cheaper (0.8 vs 2.6), which is the depth-of-overlap effect above, not
// the shape. A whole tornado is a few microseconds a tick.
//
// None of that is new here. It is what the overlap has always cost; the reason
// to write it down is that rocket_explosion spawns at its own impact point,
// which is to say on top of whoever was hit -- the expensive case, every time.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"
#include "simulation/public/simulation.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

namespace ne = network_example;

namespace {

// Not assert(): this is run under -c opt, where assert compiles out along with
// the upsert inside it, and every row then times an empty world.
void require_upserted(bool upserted, const std::string& error) {
    if (!upserted) {
        std::fprintf(stderr, "upsert failed: %s\n", error.c_str());
        std::abort();
    }
}

void add_actor(
    ne::World& world,
    ne::physics::PhysicsWorld& physics,
    const glm::vec3& position,
    std::uint32_t collider_id) {
    const ne::NetId net_id = world.spawn_enemy(position);
    const auto entity = world.find_entity(net_id);
    require(entity.has_value());
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
    require_upserted(physics.upsert_object(object, &error), error);
}

void run(
    const char* name,
    float speed,
    std::uint32_t motion_collision_mask,
    int actors,
    int ticks,
    float origin_x = 0.0f,
    bool ground_follow = false,
    bool cylinder = false) {
    ne::physics::PhysicsWorld physics(ne::physics::PhysicsWorldConfig{0, true});
    ne::World world;
    world.set_collision_world(&physics);
    // Ground under the whole path, only for the rows that ride it: the other
    // rows predate it, and a box their queries never ask about would still
    // sit in the broad phase they are timing.
    if (ground_follow) {
        ne::physics::CollisionObjectDescriptor ground;
        ground.identity = ne::physics::CollisionObjectIdentity{
            0, 899, 0,
            ne::physics::CollisionObjectKind::kTerrain,
            ne::physics::CollisionLayer::kTerrain,
        };
        ground.shape.type = ne::physics::CollisionShapeType::kBox;
        ground.shape.half_extents = glm::vec3{100.0f, 0.5f, 10.0f};
        ground.position = glm::vec3{60.0f, -0.5f, 0.0f};
        std::string error;
        require_upserted(physics.upsert_object(ground, &error), error);
    }

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

    // A ground follower hovers 1 m up; the straight-line rows keep the 0.5 m
    // they were measured at.
    const glm::vec3 origin{origin_x, ground_follow ? 1.0f : 0.5f, 0.0f};
    const glm::vec3 velocity{speed, 0.0f, 0.0f};
    const ne::NetId net_id = world.spawn_projectile(1, origin, velocity);
    const auto entity = world.find_entity(net_id);
    require(entity.has_value());
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
    auto& area_effect =
        world.registry().get<ne::ProjectileAreaEffectRuntime>(*entity);
    if (cylinder) {
        // Radius 5 across, 1 m up and down from the centre: the column from
        // the ground to 2 m, where the sphere reaches 6 m up.
        area_effect.shape = ne::AreaEffectShape::kCylinder;
        area_effect.half_height = 1.0f;
    }
    if (ground_follow) {
        area_effect.ground_follow =
            ne::AreaEffectGroundFollow{true, 1.0f, 50.0f, 0.5f, 0.5f};
    }

    physics.optimize_broad_phase();
    ne::DamagePipeline pipeline;
    std::int64_t projectiles_ns = 0;
    std::int64_t area_effects_ns = 0;
    const auto tick_once = [&](int tick, bool measure) {
        std::vector<KernelEvent> events;
        const auto server_tick = static_cast<std::uint32_t>(tick);
        auto mark = std::chrono::steady_clock::now();
        ne::simulate_projectiles(world, 1.0f / 30.0f, server_tick, &events);
        const auto after_projectiles = std::chrono::steady_clock::now();
        ne::simulate_area_effects(world, server_tick, &events, &pipeline);
        const auto after_area = std::chrono::steady_clock::now();
        // Nanoseconds: a tick of either system can take under a microsecond,
        // and summing whole microseconds per tick read those as zero.
        if (measure) {
            projectiles_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after_projectiles - mark)
                    .count();
            area_effects_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
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
        "overlaps=%-5llu shape_casts=%-5llu rays=%-5llu "
        "objects_filtered=%-6llu hits=%llu\n",
        name,
        static_cast<double>(elapsed) / static_cast<double>(ticks),
        static_cast<double>(projectiles_ns) / 1000.0 / static_cast<double>(ticks),
        static_cast<double>(area_effects_ns) / 1000.0 / static_cast<double>(ticks),
        static_cast<unsigned long long>(stats.overlap_query_count),
        static_cast<unsigned long long>(stats.shape_cast_query_count),
        static_cast<unsigned long long>(stats.ray_query_count),
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
    // The tornado's two additions, one at a time and then together. Riding the
    // ground costs a probe ray and a sweep ray each tick; the column bounds the
    // same 5 m radius to 2 m of height.
    run("ground-follow, sphere", 6.0f, KERNEL_COLLISION_LAYER_TERRAIN,
        kActors, kTicks, 0.0f, true, false);
    run("straight, cylinder", 6.0f, 0u, kActors, kTicks, 0.0f, false, true);
    run("ground-follow, cylinder", 6.0f, KERNEL_COLLISION_LAYER_TERRAIN,
        kActors, kTicks, 0.0f, true, true);
    return 0;
}
