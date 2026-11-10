// The shipped mission, played on the shipped maps.
//
// The mission's coordinates used to be written for a map centred on the origin
// with its ground at y 0, while the map is 216 m of terrain with its ground at
// y 8-38 (and at x, z 0..216 on the composed mesh). Nothing noticed, because
// nothing refuses a spawn point in mid-air: a nest stood buried or off the map,
// and put 9 gingerbread out every 8 s to fall straight to the kill floor. The
// tests that run the mission do it on a flat test plane, so this one runs it on
// the real maps from the bundle, with their navmeshes, as a dedicated server
// would. A unit that reaches the kill floor is what it counts.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_navigation.h"
#include "game_server/src/terrain_catalog.h"
#include "kernel/public/kernel_api.h"

namespace gs = network_example::game_server;

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
// Seven nest intervals: every nest's every wave would have fallen by the end.
constexpr int kRunTicks = 30 * 60;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream file("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::EntityTemplateConfig& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0u;
}

struct Unit {
    KernelVec3 first{};
    KernelVec3 last{};
    float lowest = 1e9f;
    int last_tick = 0;
    float furthest = 0.0f;
};

struct Tally {
    int spawned = 0;
    int fell = 0;
    int alive = 0;
    int left_the_door = 0;
};

Tally tally_of(const std::map<std::uint32_t, Unit>& units, int last_tick, float kill_floor) {
    Tally tally;
    for (const auto& [net_id, unit] : units) {
        ++tally.spawned;
        if (unit.last_tick == last_tick) ++tally.alive;
        // Gone within a few metres of the kill floor: nothing but the fall
        // takes it there. The floor of either map is at y 8.
        if (unit.lowest < kill_floor + 5.0f) ++tally.fell;
        if (unit.furthest > 2.0f) ++tally.left_the_door;
    }
    return tally;
}

void play(
    const std::vector<std::uint8_t>& bundle,
    const char* entry,
    std::uint16_t port) {
    std::printf("== %s\n", entry);
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), entry, "");

    std::vector<std::uint8_t> scene;
    if (config.terrain.enabled) {
        const std::vector<std::uint8_t> cooked = gs::read_cooked_terrain_recipe(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()), config.terrain);
        require(!cooked.empty());
        gs::PreparedTerrain prepared;
        std::string error;
        require(gs::prepare_catalog_terrain(
            &config, cooked, gs::TerrainRole::kServer, &prepared, &error));
        scene = prepared.collision_artifact;
    } else {
        scene = gs::load_gameplay_bundle_entry_bytes(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()),
            config.static_collision_scene.entry_path);
        config.navigation_mesh.artifact = gs::load_gameplay_bundle_entry_bytes(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()),
            config.navigation_mesh.entry_path);
    }
    require(!scene.empty());
    require(!config.navigation_mesh.artifact.empty());

    gs::PatrolNavigation navigation;
    std::string error;
    require(navigation.load(config.navigation_mesh.artifact, &error));

    // Every point the mission names: on the ground, and on the same connected
    // ground as the player. A route the giants walk is checked point by point.
    int off_the_ground = 0;
    const auto check_point = [&](const char* what, std::uint32_t node_id, const KernelVec3& point) {
        KernelVec3 ground{};
        const bool snapped = navigation.snap(point, 2.0f, &ground);
        gs::PatrolRouteRequest request;
        request.start = config.player.spawn_position;
        request.end = point;
        request.snap_half_extent_meters = 2.0f;
        std::vector<KernelVec3> route;
        const bool connected = navigation.find_route(request, &route);
        const bool on_ground = snapped && std::abs(ground.y - point.y) < 1.0f;
        std::printf(
            "  node %u %-5s (%.1f, %.2f, %.1f) navmesh y %.2f connected %d\n",
            node_id, what, point.x, point.y, point.z, snapped ? ground.y : -1.0f, connected ? 1 : 0);
        if (!on_ground || !connected) ++off_the_ground;
    };
    // Only the mission this map plays: the catalog carries every map's.
    int missions = 0;
    for (const gs::GameRuleConfig& rule : config.game_rules) {
        if (std::find(
                config.preload_director_template_ids.begin(),
                config.preload_director_template_ids.end(),
                rule.director_template_id) == config.preload_director_template_ids.end()) {
            continue;
        }
        ++missions;
        for (const gs::GameRuleNodeConfig& node : rule.nodes) {
            if (!node.has_spawn_effect) continue;
            check_point("spawn", node.node_id, node.spawn.position);
            for (const KernelVec3& waypoint : node.spawn.controller.route) {
                check_point("route", node.node_id, waypoint);
            }
        }
    }
    require(missions == 1);

    const std::uint32_t gingerbread = template_id_of(config, "gingerbread");
    const std::uint32_t giant = template_id_of(config, "gingerbread_giant");
    const std::uint32_t nest = template_id_of(config, "gingerbread_nest");
    require(gingerbread != 0u && giant != 0u && nest != 0u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartListenServer(kernel, port));
    gs::GameServer game_server(kernel, config);
    require(game_server.preload_directors());

    std::array<KernelEvent, 4096> events{};
    std::vector<KernelServerEntityState> states(1024);
    std::map<std::uint32_t, Unit> gingerbreads;
    std::map<std::uint32_t, Unit> giants;
    std::map<std::uint32_t, KernelVec3> nests;

    const auto note = [](std::map<std::uint32_t, Unit>* units, const KernelServerEntityState& state, int tick) {
        auto [it, fresh] = units->try_emplace(state.net_id);
        Unit& unit = it->second;
        if (fresh) unit.first = state.position;
        unit.last = state.position;
        unit.last_tick = tick;
        unit.lowest = std::min(unit.lowest, state.position.y);
        unit.furthest = std::max(
            unit.furthest,
            std::hypot(state.position.x - unit.first.x, state.position.z - unit.first.z));
    };

    for (int tick = 1; tick <= kRunTicks; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t event_count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            game_server.handle_event(events[index]);
        }
        game_server.tick(kTickSeconds);

        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = std::min<std::uint32_t>(
            Kernel_ServerQueryEntities(
                kernel, 0u, states.data(), static_cast<std::uint32_t>(states.size())),
            static_cast<std::uint32_t>(states.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelServerEntityState& state = states[index];
            if (state.actor_template_id == gingerbread) {
                note(&gingerbreads, state, tick);
            } else if (state.actor_template_id == giant) {
                note(&giants, state, tick);
            }
        }
        const std::uint32_t prop_count = std::min<std::uint32_t>(
            Kernel_ServerQueryEntities(
                kernel, KernelEntityType_Prop, states.data(), static_cast<std::uint32_t>(states.size())),
            static_cast<std::uint32_t>(states.size()));
        for (std::uint32_t index = 0; index < prop_count; ++index) {
            if (states[index].entity_template_id == nest) {
                nests[states[index].net_id] = states[index].position;
            }
        }
    }
    Kernel_Destroy(kernel);

    const float kill_floor = config.player.fall_death_height_meters;
    const Tally small = tally_of(gingerbreads, kRunTicks, kill_floor);
    const Tally big = tally_of(giants, kRunTicks, kill_floor);
    std::printf(
        "  %d s: gingerbread %d seen, %d fell, %d alive, %d left their door; "
        "giants %d seen, %d fell, %d alive, %d walked; nests %zu; off the ground %d\n",
        kRunTicks / 30, small.spawned, small.fell, small.alive, small.left_the_door,
        big.spawned, big.fell, big.alive, big.left_the_door, nests.size(), off_the_ground);
    for (const auto& [net_id, where] : nests) {
        std::printf("  nest #%u at (%.1f, %.2f, %.1f)\n", net_id, where.x, where.y, where.z);
    }
    for (const auto& [net_id, unit] : giants) {
        std::printf(
            "  giant #%u (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), furthest %.1f m\n",
            net_id, unit.first.x, unit.first.y, unit.first.z,
            unit.last.x, unit.last.y, unit.last.z, unit.furthest);
    }

    // The case is there: the mission ran and its nests put units out.
    require(nests.size() == 3u);
    require(small.spawned >= 6);
    require(small.left_the_door >= 6);
    require(big.spawned >= 4);
    require(big.left_the_door >= 4);
    for (const auto& [net_id, unit] : giants) {
        require(unit.furthest > 10.0f);
    }
    // And nothing it put out fell off the world.
    require(off_the_ground == 0);
    require(small.fell == 0);
    require(big.fell == 0);
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    play(bundle, "gameplay_catalog.scene_5.yaml", 8072);
    play(bundle, "gameplay_catalog.scene_6.yaml", 8073);
    std::printf("mission_on_map_test passed\n");
    return 0;
}
