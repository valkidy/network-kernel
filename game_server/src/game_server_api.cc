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
#include "physics/public/physics_world.h"
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
    GAME_SERVER_CAPABILITY_TERRAIN_QUERY |
    GAME_SERVER_CAPABILITY_STATIC_SCENE_MESH_QUERY |
    GAME_SERVER_CAPABILITY_SCENE_SLOTS;

// The terrain each kernel's last catalog load composed, for the client to draw
// (GameServer_GetTerrainInfo and friends). Keyed by kernel because a pure
// client has no GameServerHandle; replaced by every load into that kernel.
struct TerrainView {
    // Where the map's origin sits: zero for the camp, a level slot's offset
    // (docs/SCENE_TABLE_PLAN.md S4).
    float offset_x = 0.0f;
    network_example::game_server::terrain::HeightGrid heights;
    network_example::game_server::terrain::StoredRecipe recipe;
    std::vector<std::uint32_t> stamp_template_ids;      // per library stamp
    std::vector<std::int32_t> stamp_cells;              // per library stamp
};

// A mesh level's baked static scene, kept as the artifact and turned into
// triangles the first time a client asks (GameServer_GetStaticSceneMeshInfo).
struct StaticSceneView {
    float offset_x = 0.0f;
    std::vector<std::uint8_t> artifact;
    std::once_flag extracted;
    bool ok = false;
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};

    bool extract() {
        std::call_once(extracted, [this] {
            std::string error;
            ok = network_example::physics::static_scene_triangles(artifact, &positions, &indices, &error);
            if (!ok) {
                std::fprintf(stderr, "static scene mesh query: %s\n", error.c_str());
                return;
            }
            for (int axis = 0; axis < 3; ++axis) {
                min[axis] = max[axis] = positions[static_cast<std::size_t>(axis)];
            }
            for (std::size_t k = 0; k < positions.size(); ++k) {
                const int axis = static_cast<int>(k % 3u);
                min[axis] = std::min(min[axis], positions[k]);
                max[axis] = std::max(max[axis], positions[k]);
            }
            std::vector<std::uint8_t>().swap(artifact);
        });
        return ok;
    }
};

std::mutex& static_scene_views_mutex() {
    static std::mutex m;
    return m;
}

// Keyed by kernel and scene slot: slot 0 is the catalog's own map, the
// others levels a client loaded (GameServer_ClientLoadSceneSlot).
using SceneKey = std::pair<const KernelHandle*, std::uint32_t>;

std::map<SceneKey, std::shared_ptr<StaticSceneView>>& static_scene_views() {
    static std::map<SceneKey, std::shared_ptr<StaticSceneView>> views;
    return views;
}

void set_static_scene_view(
    const KernelHandle* kernel, std::uint32_t slot, std::shared_ptr<StaticSceneView> view) {
    std::scoped_lock lock(static_scene_views_mutex());
    if (view) {
        static_scene_views()[SceneKey{kernel, slot}] = std::move(view);
    } else {
        static_scene_views().erase(SceneKey{kernel, slot});
    }
}

std::shared_ptr<StaticSceneView> static_scene_view(const KernelHandle* kernel, std::uint32_t slot) {
    std::scoped_lock lock(static_scene_views_mutex());
    const auto it = static_scene_views().find(SceneKey{kernel, slot});
    return it == static_scene_views().end() ? nullptr : it->second;
}

std::mutex& terrain_views_mutex() {
    static std::mutex m;
    return m;
}

std::map<SceneKey, std::shared_ptr<const TerrainView>>& terrain_views() {
    static std::map<SceneKey, std::shared_ptr<const TerrainView>> views;
    return views;
}

void set_terrain_view(
    const KernelHandle* kernel, std::uint32_t slot, std::shared_ptr<const TerrainView> view) {
    std::scoped_lock lock(terrain_views_mutex());
    if (view) {
        terrain_views()[SceneKey{kernel, slot}] = std::move(view);
    } else {
        terrain_views().erase(SceneKey{kernel, slot});
    }
}

std::shared_ptr<const TerrainView> terrain_view(const KernelHandle* kernel, std::uint32_t slot) {
    std::scoped_lock lock(terrain_views_mutex());
    const auto it = terrain_views().find(SceneKey{kernel, slot});
    return it == terrain_views().end() ? nullptr : it->second;
}

// Every slot's views of `kernel`, for GameServer_ReleaseTerrain.
void release_scene_views(const KernelHandle* kernel) {
    {
        std::scoped_lock lock(terrain_views_mutex());
        auto& views = terrain_views();
        for (auto it = views.begin(); it != views.end();) {
            it = it->first.first == kernel ? views.erase(it) : std::next(it);
        }
    }
    std::scoped_lock lock(static_scene_views_mutex());
    auto& views = static_scene_views();
    for (auto it = views.begin(); it != views.end();) {
        it = it->first.first == kernel ? views.erase(it) : std::next(it);
    }
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
    std::shared_ptr<StaticSceneView> pending_static_scene_view;
    if (!config.terrain.enabled && !collision_scene_bytes.empty()) {
        pending_static_scene_view = std::make_shared<StaticSceneView>();
        pending_static_scene_view->artifact = std::move(collision_scene_bytes);
    }
    set_terrain_view(kernel, 0u, std::move(pending_terrain_view));
    set_static_scene_view(kernel, 0u, std::move(pending_static_scene_view));
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
        const bool has_levels = !config.scene_templates.empty();
        auto handle = std::make_unique<GameServerHandle>(kernel, std::move(config));
        if (has_levels) {
            // A catalog with levels can open them; one without is unchanged.
            network_example::game_server::SceneCatalogSource source;
            source.bundle = std::make_shared<const std::vector<std::uint8_t>>(
                bundle_bytes, bundle_bytes + bundle_size);
            source.entry_path = entry_path;
            handle->server.enable_scene_table(std::move(source));
        }
        return handle.release();
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

}  // extern "C"

namespace {

bool scene_terrain_info(KernelHandle* kernel, std::uint32_t slot, GameServerTerrainInfo* out_info) {
    if (kernel == nullptr || out_info == nullptr || out_info->struct_size < sizeof(GameServerTerrainInfo)) {
        return false;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel, slot);
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
    out_info->origin_x = view->offset_x + static_cast<float>(-(h.samples_x - 1) * 0.15);
    out_info->origin_z = static_cast<float>(-(h.samples_z - 1) * 0.15);
    out_info->min_height_cm = *lo;
    out_info->max_height_cm = *hi;
    out_info->heights_hash = view->recipe.heights_hash;
    out_info->seed = view->recipe.recipe.seed;
    out_info->placement_count = static_cast<std::uint32_t>(view->recipe.recipe.placements.size());
    return true;
}

bool scene_terrain_heights(
    KernelHandle* kernel,
    std::uint32_t slot,
    std::uint32_t x0,
    std::uint32_t z0,
    std::uint32_t width,
    std::uint32_t depth,
    std::int16_t* out_cm,
    std::uint32_t out_count) {
    if (kernel == nullptr || out_cm == nullptr || width == 0 || depth == 0) {
        return false;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel, slot);
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

std::uint32_t scene_terrain_placements(
    KernelHandle* kernel,
    std::uint32_t slot,
    GameServerTerrainPlacement* out_placements,
    std::uint32_t capacity) {
    if (kernel == nullptr) {
        return 0;
    }
    const std::shared_ptr<const TerrainView> view = terrain_view(kernel, slot);
    if (!view) {
        return 0;
    }
    const auto& recipe = view->recipe.recipe;
    const float ox = view->offset_x + static_cast<float>(-recipe.cells_x * 0.15), oz = static_cast<float>(-recipe.cells_z * 0.15);
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

bool scene_static_mesh_info(
    KernelHandle* kernel,
    std::uint32_t slot,
    GameServerStaticSceneMeshInfo* out_info) {
    if (kernel == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(GameServerStaticSceneMeshInfo)) {
        return false;
    }
    const std::shared_ptr<StaticSceneView> view = static_scene_view(kernel, slot);
    if (!view || !view->extract()) {
        return false;
    }
    *out_info = GameServerStaticSceneMeshInfo{};
    out_info->struct_size = sizeof(GameServerStaticSceneMeshInfo);
    out_info->vertex_count = static_cast<std::uint32_t>(view->positions.size() / 3u);
    out_info->triangle_count = static_cast<std::uint32_t>(view->indices.size() / 3u);
    out_info->min_x = view->min[0] + view->offset_x;
    out_info->min_y = view->min[1];
    out_info->min_z = view->min[2];
    out_info->max_x = view->max[0] + view->offset_x;
    out_info->max_y = view->max[1];
    out_info->max_z = view->max[2];
    return true;
}

bool scene_static_mesh(
    KernelHandle* kernel,
    std::uint32_t slot,
    float* out_positions,
    std::uint32_t position_capacity,
    std::uint32_t* out_indices,
    std::uint32_t index_capacity) {
    if (kernel == nullptr || out_positions == nullptr || out_indices == nullptr) {
        return false;
    }
    const std::shared_ptr<StaticSceneView> view = static_scene_view(kernel, slot);
    if (!view || !view->extract() || view->positions.size() > position_capacity ||
        view->indices.size() > index_capacity) {
        return false;
    }
    std::memcpy(out_positions, view->positions.data(), view->positions.size() * sizeof(float));
    for (std::size_t k = 0; k < view->positions.size(); k += 3u) {
        out_positions[k] += view->offset_x;
    }
    std::memcpy(out_indices, view->indices.data(), view->indices.size() * sizeof(std::uint32_t));
    return true;
}

}  // namespace

extern "C" {

bool GameServer_GetTerrainInfo(KernelHandle* kernel, GameServerTerrainInfo* out_info) {
    return scene_terrain_info(kernel, 0u, out_info);
}

bool GameServer_CopyTerrainHeights(
    KernelHandle* kernel,
    std::uint32_t x0,
    std::uint32_t z0,
    std::uint32_t width,
    std::uint32_t depth,
    std::int16_t* out_cm,
    std::uint32_t out_count) {
    return scene_terrain_heights(kernel, 0u, x0, z0, width, depth, out_cm, out_count);
}

std::uint32_t GameServer_CopyTerrainPlacements(
    KernelHandle* kernel,
    GameServerTerrainPlacement* out_placements,
    std::uint32_t capacity) {
    return scene_terrain_placements(kernel, 0u, out_placements, capacity);
}

void GameServer_ReleaseTerrain(KernelHandle* kernel) {
    release_scene_views(kernel);
}

bool GameServer_GetStaticSceneMeshInfo(
    KernelHandle* kernel,
    GameServerStaticSceneMeshInfo* out_info) {
    return scene_static_mesh_info(kernel, 0u, out_info);
}

bool GameServer_CopyStaticSceneMesh(
    KernelHandle* kernel,
    float* out_positions,
    std::uint32_t position_capacity,
    std::uint32_t* out_indices,
    std::uint32_t index_capacity) {
    return scene_static_mesh(kernel, 0u, out_positions, position_capacity, out_indices, index_capacity);
}

bool GameServer_GetSceneTerrainInfo(
    KernelHandle* kernel, uint32_t slot, GameServerTerrainInfo* out_info) {
    return scene_terrain_info(kernel, slot, out_info);
}

bool GameServer_CopySceneTerrainHeights(
    KernelHandle* kernel,
    uint32_t slot,
    uint32_t x0,
    uint32_t z0,
    uint32_t width,
    uint32_t depth,
    int16_t* out_cm,
    uint32_t out_count) {
    return scene_terrain_heights(kernel, slot, x0, z0, width, depth, out_cm, out_count);
}

uint32_t GameServer_CopySceneTerrainPlacements(
    KernelHandle* kernel,
    uint32_t slot,
    GameServerTerrainPlacement* out_placements,
    uint32_t capacity) {
    return scene_terrain_placements(kernel, slot, out_placements, capacity);
}

bool GameServer_GetSceneStaticMeshInfo(
    KernelHandle* kernel, uint32_t slot, GameServerStaticSceneMeshInfo* out_info) {
    return scene_static_mesh_info(kernel, slot, out_info);
}

bool GameServer_CopySceneStaticMesh(
    KernelHandle* kernel,
    uint32_t slot,
    float* out_positions,
    uint32_t position_capacity,
    uint32_t* out_indices,
    uint32_t index_capacity) {
    return scene_static_mesh(kernel, slot, out_positions, position_capacity, out_indices, index_capacity);
}

bool GameServer_ClientLoadSceneSlot(
    KernelHandle* kernel,
    const uint8_t* bundle_bytes,
    uint32_t bundle_size,
    const char* entry_path,
    const char* scene_template,
    uint32_t slot,
    uint32_t flags) {
    namespace gs = network_example::game_server;
    if (kernel == nullptr || bundle_bytes == nullptr || bundle_size == 0u ||
        entry_path == nullptr || scene_template == nullptr ||
        slot == 0u || slot >= gs::kSceneSlotCount) {
        return false;
    }
    try {
        gs::SceneCatalogSource source;
        source.bundle = std::make_shared<const std::vector<std::uint8_t>>(
            bundle_bytes, bundle_bytes + bundle_size);
        source.entry_path = entry_path;
        gs::PreparedScene prepared =
            gs::prepare_scene(source, scene_template, slot, gs::TerrainRole::kClient);
        if (!prepared.ok) {
            std::fprintf(stderr, "GameServer_ClientLoadSceneSlot: %s\n", prepared.error.c_str());
            return false;
        }
        const gs::GameServerGameplayConfig& config = prepared.config;
        if ((flags & GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION) != 0u) {
            KernelStaticSceneSlotConfig scene{};
            scene.struct_size = sizeof(scene);
            scene.slot = slot;
            scene.artifact_bytes = prepared.collision_artifact.data();
            scene.artifact_size = static_cast<std::uint32_t>(prepared.collision_artifact.size());
            scene.scene_id = config.static_collision_scene.scene_id;
            scene.collider_id = config.static_collision_scene.collider_id + slot;
            scene.collision_layer = config.static_collision_scene.collision_layer;
            scene.offset = config.scene_offset;
            if (!Kernel_AddStaticSceneSlot(kernel, &scene)) {
                return false;
            }
        }
        if (config.terrain.enabled) {
            auto view = std::make_shared<TerrainView>();
            view->offset_x = config.scene_offset.x;
            view->heights = std::move(prepared.terrain_heights);
            view->recipe = std::move(prepared.terrain_recipe);
            view->stamp_template_ids = config.terrain.stamp_template_ids;
            for (const auto& stamp : config.terrain.library.stamps) view->stamp_cells.push_back(stamp.cells_x);
            set_terrain_view(kernel, slot, std::move(view));
            set_static_scene_view(kernel, slot, nullptr);
        } else {
            auto view = std::make_shared<StaticSceneView>();
            view->offset_x = config.scene_offset.x;
            view->artifact = std::move(prepared.collision_artifact);
            set_static_scene_view(kernel, slot, std::move(view));
            set_terrain_view(kernel, slot, nullptr);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool GameServer_ClientUnloadSceneSlot(KernelHandle* kernel, uint32_t slot, uint32_t flags) {
    if (kernel == nullptr || slot == 0u || slot >= network_example::game_server::kSceneSlotCount) {
        return false;
    }
    set_terrain_view(kernel, slot, nullptr);
    set_static_scene_view(kernel, slot, nullptr);
    if ((flags & GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION) != 0u) {
        return Kernel_RemoveStaticSceneSlot(kernel, slot);
    }
    return true;
}

}  // extern "C"
