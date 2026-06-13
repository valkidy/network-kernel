#include "game_server/public/game_server_api.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "game_server/gameplay_config.h"
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
    GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY |
    GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE;

void initialize_load_result(KernelGameplayCatalogLoadResult* out_result) {
    if (out_result == nullptr) {
        return;
    }
    std::memset(out_result, 0, sizeof(KernelGameplayCatalogLoadResult));
    out_result->struct_size = sizeof(KernelGameplayCatalogLoadResult);
    out_result->status = KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED;
    out_result->error_code = KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_NONE;
    out_result->line = -1;
    out_result->column = -1;
}

void copy_load_result_text(char* destination, std::size_t size, const std::string& value) {
    if (destination == nullptr || size == 0) {
        return;
    }
    std::strncpy(destination, value.c_str(), size - 1);
}

void set_load_error(
    KernelGameplayCatalogLoadResult* out_result,
    std::uint32_t error_code,
    const std::string& diagnostic,
    const std::string& path = {},
    const std::string& field = {},
    std::uint32_t source_kind = KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN,
    std::uint32_t template_kind = KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN,
    std::uint32_t template_id = 0,
    std::uint32_t field_id = 0,
    std::int32_t line = -1,
    std::int32_t column = -1) {
    if (out_result == nullptr) {
        return;
    }
    out_result->status = KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED;
    out_result->error_code = error_code;
    out_result->source_kind = source_kind;
    out_result->template_kind = template_kind;
    out_result->template_id = template_id;
    out_result->field_id = field_id;
    out_result->line = line;
    out_result->column = column;
    copy_load_result_text(out_result->path, sizeof(out_result->path), path);
    copy_load_result_text(out_result->field, sizeof(out_result->field), field);
    copy_load_result_text(
        out_result->diagnostic,
        sizeof(out_result->diagnostic),
        diagnostic);
}

void set_load_error(
    KernelGameplayCatalogLoadResult* out_result,
    const network_example::game_server::DataLoadError& error) {
    set_load_error(
        out_result,
        error.error_code,
        error.what(),
        error.path,
        error.field,
        error.source_kind,
        error.template_kind,
        error.template_id,
        error.field_id,
        error.line,
        error.column);
}

void fill_load_result(
    KernelGameplayCatalogLoadResult* out_result,
    const network_example::game_server::GameServerGameplayConfig& config) {
    if (out_result == nullptr) {
        return;
    }
    const network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    out_result->status = KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS;
    out_result->error_code = KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_NONE;
    out_result->catalog_version = storage.definition.catalog_version;
    out_result->catalog_hash = storage.definition.catalog_hash;
    out_result->projectile_template_count =
        storage.definition.projectile_template_count;
    out_result->collider_template_count =
        storage.definition.collider_template_count;
    out_result->collider_binding_count =
        storage.definition.collider_binding_count;
}

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
    out_info->gameplay_catalog_load_result_size =
        sizeof(KernelGameplayCatalogLoadResult);
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

bool Kernel_LoadGameplayCatalogFromMemory(
    KernelHandle* kernel,
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const char* entry_path,
    KernelGameplayCatalogLoadResult* out_result) {
    initialize_load_result(out_result);
    if (kernel == nullptr || bundle_bytes == nullptr || bundle_size == 0 ||
        entry_path == nullptr || entry_path[0] == '\0' || out_result == nullptr) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARGUMENT,
            "invalid gameplay catalog bundle load arguments");
        return false;
    }

    try {
        network_example::game_server::GameServerGameplayConfig config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                bundle_bytes,
                bundle_size,
                entry_path);
        if (!network_example::game_server::load_kernel_gameplay_catalog(
                kernel,
                config)) {
            set_load_error(
                out_result,
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG,
                "kernel rejected gameplay catalog");
            return false;
        }
        fill_load_result(out_result, config);
        return true;
    } catch (const network_example::game_server::DataLoadError& error) {
        set_load_error(out_result, error);
        return false;
    } catch (const std::exception& error) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN,
            error.what());
        return false;
    } catch (...) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN,
            "unknown gameplay catalog bundle load failure");
        return false;
    }
}

GameServerHandle* GameServer_CreateWithGameplayCatalogFromMemory(
    KernelHandle* kernel,
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const char* entry_path,
    KernelGameplayCatalogLoadResult* out_result) {
    initialize_load_result(out_result);
    if (kernel == nullptr || bundle_bytes == nullptr || bundle_size == 0 ||
        entry_path == nullptr || entry_path[0] == '\0' || out_result == nullptr) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARGUMENT,
            "invalid gameplay catalog bundle load arguments");
        return nullptr;
    }

    try {
        network_example::game_server::GameServerGameplayConfig config =
            network_example::game_server::load_gameplay_config_from_bundle_memory(
                bundle_bytes,
                bundle_size,
                entry_path);
        if (!network_example::game_server::load_kernel_gameplay_catalog(
                kernel,
                config)) {
            set_load_error(
                out_result,
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG,
                "kernel rejected gameplay catalog");
            return nullptr;
        }
        fill_load_result(out_result, config);
        return new GameServerHandle(kernel, std::move(config));
    } catch (const network_example::game_server::DataLoadError& error) {
        set_load_error(out_result, error);
        return nullptr;
    } catch (const std::exception& error) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN,
            error.what());
        return nullptr;
    } catch (...) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN,
            "unknown gameplay catalog bundle load failure");
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
