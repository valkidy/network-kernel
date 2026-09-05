// Where a client draws a projectile through its own impact.
//
// A predicted projectile's flight is a closed-form curve, so the client can put
// it anywhere along that curve without asking. What stops it is a different
// question, and until the collision block admitted hybrid it was only asked for
// local-predicted projectiles: a hybrid one ran the curve straight through the
// wall and stayed on screen until the authoritative despawn removed it.
//
// This measures the visible half of that -- how far past the geometry the
// client keeps drawing the projectile -- for the rocket's own numbers: linear,
// no gravity, 35 m/s, against a wall 20 m out. At 30 Hz that is 1.17 m of
// travel per tick, so a handful of ticks of overshoot is metres of it.
//
// The server_snapshot_only row stands in for the old hybrid behaviour rather
// than describing a real one: the engine never builds a PredictedProjectile for
// that mode at all (handle_client_projectile_spawn_batch returns early), so
// what it reproduces here is the thing hybrid used to do -- run the curve with
// no collision test. Read it as "before", not as a mode anyone ships.
//
// The overshoot on that row is the unbounded figure: nothing here removes the
// projectile except its own 75-tick lifetime. In a running session the
// authoritative despawn removes it sooner, so what a player actually saw was
// bounded by how long that despawn took to arrive -- one tick of it is the
// per-tick distance below. Over a loopback that is a metre or two; over a real
// link it is the same arithmetic against the latency. The mechanism is what
// this isolates, not the magnitude on any particular connection.
//
// Reported as distances, deliberately not asserted: client_mode_test already
// pins the behaviour, and this is here to say how much of it a player sees.
// Run it with
//   bazel run -c opt //engine/src/tests/kernel_tests:impact_prediction_bench

// Everything from the standard library comes in before the `private public`
// below: kernel.h pulls libc++ headers of its own, and re-parsing one of those
// with `private` redefined is a hard error rather than a warning.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/collision_types.h"
#include "physics/public/physics_world.h"
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
// rocket_projectile's own mechanics.
constexpr float kSpeedMetersPerSecond = 35.0f;
constexpr std::uint32_t kLifetimeTicks = 75;
constexpr float kWallDistanceMeters = 20.0f;
constexpr float kWallHalfExtentMeters = 0.1f;
// What one tick of flight covers, which is the unit the overshoot is really in.
constexpr float kMetersPerTick = kSpeedMetersPerSecond * kTickSeconds;

KernelColliderTemplateDefinition rocket_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 10;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.shape_params = KernelVec4{0.3f, 0.3f, 0.3f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    return collider;
}

void load_rocket_catalog(
    network_example::KernelEngine* client,
    std::uint8_t sync_mode) {
    KernelProjectileTemplateDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.projectile_template_id = 3;
    definition.weapon_id = 3;
    definition.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    definition.mechanics.projectile_type = KernelProjectileType_Standard;
    definition.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    definition.mechanics.sync_mode = sync_mode;
    definition.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    definition.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    definition.mechanics.damage = 45;
    definition.mechanics.speed = kSpeedMetersPerSecond;
    definition.mechanics.lifetime_ticks = kLifetimeTicks;
    definition.mechanics.collider_template_id = 10;
    definition.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    definition.mechanics.max_hit_count = 1;

    KernelColliderTemplateDefinition collider = rocket_collider_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 0xc0ffeeull;
    catalog.projectile_templates = &definition;
    catalog.projectile_template_count = 1;
    catalog.collider_templates = &collider;
    catalog.collider_template_count = 1;
    require(client->load_gameplay_catalog(catalog));
}

void install_wall(network_example::KernelEngine* client) {
    client->prediction_physics_world_ =
        std::make_unique<network_example::physics::PhysicsWorld>(
            network_example::physics::PhysicsWorldConfig{});
    require(client->prediction_physics_world_->valid());
    network_example::physics::CollisionObjectDescriptor object{};
    object.identity.collider_id = 900;
    object.identity.kind = network_example::physics::CollisionObjectKind::kTerrain;
    object.identity.layer = network_example::physics::CollisionLayer::kTerrain;
    object.shape.type = network_example::physics::CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3{
        kWallHalfExtentMeters, 5.0f, 5.0f};
    object.position = glm::vec3{kWallDistanceMeters, 0.0f, 0.0f};
    std::string error;
    require(client->prediction_physics_world_->upsert_object(object, &error));
}

struct Row {
    const char* mode = "";
    bool stopped = false;
    float stop_x = 0.0f;
    std::uint32_t stop_tick = 0;
    float overshoot_m = 0.0f;
    std::uint32_t drawn_past_wall_ticks = 0;
};

Row measure(std::uint8_t sync_mode, const char* name) {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    load_rocket_catalog(&client, sync_mode);
    install_wall(&client);

    network_example::KernelEngine::PredictedProjectile projectile;
    projectile.entity_id = 9000;
    projectile.net_id = 101;
    projectile.owner_peer = 7;
    projectile.action_instance_id = 1234;
    projectile.position = glm::vec3{0.0f, 0.0f, 0.0f};
    projectile.velocity = glm::vec3{kSpeedMetersPerSecond, 0.0f, 0.0f};
    projectile.spawn_position = projectile.position;
    projectile.initial_velocity = projectile.velocity;
    projectile.motion_model = network_example::ProjectileMotionModel::kLinear;
    projectile.max_lifetime_ticks = kLifetimeTicks;
    projectile.projectile_template_id = 3;
    projectile.collider_template_id = 10;
    projectile.weapon_id = 3;
    projectile.sync_mode = sync_mode;
    projectile.bound = true;
    client.predicted_projectiles_.push_back(projectile);

    Row row;
    row.mode = name;
    const float wall_near_face = kWallDistanceMeters - kWallHalfExtentMeters;
    for (std::uint32_t tick = 1; tick <= kLifetimeTicks; ++tick) {
        client.advance_predicted_projectiles(kTickSeconds);
        if (client.predicted_projectiles_.empty()) {
            break;
        }
        const network_example::KernelEngine::PredictedProjectile& state =
            client.predicted_projectiles_.front();
        if (state.locally_terminated) {
            row.stopped = true;
            row.stop_x = state.position.x;
            row.stop_tick = tick;
            break;
        }
        if (state.position.x > wall_near_face) {
            ++row.drawn_past_wall_ticks;
            row.stop_x = state.position.x;
            row.stop_tick = tick;
        }
    }
    row.overshoot_m = std::max(0.0f, row.stop_x - wall_near_face);
    return row;
}

}  // namespace

int main() {
    std::printf(
        "CLIENT-DRAWN IMPACT (rocket: linear, %.0f m/s, wall at %.1f m, "
        "%.2f m per tick)\n",
        kSpeedMetersPerSecond,
        kWallDistanceMeters,
        kMetersPerTick);
    std::printf(
        "%34s %9s %10s %12s %14s\n",
        "sync_mode",
        "stopped",
        "stop x m",
        "overshoot m",
        "ticks past");
    const struct {
        std::uint8_t mode;
        const char* name;
    } cases[] = {
        {KernelProjectileSyncMode_LocalPredictedDeterministic,
         "local_predicted_deterministic"},
        {KernelProjectileSyncMode_HybridDeterministicThenSnapshot,
         "hybrid (after this fix)"},
        {KernelProjectileSyncMode_ServerSnapshotOnly,
         "no collision test (was hybrid)"},
    };
    for (const auto& entry : cases) {
        const Row row = measure(entry.mode, entry.name);
        std::printf(
            "%34s %9s %10.2f %12.2f %14u\n",
            row.mode,
            row.stopped ? "yes" : "no",
            row.stop_x,
            row.overshoot_m,
            row.drawn_past_wall_ticks);
    }
    std::printf("\n");
    return 0;
}
