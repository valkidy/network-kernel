// The terrain level end to end through the catalog (implementation plan P3):
// the bundle's gameplay_catalog.scene_6.yaml (scene template 6, the kernel terrain) with its
// cooked recipe loads, the
// client and server prepare paths compose the recorded map, a Unity Host
// (GameServer_CreateWithGameplayCatalogFromMemory) and a pure client
// (Kernel_LoadGameplayCatalogFromMemory) both accept it, and every way a bundle
// could disagree with itself is refused rather than composed into another map.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "engine/src/kernel/public/kernel_api.h"
#include "game_server/public/game_server_api.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_navigation.h"
#include "game_server/src/terrain/terrain_navmesh.h"
#include "game_server/src/terrain_catalog.h"

namespace {

namespace gs = network_example::game_server;
namespace terrain = network_example::game_server::terrain;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

// The shipping catalog starting on scene template 6, the kernel terrain, as
// the entry the bundle build derives for it (gameplay_catalog.yaml with
// start_scene set; docs/START_SCENE_PLAN.md).
constexpr const char* kEntry = "gameplay_catalog.scene_6.yaml";
constexpr const char* kRecipe = "terrain/gameplay_catalog.nktr";
constexpr const char* kTerrainTemplate = "scene_templates/6_scene_kernel_terrain_test.yaml";

std::vector<std::uint8_t> read_bundle(const char* file = "game_server/gameplay_catalog_bundle/bundle.zip") {
    std::ifstream s(file, std::ios::binary);
    require(s.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
}

std::string entry_text(const std::vector<std::uint8_t>& bundle, const std::string& path) {
    const std::vector<std::uint8_t> b = gs::load_gameplay_bundle_entry_bytes(bundle.data(), static_cast<std::uint32_t>(bundle.size()), path);
    return std::string(b.begin(), b.end());
}

gs::GameServerGameplayConfig load(const std::vector<std::uint8_t>& bundle, const char* entry = kEntry) {
    return gs::load_gameplay_config_from_bundle_memory(bundle.data(), static_cast<std::uint32_t>(bundle.size()), entry);
}

// The catalog from a directory: the edited yaml next to links to the real
// catalog's folders (its paths are relative to the catalog, like the bundle's).
// The catalog from a directory: `yaml` as its root and `terrain_template` as
// scene template 6, the rest the shipped files.
bool loads(const std::string& yaml, const std::string& terrain_template, std::string* why) {
    namespace fs = std::filesystem;
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = fs::path(tmp != nullptr ? tmp : "/tmp") / "terrain_catalog";
    static bool linked = false;
    const fs::path shipped = fs::absolute("game_server/gameplay_catalog");
    if (!linked) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        for (const fs::directory_entry& e : fs::directory_iterator(shipped)) {
            if (e.is_directory() && e.path().filename() != "scene_templates") {
                fs::create_directory_symlink(e.path(), dir / e.path().filename());
            }
        }
        linked = true;
    }
    fs::remove_all(dir / "scene_templates");
    fs::create_directories(dir / "scene_templates");
    for (const fs::directory_entry& e : fs::directory_iterator(shipped / "scene_templates")) {
        fs::copy_file(e.path(), dir / "scene_templates" / e.path().filename());
    }
    { std::ofstream(dir / kTerrainTemplate, std::ios::trunc) << terrain_template; }
    { std::ofstream(dir / "terrain.yaml") << yaml; }
    try {
        gs::load_gameplay_config_from_catalog_file((dir / "terrain.yaml").string());
        return true;
    } catch (const std::exception& e) {
        *why = e.what();
        return false;
    }
}

gs::PreparedTerrain config_and_prepare(const std::vector<std::uint8_t>& bundle) {
    gs::GameServerGameplayConfig config = load(bundle);
    const gs::TerrainCatalogConfig& t = config.terrain;
    require(t.enabled && t.recipe_entry_path == kRecipe);
    require(t.library.stamps.size() == 4 && t.library.ground_maps.size() == 1);
    require(t.stamp_template_ids == (std::vector<std::uint32_t>{1, 2, 3, 4}));
    require(t.base.cells_x == 720 && t.base.cells_z == 720);
    require(config.static_collision_scene.entry_path.empty() && config.static_collision_scene.scene_id == 1);
    require(config.navigation_mesh.entry_path.empty() && config.navigation_mesh.artifact.empty());
    const gs::PlayableBoundsConfig& b = config.map.playable_bounds;
    require(b.authored && b.min_x == -108.0f && b.max_x == 108.0f && b.min_z == -108.0f && b.max_z == 108.0f);

    const std::vector<std::uint8_t> cooked = gs::read_cooked_terrain_recipe(bundle.data(), static_cast<std::uint32_t>(bundle.size()), t);
    require(!cooked.empty());
    std::string error;

    // Client: compose the recorded map.
    gs::GameServerGameplayConfig client = config;
    gs::PreparedTerrain pc;
    require(gs::prepare_catalog_terrain(&client, cooked, gs::TerrainRole::kClient, &pc, &error));
    require(!pc.generated && !pc.collision_artifact.empty());
    require(terrain::height_grid_hash(pc.heights) == pc.recipe.heights_hash);
    require(client.navigation_mesh.artifact.empty());
    std::printf("cooked recipe: seed %d, %zu placements, heights %016llx, collision %zu bytes\n", pc.recipe.recipe.seed,
        pc.recipe.recipe.placements.size(), static_cast<unsigned long long>(pc.recipe.heights_hash), pc.collision_artifact.size());

    // Server: the same map, plus a navmesh game_server can route on.
    gs::GameServerGameplayConfig server = config;
    gs::PreparedTerrain ps;
    require(gs::prepare_catalog_terrain(&server, cooked, gs::TerrainRole::kServer, &ps, &error));
    require(ps.recipe.heights_hash == pc.recipe.heights_hash && ps.collision_artifact == pc.collision_artifact);
    require(terrain::is_navmesh_tiles(server.navigation_mesh.artifact));
    gs::PatrolNavigation nav;
    require(nav.load(server.navigation_mesh.artifact, &error));
    gs::PatrolRouteRequest route;
    route.start = KernelVec3{-95.0f, 30.0f, -95.0f};
    route.end = KernelVec3{95.0f, 30.0f, 95.0f};
    route.snap_half_extent_meters = 20.0f;
    std::vector<KernelVec3> waypoints;
    require(nav.find_route(route, &waypoints) && !waypoints.empty());

    // Refusals.
    gs::GameServerGameplayConfig c2 = config;
    gs::PreparedTerrain p2;
    require(!gs::prepare_catalog_terrain(&c2, {}, gs::TerrainRole::kClient, &p2, &error));
    require(error.find("no cooked terrain recipe") != std::string::npos);

    terrain::StoredRecipe tampered;
    require(terrain::parse_recipe(cooked, &tampered, &error));
    tampered.recipe.placements[0].base_cm += 150;
    c2 = config;
    require(!gs::prepare_catalog_terrain(&c2, terrain::write_recipe(tampered), gs::TerrainRole::kClient, &p2, &error));
    require(error.find("composes to") != std::string::npos);
    std::printf("tampered recipe refused: %s\n", error.c_str());

    c2 = config;
    c2.terrain.library.stamps[0].cm[100] += 1;
    require(!gs::prepare_catalog_terrain(&c2, cooked, gs::TerrainRole::kClient, &p2, &error));
    require(error.find("different stamp") != std::string::npos);
    std::printf("other library refused: %s\n", error.c_str());

    // The default entry starts on the initial camp, its mesh ground:
    // the baked mesh and its navmesh, no terrain. Template 6's entry is the
    // terrain; template 5's is the mesh again.
    const gs::GameServerGameplayConfig shipping = load(bundle, "gameplay_catalog.yaml");
    require(!shipping.terrain.enabled && !shipping.static_collision_scene.entry_path.empty());
    require(!shipping.navigation_mesh.entry_path.empty() && !shipping.map.playable_bounds.authored);
    require(shipping.player.has_spawn_position && shipping.player.actor_template_id != 0);
    require(load(bundle, "gameplay_catalog.scene_6.yaml").terrain.enabled);
    require(!load(bundle, "gameplay_catalog.scene_5.yaml").terrain.enabled);
    return pc;
}

KernelConfig kernel_config(KernelMode mode) {
    KernelConfig config{};
    config.mode = mode;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

// Kernel tests bind real ports; 8053 is unused elsewhere (test-port-collisions).
constexpr std::uint16_t kPort = 8053;

bool pump_until(KernelHandle* server, KernelHandle* client, KernelGameplayCatalogSyncState want) {
    for (int i = 0; i < 2000; ++i) {
        Kernel_Update(server, 1.0f / 60.0f);
        Kernel_Update(client, 1.0f / 60.0f);
        KernelGameplayCatalogSyncStatus status{};
        status.struct_size = sizeof(status);
        if (!Kernel_GetGameplayCatalogSyncStatus(client, &status) || status.state == KernelGameplayCatalogSyncState_Failed ||
            status.state == KernelGameplayCatalogSyncState_Disconnected) {
            return false;
        }
        if (status.state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// The real flow: a Unity Host (game server + listen server, catalog from the
// bundle) serves the bundle; a pure client syncs its manifest, then loads the
// catalog -- composing the map -- before Welcome, as NetworkClient does.
// The client kernel after the load (still alive), or nullptr when the load failed.
KernelHandle* client_after_load(KernelHandle* host, const std::vector<std::uint8_t>& client_bundle, KernelGameplayCatalogLoadResult* result) {
    KernelConfig client_config = kernel_config(KernelMode_Client);
    KernelHandle* client = Kernel_Create(&client_config);
    require(client != nullptr);
    KernelGameplayCatalogSyncClientConfig sync{};
    sync.struct_size = sizeof(sync);
    sync.max_bundle_size = KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE;
    sync.timeout_ms = 5000u;
    require(Kernel_StartClientCatalogSync(client, ("127.0.0.1:" + std::to_string(kPort)).c_str(), &sync));
    require(pump_until(host, client, KernelGameplayCatalogSyncState_ManifestReady));
    *result = KernelGameplayCatalogLoadResult{};
    const bool loaded = Kernel_LoadGameplayCatalogFromMemory(
        client, client_bundle.data(), static_cast<std::uint32_t>(client_bundle.size()), kEntry, result);
    if (!loaded) {
        Kernel_Destroy(client);
        return nullptr;
    }
    return client;
}

// What a client draws from: info, a rectangle of heights, the placements.
struct Drawn {
    GameServerTerrainInfo info{};
    std::vector<std::int16_t> heights;
    std::vector<GameServerTerrainPlacement> placements;
};

Drawn query_terrain(KernelHandle* kernel) {
    Drawn d;
    d.info.struct_size = sizeof(d.info);
    require(GameServer_GetTerrainInfo(kernel, &d.info));
    d.heights.resize(40 * 30);
    require(GameServer_CopyTerrainHeights(kernel, 300, 200, 40, 30, d.heights.data(), static_cast<std::uint32_t>(d.heights.size())));
    const std::uint32_t n = GameServer_CopyTerrainPlacements(kernel, nullptr, 0);
    d.placements.resize(n);
    require(GameServer_CopyTerrainPlacements(kernel, d.placements.data(), n) == n);
    return d;
}

void c_api_paths(const std::vector<std::uint8_t>& bundle, const gs::PreparedTerrain& expected) {
    KernelConfig host_config = kernel_config(KernelMode_ListenServer);
    KernelHandle* host = Kernel_Create(&host_config);
    require(host != nullptr);
    KernelGameplayCatalogLoadResult result{};
    GameServerHandle* game_server = GameServer_CreateWithGameplayCatalogFromMemory(
        host, bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry, &result);
    if (game_server == nullptr) std::fprintf(stderr, "host load: %s\n", result.diagnostic);
    require(game_server != nullptr);
    KernelGameplayCatalogSyncServerConfig serve{};
    serve.struct_size = sizeof(serve);
    serve.bundle_bytes = bundle.data();
    serve.bundle_size = static_cast<std::uint32_t>(bundle.size());
    serve.entry_path = kEntry;
    serve.content_namespace = "terrain_catalog_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(host, &serve, &manifest));
    require(Kernel_StartListenServer(host, kPort));
    for (int k = 0; k < 30; ++k) GameServer_Tick(game_server, 1.0f / 30.0f);

    KernelHandle* client = client_after_load(host, bundle, &result);
    if (client == nullptr) std::fprintf(stderr, "client load: %s\n", result.diagnostic);
    require(client != nullptr);
    std::printf("host + client loaded the terrain level\n");
    GameServerStaticSceneMeshInfo no_mesh{};
    no_mesh.struct_size = sizeof(no_mesh);
    require(!GameServer_GetStaticSceneMeshInfo(client, &no_mesh) && !GameServer_GetStaticSceneMeshInfo(host, &no_mesh));

    // Terrain queries (P5): the Host and the client see the same map, and it
    // is the map the catalog path composes.
    GameServerAbiInfo abi{};
    require(GameServer_GetAbiInfo(&abi, sizeof(abi)));
    require((abi.capability_flags & GAME_SERVER_CAPABILITY_TERRAIN_QUERY) != 0);
    const Drawn h = query_terrain(host), c = query_terrain(client);
    require(h.info.heights_hash == expected.recipe.heights_hash && c.info.heights_hash == h.info.heights_hash);
    require(c.info.samples_x == 721 && c.info.samples_z == 721 && c.info.cell_mm == 300);
    require(c.info.origin_x == -108.0f && c.info.origin_z == -108.0f && c.info.seed == expected.recipe.recipe.seed);
    require(c.heights == h.heights);
    for (int j = 0; j < 30; ++j)
        for (int i = 0; i < 40; ++i) require(c.heights[static_cast<std::size_t>(j) * 40 + i] == expected.heights.at(300 + i, 200 + j));
    require(c.placements.size() == expected.recipe.recipe.placements.size() && c.placements.size() == c.info.placement_count);
    for (std::size_t k = 0; k < c.placements.size(); ++k) {
        const GameServerTerrainPlacement& p = c.placements[k];
        const auto& r = expected.recipe.recipe.placements[k];
        require(p.stamp_template_id >= 1 && p.stamp_template_id <= 4 && p.turns == r.turns && p.mirror == (r.mirror ? 1 : 0));
        require(std::fabs(p.min_x - (-108.0f + r.x_cells * 0.3f)) < 1e-3f && std::fabs(p.base_y - r.base_cm / 100.0f) < 1e-4f);
        require(p.min_x >= -108.0f && p.min_x + p.size_x <= 108.0f);
    }
    std::int16_t one = 0;
    require(!GameServer_CopyTerrainHeights(client, 720, 0, 2, 1, &one, 2));      // past the edge
    std::printf("terrain queries: %u x %u samples, %zu placements, heights %016llx on host and client\n", c.info.samples_x,
        c.info.samples_z, c.placements.size(), static_cast<unsigned long long>(c.info.heights_hash));
    GameServer_ReleaseTerrain(client);
    GameServerTerrainInfo gone{};
    gone.struct_size = sizeof(gone);
    require(!GameServer_GetTerrainInfo(client, &gone));
    require(GameServer_CopyTerrainPlacements(client, nullptr, 0) == 0);
    Kernel_Destroy(client);

    // A bundle without the recipe (bundle_uncooked, the cook's input): a client cannot know the map.
    const std::vector<std::uint8_t> bare = read_bundle("game_server/gameplay_catalog_bundle/bundle_uncooked.zip");
    require(client_after_load(host, bare, &result) == nullptr);
    require(result.error_code == KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY);
    std::printf("bundle without recipe refused: %s\n", result.diagnostic);

    GameServer_ReleaseTerrain(host);
    GameServer_Destroy(game_server);
    Kernel_Destroy(host);
}

// The mesh map (the default entry, composed_mesh): its baked scene comes back
// as triangles for the client to draw; a terrain level has none.
void static_scene_mesh_paths(const std::vector<std::uint8_t>& bundle) {
    GameServerAbiInfo abi{};
    require(GameServer_GetAbiInfo(&abi, sizeof(abi)));
    require((abi.capability_flags & GAME_SERVER_CAPABILITY_STATIC_SCENE_MESH_QUERY) != 0);
    KernelConfig host_config = kernel_config(KernelMode_ListenServer);
    KernelHandle* host = Kernel_Create(&host_config);
    require(host != nullptr);
    KernelGameplayCatalogLoadResult result{};
    GameServerHandle* game_server = GameServer_CreateWithGameplayCatalogFromMemory(
        host, bundle.data(), static_cast<std::uint32_t>(bundle.size()), "gameplay_catalog.scene_5.yaml", &result);
    if (game_server == nullptr) std::fprintf(stderr, "mesh host load: %s\n", result.diagnostic);
    require(game_server != nullptr);
    GameServerTerrainInfo no_terrain{};
    no_terrain.struct_size = sizeof(no_terrain);
    require(!GameServer_GetTerrainInfo(host, &no_terrain));

    GameServerStaticSceneMeshInfo info{};
    info.struct_size = sizeof(info);
    require(GameServer_GetStaticSceneMeshInfo(host, &info));
    require(info.vertex_count >= 3u && info.triangle_count >= 1u);
    // composed_216m_ground_0001_seed1_collision: 0..216 m across, 8-38 m up.
    require(std::fabs(info.min_x) < 0.5f && std::fabs(info.max_x - 216.0f) < 0.5f);
    require(std::fabs(info.min_z) < 0.5f && std::fabs(info.max_z - 216.0f) < 0.5f);
    require(info.min_y > 7.0f && info.max_y < 40.0f);
    std::vector<float> positions(static_cast<std::size_t>(info.vertex_count) * 3u);
    std::vector<std::uint32_t> indices(static_cast<std::size_t>(info.triangle_count) * 3u);
    require(!GameServer_CopyStaticSceneMesh(host, positions.data(), static_cast<std::uint32_t>(positions.size() - 1),
        indices.data(), static_cast<std::uint32_t>(indices.size())));
    require(GameServer_CopyStaticSceneMesh(host, positions.data(), static_cast<std::uint32_t>(positions.size()),
        indices.data(), static_cast<std::uint32_t>(indices.size())));
    // A ground: almost every triangle faces up in the order given.
    std::size_t up = 0;
    for (std::size_t t = 0; t < indices.size(); t += 3) {
        require(indices[t] < info.vertex_count && indices[t + 1] < info.vertex_count && indices[t + 2] < info.vertex_count);
        const float* a = &positions[indices[t] * 3u];
        const float* b = &positions[indices[t + 1] * 3u];
        const float* c = &positions[indices[t + 2] * 3u];
        const float ux = b[0] - a[0], uz = b[2] - a[2], vx = c[0] - a[0], vz = c[2] - a[2];
        if (uz * vx - ux * vz > 0.0f) ++up;   // y of cross(b - a, c - a)
    }
    std::printf("static scene mesh: %u vertices, %u triangles, %zu face up, y %.2f..%.2f\n", info.vertex_count,
        info.triangle_count, up, info.min_y, info.max_y);
    require(up * 100 >= static_cast<std::size_t>(info.triangle_count) * 95);
    GameServer_ReleaseTerrain(host);
    require(!GameServer_GetStaticSceneMeshInfo(host, &info));
    GameServer_Destroy(game_server);
    Kernel_Destroy(host);
}

void catalog_rules(const std::vector<std::uint8_t>& bundle) {
    const std::string yaml = entry_text(bundle, kEntry);
    const std::string terrain = entry_text(bundle, kTerrainTemplate);
    const auto replaced = [](const std::string& text, const std::string& from, const std::string& to) {
        const std::size_t at = text.find(from);
        require(at != std::string::npos);
        std::string edited = text;
        edited.replace(at, from.size(), to);
        return edited;
    };
    // An edit to the root, and one to the terrain's own template.
    const auto root = [&](const std::string& from, const std::string& to) { return loads(replaced(yaml, from, to), terrain, nullptr); };
    std::string why;
    const auto root_why = [&](const std::string& from, const std::string& to) { return loads(replaced(yaml, from, to), terrain, &why); };
    const auto map_why = [&](const std::string& from, const std::string& to) { return loads(yaml, replaced(terrain, from, to), &why); };
    (void)root;
    require(loads(yaml, terrain, &why));            // the unedited copy loads from a directory too
    require(!root_why("\nstatic_collision_scene:\n", "\nnavigation_mesh:\n  entry_path: x.navmesh\nstatic_collision_scene:\n"));
    require(why.find("exclusive") != std::string::npos);
    require(!root_why("\nstatic_collision_scene:\n  scene_id: 1\n", "\nstatic_collision_scene:\n  entry_path: x.joltmesh\n  scene_id: 1\n"));
    require(why.find("no entry_path with terrain") != std::string::npos);
    require(!map_why("map_size: {x: 216.0, z: 216.0}", "map_size: {x: 200.0, z: 216.0}"));
    require(why.find("multiple of 12") != std::string::npos);
    require(!root_why("\nstatic_collision_scene:\n", "\nmap:\n  playable_bounds: {min: {x: -120.0, z: -100.0}, max: {x: 100.0, z: 100.0}}\nstatic_collision_scene:\n"));
    require(why.find("outside the terrain") != std::string::npos);
    require(root_why("\nstatic_collision_scene:\n", "\nmap:\n  playable_bounds: {min: {x: -90.0, z: -100.0}, max: {x: 100.0, z: 100.0}}\nstatic_collision_scene:\n"));
    require(!map_why("stamps: {count: 12, min_count: 10}", "stamps: {count: 12, pool: [9]}"));
    require(why.find("no template") != std::string::npos);
    std::printf("catalog rules: all refused as expected\n");
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const gs::PreparedTerrain expected = config_and_prepare(bundle);
    c_api_paths(bundle, expected);
    static_scene_mesh_paths(bundle);
    catalog_rules(bundle);
    std::printf("terrain_catalog_test passed\n");
    return 0;
}
