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

}  // namespace

int main() {
    KernelConfig unstarted_config = listen_server_config();
    KernelHandle* unstarted_kernel = Kernel_Create(&unstarted_config);
    assert(unstarted_kernel != nullptr);
    network_example::game_server::GameServer unstarted_game_server(unstarted_kernel);
    unstarted_game_server.tick(1.0f / 30.0f);
    assert(unstarted_game_server.enemy_manager().enemy_count() == 0);
    Kernel_Destroy(unstarted_kernel);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartListenServer(kernel, 7777));

    network_example::game_server::GameServer game_server(kernel);
    handle_pending_events(kernel, &game_server);
    game_server.tick(1.0f / 30.0f);
    require(game_server.enemy_manager().enemy_count() == 1);

    std::array<KernelServerEntityState, 8> enemy_states{};
    std::uint32_t enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    const std::uint32_t enemy_net_id = enemy_states[0].net_id;
    require(enemy_states[0].position.x == 6.0f);
    require(enemy_states[0].animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(enemy_states[0].velocity.x == 0.0f);
    require(enemy_states[0].velocity.y == 0.0f);
    require(enemy_states[0].velocity.z == 0.0f);

    std::array<KernelServerEntityState, 8> player_states{};
    std::uint32_t player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 100);

    Kernel_Update(kernel, 1.0f / 30.0f);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 95);

    run_server_frame(kernel, &game_server);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 95);

    run_server_frames(kernel, &game_server, 30);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 90);

    run_server_frames(kernel, &game_server, 30);
    player_count = query_players(kernel, &player_states);
    require(player_count == 1);
    require(player_states[0].hp == 85);

    run_server_frame(kernel, &game_server);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require((enemy_states[0].visual_flags & KERNEL_VISUAL_FLAG_RELOADING) != 0);
    require(enemy_states[0].net_id == enemy_net_id);

    Kernel_Update(kernel, 1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    require(enemy_count == 1);
    require(enemy_states[0].position.x == 6.0f);

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

    KernelConfig dedicated_config = dedicated_server_config();
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    require(dedicated_kernel != nullptr);
    require(Kernel_StartDedicatedServer(dedicated_kernel, 7781));

    network_example::game_server::GameServer dedicated_game_server(dedicated_kernel);
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
