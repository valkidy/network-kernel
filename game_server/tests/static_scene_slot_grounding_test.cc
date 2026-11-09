// A player stands on a static scene slot's terrain exactly as on the camp's
// (docs/SCENE_TABLE_PLAN.md S1), through the public API on a real listen host.
//
// The shipping catalog's camp (composed_mesh) runs as slot 0; the same
// artifact goes into slot 1 at x + 4096 with Kernel_AddStaticSceneSlot after
// the host has started. The host's player is teleported to the same local
// point in each and left to settle:
//   - camp            -> the control height;
//   - slot 1          -> must match it;
//   - between slots   -> no ground, falls;
//   - slot 1 removed  -> falls from where it stood.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "engine/src/kernel/public/kernel_api.h"
#include "game_server/public/game_server_api.h"
#include "game_server/src/gameplay_config.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "static_scene_slot_grounding_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.composed_mesh.yaml";
// Kernel tests bind real ports; 8238 is unused elsewhere (test-port-collisions).
constexpr std::uint16_t kPort = 8238;
constexpr float kSlotSpacing = 4096.0f;
constexpr float kDt = 1.0f / 30.0f;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Host {
    KernelHandle* kernel = nullptr;
    GameServerHandle* game = nullptr;
    std::uint32_t player = 0;
    std::uint32_t sequence = 1;

    void frame() {
        KernelPlayerInput input{};
        input.input_seq = sequence++;
        input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
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

    // Teleport, then let it settle for `frames`; the height it ends at.
    float drop_at(KernelVec3 at, int frames) {
        const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, player, &at, &facing));
        for (int i = 0; i < frames; ++i) frame();
        return position().y;
    }
};

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_bundle_memory(bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry);
    const gs::StaticCollisionSceneConfig& camp_scene = config.static_collision_scene;
    require(!camp_scene.entry_path.empty());
    require(config.player.has_spawn_position);
    const std::vector<std::uint8_t> artifact =
        gs::load_gameplay_bundle_entry_bytes(bundle.data(), static_cast<std::uint32_t>(bundle.size()), camp_scene.entry_path);
    require(!artifact.empty());

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
    require(Kernel_StartListenServer(host.kernel, kPort));

    KernelAbiInfo abi{};
    require(Kernel_GetAbiInfo(&abi, sizeof(abi)));
    require((abi.capability_flags & KERNEL_CAPABILITY_STATIC_SCENE_SLOTS) != 0);

    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(host.kernel, &local));
    host.player = local.player_net_id;
    require(host.player != 0u);
    for (int i = 0; i < 10; ++i) host.frame();

    KernelStaticSceneSlotConfig slot{};
    slot.struct_size = sizeof(slot);
    slot.slot = 1u;
    slot.artifact_bytes = artifact.data();
    slot.artifact_size = static_cast<std::uint32_t>(artifact.size());
    slot.scene_id = camp_scene.scene_id;
    slot.collider_id = camp_scene.collider_id + 1u;
    slot.collision_layer = camp_scene.collision_layer;
    slot.offset = KernelVec3{kSlotSpacing, 0.0f, 0.0f};
    require(Kernel_AddStaticSceneSlot(host.kernel, &slot));

    const KernelVec3 spawn = config.player.spawn_position;
    const auto shifted = [&](float dx) { return KernelVec3{spawn.x + dx, spawn.y + 2.0f, spawn.z}; };

    const float camp_y = host.drop_at(shifted(0.0f), 60);
    // Grounded control: it came to rest near the spawn, not through the floor.
    require(std::fabs(camp_y - spawn.y) < 4.0f);
    const float slot_y = host.drop_at(shifted(kSlotSpacing), 60);
    const float gap_y = host.drop_at(shifted(kSlotSpacing * 0.5f), 45);
    std::printf("camp %.3f  slot1 %.3f  gap %.3f (from %.3f)\n", camp_y, slot_y, gap_y, spawn.y + 2.0f);
    require(std::fabs(slot_y - camp_y) < 0.05f);
    // About 11 m of free fall in 1.5 s; anything standing would be at camp_y.
    require(gap_y < camp_y - 3.0f);

    // Standing on slot 1, then slot 1 goes: down it goes.
    const float before = host.drop_at(shifted(kSlotSpacing), 60);
    require(std::fabs(before - camp_y) < 0.05f);
    require(Kernel_RemoveStaticSceneSlot(host.kernel, 1u));
    for (int i = 0; i < 30; ++i) host.frame();
    const float after = host.position().y;
    std::printf("on slot1 %.3f, 1 s after removal %.3f\n", before, after);
    require(after < before - 3.0f);

    GameServer_Destroy(host.game);
    Kernel_Destroy(host.kernel);
    std::printf("static_scene_slot_grounding_test passed\n");
    return 0;
}
