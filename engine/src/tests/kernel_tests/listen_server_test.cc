#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"
#include "world/public/components.h"

namespace {

RenderEntityState find_player(const std::array<RenderEntityState, 16>& states, std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == 1) {
            return states[index];
        }
    }
    assert(false);
    return RenderEntityState{};
}

bool has_non_player_state(
    const std::array<RenderEntityState, 16>& states,
    std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type != 1) {
            return true;
        }
    }
    return false;
}

bool has_entity(
    const std::array<RenderEntityState, 16>& states,
    std::uint32_t count,
    std::uint32_t net_id) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].net_id == net_id) {
            return true;
        }
    }
    return false;
}

RenderEntityState find_entity(
    const std::array<RenderEntityState, 16>& states,
    std::uint32_t count,
    std::uint32_t net_id) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].net_id == net_id) {
            return states[index];
        }
    }
    assert(false);
    return RenderEntityState{};
}

RenderEntityState find_projectile_action(
    const std::array<RenderEntityState, 16>& states,
    std::uint32_t count,
    std::uint32_t client_action_id) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == 3 &&
            states[index].client_action_id == client_action_id) {
            return states[index];
        }
    }
    assert(false);
    return RenderEntityState{};
}

bool has_projectile_action(
    const std::array<RenderEntityState, 16>& states,
    std::uint32_t count,
    std::uint32_t client_action_id) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == 3 &&
            states[index].client_action_id == client_action_id) {
            return true;
        }
    }
    return false;
}

KernelServerEntityState find_server_entity(
    const std::array<KernelServerEntityState, 16>& states,
    std::uint32_t count,
    std::uint32_t net_id) {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].net_id == net_id) {
            return states[index];
        }
    }
    assert(false);
    return KernelServerEntityState{};
}

KernelWeaponMechanicsDefinition projectile_weapon(
    std::uint8_t weapon_id,
    float speed,
    std::uint8_t motion_model) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(weapon);
    weapon.weapon_id = weapon_id;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = 30;
    weapon.damage = 40;
    weapon.cooldown_ticks = 30;
    weapon.reload_ticks = 60;
    weapon.projectile.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    weapon.projectile.motion_model = motion_model;
    weapon.projectile.speed = speed;
    weapon.projectile.lifetime_seconds = 3.0f;
    weapon.projectile.explosion_radius = 4.0f;
    if (motion_model == KernelProjectileMotionModel_Parabolic) {
        weapon.projectile.gravity = KernelVec3{0.0f, -9.8f, 0.0f};
    }
    return weapon;
}

void configure_local_player(KernelHandle* kernel, std::uint32_t player_net_id) {
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_id = 0;
    combat.move_speed_meters_per_second = 5.0f;
    combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    combat.ammo[0] = 30;
    combat.reserve_ammo[0] = 90;
    combat.ammo[2] = 30;
    combat.reserve_ammo[2] = 90;
    combat.ammo[3] = 6;
    combat.reserve_ammo[3] = 18;
    assert(Kernel_ServerSetEntityCombatState(kernel, player_net_id, &combat));

    KernelWeaponMechanicsDefinition rifle{};
    rifle.struct_size = sizeof(rifle);
    rifle.weapon_id = 0;
    rifle.fire_mode = KernelWeaponFireMode_Hitscan;
    rifle.magazine_size = 30;
    rifle.damage = 25;
    rifle.cooldown_ticks = 3;
    rifle.reload_ticks = 30;
    rifle.max_range = 100.0f;
    rifle.pellet_count = 1;
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, player_net_id, &rifle));

    KernelWeaponMechanicsDefinition grenade =
        projectile_weapon(2, 15.0f, KernelProjectileMotionModel_Parabolic);
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, player_net_id, &grenade));
    KernelWeaponMechanicsDefinition rocket =
        projectile_weapon(3, 35.0f, KernelProjectileMotionModel_Linear);
    rocket.magazine_size = 6;
    rocket.damage = 45;
    rocket.reload_ticks = 75;
    rocket.projectile.lifetime_seconds = 2.5f;
    rocket.projectile.explosion_radius = 3.0f;
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, player_net_id, &rocket));
}

}  // namespace

int main() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;

    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartListenServer(kernel, 7777));

    std::array<KernelEvent, 16> events{};
    const std::uint32_t event_count =
        Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    bool saw_player_joined = false;
    bool saw_player_spawned = false;
    for (std::uint32_t index = 0; index < event_count; ++index) {
        saw_player_joined =
            saw_player_joined || events[index].type == KernelEventType_PlayerJoined;
        saw_player_spawned =
            saw_player_spawned || events[index].type == KernelEventType_EntitySpawned;
    }
    assert(saw_player_joined);
    assert(saw_player_spawned);

    std::array<RenderEntityState, 16> before_states{};
    std::uint32_t before_count = Kernel_GetRenderStates(
        kernel,
        before_states.data(),
        static_cast<std::uint32_t>(before_states.size()));
    assert(before_count == 1);
    assert(!has_non_player_state(before_states, before_count));
    RenderEntityState before_player = find_player(before_states, before_count);
    KernelLocalPlayerInfo local_info{};
    assert(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    configure_local_player(kernel, local_info.player_net_id);
    Kernel_Update(kernel, 0.0f);
    before_count = Kernel_GetRenderStates(
        kernel,
        before_states.data(),
        static_cast<std::uint32_t>(before_states.size()));
    before_player = find_player(before_states, before_count);
    assert(local_info.player_net_id == before_player.net_id);
    assert(before_player.hp == 100);
    assert(before_player.max_hp == 100);
    std::array<KernelServerEntityState, 16> queried_players{};
    const std::uint32_t player_query_count = Kernel_ServerQueryEntities(
        kernel,
        1,
        queried_players.data(),
        static_cast<std::uint32_t>(queried_players.size()));
    assert(player_query_count == 1);
    const KernelServerEntityState queried_player =
        find_server_entity(queried_players, player_query_count, before_player.net_id);
    assert(queried_player.valid);
    assert(queried_player.entity_type == 1);
    assert(queried_player.hp == 100);
    assert(queried_player.max_hp == 100);

    PlayerInput input{};
    input.input_seq = 1;
    input.client_action_time_us = 0;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);

    Kernel_Update(kernel, 0.0f);
    std::array<RenderEntityState, 16> predicted_states{};
    const std::uint32_t predicted_count = Kernel_GetRenderStates(
        kernel,
        predicted_states.data(),
        static_cast<std::uint32_t>(predicted_states.size()));
    assert(predicted_count == 1);
    assert(!has_non_player_state(predicted_states, predicted_count));
    const RenderEntityState predicted_player =
        find_player(predicted_states, predicted_count);
    assert(predicted_player.position.x > before_player.position.x);
    assert(predicted_player.hp == 100);
    assert(predicted_player.max_hp == 100);

    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 16> after_states{};
    const std::uint32_t after_count = Kernel_GetRenderStates(
        kernel,
        after_states.data(),
        static_cast<std::uint32_t>(after_states.size()));
    assert(after_count == 1);
    assert(!has_non_player_state(after_states, after_count));
    const RenderEntityState after_player = find_player(after_states, after_count);
    assert(after_player.position.x > before_player.position.x);
    assert(after_player.hp == 100);
    assert(after_player.max_hp == 100);

    PlayerInput fire_input{};
    fire_input.input_seq = 2;
    fire_input.client_action_time_us = 66666;
    fire_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    fire_input.buttons = InputButton_Fire;
    fire_input.selected_weapon = 0;
    Kernel_SubmitInput(kernel, 1, &fire_input);
    Kernel_Update(kernel, 1.0f / 30.0f);

    std::array<KernelEvent, 16> combat_events{};
    const std::uint32_t combat_event_count = Kernel_PollEvents(
        kernel,
        combat_events.data(),
        static_cast<std::uint32_t>(combat_events.size()));
    bool saw_fire_confirmed = false;
    for (std::uint32_t index = 0; index < combat_event_count; ++index) {
        saw_fire_confirmed =
            saw_fire_confirmed ||
            combat_events[index].type == KernelEventType_FireConfirmed;
        assert(combat_events[index].type != KernelEventType_DamageApplied);
    }
    assert(saw_fire_confirmed);

    PlayerInput projectile_input{};
    projectile_input.input_seq = 3;
    projectile_input.client_action_time_us = 100000;
    projectile_input.client_action_id = 3001;
    projectile_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    projectile_input.buttons = InputButton_Fire;
    projectile_input.selected_weapon = 2;
    Kernel_SubmitInput(kernel, 1, &projectile_input);

    std::array<RenderEntityState, 16> predicted_projectile_states{};
    const std::uint32_t predicted_projectile_count = Kernel_GetRenderStates(
        kernel,
        predicted_projectile_states.data(),
        static_cast<std::uint32_t>(predicted_projectile_states.size()));
    const RenderEntityState predicted_projectile = find_projectile_action(
        predicted_projectile_states,
        predicted_projectile_count,
        projectile_input.client_action_id);
    assert(predicted_projectile.entity_id != 0);
    assert(predicted_projectile.net_id == 0);

    Kernel_Update(kernel, 1.0f);

    std::array<KernelEvent, 16> projectile_events{};
    const std::uint32_t projectile_event_count = Kernel_PollEvents(
        kernel,
        projectile_events.data(),
        static_cast<std::uint32_t>(projectile_events.size()));
    bool saw_projectile_fire_confirmed = false;
    std::uint32_t projectile_net_id = 0;
    for (std::uint32_t index = 0; index < projectile_event_count; ++index) {
        saw_projectile_fire_confirmed =
            saw_projectile_fire_confirmed ||
            projectile_events[index].type == KernelEventType_FireConfirmed;
        if (projectile_events[index].type == KernelEventType_EntitySpawned &&
            projectile_events[index].net_id != local_info.player_net_id) {
            projectile_net_id = projectile_events[index].net_id;
            assert(projectile_events[index].code == 3);
        }
    }
    assert(saw_projectile_fire_confirmed);
    assert(projectile_net_id != 0);

    std::array<RenderEntityState, 16> projectile_states{};
    const std::uint32_t projectile_count = Kernel_GetRenderStates(
        kernel,
        projectile_states.data(),
        static_cast<std::uint32_t>(projectile_states.size()));
    const RenderEntityState projectile_state =
        find_entity(projectile_states, projectile_count, projectile_net_id);
    assert(projectile_state.entity_type == 3);
    assert(projectile_state.entity_id == predicted_projectile.entity_id);
    assert(projectile_state.client_action_id == projectile_input.client_action_id);

    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<RenderEntityState, 16> moved_projectile_states{};
    const std::uint32_t moved_projectile_count = Kernel_GetRenderStates(
        kernel,
        moved_projectile_states.data(),
        static_cast<std::uint32_t>(moved_projectile_states.size()));
    const RenderEntityState moved_projectile_state =
        find_entity(moved_projectile_states, moved_projectile_count, projectile_net_id);
    assert(moved_projectile_state.entity_type == 3);
    assert(moved_projectile_state.position.x > projectile_state.position.x);

    PlayerInput rocket_input{};
    rocket_input.input_seq = 4;
    rocket_input.client_action_time_us = 133333;
    rocket_input.client_action_id = 3002;
    rocket_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    rocket_input.buttons = InputButton_Fire;
    rocket_input.selected_weapon = network_example::kWeaponSlot3;
    Kernel_SubmitInput(kernel, 1, &rocket_input);

    std::array<RenderEntityState, 16> predicted_rocket_states{};
    const std::uint32_t predicted_rocket_count = Kernel_GetRenderStates(
        kernel,
        predicted_rocket_states.data(),
        static_cast<std::uint32_t>(predicted_rocket_states.size()));
    const RenderEntityState predicted_rocket = find_projectile_action(
        predicted_rocket_states,
        predicted_rocket_count,
        rocket_input.client_action_id);
    assert(predicted_rocket.entity_id != 0);
    assert(predicted_rocket.net_id == 0);

    Kernel_Update(kernel, 1.0f);
    std::array<KernelEvent, 16> rocket_events{};
    const std::uint32_t rocket_event_count = Kernel_PollEvents(
        kernel,
        rocket_events.data(),
        static_cast<std::uint32_t>(rocket_events.size()));
    std::uint32_t rocket_net_id = 0;
    for (std::uint32_t index = 0; index < rocket_event_count; ++index) {
        if (rocket_events[index].type == KernelEventType_EntitySpawned &&
            rocket_events[index].net_id != local_info.player_net_id) {
            rocket_net_id = rocket_events[index].net_id;
            assert(rocket_events[index].code == 3);
        }
    }
    assert(rocket_net_id != 0);

    std::array<RenderEntityState, 16> rocket_states{};
    const std::uint32_t rocket_count = Kernel_GetRenderStates(
        kernel,
        rocket_states.data(),
        static_cast<std::uint32_t>(rocket_states.size()));
    const RenderEntityState rocket_state =
        find_entity(rocket_states, rocket_count, rocket_net_id);
    assert(rocket_state.entity_type == 3);
    assert(rocket_state.entity_id == predicted_rocket.entity_id);
    assert(rocket_state.client_action_id == rocket_input.client_action_id);

    PlayerInput rejected_projectile_input{};
    rejected_projectile_input.input_seq = 5;
    rejected_projectile_input.client_action_time_us = 166666;
    rejected_projectile_input.client_action_id = 3003;
    rejected_projectile_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    rejected_projectile_input.buttons = InputButton_Fire;
    rejected_projectile_input.selected_weapon = 2;
    Kernel_SubmitInput(kernel, 1, &rejected_projectile_input);
    std::array<RenderEntityState, 16> rejected_predicted_states{};
    const std::uint32_t rejected_predicted_count = Kernel_GetRenderStates(
        kernel,
        rejected_predicted_states.data(),
        static_cast<std::uint32_t>(rejected_predicted_states.size()));
    assert(has_projectile_action(
        rejected_predicted_states,
        rejected_predicted_count,
        rejected_projectile_input.client_action_id));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<RenderEntityState, 16> rejected_after_states{};
    const std::uint32_t rejected_after_count = Kernel_GetRenderStates(
        kernel,
        rejected_after_states.data(),
        static_cast<std::uint32_t>(rejected_after_states.size()));
    assert(!has_projectile_action(
        rejected_after_states,
        rejected_after_count,
        rejected_projectile_input.client_action_id));

    KernelServerEntityCreateInfo enemy_create{};
    enemy_create.struct_size = sizeof(enemy_create);
    enemy_create.entity_type = 2;
    enemy_create.position = KernelVec3{6.0f, 0.0f, 0.0f};
    enemy_create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    enemy_create.animation_state = 4;
    enemy_create.visual_flags = 8;
    std::uint32_t enemy_net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &enemy_create, &enemy_net_id));
    assert(enemy_net_id != 0);
    std::array<KernelServerEntityState, 16> queried_enemies{};
    const std::uint32_t enemy_query_count = Kernel_ServerQueryEntities(
        kernel,
        2,
        queried_enemies.data(),
        static_cast<std::uint32_t>(queried_enemies.size()));
    assert(enemy_query_count == 1);
    const KernelServerEntityState queried_enemy =
        find_server_entity(queried_enemies, enemy_query_count, enemy_net_id);
    assert(queried_enemy.valid);
    assert(queried_enemy.position.x == enemy_create.position.x);

    Kernel_Update(kernel, 0.0f);
    std::array<RenderEntityState, 16> spawned_states{};
    const std::uint32_t spawned_count = Kernel_GetRenderStates(
        kernel,
        spawned_states.data(),
        static_cast<std::uint32_t>(spawned_states.size()));
    assert(spawned_count >= 2);
    assert(has_entity(spawned_states, spawned_count, enemy_net_id));

    KernelServerEntityCreateInfo far_enemy_create = enemy_create;
    far_enemy_create.position = KernelVec3{100.0f, 0.0f, 0.0f};
    std::uint32_t far_enemy_net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &far_enemy_create, &far_enemy_net_id));
    assert(far_enemy_net_id != 0);
    Kernel_Update(kernel, 0.0f);
    std::array<RenderEntityState, 16> filtered_states{};
    const std::uint32_t filtered_count = Kernel_GetRenderStates(
        kernel,
        filtered_states.data(),
        static_cast<std::uint32_t>(filtered_states.size()));
    assert(!has_entity(filtered_states, filtered_count, far_enemy_net_id));

    std::array<KernelEvent, 16> spawn_events{};
    const std::uint32_t spawn_event_count = Kernel_PollEvents(
        kernel,
        spawn_events.data(),
        static_cast<std::uint32_t>(spawn_events.size()));
    bool saw_enemy_spawned = false;
    for (std::uint32_t index = 0; index < spawn_event_count; ++index) {
        saw_enemy_spawned =
            saw_enemy_spawned ||
            (spawn_events[index].type == KernelEventType_EntitySpawned &&
             spawn_events[index].net_id == enemy_net_id);
    }
    assert(saw_enemy_spawned);

    assert(Kernel_ServerDestroyEntity(
        kernel,
        enemy_net_id,
        KernelDespawnReason_Destroyed));
    assert(Kernel_ServerDestroyEntity(
        kernel,
        far_enemy_net_id,
        KernelDespawnReason_Destroyed));
    Kernel_Update(kernel, 0.0f);

    std::array<RenderEntityState, 16> despawned_states{};
    const std::uint32_t despawned_count = Kernel_GetRenderStates(
        kernel,
        despawned_states.data(),
        static_cast<std::uint32_t>(despawned_states.size()));
    assert(!has_entity(despawned_states, despawned_count, enemy_net_id));
    assert(!has_entity(despawned_states, despawned_count, far_enemy_net_id));

    Kernel_Destroy(kernel);
    return 0;
}
