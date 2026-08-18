// What copying collision geometry onto weapon-fired projectiles costs.
//
// Before, fire_projectile dropped collision_geometry, so build_projectile_
// collision_query fell through to make_segment_projectile_query and every
// weapon-fired projectile resolved its hits with ray_cast_all -- a thin segment
// between its previous and current position. With the geometry present the same
// projectile resolves with shape_cast_all, sweeping the volume its collider
// template authors. A ray is the cheapest narrow-phase primitive Jolt has and a
// swept volume also presents a larger AABB to the broad phase, so this measures
// what that correctness fix costs per tick.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/collision_filter.h"
#include "simulation/public/simulation.h"

namespace ne = network_example;

namespace {

enum class Geometry { kSegmentRay, kSweptSphere, kSweptBox };

void add_actor(
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
}

void run(const char* name, Geometry geometry, int projectiles, int actors, int ticks) {
    ne::physics::PhysicsWorld physics(ne::physics::PhysicsWorldConfig{0, true});
    ne::World world;
    world.set_collision_world(&physics);

    // Bystanders: close enough to be broad-phase candidates, far enough off the
    // flight path that neither a ray nor a swept volume reaches them, so this
    // isolates query cost rather than measuring the extra hits.
    std::uint32_t collider_id = 400;
    for (int index = 0; index < actors; ++index) {
        add_actor(
            world,
            physics,
            glm::vec3{static_cast<float>(index) * 2.0f, 0.5f, 4.0f},
            collider_id++);
    }

    for (int index = 0; index < projectiles; ++index) {
        const glm::vec3 origin{0.0f, 0.5f, static_cast<float>(index) * 0.1f};
        const glm::vec3 velocity{35.0f, 0.0f, 0.0f};
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
        if (geometry != Geometry::kSegmentRay) {
            state.has_collision_geometry = true;
            if (geometry == Geometry::kSweptSphere) {
                state.collision_geometry.shape_type = ne::ColliderShapeType::kSphere;
                state.collision_geometry.radius = 0.5f;
            } else {
                state.collision_geometry.shape_type = ne::ColliderShapeType::kAabb;
                state.collision_geometry.half_extents = glm::vec3{0.2f, 0.25f, 0.2f};
            }
        }
    }

    // Jolt's broad-phase tree is built on demand and this world never steps, so
    // without this the queries below run against an unbuilt tree and measure
    // nothing useful.
    physics.optimize_broad_phase();
    physics.reset_query_stats();
    const auto started = std::chrono::steady_clock::now();
    for (int tick = 1; tick <= ticks; ++tick) {
        std::vector<KernelEvent> events;
        ne::simulate_projectiles(
            world, 1.0f / 30.0f, static_cast<std::uint32_t>(tick), &events);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const ne::physics::CollisionQueryStats stats = physics.query_stats();
    std::printf(
        "%-30s %8.2f us/tick  rays=%-7llu shape_casts=%-7llu broadphase=%llu\n",
        name,
        static_cast<double>(elapsed) / static_cast<double>(ticks),
        static_cast<unsigned long long>(stats.ray_query_count),
        static_cast<unsigned long long>(stats.shape_cast_query_count),
        static_cast<unsigned long long>(stats.broadphase_layer_filter_checks));
}

// The two primitives on their own, same world and same span, with none of
// simulate_projectiles' other per-tick work in the way.
void run_primitives(int actors, int iterations, bool stats_enabled) {
    ne::physics::PhysicsWorld physics(
        ne::physics::PhysicsWorldConfig{0, stats_enabled});
    ne::World world;
    world.set_collision_world(&physics);
    std::uint32_t collider_id = 700;
    for (int index = 0; index < actors; ++index) {
        add_actor(
            world,
            physics,
            glm::vec3{static_cast<float>(index) * 2.0f, 0.5f, 4.0f},
            collider_id++);
    }

    physics.optimize_broad_phase();

    const glm::vec3 origin{0.0f, 0.5f, 0.0f};
    // One tick of travel at 35 m/s, which is what a rocket covers.
    const glm::vec3 span{35.0f / 30.0f, 0.0f, 0.0f};
    ne::physics::CollisionQueryFilter filter =
        ne::collision_filter_from_mask(ne::kCollisionLayerHostileSide);

    ne::physics::RayCastRequest ray;
    ray.origin = origin;
    ray.direction = span;
    ray.max_distance = glm::length(span);
    ray.filter = filter;

    ne::physics::ShapeCastRequest sphere;
    sphere.shape.type = ne::physics::CollisionShapeType::kSphere;
    sphere.shape.radius = 0.5f;
    sphere.start = origin;
    sphere.displacement = span;
    sphere.filter = filter;

    ne::physics::ShapeCastRequest box;
    box.shape.type = ne::physics::CollisionShapeType::kBox;
    box.shape.half_extents = glm::vec3{0.2f, 0.25f, 0.2f};
    box.start = origin;
    box.displacement = span;
    box.filter = filter;

    const auto time = [iterations](const char* name, auto&& call) {
        const auto started = std::chrono::steady_clock::now();
        std::size_t sink = 0;
        for (int index = 0; index < iterations; ++index) {
            sink += call().size();
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        std::printf("%-30s %8.0f ns/query  (hits seen %zu)\n",
                    name,
                    static_cast<double>(elapsed) /
                        static_cast<double>(iterations),
                    sink);
    };

    std::printf("\nquery primitives alone, %d bystanders, %d iterations, "
                "query stats %s\n",
                actors, iterations, stats_enabled ? "ON" : "OFF");
    time("  ray_cast_all (before)", [&] { return physics.ray_cast_all(ray); });
    time("  shape_cast_all sphere (after)",
         [&] { return physics.shape_cast_all(sphere); });
    time("  shape_cast_all box (after)",
         [&] { return physics.shape_cast_all(box); });
}

}  // namespace

int main() {
    constexpr int kTicks = 4000;
    for (const int count : {8, 32}) {
        std::printf("\n%d projectiles in flight, 24 bystanders, %d ticks\n",
                    count, kTicks);
        run("  segment ray (before)", Geometry::kSegmentRay, count, 24, kTicks);
        run("  swept sphere r=0.5 (after)", Geometry::kSweptSphere, count, 24, kTicks);
        run("  swept box 0.2/0.25/0.2 (after)", Geometry::kSweptBox, count, 24, kTicks);
    }
    run_primitives(24, 200000, true);
    run_primitives(24, 200000, false);
    return 0;
}
