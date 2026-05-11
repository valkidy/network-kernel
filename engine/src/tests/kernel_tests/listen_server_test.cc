#include <array>
#include <cassert>

#include "kernel/public/kernel_api.h"

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
    const std::uint32_t before_count = Kernel_GetRenderStates(
        kernel,
        before_states.data(),
        static_cast<std::uint32_t>(before_states.size()));
    assert(before_count == 1);
    assert(!has_non_player_state(before_states, before_count));
    const RenderEntityState before_player = find_player(before_states, before_count);
    KernelLocalPlayerInfo local_info{};
    assert(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    assert(local_info.player_net_id == before_player.net_id);
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

    PlayerInput input{};
    input.input_seq = 1;
    input.client_tick = 0;
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

    PlayerInput fire_input{};
    fire_input.input_seq = 2;
    fire_input.client_tick = 2;
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
    projectile_input.client_tick = 3;
    projectile_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    projectile_input.buttons = InputButton_Fire;
    projectile_input.selected_weapon = 2;
    Kernel_SubmitInput(kernel, 1, &projectile_input);
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
