#include <cassert>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

namespace {

// require, not assert: the suite is run under -c opt, which defines NDEBUG and
// compiles every assert away. An assert here would pass without looking.
void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

network_example::Health& health(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Health>(*entity);
}

network_example::NetId spawn_enemy(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId enemy = world.spawn_enemy(position);
    health(world, enemy) = network_example::Health{100, 100};
    const auto entity = world.find_entity(enemy);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    return enemy;
}

// A swinger carries the cone already resolved into numbers. That is the shape
// the simulation sees: the kernel reads range and fov off the collider template
// once, when the weapon is configured, because nothing down here can look a
// collider template up.
network_example::NetId spawn_swinger(
    network_example::World& world,
    float range_meters,
    float fov_degrees,
    std::uint32_t projectile_template_id) {
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto entity = world.find_entity(player);
    assert(entity.has_value());
    network_example::WeaponState& weapon =
        world.registry().get<network_example::WeaponState>(*entity);
    weapon.weapon_slot_count = 1;
    weapon.weapon_ids[0] = 0;
    weapon.ammo[0] = 10;
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*entity);
    tuning.configured[0] = true;
    network_example::WeaponMechanicsDefinition& claw = tuning.definitions[0];
    claw.id = 0;
    claw.mode = network_example::WeaponFireMode::kMelee;
    claw.magazine_size = 10;
    claw.damage = 35;
    claw.collision_mask = network_example::kCollisionMaskDamageable;
    claw.projectile_template_id = projectile_template_id;
    claw.melee_range = range_meters;
    claw.melee_fov_degrees = fov_degrees;
    return player;
}

void swing(network_example::World& world, const glm::vec3& aim) {
    KernelPlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{aim.x, aim.y, aim.z};
    input.buttons = InputButton_Fire;
    input.selected_weapon = 0;
    const std::vector<network_example::QueuedInput> inputs = {
        network_example::QueuedInput{1, input},
    };
    std::vector<KernelEvent> events;
    network_example::DamagePipeline pipeline;
    network_example::simulate_weapons(
        world,
        inputs,
        network_example::WeaponSimulationContext{
            nullptr, nullptr, &pipeline, 0, 0, 0.0f, 0, nullptr},
        &events);
    pipeline.confirm_ready(world, 0, 0, &events);
}

// The whole point of the cone. Both targets are the same distance away, so
// nothing but the angle can separate them -- a swing resolved as a plain sphere
// of the same reach would hit both, and this is what says it does not.
//
// The shot template is what makes that true. Left at the default of one target
// the swing would damage the front one anyway, because equal distances are
// broken by net id and the front enemy is spawned first -- so the test would
// pass with the angle test deleted, which is exactly what it did before this
// template was added.
void a_swing_reaches_what_is_in_front_and_not_what_is_behind() {
    network_example::World world;
    const network_example::NetId front =
        spawn_enemy(world, glm::vec3{1.5f, 0.0f, 0.0f});
    const network_example::NetId behind =
        spawn_enemy(world, glm::vec3{-1.5f, 0.0f, 0.0f});
    network_example::RuntimeProjectileTemplate shot{};
    shot.projectile_template_id = 7;
    shot.max_hit_count = 4;
    world.set_projectile_templates({shot});
    spawn_swinger(world, 2.2f, 100.0f, 7);

    swing(world, glm::vec3{1.0f, 0.0f, 0.0f});

    require(health(world, front).hp == 65);
    require(health(world, behind).hp == 100);
}

void a_swing_does_not_reach_past_its_range() {
    network_example::World world;
    const network_example::NetId out_of_reach =
        spawn_enemy(world, glm::vec3{4.0f, 0.0f, 0.0f});
    spawn_swinger(world, 2.2f, 100.0f, 0);

    swing(world, glm::vec3{1.0f, 0.0f, 0.0f});

    require(health(world, out_of_reach).hp == 100);
}

// max_hit_count is authored on the shot template, and the targets that survive
// it are the nearest ones -- not whichever the broad phase happened to produce
// first, which is why the hits are sorted by distance before the cap applies.
void a_swing_cleaves_the_nearest_targets_up_to_max_hit_count() {
    network_example::World world;
    const network_example::NetId nearest =
        spawn_enemy(world, glm::vec3{0.8f, 0.0f, 0.0f});
    const network_example::NetId middle =
        spawn_enemy(world, glm::vec3{1.4f, 0.0f, 0.6f});
    const network_example::NetId furthest =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, -0.6f});
    network_example::RuntimeProjectileTemplate shot{};
    shot.projectile_template_id = 7;
    shot.max_hit_count = 2;
    world.set_projectile_templates({shot});
    spawn_swinger(world, 2.4f, 100.0f, 7);

    swing(world, glm::vec3{1.0f, 0.0f, 0.0f});

    require(health(world, nearest).hp == 65);
    require(health(world, middle).hp == 65);
    require(health(world, furthest).hp == 100);
}

}  // namespace

int main() {
    a_swing_reaches_what_is_in_front_and_not_what_is_behind();
    a_swing_does_not_reach_past_its_range();
    a_swing_cleaves_the_nearest_targets_up_to_max_hit_count();
    return 0;
}
