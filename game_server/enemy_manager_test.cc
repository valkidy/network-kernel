#include "game_server/game_server.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#include "kernel/public/kernel_api.h"

namespace {

KernelConfig listen_server_config() {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelConfig dedicated_server_config() {
    KernelConfig config = listen_server_config();
    config.mode = KernelMode_DedicatedServer;
    config.tick.snapshot_rate = 15;
    return config;
}

network_example::game_server::GameServerGameplayConfig single_spawn_gameplay_config() {
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    config.enemy.spawn_count = 1;
    config.enemy.spawn_radius = 0.0f;
    config.enemy.spawn_position = KernelVec3{6.0f, 0.0f, 0.0f};
    return config;
}

void handle_pending_events(
    KernelHandle* kernel,
    network_example::game_server::GameServer* game_server) {
    std::array<KernelEvent, 32> events{};
    const std::uint32_t count = Kernel_PollEvents(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        game_server->handle_event(events[index]);
    }
}

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

std::uint32_t query_entities(
    KernelHandle* kernel,
    std::uint16_t entity_type,
    std::array<KernelServerEntityState, 8>* states) {
    for (KernelServerEntityState& state : *states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    return Kernel_ServerQueryEntities(
        kernel,
        entity_type,
        states->data(),
        static_cast<std::uint32_t>(states->size()));
}

std::uint32_t query_enemies(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_entities(
        kernel,
        network_example::game_server::kEntityTypeEnemy,
        states);
}

std::uint32_t query_players(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_entities(
        kernel,
        network_example::game_server::kEntityTypePlayer,
        states);
}

std::uint32_t query_projectiles(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_entities(kernel, 3, states);
}

bool render_states_include_type(KernelHandle* kernel, std::uint16_t entity_type) {
    std::array<RenderEntityState, 16> states{};
    const std::uint32_t count = Kernel_GetRenderStates(
        kernel,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == entity_type) {
            return true;
        }
    }
    return false;
}

void run_server_frame(
    KernelHandle* kernel,
    network_example::game_server::GameServer* game_server) {
    game_server->tick(1.0f / 30.0f);
    Kernel_Update(kernel, 1.0f / 30.0f);
}

void run_server_frames(
    KernelHandle* kernel,
    network_example::game_server::GameServer* game_server,
    int count) {
    for (int index = 0; index < count; ++index) {
        run_server_frame(kernel, game_server);
    }
}

void submit_player_fire(KernelHandle* kernel,
                        std::uint32_t input_seq,
                        std::uint8_t weapon_id) {
    PlayerInput input{};
    input.input_seq = input_seq;
    input.buttons = InputButton_Fire;
    input.selected_weapon = weapon_id;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitInput(kernel, 1, &input);
}

PlayerInput stationary_input(std::uint32_t input_seq) {
    PlayerInput input{};
    input.input_seq = input_seq;
    input.client_action_time_us =
        static_cast<std::uint64_t>(input_seq) * 33333u;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    return input;
}

}  // namespace

int main() {
    KernelConfig unstarted_config = listen_server_config();
    KernelHandle* unstarted_kernel = Kernel_Create(&unstarted_config);
    assert(unstarted_kernel != nullptr);
    network_example::game_server::GameServer unstarted_game_server(
        unstarted_kernel,
        single_spawn_gameplay_config());
    unstarted_game_server.tick(1.0f / 30.0f);
    assert(unstarted_game_server.enemy_manager().enemy_count() == 0);
    Kernel_Destroy(unstarted_kernel);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartListenServer(kernel, 7777));

    network_example::game_server::GameServer game_server(
        kernel,
        single_spawn_gameplay_config());
    handle_pending_events(kernel, &game_server);
    game_server.tick(1.0f / 30.0f);
    require(game_server.enemy_manager().enemy_count() == 1);

    std::array<KernelServerEntityState, 8> enemy_states{};
    std::uint32_t enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    const std::uint32_t enemy_net_id = enemy_states[0].net_id;
    require(game_server.enemy_manager().enemies()[0].ammo == 119);
    require(game_server.enemy_manager().enemies()[0].reserve_ammo == 240);
    KernelWeaponMechanicsDefinition enemy_weapon{};
    enemy_weapon.struct_size = sizeof(enemy_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        enemy_net_id,
        network_example::game_server::kWeaponGrenade,
        &enemy_weapon));
    require(enemy_weapon.weapon_id == network_example::game_server::kWeaponGrenade);
    require(enemy_weapon.damage == 1);
    require(enemy_weapon.cooldown_ticks == 1);
    require(enemy_weapon.magazine_size == 120);
    KernelWeaponMechanicsDefinition unavailable_enemy_weapon{};
    unavailable_enemy_weapon.struct_size = sizeof(unavailable_enemy_weapon);
    require(!Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        enemy_net_id,
        network_example::game_server::kWeaponRifle,
        &unavailable_enemy_weapon));
    require(enemy_states[0].position.x == 6.0f);
    require(enemy_states[0].animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(enemy_states[0].velocity.x == 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    std::array<KernelServerEntityState, 8> player_states{};
    std::uint32_t player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 1000);

    Kernel_Update(kernel, 1.0f / 30.0f);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 1000);
    require(player_states[0].max_hp == 1000);
    const std::uint32_t player_net_id = player_states[0].net_id;
    KernelWeaponMechanicsDefinition player_rifle{};
    player_rifle.struct_size = sizeof(player_rifle);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        player_net_id,
        network_example::game_server::kWeaponRifle,
        &player_rifle));
    require(player_rifle.weapon_id == network_example::game_server::kWeaponRifle);
    KernelWeaponMechanicsDefinition player_shotgun{};
    player_shotgun.struct_size = sizeof(player_shotgun);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        player_net_id,
        network_example::game_server::kWeaponShotgun,
        &player_shotgun));
    require(player_shotgun.weapon_id == network_example::game_server::kWeaponShotgun);
    KernelWeaponMechanicsDefinition unavailable_player_weapon{};
    unavailable_player_weapon.struct_size = sizeof(unavailable_player_weapon);
    require(!Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        player_net_id,
        network_example::game_server::kWeaponFireFloor,
        &unavailable_player_weapon));

    run_server_frame(kernel, &game_server);
    std::array<KernelServerEntityState, 8> projectile_states{};
    std::uint32_t projectile_count = query_projectiles(kernel, &projectile_states);
    require(projectile_count >= 1);
    require(projectile_states[0].owner_peer == 0);
    require(projectile_states[0].velocity.x < 0.0f);

    run_server_frame(kernel, &game_server);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 1000);

    run_server_frames(kernel, &game_server, 30);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 1000);

    run_server_frames(kernel, &game_server, 30);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 1000);

    run_server_frame(kernel, &game_server);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require((enemy_states[0].visual_flags & KERNEL_VISUAL_FLAG_RELOADING) == 0);
    require(enemy_states[0].net_id == enemy_net_id);

    Kernel_Update(kernel, 1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].position.x == 6.0f);

    std::uint32_t next_input_seq = 1;
    for (int shot = 0; shot < 19; ++shot) {
        submit_player_fire(kernel, next_input_seq++, 0);
        run_server_frame(kernel, &game_server);
        run_server_frames(kernel, &game_server, 3);
    }
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].hp < 50);
    require(enemy_states[0].velocity.x > 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);
    const float fleeing_enemy_x = enemy_states[0].position.x;
    run_server_frame(kernel, &game_server);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].position.x > fleeing_enemy_x);

    KernelVec3 far_position{100.0f, 0.0f, 0.0f};
    KernelQuat identity_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel,
        enemy_net_id,
        &far_position,
        &identity_rotation));
    game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(enemy_states[0].velocity.x != 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    game_server.enemy_manager().despawn_all(KernelDespawnReason_Destroyed);
    game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 0);
    require(game_server.enemy_manager().enemy_count() == 0);

    Kernel_Destroy(kernel);

    KernelConfig host_config = listen_server_config();
    host_config.tick.snapshot_rate = host_config.tick.server_tick_rate;
    KernelHandle* host_kernel = Kernel_Create(&host_config);
    require(host_kernel != nullptr);
    require(Kernel_StartListenServer(host_kernel, 7782));

    network_example::game_server::GameServer host_game_server(
        host_kernel,
        single_spawn_gameplay_config());
    for (std::uint32_t frame = 1; frame <= 12; ++frame) {
        const PlayerInput input = stationary_input(frame);
        Kernel_SubmitInput(host_kernel, 1, &input);
        Kernel_Update(host_kernel, 1.0f / 30.0f);
        handle_pending_events(host_kernel, &host_game_server);
        host_game_server.tick(1.0f / 30.0f);
    }
    require(host_game_server.enemy_manager().enemy_count() == 1);
    require(render_states_include_type(
        host_kernel,
        network_example::game_server::kEntityTypeEnemy));
    Kernel_Destroy(host_kernel);

    KernelConfig player_death_config = listen_server_config();
    player_death_config.tick.snapshot_rate =
        player_death_config.tick.server_tick_rate;
    KernelHandle* player_death_kernel = Kernel_Create(&player_death_config);
    require(player_death_kernel != nullptr);
    require(Kernel_StartListenServer(player_death_kernel, 7783));

    network_example::game_server::GameServer player_death_game_server(
        player_death_kernel,
        single_spawn_gameplay_config());
    handle_pending_events(player_death_kernel, &player_death_game_server);
    player_death_game_server.tick(1.0f / 30.0f);
    run_server_frames(player_death_kernel, &player_death_game_server, 60);
    require(query_players(player_death_kernel, &player_states) == 1);
    require(player_states[0].hp == 1000);
    require(render_states_include_type(
        player_death_kernel,
        network_example::game_server::kEntityTypeEnemy));
    Kernel_Destroy(player_death_kernel);

    KernelConfig dedicated_config = dedicated_server_config();
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    require(dedicated_kernel != nullptr);
    require(Kernel_StartDedicatedServer(dedicated_kernel, 7781));

    network_example::game_server::GameServer dedicated_game_server(
        dedicated_kernel,
        single_spawn_gameplay_config());
    dedicated_game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(dedicated_kernel, &enemy_states);
    require(enemy_count == 0);

    KernelEvent player_joined{};
    player_joined.type = KernelEventType_PlayerJoined;
    player_joined.peer_id = 1;
    dedicated_game_server.handle_event(player_joined);
    dedicated_game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(dedicated_kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(enemy_states[0].velocity.x != 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    Kernel_Destroy(dedicated_kernel);

    return 0;
}
