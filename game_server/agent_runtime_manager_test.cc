#include "game_server/game_server.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>

#include "kernel/public/kernel_api.h"

namespace {

constexpr std::uint16_t kMaxReserveMagazines =
    std::numeric_limits<std::uint16_t>::max();

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
    config.agent.spawn_count = 1;
    config.agent.spawn_radius = 0.0f;
    config.agent.spawn_position = KernelVec3{6.0f, 0.0f, 0.0f};
    for (network_example::game_server::EntityTemplateConfig& entity_template :
         config.entity_templates) {
        if (entity_template.entity_type != KernelEntityType_Director) {
            continue;
        }
        entity_template.director_spawn_target_count = 1;
        entity_template.director_spawn_radius = 0.0f;
        entity_template.director_spawn_position = config.agent.spawn_position;
    }
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

std::uint32_t query_actors_by_type(
    KernelHandle* kernel,
    std::uint16_t actor_type,
    std::array<KernelServerEntityState, 8>* states) {
    std::array<KernelServerEntityState, 8> actor_states{};
    const std::uint32_t count = query_entities(
        kernel,
        network_example::game_server::kEntityTypeActor,
        &actor_states);
    std::uint32_t write_index = 0;
    for (std::uint32_t index = 0; index < count && write_index < states->size(); ++index) {
        if (actor_states[index].actor_type != actor_type) {
            continue;
        }
        (*states)[write_index++] = actor_states[index];
    }
    return write_index;
}

std::uint32_t query_enemies(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_actors_by_type(
        kernel,
        network_example::game_server::kActorTypeAgent,
        states);
}

std::uint32_t query_players(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_actors_by_type(
        kernel,
        network_example::game_server::kActorTypePlayer,
        states);
}

std::uint32_t query_projectiles(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_entities(kernel, 3, states);
}

std::uint32_t query_directors(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    return query_entities(kernel, KernelEntityType_Director, states);
}

bool render_states_include_actor_type(KernelHandle* kernel, std::uint16_t actor_type) {
    std::array<RenderEntityState, 16> states{};
    const std::uint32_t count = Kernel_GetRenderStates(
        kernel,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == network_example::game_server::kEntityTypeActor &&
            states[index].actor_type == actor_type) {
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
    assert(unstarted_game_server.agent_runtime_manager().agent_count() == 0);
    Kernel_Destroy(unstarted_kernel);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartListenServer(kernel, 7777));

    network_example::game_server::GameServerGameplayConfig gameplay_config =
        single_spawn_gameplay_config();
    gameplay_config.actor_templates[1].sentry.alert_ticks = 3;
    gameplay_config.actor_templates[1].sentry.forget_ticks = 3;
    network_example::game_server::GameServer game_server(kernel, gameplay_config);
    handle_pending_events(kernel, &game_server);
    run_server_frames(kernel, &game_server, 3);
    require(game_server.agent_runtime_manager().agent_count() == 1);
    std::array<KernelServerEntityState, 8> director_states{};
    require(query_directors(kernel, &director_states) == 1);

    std::array<KernelServerEntityState, 8> enemy_states{};
    std::uint32_t agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    const std::uint32_t enemy_net_id = enemy_states[0].net_id;
    require(enemy_states[0].ammo[network_example::game_server::kWeaponGrenade] == 120);
    require(
        enemy_states[0].reserve_magazines[network_example::game_server::kWeaponGrenade] ==
        kMaxReserveMagazines);
    KernelWeaponMechanicsDefinition enemy_weapon{};
    enemy_weapon.struct_size = sizeof(enemy_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        enemy_net_id,
        network_example::game_server::kWeaponGrenade,
        &enemy_weapon));
    require(enemy_weapon.weapon_id == network_example::game_server::kWeaponGrenade);
    require(enemy_weapon.damage == 1);
    require(enemy_weapon.fire_action_template_id != 0);
    require(enemy_weapon.reload_action_template_id != 0);
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
            0);
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
    KernelWeaponMechanicsDefinition player_rocket{};
    player_rocket.struct_size = sizeof(player_rocket);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        player_net_id,
        network_example::game_server::kWeaponRocket,
        &player_rocket));
    require(player_rocket.weapon_id == network_example::game_server::kWeaponRocket);
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

    Kernel_Update(kernel, 1.0f / 30.0f);
    game_server.tick(1.0f / 30.0f);
    require(game_server.agent_runtime_manager().agents()[0].sentry.state ==
            network_example::game_server::AgentSentryState::kAlert);
    std::array<KernelServerEntityState, 8> projectile_states{};
    std::uint32_t projectile_count = query_projectiles(kernel, &projectile_states);
    require(projectile_count == 0);

    run_server_frames(kernel, &game_server, 5);
    require(game_server.agent_runtime_manager().agents()[0].sentry.state ==
            network_example::game_server::AgentSentryState::kAttack);
    projectile_count = query_projectiles(kernel, &projectile_states);
    require(projectile_count >= 1);
    require(projectile_states[0].owner_peer == 0);
    require(projectile_states[0].velocity.x < 0.0f);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require(enemy_states[0].position.x == 6.0f);
    require(enemy_states[0].velocity.x == 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    bool saw_spammer_empty_magazine = false;
    bool saw_spammer_reload = false;
    bool saw_spammer_reloaded = false;
    for (int frame = 0; frame < 180 && !saw_spammer_reloaded; ++frame) {
        run_server_frame(kernel, &game_server);
        agent_count = query_enemies(kernel, &enemy_states);
        require(agent_count == 1);
        const KernelServerEntityState& enemy_state = enemy_states[0];
        if (enemy_state.ammo[network_example::game_server::kWeaponGrenade] == 0 &&
            enemy_state.reserve_magazines[network_example::game_server::kWeaponGrenade] ==
                kMaxReserveMagazines) {
            saw_spammer_empty_magazine = true;
        }
        if (saw_spammer_empty_magazine && enemy_state.is_reloading != 0u) {
            saw_spammer_reload = true;
        }
        if (saw_spammer_reload && enemy_state.is_reloading == 0u &&
            enemy_state.ammo[network_example::game_server::kWeaponGrenade] == 120 &&
            enemy_state.reserve_magazines[network_example::game_server::kWeaponGrenade] ==
                kMaxReserveMagazines - 1u) {
            saw_spammer_reloaded = true;
        }
    }
    require(saw_spammer_empty_magazine);
    require(saw_spammer_reload);
    require(saw_spammer_reloaded);

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
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require((enemy_states[0].visual_flags & KERNEL_VISUAL_FLAG_RELOADING) == 0);
    require(enemy_states[0].net_id == enemy_net_id);

    Kernel_Update(kernel, 1.0f / 30.0f);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require(enemy_states[0].position.x == 6.0f);

    KernelVec3 far_position{100.0f, 0.0f, 0.0f};
    KernelQuat identity_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel,
        player_net_id,
        &far_position,
        &identity_rotation));
    Kernel_Update(kernel, 1.0f / 30.0f);
    game_server.tick(1.0f / 30.0f);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require(game_server.agent_runtime_manager().agents()[0].sentry.state ==
            network_example::game_server::AgentSentryState::kAttack);
    run_server_frames(kernel, &game_server, 2);
    require(game_server.agent_runtime_manager().agents()[0].sentry.state ==
            network_example::game_server::AgentSentryState::kAlert);
    run_server_frames(kernel, &game_server, 3);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require(game_server.agent_runtime_manager().agents()[0].sentry.state ==
            network_example::game_server::AgentSentryState::kIdle);
    require(enemy_states[0].animation_state ==
            0);
    require(enemy_states[0].velocity.x == 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    game_server.agent_runtime_manager().despawn_all(KernelDespawnReason_Destroyed);
    game_server.tick(1.0f / 30.0f);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 1);
    require(game_server.agent_runtime_manager().agent_count() == 0);
    Kernel_Update(kernel, 1.0f / 30.0f);
    agent_count = query_enemies(kernel, &enemy_states);
    require(agent_count == 0);
    require(query_directors(kernel, &director_states) == 0);
    require(game_server.agent_runtime_manager().agent_count() == 0);

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
    require(host_game_server.agent_runtime_manager().agent_count() == 1);
    require(render_states_include_actor_type(
        host_kernel,
        network_example::game_server::kActorTypeAgent));
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
    require(render_states_include_actor_type(
        player_death_kernel,
        network_example::game_server::kActorTypeAgent));
    Kernel_Destroy(player_death_kernel);

    KernelConfig dedicated_config = dedicated_server_config();
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    require(dedicated_kernel != nullptr);
    require(Kernel_StartDedicatedServer(dedicated_kernel, 7781));

    network_example::game_server::GameServer dedicated_game_server(
        dedicated_kernel,
        single_spawn_gameplay_config());
    dedicated_game_server.tick(1.0f / 30.0f);
    agent_count = query_enemies(dedicated_kernel, &enemy_states);
    require(agent_count == 0);

    KernelEvent player_joined{};
    player_joined.type = KernelEventType_PlayerJoined;
    player_joined.peer_id = 1;
    dedicated_game_server.handle_event(player_joined);
    run_server_frames(dedicated_kernel, &dedicated_game_server, 3);
    agent_count = query_enemies(dedicated_kernel, &enemy_states);
    require(agent_count == 1);
    require(enemy_states[0].animation_state ==
            0);
    require(enemy_states[0].velocity.x == 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    Kernel_Destroy(dedicated_kernel);

    return 0;
}
