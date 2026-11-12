// finish_scene_placement (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G1/G2): grounded
// scene props land on the scene's ground, and map_edge_barrier lays player
// barriers along the map's rectangle. Then the shipping camp end to end
// through the public API, the way the Unity host loads it: the host's player
// walks from its spawn four ways and never leaves the map, while the same
// walks with player_barrier struck from its movement mask -- what every agent
// has -- carry it over an edge. That control is what shows the walls, not the
// ground, did the stopping.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/src/kernel/public/kernel_api.h"
#include "game_server/public/game_server_api.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_placement.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_placement_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

// A 12 m square at the origin's corner, flat at y = 0: two segments a side.
void flat_square_edges() {
    std::vector<KernelVec3> ground;
    for (int i = 0; i <= 24; ++i) {
        for (int j = 0; j <= 24; ++j) {
            ground.push_back(KernelVec3{i * 0.5f, 0.0f, j * 0.5f});
        }
    }
    gs::MapEdgeBarrierConfig rule;
    rule.enabled = true;
    const std::vector<KernelPlayerBarrierBox> walls =
        gs::map_edge_barriers(rule, 0.0f, 0.0f, 12.0f, 12.0f, ground);
    require(walls.size() == 8u);
    // The first is the south edge's west half: its outer face on z = 0, from
    // 1 m under the ground to 4 m over it, reaching a thickness past its ends.
    const KernelPlayerBarrierBox& south = walls.front();
    require(near(south.center.x, 3.0f) && near(south.center.z, 0.5f));
    require(near(south.center.y, 1.5f) && near(south.half_extents.y, 2.5f));
    require(near(south.half_extents.x, 4.0f) && near(south.half_extents.z, 0.5f));
    for (const KernelPlayerBarrierBox& wall : walls) {
        // Every wall stands inside the square, never outside it.
        require(wall.center.x - wall.half_extents.x >= -1.0f - 1e-4f);
        require(wall.center.x + wall.half_extents.x <= 13.0f + 1e-4f);
        require(wall.yaw_radians == 0.0f);
    }

    // Disabled, or no ground: nothing.
    rule.enabled = false;
    require(gs::map_edge_barriers(rule, 0.0f, 0.0f, 12.0f, 12.0f, ground).empty());
    rule.enabled = true;
    require(gs::map_edge_barriers(rule, 0.0f, 0.0f, 12.0f, 12.0f, {}).empty());

    // A map too big for 6 m segments grows them to stay inside a slot's limit.
    std::vector<KernelVec3> rim;
    for (float a = 0.0f; a <= 2000.0f; a += 10.0f) {
        rim.push_back(KernelVec3{a, 0.0f, 0.0f});
        rim.push_back(KernelVec3{a, 0.0f, 2000.0f});
        rim.push_back(KernelVec3{0.0f, 0.0f, a});
        rim.push_back(KernelVec3{2000.0f, 0.0f, a});
    }
    const std::vector<KernelPlayerBarrierBox> big =
        gs::map_edge_barriers(rule, 0.0f, 0.0f, 2000.0f, 2000.0f, rim);
    require(!big.empty());
    require(big.size() <= KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT);
    std::printf("edges: 8 walls on a flat 12 m square; a 2 km map keeps to %zu\n", big.size());
}

// A terrain's heights: grounded props take the ground's y, a prop off the map
// is refused, and the edge walls follow the map's own rectangle.
void terrain_ground() {
    gs::terrain::HeightGrid heights;
    heights.samples_x = 41;  // 12 m
    heights.samples_z = 41;
    heights.cm.assign(41u * 41u, 1000);  // 10 m

    gs::GameServerGameplayConfig config;
    gs::ScenePropConfig on_map;
    on_map.position = KernelVec3{1.0f, 50.0f, -2.0f};
    on_map.grounded = true;
    gs::ScenePropConfig authored;
    authored.position = KernelVec3{1.0f, 33.0f, 1.0f};
    config.scene_props = {on_map, authored};
    config.map_edge_barrier.enabled = true;
    std::string error;
    require(gs::finish_scene_placement(&config, gs::SceneGround{&heights, {}}, &error));
    require(near(config.scene_props[0].position.y, 10.0f));
    // Not grounded: left exactly as authored.
    require(config.scene_props[1].position.y == 33.0f);
    require(config.resolved_player_barriers.size() == 8u);
    for (const KernelPlayerBarrierBox& wall : config.resolved_player_barriers) {
        require(near(wall.center.y - wall.half_extents.y, 9.0f));
        require(near(wall.center.y + wall.half_extents.y, 14.0f));
    }

    gs::GameServerGameplayConfig off_map;
    gs::ScenePropConfig outside;
    outside.position = KernelVec3{40.0f, 10.0f, 0.0f};
    outside.grounded = true;
    off_map.scene_props = {outside};
    require(!gs::finish_scene_placement(&off_map, gs::SceneGround{&heights, {}}, &error));
    require(error.find("no ground") != std::string::npos);

    gs::GameServerGameplayConfig crowded;
    crowded.player_barriers.assign(
        KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT + 1u,
        KernelPlayerBarrierBox{KernelVec3{0, 0, 0}, KernelVec3{1, 1, 1}, 0.0f});
    require(!gs::finish_scene_placement(&crowded, gs::SceneGround{&heights, {}}, &error));
    require(error.find("at most") != std::string::npos);

    gs::GameServerGameplayConfig groundless;
    groundless.map_edge_barrier.enabled = true;
    require(!gs::finish_scene_placement(&groundless, gs::SceneGround{}, &error));
    std::printf("terrain: grounded props land, off-map and over-full scenes are refused\n");
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------ the camp

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr float kDt = 1.0f / 30.0f;

struct Host {
    KernelHandle* kernel = nullptr;
    GameServerHandle* game = nullptr;
    std::uint32_t player = 0;
    std::uint32_t sequence = 1;

    void frame(KernelVec2 move) {
        KernelPlayerInput input{};
        input.input_seq = sequence++;
        input.move = move;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        Kernel_SubmitPlayerInput(kernel, 1, &input);
        Kernel_Update(kernel, kDt);
        KernelEvent events[64];
        std::uint32_t count = 0;
        while ((count = Kernel_PollEvents(kernel, events, 64u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) GameServer_HandleEvent(game, &events[i]);
        }
        GameServer_Tick(game, kDt);
    }

    KernelVec3 position() const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, player, &state));
        return state.position;
    }
};

// Where the walks from the camp's spawn ended. The camp is exported from
// unity-terrain-example and its shape changes with every re-authoring, so
// nothing here knows where its edge is: the player walks from wherever it
// spawned, one way after another, until it stops (a wall, a prop) or drops
// off the map.
struct Walks {
    int fell = 0;
    int held = 0;
};

Walks walk_from_spawn(const std::vector<std::uint8_t>& bundle, std::uint16_t port, bool strike_barrier) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 512;
    Host host;
    host.kernel = Kernel_Create(&kernel_config);
    require(host.kernel != nullptr);
    KernelGameplayCatalogLoadResult result{};
    host.game = GameServer_CreateWithGameplayCatalogFromMemory(
        host.kernel, bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry, &result);
    if (host.game == nullptr) std::fprintf(stderr, "load: %s\n", result.diagnostic);
    require(host.game != nullptr);
    require(Kernel_StartListenServer(host.kernel, port));
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(host.kernel, &local));
    host.player = local.player_net_id;
    require(host.player != 0u);
    // Struck once the player has joined: joining sets the template's mask.
    for (int i = 0; i < 60; ++i) host.frame(KernelVec2{0.0f, 0.0f});
    if (strike_barrier) {
        require(Kernel_ServerSetEntityMovementCollisionMask(
            host.kernel, host.player,
            KERNEL_MOVEMENT_MASK_DEFAULT | KERNEL_MOVEMENT_LAYER_LIMB));
    }
    const KernelVec3 spawn = host.position();
    // Grounded control: it is standing on the map, not falling off it.
    require(spawn.y > -5.0f);

    Walks walks;
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    const KernelVec2 ways[4] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
    for (const KernelVec2& way : ways) {
        const KernelVec3 back{spawn.x, spawn.y + 0.5f, spawn.z};
        require(Kernel_ServerSetEntityTransform(host.kernel, host.player, &back, &facing));
        for (int i = 0; i < 30; ++i) host.frame(KernelVec2{0.0f, 0.0f});
        const KernelVec3 start = host.position();
        KernelVec3 last = start;
        bool fell = false;
        int still = 0;
        // 60 s: far past any wall of a map a camp could be; a walk pressed
        // along a slanted wall slides until it settles in a corner.
        for (int i = 0; i < 1800 && still < 15; ++i) {
            host.frame(way);
            const KernelVec3 at = host.position();
            if (at.y < start.y - 3.0f) {
                fell = true;
                break;
            }
            const float moved = std::hypot(at.x - last.x, at.z - last.z);
            still = moved < 0.005f ? still + 1 : 0;
            last = at;
        }
        std::printf("%s: walk (%.0f, %.0f) from (%.2f, %.2f) %s at (%.2f, %.2f, %.2f)\n",
            strike_barrier ? "no barrier layer" : "player mask", way.x, way.y, start.x, start.z,
            fell ? "fell" : "held", last.x, last.y, last.z);
        ++(fell ? walks.fell : walks.held);
    }
    GameServer_Destroy(host.game);
    Kernel_Destroy(host.kernel);
    return walks;
}

void camp_edge() {
    const std::vector<std::uint8_t> bundle = read_file("game_server/gameplay_catalog_bundle/bundle.zip");
    // Kernel tests bind real ports; 8246-8247 are unused elsewhere (test-port-collisions).
    const Walks walled = walk_from_spawn(bundle, 8246, false);
    const Walks open = walk_from_spawn(bundle, 8247, true);
    // Whichever way the player walks, the camp's edge walls keep it on.
    require(walled.fell == 0);
    // Control: without the barrier layer some walk goes off the map, so the
    // walks do reach an edge and a fall is seen.
    require(open.fell > 0);
    std::printf("camp: the edge walls hold the player on all 4 walks; without the layer %d of 4 fall off\n",
        open.fell);
}

}  // namespace

int main() {
    flat_square_edges();
    terrain_ground();
    camp_edge();
    std::printf("scene_placement_test passed\n");
    return 0;
}
