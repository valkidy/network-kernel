#include "game_server/public/game_server_api.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include "game_server/game_server.h"

struct GameServerHandle {
    explicit GameServerHandle(
        KernelHandle* kernel,
        network_example::game_server::GameServerGameplayConfig config =
            network_example::game_server::default_game_server_gameplay_config())
        : server(kernel, std::move(config)) {}

    network_example::game_server::GameServer server;
};

namespace {

constexpr std::uint64_t kCapabilityFlags =
    GAME_SERVER_CAPABILITY_ENEMY_MANAGER |
    GAME_SERVER_CAPABILITY_EVENT_HANDLING |
    GAME_SERVER_CAPABILITY_DESPAWN_ALL |
    GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY |
    GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY;

}  // namespace

extern "C" {

bool GameServer_GetAbiInfo(
    GameServerAbiInfo* out_info,
    std::uint32_t out_info_size) {
    if (out_info == nullptr || out_info_size < sizeof(GameServerAbiInfo)) {
        return false;
    }

    out_info->struct_size = sizeof(GameServerAbiInfo);
    out_info->abi_version = GAME_SERVER_ABI_VERSION;
    out_info->weapon_template_info_size = sizeof(GameServerWeaponTemplateInfo);
    out_info->capability_flags = kCapabilityFlags;
    return true;
}

GameServerHandle* GameServer_Create(KernelHandle* kernel) {
    if (kernel == nullptr) {
        return nullptr;
    }

    try {
        return new GameServerHandle(kernel);
    } catch (...) {
        return nullptr;
    }
}

GameServerHandle* GameServer_CreateWithWeaponTemplateDirectory(
    KernelHandle* kernel,
    const char* template_directory) {
    if (kernel == nullptr || template_directory == nullptr || template_directory[0] == '\0') {
        return nullptr;
    }

    try {
        return new GameServerHandle(
            kernel,
            network_example::game_server::load_gameplay_config_from_weapon_template_directory(
                template_directory));
    } catch (...) {
        return nullptr;
    }
}

void GameServer_Destroy(GameServerHandle* game_server) {
    delete game_server;
}

void GameServer_HandleEvent(
    GameServerHandle* game_server,
    const KernelEvent* event) {
    if (game_server == nullptr || event == nullptr) {
        return;
    }

    try {
        game_server->server.handle_event(*event);
    } catch (...) {
    }
}

void GameServer_Tick(GameServerHandle* game_server, float delta_seconds) {
    if (game_server == nullptr) {
        return;
    }

    try {
        game_server->server.tick(delta_seconds);
    } catch (...) {
    }
}

std::uint32_t GameServer_GetEnemyCount(GameServerHandle* game_server) {
    if (game_server == nullptr) {
        return 0;
    }

    try {
        return static_cast<std::uint32_t>(
            game_server->server.enemy_manager().enemy_count());
    } catch (...) {
        return 0;
    }
}

bool GameServer_QueryWeaponTemplate(
    GameServerHandle* game_server,
    std::uint8_t weapon_id,
    GameServerWeaponTemplateInfo* out_info) {
    if (game_server == nullptr) {
        return false;
    }

    try {
        return game_server->server.query_weapon_template(weapon_id, out_info);
    } catch (...) {
        return false;
    }
}

void GameServer_DespawnAll(
    GameServerHandle* game_server,
    std::uint32_t reason) {
    if (game_server == nullptr) {
        return;
    }

    try {
        game_server->server.enemy_manager().despawn_all(reason);
    } catch (...) {
    }
}

}  // extern "C"
