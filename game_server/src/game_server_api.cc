#include "game_server/public/game_server_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"
#include "game_server/src/game_server.h"

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
    GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE |
    GAME_SERVER_CAPABILITY_LOADOUT_MESSAGES |
    GAME_SERVER_CAPABILITY_TERRAIN_QUERY;

// The terrain each kernel's last catalog load composed, for the client to draw
// (GameServer_GetTerrainInfo and friends). Keyed by kernel because a pure
// client has no GameServerHandle; replaced by every load into that kernel.
struct TerrainView {
    network_example::game_server::terrain::HeightGrid heights;
    network_example::game_server::terrain::StoredRecipe recipe;
    std::vector<std::uint32_t> stamp_template_ids;      // per library stamp
    std::vector<std::int32_t> stamp_cells;              // per library stamp
};

std::mutex& terrain_views_mutex() {
    static std::mutex m;
    return m;
}

std::map<const KernelHandle*, std::shared_ptr<const TerrainView>>& terrain_views() {
    static std::map<const KernelHandle*, std::shared_ptr<const TerrainView>> views;
    return views;
}

void set_terrain_view(const KernelHandle* kernel, std::shared_ptr<const TerrainView> view) {
    std::scoped_lock lock(terrain_views_mutex());
    if (view) {
        terrain_views()[kernel] = std::move(view);
    } else {
        terrain_views().erase(kernel);
    }
}

std::shared_ptr<const TerrainView> terrain_view(const KernelHandle* kernel) {
    std::scoped_lock lock(terrain_views_mutex());
    const auto it = terrain_views().find(kernel);
    return it == terrain_views().end() ? nullptr : it->second;
}

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
    std::fprintf(
        stderr,
        "gameplay catalog load failed: template_kind=%u template_id=%u "
        "path=%s field=%s diagnostic=%s\n",
        static_cast<unsigned int>(error.template_kind),
        static_cast<unsigned int>(error.template_id),
        error.path.c_str(),
        error.field.c_str(),
        error.what());
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

bool load_gameplay_catalog_bundle(
    KernelHandle* kernel,
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const char* entry_path,
    KernelGameplayCatalogLoadResult* out_result,
    network_example::game_server::GameServerGameplayConfig* out_config) {
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle_bytes,
            bundle_size,
            entry_path);

    std::vector<std::uint8_t> collision_scene_bytes;
    std::shared_ptr<const TerrainView> pending_terrain_view;
    const network_example::game_server::StaticCollisionSceneConfig&
        static_scene = config.static_collision_scene;
    if (!static_scene.entry_path.empty()) {
        collision_scene_bytes =
            network_example::game_server::load_gameplay_bundle_entry_bytes(
                bundle_bytes,
                bundle_size,
                static_scene.entry_path);
    }
    if (config.terrain.enabled) {
        // A Host (out_config: it runs the game server) also gets the navmesh; a
        // client only composes, and only from a cooked recipe.
        namespace gs = network_example::game_server;
        gs::PreparedTerrain prepared;
        std::string error;
        const std::vector<std::uint8_t> cooked =
            gs::read_cooked_terrain_recipe(bundle_bytes, bundle_size, config.terrain);
        if (!gs::prepare_catalog_terrain(
                &config,
                cooked,
                out_config != nullptr ? gs::TerrainRole::kServer : gs::TerrainRole::kClient,
                &prepared,
                &error)) {
            set_load_error(
                out_result,
                cooked.empty() ? KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY
                               : KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG,
                error.c_str(),
                config.terrain.recipe_entry_path.c_str(),
                "terrain",
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
            return false;
        }
        collision_scene_bytes = std::move(prepared.collision_artifact);
        auto view = std::make_shared<TerrainView>();
        view->heights = std::move(prepared.heights);
        view->recipe = std::move(prepared.recipe);
        view->stamp_template_ids = config.terrain.stamp_template_ids;
        for (const auto& stamp : config.terrain.library.stamps) view->stamp_cells.push_back(stamp.cells_x);
        pending_terrain_view = std::move(view);
    }

    if (!collision_scene_bytes.empty()) {
        const network_example::game_server::KernelGameplayCatalogStorage storage =
            network_example::game_server::build_kernel_gameplay_catalog(config);
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = collision_scene_bytes.data();
        scene_config.artifact_size =
            static_cast<std::uint32_t>(collision_scene_bytes.size());
        scene_config.scene_id = static_scene.scene_id;
        scene_config.collider_id = static_scene.collider_id;
        scene_config.collision_layer = static_scene.collision_layer;
        std::uint32_t static_scene_rejected = 0u;
        KernelGameplayCatalogLoadOptions options{};
        options.struct_size = sizeof(options);
        options.static_collision_scene = &scene_config;
        options.out_static_scene_rejected = &static_scene_rejected;
        if (!Kernel_LoadGameplayCatalog(
                kernel,
                &storage.definition,
                &options)) {
            set_load_error(
                out_result,
                KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG,
                static_scene_rejected != 0u
                    ? "kernel rejected static collision scene registration"
                    : "kernel rejected gameplay catalog",
                static_scene_rejected != 0u ? static_scene.entry_path : "",
                static_scene_rejected != 0u ? "static_collision_scene" : "",
                KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE,
                KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG);
            return false;
        }
    } else if (!network_example::game_server::load_kernel_gameplay_catalog(
                   kernel,
                   config)) {
        set_load_error(
            out_result,
            KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG,
            "kernel rejected gameplay catalog");
        return false;
    }

    // Only a load that went through replaces what the terrain queries see.
    set_terrain_view(kernel, std::move(pending_terrain_view));
    fill_load_result(out_result, config);
    if (out_config != nullptr) {
        *out_config = std::move(config);
    }
    return true;
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
        return load_gameplay_catalog_bundle(
            kernel,
            bundle_bytes,
            bundle_size,
            entry_path,
            out_result,
            nullptr);
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
        network_example::game_server::GameServerGameplayConfig config;
        if (!load_gameplay_catalog_bundle(
                kernel,
                bundle_bytes,
                bundle_size,
                entry_path,
                out_result,
                &config)) {
            return nullptr;
        }
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
            game_server->server.agent_runtime_manager().agent_count());
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
        game_server->server.agent_runtime_manager().despawn_all(reason);
    } catch (...) {
    }
}

bool GameServer_GetTerrainInfo(KernelHandle* kernel, GameServerTerrainInfo* out_info) {
    if (kernel == nullptr || out_info == nullptr || out_info->struct_size < sizeof(GameServerTerrainInfo)) {
        return false;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel);
    if (!view) {
        return false;
    }
    const auto& h = view->heights;
    const auto [lo, hi] = std::minmax_element(h.cm.begin(), h.cm.end());
    *out_info = GameServerTerrainInfo{};
    out_info->struct_size = sizeof(GameServerTerrainInfo);
    out_info->samples_x = static_cast<std::uint32_t>(h.samples_x);
    out_info->samples_z = static_cast<std::uint32_t>(h.samples_z);
    out_info->cell_mm = static_cast<std::uint32_t>(network_example::game_server::terrain::kCellMm);
    out_info->origin_x = static_cast<float>(-(h.samples_x - 1) * 0.15);
    out_info->origin_z = static_cast<float>(-(h.samples_z - 1) * 0.15);
    out_info->min_height_cm = *lo;
    out_info->max_height_cm = *hi;
    out_info->heights_hash = view->recipe.heights_hash;
    out_info->seed = view->recipe.recipe.seed;
    out_info->placement_count = static_cast<std::uint32_t>(view->recipe.recipe.placements.size());
    return true;
}

bool GameServer_CopyTerrainHeights(
    KernelHandle* kernel,
    std::uint32_t x0,
    std::uint32_t z0,
    std::uint32_t width,
    std::uint32_t depth,
    std::int16_t* out_cm,
    std::uint32_t out_count) {
    if (kernel == nullptr || out_cm == nullptr || width == 0 || depth == 0) {
        return false;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel);
    if (!view) {
        return false;
    }
    const auto& h = view->heights;
    const std::uint64_t nx = static_cast<std::uint64_t>(h.samples_x), nz = static_cast<std::uint64_t>(h.samples_z);
    if (static_cast<std::uint64_t>(x0) + width > nx || static_cast<std::uint64_t>(z0) + depth > nz ||
        static_cast<std::uint64_t>(width) * depth > out_count) {
        return false;
    }
    for (std::uint32_t j = 0; j < depth; ++j) {
        const std::int16_t* row = h.cm.data() + (static_cast<std::size_t>(z0) + j) * nx + x0;
        std::memcpy(out_cm + static_cast<std::size_t>(j) * width, row, static_cast<std::size_t>(width) * sizeof(std::int16_t));
    }
    return true;
}

std::uint32_t GameServer_CopyTerrainPlacements(
    KernelHandle* kernel,
    GameServerTerrainPlacement* out_placements,
    std::uint32_t capacity) {
    if (kernel == nullptr) {
        return 0;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel);
    if (!view) {
        return 0;
    }
    const auto& recipe = view->recipe.recipe;
    const float ox = static_cast<float>(-recipe.cells_x * 0.15), oz = static_cast<float>(-recipe.cells_z * 0.15);
    const std::uint32_t total = static_cast<std::uint32_t>(recipe.placements.size());
    for (std::uint32_t k = 0; k < total && k < capacity && out_placements != nullptr; ++k) {
        const auto& p = recipe.placements[k];
        GameServerTerrainPlacement& o = out_placements[k];
        o = GameServerTerrainPlacement{};
        o.struct_size = sizeof(GameServerTerrainPlacement);
        o.stamp_template_id = view->stamp_template_ids[p.stamp];
        o.min_x = ox + static_cast<float>(p.x_cells * 0.3);
        o.min_z = oz + static_cast<float>(p.z_cells * 0.3);
        o.size_x = o.size_z = static_cast<float>(view->stamp_cells[p.stamp] * 0.3);
        o.base_y = static_cast<float>(p.base_cm / 100.0);
        o.turns = p.turns;
        o.mirror = p.mirror ? 1 : 0;
    }
    return total;
}

void GameServer_ReleaseTerrain(KernelHandle* kernel) {
    set_terrain_view(kernel, nullptr);
}

}  // extern "C"
