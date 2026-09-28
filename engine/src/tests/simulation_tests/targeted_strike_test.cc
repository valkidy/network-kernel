// A targeted strike resolves its landing point on the server from the aim:
// the first actor, terrain or static obstacle within max_range, then the
// ground under it. Aiming at nothing, or at something with no ground under
// it, refuses the shot without spending ammunition or cooldown.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"
#include "sync/public/history_buffer.h"
#include "world/public/world.h"

namespace {

constexpr std::uint8_t kStrikeWeapon = 0;
constexpr std::uint32_t kMarkerTemplate = 40;
constexpr float kMaxRange = 30.0f;
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

void add_box(
    network_example::physics::PhysicsWorld* physics,
    std::uint32_t collider_id,
    network_example::physics::CollisionObjectKind kind,
    network_example::physics::CollisionLayer layer,
    const glm::vec3& center,
    const glm::vec3& half_extents) {
    network_example::physics::CollisionObjectDescriptor box;
    box.identity = network_example::physics::CollisionObjectIdentity{
        0, collider_id, 0, kind, layer};
    box.shape.type = network_example::physics::CollisionShapeType::kBox;
    box.shape.half_extents = half_extents;
    box.position = center;
    std::string error;
    require(physics->upsert_object(box, &error));
}

// Ground with its top at y = 0 from x = -20 to x = 12. A wall stands on it at
// x = 10 (faces at 9.5 and 10.5) for |z| <= 5. Past the ground's edge, off to
// one side where a ray from the player clears the wall, a block floats at
// x = 18 with nothing under it.
struct Scene {
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    network_example::NetId player = 0;

    Scene() {
        add_box(
            &physics, 300,
            network_example::physics::CollisionObjectKind::kTerrain,
            network_example::physics::CollisionLayer::kTerrain,
            glm::vec3{-4.0f, -0.5f, 0.0f}, glm::vec3{16.0f, 0.5f, 50.0f});
        add_box(
            &physics, 301,
            network_example::physics::CollisionObjectKind::kStaticObstacle,
            network_example::physics::CollisionLayer::kStaticObstacle,
            glm::vec3{10.0f, 2.0f, 0.0f}, glm::vec3{0.5f, 2.0f, 5.0f});
        add_box(
            &physics, 302,
            network_example::physics::CollisionObjectKind::kStaticObstacle,
            network_example::physics::CollisionLayer::kStaticObstacle,
            glm::vec3{18.0f, 1.0f, 12.0f}, glm::vec3{0.5f, 0.5f, 2.0f});
        world.set_collision_world(&physics);

        network_example::RuntimeProjectileTemplate marker;
        marker.projectile_template_id = kMarkerTemplate;
        marker.weapon_id = kStrikeWeapon;
        marker.projectile_type = network_example::ProjectileType::kStandard;
        marker.sync_mode =
            network_example::ProjectileSyncMode::kServerSnapshotOnly;
        marker.damage_shape = network_example::ProjectileDamageShape::kNone;
        marker.speed = 0.0f;
        marker.lifetime_ticks = 20;
        marker.collision_mask = KERNEL_COLLISION_MASK_NONE;
        world.set_projectile_templates({marker});
        world.set_action_templates({
            {1000u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 30u, 1u, 30u, 0u},
            {2000u, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
        });

        player = world.spawn_player(1, glm::vec3{0.0f});
        const auto entity = world.find_entity(player);
        require(entity.has_value());
        // spawn_player leaves health at zero, and the dead do not fire.
        world.registry().get<network_example::Health>(*entity) =
            network_example::Health{100, 100};
        world.registry().get<network_example::Hitbox>(*entity) =
            network_example::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
        network_example::WeaponTuning& tuning =
            world.registry().get_or_emplace<network_example::WeaponTuning>(*entity);
        network_example::WeaponMechanicsDefinition strike;
        strike.id = kStrikeWeapon;
        strike.mode = network_example::WeaponFireMode::kTargetedStrike;
        strike.magazine_size = 3;
        strike.max_range = kMaxRange;
        strike.pellet_count = 1;
        strike.projectile_template_id = kMarkerTemplate;
        strike.fire_action_template_id = 1000u;
        strike.reload_action_template_id = 2000u;
        tuning.configured[kStrikeWeapon] = true;
        tuning.definitions[kStrikeWeapon] = strike;
        network_example::WeaponState& weapon =
            world.registry().get_or_emplace<network_example::WeaponState>(*entity);
        weapon.weapon_slot_count = 1;
        weapon.weapon_ids[0] = kStrikeWeapon;
        weapon.ammo[0] = strike.magazine_size;
        weapon.reserve_magazines[0] = 1;
    }

    network_example::WeaponState& weapon() {
        return world.registry().get<network_example::WeaponState>(
            *world.find_entity(player));
    }
};

struct FireResult {
    std::optional<glm::vec3> landed;
    std::vector<network_example::ActionOutcome> outcomes;
};

// Fires once, aiming from the player's launch point (1 m up) towards `aim_at`.
FireResult fire_at(
    Scene& scene,
    const glm::vec3& aim_at,
    const network_example::HistoryFrame* rewind_frame = nullptr) {
    const glm::vec3 direction =
        glm::normalize(aim_at - glm::vec3{0.0f, 1.0f, 0.0f});
    KernelPlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{direction.x, direction.y, direction.z};
    input.selected_weapon = kStrikeWeapon;
    input.action_intent =
        KernelActionIntent{9001u, KernelActionBinding_PrimaryFire, 0u, 0u};
    input.action_input = KernelActionInput{9001u, 1u, 0u, 0u};

    FireResult result;
    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        scene.world,
        {network_example::QueuedInput{1, input}},
        network_example::WeaponSimulationContext{
            nullptr,
            rewind_frame,
            nullptr,
            0,
            0,
            kTickSeconds,
            0,
            &result.outcomes,
            nullptr},
        &events);

    auto view = scene.world.registry()
                    .view<network_example::ProjectileState,
                          network_example::Transform>();
    for (const entt::entity entity : view) {
        if (view.get<network_example::ProjectileState>(entity)
                .projectile_template_id == kMarkerTemplate) {
            result.landed =
                view.get<network_example::Transform>(entity).position;
        }
    }
    return result;
}

bool refused(const FireResult& result) {
    for (const network_example::ActionOutcome& outcome : result.outcomes) {
        if (outcome.type == network_example::ActionOutcomeType::Corrected &&
            outcome.reason == KernelLocalActionResultReason_EffectFailed) {
            return true;
        }
    }
    return false;
}

bool near(const glm::vec3& lhs, const glm::vec3& rhs, float tolerance) {
    return glm::length(lhs - rhs) <= tolerance;
}

// Aimed at open ground, it lands where the aim meets the ground and costs one
// round.
void aim_at_ground_lands_there() {
    Scene scene;
    const FireResult result = fire_at(scene, glm::vec3{6.0f, 0.0f, 2.0f});
    require(result.landed.has_value());
    require(near(*result.landed, glm::vec3{6.0f, 0.0f, 2.0f}, 0.05f));
    require(scene.weapon().ammo[0] == 2u);
    require(!refused(result));
}

// Aimed at a wall, it backs off the face and drops to the foot of the wall on
// the shooter's side, not into or behind it.
void aim_at_wall_lands_at_its_foot() {
    Scene scene;
    const FireResult result = fire_at(scene, glm::vec3{20.0f, 1.0f, 0.0f});
    require(result.landed.has_value());
    require(std::fabs(result.landed->y) < 0.01f);
    require(result.landed->x > 9.3f && result.landed->x < 9.5f);
}

// Aimed at an actor, it lands at the actor's feet where the shooter saw it --
// the rewound frame -- and a wall in between still wins.
void aim_at_actor_lands_under_it() {
    network_example::HistoryFrame frame;
    frame.valid = true;
    network_example::HitVolumeSnapshot enemy;
    enemy.net_id = 777;
    enemy.center = glm::vec3{5.0f, 1.0f, 4.0f};
    enemy.half_extents = glm::vec3{0.4f, 0.8f, 0.4f};
    enemy.alive = 1;
    frame.volumes.push_back(enemy);

    Scene scene;
    const FireResult result =
        fire_at(scene, glm::vec3{5.0f, 1.0f, 4.0f}, &frame);
    require(result.landed.has_value());
    require(std::fabs(result.landed->y) < 0.01f);
    // The near face of the hitbox, straight under it.
    require(glm::length(glm::vec2{result.landed->x - 5.0f,
                                  result.landed->z - 4.0f}) < 0.6f);

    frame.volumes[0].center = glm::vec3{15.0f, 1.0f, 0.0f};
    Scene walled;
    const FireResult blocked =
        fire_at(walled, glm::vec3{15.0f, 1.0f, 0.0f}, &frame);
    require(blocked.landed.has_value());
    require(blocked.landed->x < 9.5f);
}

// Aimed at the sky, or past max_range, nothing is hit: refused, no round
// spent, no cooldown started. The ground shot above is the control.
void aim_at_nothing_is_refused() {
    Scene scene;
    const std::uint32_t cooldown_before =
        scene.weapon().next_primary_commit_tick[0];
    const FireResult sky = fire_at(scene, glm::vec3{3.0f, 20.0f, 0.0f});
    require(!sky.landed.has_value());
    require(refused(sky));
    require(scene.weapon().ammo[0] == 3u);
    require(scene.weapon().next_primary_commit_tick[0] == cooldown_before);

    // Ground 40 m out along a shallow aim is past the 30 m range.
    Scene far;
    const FireResult beyond = fire_at(far, glm::vec3{-40.0f, 0.0f, 0.0f});
    require(!beyond.landed.has_value());
    require(refused(beyond));
    require(far.weapon().ammo[0] == 3u);
}

// Aimed at something with no ground under it, refused the same way. The wall
// shot above, which hits a block that does stand on ground, is the control.
void aim_at_unsupported_block_is_refused() {
    Scene scene;
    const FireResult result = fire_at(scene, glm::vec3{18.0f, 1.0f, 12.0f});
    require(!result.landed.has_value());
    require(refused(result));
    require(scene.weapon().ammo[0] == 3u);
}

}  // namespace

int main() {
    aim_at_ground_lands_there();
    aim_at_wall_lands_at_its_foot();
    aim_at_actor_lands_under_it();
    aim_at_nothing_is_refused();
    aim_at_unsupported_block_is_refused();
    std::printf("targeted_strike_test passed\n");
    return 0;
}
