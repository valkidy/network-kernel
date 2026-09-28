// The descent launch rule: a projectile spawned at a landing target starts
// above it and falls onto it in a straight line after fall_ticks.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

constexpr float kTickSeconds = 1.0f / 30.0f;

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

bool near(const glm::vec3& lhs, const glm::vec3& rhs, float tolerance) {
    return glm::length(lhs - rhs) <= tolerance;
}

network_example::RuntimeProjectileTemplate descent_template(
    float elevation_min,
    float elevation_max) {
    network_example::RuntimeProjectileTemplate projectile_template;
    projectile_template.projectile_template_id = 41;
    projectile_template.projectile_type =
        network_example::ProjectileType::kStandard;
    projectile_template.motion_model =
        network_example::ProjectileMotionModel::kLinear;
    projectile_template.sync_mode =
        network_example::ProjectileSyncMode::kServerSnapshotOnly;
    projectile_template.damage = 0;
    projectile_template.damage_shape =
        network_example::ProjectileDamageShape::kNone;
    projectile_template.speed = 0.0f;
    projectile_template.lifetime_ticks = 20;
    projectile_template.collision_mask = KERNEL_COLLISION_LAYER_TERRAIN;
    projectile_template.launch_type =
        network_example::ProjectileLaunchType::kDescent;
    projectile_template.launch_elevation_min_degrees = elevation_min;
    projectile_template.launch_elevation_max_degrees = elevation_max;
    projectile_template.launch_height = 40.0f;
    projectile_template.launch_fall_ticks = 15;
    return projectile_template;
}

float elevation_degrees(const glm::vec3& velocity) {
    return glm::degrees(std::asin(-velocity.y / glm::length(velocity)));
}

// A flat slab of terrain whose top is at y = 1.
void add_ground(network_example::physics::PhysicsWorld* physics) {
    network_example::physics::CollisionObjectDescriptor ground;
    ground.identity = network_example::physics::CollisionObjectIdentity{
        0,
        200,
        0,
        network_example::physics::CollisionObjectKind::kTerrain,
        network_example::physics::CollisionLayer::kTerrain,
    };
    ground.shape.type = network_example::physics::CollisionShapeType::kBox;
    ground.shape.half_extents = glm::vec3{50.0f, 0.5f, 50.0f};
    ground.position = glm::vec3{0.0f, 0.5f, 0.0f};
    std::string error;
    require(physics->upsert_object(ground, &error));
}

// At a fixed 80 degrees the start is fully determined: 40 m up, and back along
// the heading by 40 / tan(80). A heading with a vertical part counts only for
// its horizontal direction.
void descent_starts_above_and_behind_and_lands_on_time() {
    const network_example::RuntimeProjectileTemplate projectile_template =
        descent_template(80.0f, 80.0f);
    const glm::vec3 target{10.0f, 2.0f, -3.0f};
    const network_example::ProjectileLaunch launch =
        network_example::descent_launch(
            projectile_template,
            target,
            glm::vec3{0.0f, -0.9f, 0.3f},
            network_example::projectile_launch_seed(1, 2, 41, 0),
            kTickSeconds,
            nullptr);
    const float behind = 40.0f / std::tan(glm::radians(80.0f));
    require(near(
        launch.origin,
        glm::vec3{10.0f, 42.0f, -3.0f - behind},
        1e-3f));
    const glm::vec3 arrival =
        launch.origin + launch.velocity * (15.0f * kTickSeconds);
    require(near(arrival, target, 1e-3f));
    require(std::fabs(elevation_degrees(launch.velocity) - 80.0f) < 1e-3f);
}

// The elevation is picked per seed inside the authored range, spreads across
// it, and repeats for a repeated seed.
void elevation_is_picked_per_seed_within_the_range() {
    const network_example::RuntimeProjectileTemplate ranged =
        descent_template(75.0f, 85.0f);
    float lowest = 90.0f;
    float highest = 0.0f;
    for (std::uint32_t action = 1; action <= 64; ++action) {
        const std::uint64_t seed =
            network_example::projectile_launch_seed(7, action, 41, 0);
        const network_example::ProjectileLaunch launch =
            network_example::descent_launch(
                ranged, glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, seed,
                kTickSeconds, nullptr);
        const float elevation = elevation_degrees(launch.velocity);
        require(elevation >= 75.0f - 1e-3f && elevation <= 85.0f + 1e-3f);
        lowest = std::min(lowest, elevation);
        highest = std::max(highest, elevation);

        const network_example::ProjectileLaunch again =
            network_example::descent_launch(
                ranged, glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, seed,
                kTickSeconds, nullptr);
        require(launch.origin == again.origin);
        require(launch.velocity == again.velocity);
    }
    // 64 draws that stayed within two degrees of each other would mean the
    // seed is not reaching the pick.
    require(lowest < 77.0f);
    require(highest > 83.0f);
}

void every_seed_input_changes_the_seed() {
    const std::uint64_t base =
        network_example::projectile_launch_seed(1, 2, 3, 4);
    require(network_example::projectile_launch_seed(1, 2, 3, 4) == base);
    require(network_example::projectile_launch_seed(9, 2, 3, 4) != base);
    require(network_example::projectile_launch_seed(1, 9, 3, 4) != base);
    require(network_example::projectile_launch_seed(1, 2, 9, 4) != base);
    require(network_example::projectile_launch_seed(1, 2, 3, 9) != base);
}

// A target above the ground is dropped onto it; with no ground under it, it is
// used as given.
void target_is_dropped_onto_the_ground_under_it() {
    network_example::physics::PhysicsWorld physics;
    add_ground(&physics);
    const network_example::RuntimeProjectileTemplate projectile_template =
        descent_template(80.0f, 80.0f);
    const std::uint64_t seed =
        network_example::projectile_launch_seed(1, 2, 41, 0);

    const glm::vec3 floating{3.0f, 6.0f, 4.0f};
    const network_example::ProjectileLaunch grounded =
        network_example::descent_launch(
            projectile_template, floating, glm::vec3{1.0f, 0.0f, 0.0f}, seed,
            kTickSeconds, &physics);
    const glm::vec3 landing =
        grounded.origin + grounded.velocity * (15.0f * kTickSeconds);
    require(near(landing, glm::vec3{3.0f, 1.0f, 4.0f}, 1e-3f));

    const glm::vec3 off_the_edge{300.0f, 6.0f, 4.0f};
    const network_example::ProjectileLaunch unsupported =
        network_example::descent_launch(
            projectile_template, off_the_edge, glm::vec3{1.0f, 0.0f, 0.0f},
            seed, kTickSeconds, &physics);
    require(near(
        unsupported.origin + unsupported.velocity * (15.0f * kTickSeconds),
        off_the_edge,
        1e-3f));
}

// Spawned through the action-graph path, the projectile is still in the air
// the tick before it lands, and it fires on_projectile_impact on the ground at
// the target the tick its sweep crosses the surface.
void graph_spawned_descent_impacts_the_target() {
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    add_ground(&physics);
    world.set_collision_world(&physics);

    network_example::RuntimeProjectileTemplate meteor =
        descent_template(80.0f, 80.0f);
    meteor.projectile_impact_binding =
        network_example::compile_spawn_projectile_binding(
            network_example::TriggerEventType::kProjectileImpact, 9);
    network_example::RuntimeProjectileTemplate blast;
    blast.projectile_template_id = 9;
    blast.projectile_type = network_example::ProjectileType::kAreaEffect;
    blast.damage = 1;
    blast.damage_interval_ticks = 1;
    blast.lifetime_ticks = 2;
    blast.area_radius = 1.0f;
    blast.collision_mask = network_example::kCollisionMaskDamageable;
    world.set_projectile_templates({meteor, blast});

    const glm::vec3 target{5.0f, 1.0f, 5.0f};
    require(network_example::spawn_action_graph_projectile(
        world, 41, 1, 77, 5001, target, glm::vec3{0.0f, 0.0f, 1.0f}, 0,
        kTickSeconds));

    const auto find_by_template = [&world](std::uint32_t template_id) {
        auto view = world.registry()
                        .view<network_example::NetworkIdentity,
                              network_example::ProjectileState>();
        for (const entt::entity entity : view) {
            if (view.get<network_example::ProjectileState>(entity)
                    .projectile_template_id == template_id) {
                return view.get<network_example::NetworkIdentity>(entity).net_id;
            }
        }
        return network_example::NetId{0};
    };
    const network_example::NetId meteor_id = find_by_template(41);
    require(meteor_id != 0);
    const auto meteor_entity = world.find_entity(meteor_id);
    require(meteor_entity.has_value());
    require(world.registry()
                .get<network_example::Transform>(*meteor_entity)
                .position.y > 40.0f);

    std::vector<KernelEvent> events;
    std::uint32_t impact_tick = 0;
    for (std::uint32_t tick = 1; tick <= 20 && impact_tick == 0; ++tick) {
        network_example::simulate_projectiles(world, kTickSeconds, tick, &events);
        if (find_by_template(9) != 0) {
            impact_tick = tick;
        }
    }
    require(impact_tick == 15 || impact_tick == 16);
    require(!world.find_entity(meteor_id).has_value());
    const auto blast_entity = world.find_entity(find_by_template(9));
    require(blast_entity.has_value());
    const glm::vec3 blast_position =
        world.registry()
            .get<network_example::ProjectileState>(*blast_entity)
            .spawn_position;
    require(near(blast_position, target, 0.05f));
}

}  // namespace

int main() {
    descent_starts_above_and_behind_and_lands_on_time();
    elevation_is_picked_per_seed_within_the_range();
    every_seed_input_changes_the_seed();
    target_is_dropped_onto_the_ground_under_it();
    graph_spawned_descent_impacts_the_target();
    std::printf("descent_launch_test passed\n");
    return 0;
}
