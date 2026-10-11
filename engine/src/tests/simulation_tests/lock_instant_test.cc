// A lock_instant weapon damages up to max_hit_count actors locked inside its
// cone on every commit: the nearest first, any a static wall hides skipped,
// only on a side the weapon hits, where the shooter saw them when there is a
// rewound frame. A commit that locks nothing still spends its ammunition and
// leaves the hold running.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"
#include "sync/public/history_buffer.h"
#include "world/public/world.h"

namespace {

namespace ne = network_example;

constexpr std::uint8_t kLockWeapon = 0;
constexpr std::uint32_t kShotTemplate = 7;
constexpr std::uint32_t kFireAction = 1000;
constexpr std::uint32_t kReloadAction = 2000;
constexpr std::uint16_t kMagazine = 12;
// The shot template's max_hit_count, and so what every commit spends.
constexpr std::uint16_t kLockCount = 3;
constexpr std::uint16_t kDamage = 10;

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

// A shooter at the origin aiming down +x. Its launch point is 1 m up; targets
// are tested at their hitbox centre, 0.5 m up.
struct Scene {
    ne::World world;
    ne::physics::PhysicsWorld physics;
    ne::NetId shooter = 0;

    Scene() {
        world.set_collision_world(&physics);
        ne::RuntimeProjectileTemplate shot{};
        shot.projectile_template_id = kShotTemplate;
        shot.max_hit_count = kLockCount;
        world.set_projectile_templates({shot});
        // Held, a commit every 2 ticks, kLockCount rounds each.
        world.set_action_templates({
            {kFireAction,
             KernelActionTriggerMode_Hold,
             KernelActionTemplateFlag_CancelOnRelease,
             kLockCount,
             0u,
             2u,
             0u,
             2u,
             6u},
            {kReloadAction, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
        });

        shooter = world.spawn_player(1, glm::vec3{0.0f});
        const auto entity = world.find_entity(shooter);
        require(entity.has_value());
        world.registry().get<ne::Health>(*entity) = ne::Health{100, 100};
        world.registry().get<ne::Hitbox>(*entity) =
            ne::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
        ne::WeaponMechanicsDefinition lock;
        lock.id = kLockWeapon;
        lock.mode = ne::WeaponFireMode::kLockInstant;
        lock.magazine_size = kMagazine;
        lock.damage = kDamage;
        lock.collision_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
        lock.projectile_template_id = kShotTemplate;
        lock.fire_action_template_id = kFireAction;
        lock.reload_action_template_id = kReloadAction;
        lock.pellet_count = 1;
        // 20 m reach, 60 degrees wide: what the kernel resolves off the cone.
        lock.melee_range = 20.0f;
        lock.melee_fov_degrees = 60.0f;
        ne::WeaponTuning& tuning = world.registry().get<ne::WeaponTuning>(*entity);
        tuning.configured[kLockWeapon] = true;
        tuning.definitions[kLockWeapon] = lock;
        ne::WeaponState& weapon = world.registry().get<ne::WeaponState>(*entity);
        weapon.weapon_slot_count = 1;
        weapon.weapon_ids[0] = kLockWeapon;
        weapon.ammo[0] = kMagazine;
        weapon.reserve_magazines[0] = 1;
    }

    // `side` is the layer of the actor's hit collider; zero leaves it without
    // one, which any weapon may lock, as an absent side reads everywhere else.
    ne::NetId spawn_target(const glm::vec3& position, std::uint32_t side) {
        const ne::NetId target = world.spawn_enemy(position);
        const auto entity = world.find_entity(target);
        require(entity.has_value());
        world.registry().get<ne::Health>(*entity) = ne::Health{100, 100};
        world.registry().get<ne::Hitbox>(*entity) =
            ne::Hitbox{{0.0f, 0.5f, 0.0f}, {0.3f, 0.5f, 0.3f}, 0};
        if (side != 0u) {
            ne::ColliderInstance hit_collider;
            hit_collider.entity_net_id = target;
            hit_collider.layer_mask = side;
            world.collider_registry().upsert_entity_collider(
                target, 50u, hit_collider);
        }
        return target;
    }

    void add_wall(const glm::vec3& center, const glm::vec3& half_extents) {
        ne::physics::CollisionObjectDescriptor box;
        box.identity = ne::physics::CollisionObjectIdentity{
            0,
            900,
            0,
            ne::physics::CollisionObjectKind::kStaticObstacle,
            ne::physics::CollisionLayer::kStaticObstacle};
        box.shape.type = ne::physics::CollisionShapeType::kBox;
        box.shape.half_extents = half_extents;
        box.position = center;
        std::string error;
        require(physics.upsert_object(box, &error));
    }

    std::uint16_t hp(ne::NetId net_id) {
        return world.registry().get<ne::Health>(*world.find_entity(net_id)).hp;
    }

    std::uint16_t ammo() {
        return world.registry()
            .get<ne::WeaponState>(*world.find_entity(shooter))
            .ammo[0];
    }

    // One tick of a hold that started with this instance. Commits land on
    // ticks 0, 2, 4...
    std::vector<ne::ActionOutcome> hold(
        std::uint32_t tick,
        const ne::HistoryFrame* rewind_frame = nullptr) {
        KernelPlayerInput input{};
        input.input_seq = tick + 1u;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        input.selected_weapon = kLockWeapon;
        input.action_intent =
            KernelActionIntent{7001u, KernelActionBinding_PrimaryFire, 0u, 0u};
        input.action_input = KernelActionInput{7001u, 1u, 0u, 0u};
        std::vector<ne::ActionOutcome> outcomes;
        std::vector<KernelEvent> events;
        ne::simulate_weapons(
            world,
            {ne::QueuedInput{1, input}},
            ne::WeaponSimulationContext{
                nullptr,
                rewind_frame,
                nullptr,
                rewind_frame != nullptr ? rewind_frame->server_tick : tick,
                tick,
                0.0f,
                0,
                &outcomes,
                nullptr},
            &events);
        return outcomes;
    }
};

bool has_outcome(
    const std::vector<ne::ActionOutcome>& outcomes,
    ne::ActionOutcomeType type) {
    for (const ne::ActionOutcome& outcome : outcomes) {
        if (outcome.type == type) {
            return true;
        }
    }
    return false;
}

void locks_the_nearest_in_the_cone_up_to_max_hit_count() {
    Scene scene;
    const ne::NetId near = scene.spawn_target({4.0f, 0.0f, 0.0f}, 0u);
    const ne::NetId mid = scene.spawn_target({6.0f, 0.0f, 1.0f}, 0u);
    const ne::NetId far = scene.spawn_target({8.0f, 0.0f, -1.0f}, 0u);
    const ne::NetId farthest = scene.spawn_target({10.0f, 0.0f, 0.0f}, 0u);
    const ne::NetId behind = scene.spawn_target({-4.0f, 0.0f, 0.0f}, 0u);
    const ne::NetId out_of_reach = scene.spawn_target({25.0f, 0.0f, 0.0f}, 0u);

    const std::vector<ne::ActionOutcome> outcomes = scene.hold(0);

    require(has_outcome(outcomes, ne::ActionOutcomeType::Committed));
    require(scene.hp(near) == 100 - kDamage);
    require(scene.hp(mid) == 100 - kDamage);
    require(scene.hp(far) == 100 - kDamage);
    // The fourth is in the cone, but the lock is full.
    require(scene.hp(farthest) == 100);
    require(scene.hp(behind) == 100);
    require(scene.hp(out_of_reach) == 100);
    require(scene.ammo() == kMagazine - kLockCount);
}

// D5/D9 of the plan: nothing in the cone is a shot that hit nothing, not a
// refused one. A refusal would be Corrected, which the client reads as the end
// of the hold -- so the second commit, which does find a target, is the proof.
void a_commit_that_locks_nothing_still_spends_and_keeps_holding() {
    Scene scene;
    const std::vector<ne::ActionOutcome> empty = scene.hold(0);
    require(has_outcome(empty, ne::ActionOutcomeType::Committed));
    require(!has_outcome(empty, ne::ActionOutcomeType::Corrected));
    require(scene.ammo() == kMagazine - kLockCount);

    const ne::NetId arrival = scene.spawn_target({5.0f, 0.0f, 0.0f}, 0u);
    require(!has_outcome(scene.hold(1), ne::ActionOutcomeType::Corrected));
    const std::vector<ne::ActionOutcome> second = scene.hold(2);
    require(has_outcome(second, ne::ActionOutcomeType::Committed));
    require(scene.hp(arrival) == 100 - kDamage);
    require(scene.ammo() == kMagazine - 2 * kLockCount);
}

// The wall stands across the nearest target's line only; the one it hides
// gives its place to the next nearest, so two are still locked.
void a_wall_hides_a_target_and_the_next_nearest_takes_its_place() {
    Scene scene;
    scene.add_wall({5.0f, 2.0f, 0.0f}, {0.25f, 2.0f, 1.0f});
    const ne::NetId hidden = scene.spawn_target({7.0f, 0.0f, 0.0f}, 0u);
    const ne::NetId beside = scene.spawn_target({8.0f, 0.0f, 3.5f}, 0u);
    const ne::NetId further = scene.spawn_target({10.0f, 0.0f, -4.0f}, 0u);
    const ne::NetId furthest = scene.spawn_target({14.0f, 0.0f, 5.0f}, 0u);

    scene.hold(0);

    require(scene.hp(hidden) == 100);
    require(scene.hp(beside) == 100 - kDamage);
    require(scene.hp(further) == 100 - kDamage);
    require(scene.hp(furthest) == 100 - kDamage);
}

// The weapon names hostile_side. A player-side actor nearer than the enemy is
// passed over, not locked and spared at the damage stage.
void only_actors_on_a_side_the_weapon_hits_are_locked() {
    Scene scene;
    const ne::NetId ally =
        scene.spawn_target({4.0f, 0.0f, 0.0f}, KERNEL_COLLISION_LAYER_PLAYER_SIDE);
    const ne::NetId enemy =
        scene.spawn_target({6.0f, 0.0f, 0.0f}, KERNEL_COLLISION_LAYER_HOSTILE_SIDE);

    scene.hold(0);

    require(scene.hp(ally) == 100);
    require(scene.hp(enemy) == 100 - kDamage);
}

// Live, the target stands out of reach. The rewound frame -- where the shooter
// saw it -- has it in the cone, and that is what decides.
void the_rewound_frame_decides_where_targets_were() {
    Scene scene;
    const ne::NetId target = scene.spawn_target({30.0f, 0.0f, 0.0f}, 0u);
    const ne::NetId dead_then = scene.spawn_target({3.0f, 0.0f, 0.0f}, 0u);

    ne::HistoryFrame frame;
    frame.server_tick = 0;
    frame.valid = true;
    frame.volumes.push_back(ne::HitVolumeSnapshot{
        target,
        glm::vec3{5.0f, 0.5f, 0.0f},
        glm::vec3{0.3f, 0.5f, 0.3f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        ne::kHitZoneUnscaled,
        0,
        1,
        0});
    // Alive now, dead in the frame: not lockable at the moment it was seen.
    frame.volumes.push_back(ne::HitVolumeSnapshot{
        dead_then,
        glm::vec3{3.0f, 0.5f, 0.0f},
        glm::vec3{0.3f, 0.5f, 0.3f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        ne::kHitZoneUnscaled,
        0,
        0,
        0});

    scene.hold(0, &frame);

    require(scene.hp(target) == 100 - kDamage);
    require(scene.hp(dead_then) == 100);
}

}  // namespace

int main() {
    locks_the_nearest_in_the_cone_up_to_max_hit_count();
    a_commit_that_locks_nothing_still_spends_and_keeps_holding();
    a_wall_hides_a_target_and_the_next_nearest_takes_its_place();
    only_actors_on_a_side_the_weapon_hits_are_locked();
    the_rewound_frame_decides_where_targets_were();
    std::puts("lock_instant_test passed");
    return 0;
}
