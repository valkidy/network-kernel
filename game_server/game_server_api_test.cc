#include "game_server/public/game_server_api.h"

#include <array>
#include <cassert>
#include <cstdint>

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

void handle_pending_events(
    KernelHandle* kernel,
    GameServerHandle* game_server) {
    std::array<KernelEvent, 32> events{};
    const std::uint32_t count = Kernel_PollEvents(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        GameServer_HandleEvent(game_server, &events[index]);
    }
}

std::uint32_t query_enemy_count(KernelHandle* kernel) {
    std::array<KernelServerEntityState, 8> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    return Kernel_ServerQueryEntities(
        kernel,
        2,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
}

}  // namespace

int main() {
    GameServerAbiInfo info{};
    assert(GameServer_GetAbiInfo(&info, sizeof(info)));
    assert(info.struct_size == sizeof(GameServerAbiInfo));
    assert(info.abi_version == GAME_SERVER_ABI_VERSION);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_ENEMY_MANAGER) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_EVENT_HANDLING) != 0);
    assert((info.capability_flags & GAME_SERVER_CAPABILITY_DESPAWN_ALL) != 0);
    assert(!GameServer_GetAbiInfo(nullptr, sizeof(info)));
    assert(!GameServer_GetAbiInfo(&info, sizeof(info) - 1));

    assert(GameServer_Create(nullptr) == nullptr);
    GameServer_Destroy(nullptr);
    GameServer_HandleEvent(nullptr, nullptr);
    GameServer_Tick(nullptr, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(nullptr) == 0);
    GameServer_DespawnAll(nullptr, KernelDespawnReason_Destroyed);

    KernelConfig config = listen_server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartListenServer(kernel, 7777));

    GameServerHandle* game_server = GameServer_Create(kernel);
    assert(game_server != nullptr);
    handle_pending_events(kernel, game_server);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(game_server) == 1);
    assert(query_enemy_count(kernel) == 1);

    GameServer_DespawnAll(game_server, KernelDespawnReason_Destroyed);
    GameServer_Tick(game_server, 1.0f / 30.0f);
    assert(GameServer_GetEnemyCount(game_server) == 0);
    assert(query_enemy_count(kernel) == 0);

    GameServer_Destroy(game_server);
    Kernel_Destroy(kernel);
    return 0;
}
