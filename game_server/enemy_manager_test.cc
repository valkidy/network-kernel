#include "game_server/game_server.h"

#include <array>
#include <cassert>
#include <cstdint>

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

std::uint32_t query_enemies(
    KernelHandle* kernel,
    std::array<KernelServerEntityState, 8>* states) {
    for (KernelServerEntityState& state : *states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    return Kernel_ServerQueryEntities(
        kernel,
        network_example::game_server::kEntityTypeEnemy,
        states->data(),
        static_cast<std::uint32_t>(states->size()));
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
    assert(kernel != nullptr);
    assert(Kernel_StartListenServer(kernel, 7777));

    network_example::game_server::GameServer game_server(kernel);
    handle_pending_events(kernel, &game_server);
    game_server.tick(1.0f / 30.0f);
    assert(game_server.enemy_manager().enemy_count() == 1);

    std::array<KernelServerEntityState, 8> enemy_states{};
    std::uint32_t enemy_count = query_enemies(kernel, &enemy_states);
    assert(enemy_count == 1);
    const std::uint32_t enemy_net_id = enemy_states[0].net_id;
    assert(enemy_states[0].position.x == 6.0f);
    assert(enemy_states[0].animation_state ==
           network_example::game_server::kEnemyAnimationChasing);
    assert(enemy_states[0].velocity.x < 0.0f);

    game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    assert(enemy_count == 1);
    assert(enemy_states[0].net_id == enemy_net_id);

    Kernel_Update(kernel, 1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    assert(enemy_count == 1);
    assert(enemy_states[0].position.x < 6.0f);

    KernelVec3 far_position{100.0f, 0.0f, 0.0f};
    KernelQuat identity_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    assert(Kernel_ServerSetEntityTransform(
        kernel,
        enemy_net_id,
        &far_position,
        &identity_rotation));
    game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    assert(enemy_count == 1);
    assert(enemy_states[0].animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(enemy_states[0].velocity.x == 0.0f);
    assert(enemy_states[0].velocity.y == 0.0f);
    assert(enemy_states[0].velocity.z == 0.0f);

    game_server.enemy_manager().despawn_all(KernelDespawnReason_Destroyed);
    game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(kernel, &enemy_states);
    assert(enemy_count == 0);
    assert(game_server.enemy_manager().enemy_count() == 0);

    Kernel_Destroy(kernel);

    KernelConfig dedicated_config = dedicated_server_config();
    KernelHandle* dedicated_kernel = Kernel_Create(&dedicated_config);
    assert(dedicated_kernel != nullptr);
    assert(Kernel_StartDedicatedServer(dedicated_kernel, 7781));

    network_example::game_server::GameServer dedicated_game_server(dedicated_kernel);
    dedicated_game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(dedicated_kernel, &enemy_states);
    assert(enemy_count == 0);

    KernelEvent player_joined{};
    player_joined.type = KernelEventType_PlayerJoined;
    player_joined.peer_id = 1;
    dedicated_game_server.handle_event(player_joined);
    dedicated_game_server.tick(1.0f / 30.0f);
    enemy_count = query_enemies(dedicated_kernel, &enemy_states);
    assert(enemy_count == 1);
    assert(enemy_states[0].animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(enemy_states[0].velocity.x == 0.0f);
    assert(enemy_states[0].velocity.y == 0.0f);
    assert(enemy_states[0].velocity.z == 0.0f);

    Kernel_Destroy(dedicated_kernel);
    return 0;
}
