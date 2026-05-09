#include <cassert>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

namespace {

network_example::Health& health(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Health>(*entity);
}

network_example::WeaponState& weapon_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::WeaponState>(*entity);
}

PlayerInput fire_input(std::uint8_t weapon_id) {
    PlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;
    input.selected_weapon = weapon_id;
    return input;
}

std::vector<network_example::QueuedInput> queue(PlayerInput input) {
    return {network_example::QueuedInput{1, input}};
}

std::uint32_t count_events(
    const std::vector<KernelEvent>& events,
    KernelEventType type) {
    std::uint32_t count = 0;
    for (const KernelEvent& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

void rejects_fire_during_cooldown_and_reload() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponRifle)),
        0,
        &events);
    assert(health(world, enemy).hp == 25);
    assert(weapon_state(world, player).ammo[network_example::kWeaponRifle] == 29);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponRifle)),
        1,
        &events);
    assert(events.empty());
    assert(health(world, enemy).hp == 25);

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponRifle)),
        3,
        &events);
    assert(health(world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_DamageApplied) == 1);

    network_example::World reload_world;
    const network_example::NetId reload_player =
        reload_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId reload_enemy =
        reload_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::WeaponState& reload_weapon =
        weapon_state(reload_world, reload_player);
    reload_weapon.ammo[network_example::kWeaponRifle] = 0;
    reload_weapon.reserve_ammo[network_example::kWeaponRifle] = 30;

    PlayerInput reload_input{};
    reload_input.buttons = InputButton_Reload;
    reload_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    events.clear();
    network_example::simulate_weapons(reload_world, queue(reload_input), 5, &events);
    assert(events.empty());
    assert(reload_weapon.is_reloading);

    events.clear();
    network_example::simulate_weapons(
        reload_world,
        queue(fire_input(network_example::kWeaponRifle)),
        6,
        &events);
    assert(events.empty());
    assert(health(reload_world, reload_enemy).hp == 50);

    network_example::simulate_weapons(reload_world, {}, 35, &events);
    assert(!reload_weapon.is_reloading);
    assert(reload_weapon.ammo[network_example::kWeaponRifle] == 30);

    events.clear();
    network_example::simulate_weapons(
        reload_world,
        queue(fire_input(network_example::kWeaponRifle)),
        36,
        &events);
    assert(health(reload_world, reload_enemy).hp == 25);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
}

void shotgun_applies_multiple_pellets() {
    network_example::World world;
    world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponShotgun)),
        0,
        &events);

    assert(health(world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
    assert(count_events(events, KernelEventType_DamageApplied) == 5);
}

void grenade_sweeps_and_explodes_with_falloff() {
    network_example::World world;
    world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId near_enemy =
        world.spawn_enemy(glm::vec3{3.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        world.spawn_enemy(glm::vec3{5.5f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponGrenade)),
        0,
        &events);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);

    network_example::NetId projectile = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned) {
            projectile = event.net_id;
        }
    }
    assert(projectile != 0);

    events.clear();
    network_example::simulate_projectiles(world, 0.2f, 1, &events);

    assert(!world.find_entity(projectile).has_value());
    assert(health(world, near_enemy).hp == 0);
    assert(health(world, far_enemy).hp > 0);
    assert(health(world, far_enemy).hp < 50);
    assert(count_events(events, KernelEventType_Explosion) == 1);
    assert(count_events(events, KernelEventType_DamageApplied) >= 2);
}

void rewind_hitscan_uses_historical_hit_volumes() {
    network_example::World current_world;
    current_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId current_enemy =
        current_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const auto current_enemy_entity = current_world.find_entity(current_enemy);
    assert(current_enemy_entity.has_value());
    current_world.registry().get<network_example::Transform>(*current_enemy_entity).position =
        glm::vec3{120.0f, 0.0f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        current_world,
        queue(fire_input(network_example::kWeaponRifle)),
        0,
        &events);
    assert(health(current_world, current_enemy).hp == 50);
    assert(count_events(events, KernelEventType_DamageApplied) == 0);

    network_example::World rewound_world;
    rewound_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId rewound_enemy =
        rewound_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(4);
    history.write_frame(rewound_world, 4);
    const auto rewound_enemy_entity = rewound_world.find_entity(rewound_enemy);
    assert(rewound_enemy_entity.has_value());
    rewound_world.registry().get<network_example::Transform>(*rewound_enemy_entity).position =
        glm::vec3{20.0f, 0.0f, 0.0f};

    events.clear();
    network_example::simulate_weapons(
        rewound_world,
        queue(fire_input(network_example::kWeaponRifle)),
        5,
        &events,
        history.find_frame(4));
    assert(health(rewound_world, rewound_enemy).hp == 25);
    assert(count_events(events, KernelEventType_DamageApplied) == 1);
}

void rewind_shotgun_respects_range() {
    network_example::World shotgun_world;
    shotgun_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        shotgun_world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(2);
    history.write_frame(shotgun_world, 2);
    shotgun_world.registry().get<network_example::Transform>(
        *shotgun_world.find_entity(enemy)).position = glm::vec3{50.0f, 0.0f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        shotgun_world,
        queue(fire_input(network_example::kWeaponShotgun)),
        3,
        &events,
        history.find_frame(2));
    assert(health(shotgun_world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_DamageApplied) == 5);

    network_example::World range_world;
    range_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        range_world.spawn_enemy(glm::vec3{120.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer range_history(1);
    range_history.write_frame(range_world, 1);
    events.clear();
    network_example::simulate_weapons(
        range_world,
        queue(fire_input(network_example::kWeaponRifle)),
        2,
        &events,
        range_history.find_frame(1));
    assert(health(range_world, far_enemy).hp == 50);
    assert(count_events(events, KernelEventType_DamageApplied) == 0);
}

}  // namespace

int main() {
    rejects_fire_during_cooldown_and_reload();
    shotgun_applies_multiple_pellets();
    grenade_sweeps_and_explodes_with_falloff();
    rewind_hitscan_uses_historical_hit_volumes();
    rewind_shotgun_respects_range();
    return 0;
}
