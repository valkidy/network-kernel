// finish_scene_placement (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G1/G2): grounded
// scene props land on the scene's ground, and map_edge_barrier lays player
// barriers along the map's rectangle. Then the shipping camp end to end
// through the public API, the way the Unity host loads it: the host's player
// walks at the map's east edge and is stopped there, while the same walk with
// player_barrier struck from its movement mask -- what every agent has --
// carries it over the edge. That control is what shows the wall, not the
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
// The composed mesh runs 0..216 on x; the east wall's inner face is 1 m in.
constexpr float kEastEdge = 216.0f;

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

// Drops the host's player 3 m in from the east edge, lets it land, walks it
// east for 3 s, and reports where it ended.
KernelVec3 walk_east(const std::vector<std::uint8_t>& bundle, std::uint16_t port, bool strike_barrier) {
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
    for (int i = 0; i < 10; ++i) host.frame(KernelVec2{0.0f, 0.0f});

    if (strike_barrier) {
        require(Kernel_ServerSetEntityMovementCollisionMask(
            host.kernel, host.player,
            KERNEL_MOVEMENT_MASK_DEFAULT | KERNEL_MOVEMENT_LAYER_LIMB));
    }
    const KernelVec3 drop{kEastEdge - 3.0f, 80.0f, 77.0f};
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(host.kernel, host.player, &drop, &facing));
    for (int i = 0; i < 150; ++i) host.frame(KernelVec2{0.0f, 0.0f});
    const KernelVec3 landed = host.position();
    // Grounded control: it is standing on the map, not still falling.
    require(landed.y > 0.0f && landed.y < 79.0f);
    for (int i = 0; i < 90; ++i) host.frame(KernelVec2{1.0f, 0.0f});
    const KernelVec3 ended = host.position();
    std::printf("%s: landed (%.2f, %.2f) -> ended (%.2f, %.2f, %.2f)\n",
        strike_barrier ? "no barrier layer" : "player mask",
        landed.x, landed.y, ended.x, ended.y, ended.z);
    GameServer_Destroy(host.game);
    Kernel_Destroy(host.kernel);
    return ended;
}

void camp_edge() {
    const std::vector<std::uint8_t> bundle = read_file("game_server/gameplay_catalog_bundle/bundle.zip");
    // Kernel tests bind real ports; 8246-8247 are unused elsewhere (test-port-collisions).
    const KernelVec3 stopped = walk_east(bundle, 8246, false);
    const KernelVec3 carried = walk_east(bundle, 8247, true);
    // The wall's inner face is 1 m in from the edge; the capsule rests a
    // radius short of it.
    require(stopped.x < kEastEdge - 1.0f);
    require(stopped.x > kEastEdge - 2.0f);
    // Without the layer the same walk goes off the map.
    require(carried.x > kEastEdge);
    std::printf("camp: the edge wall stops the player at x=%.2f; without the layer it walks to %.2f\n",
        stopped.x, carried.x);
}

}  // namespace

int main() {
    flat_square_edges();
    terrain_ground();
    camp_edge();
    std::printf("scene_placement_test passed\n");
    return 0;
}
