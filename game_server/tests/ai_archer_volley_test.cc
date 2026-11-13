// gingerbread_archer (entity 41) with its shipped loadout, weapon 24: one press
// fires three parabolic arrows one after another, not a fan and not a single
// shot. Facing a player 15 m off -- inside the 18 m stop distance and the 25 m
// skill range -- the archer has to:
//   - spend ammo three at a time, each volley's three arrows within the fire
//     action's 9 + 2 * 6 = 21 ticks of its first,
//   - land arrows on the player, which proves the ballistic solver aimed
//     the arc (a flat aim_dir from 15 m lands the arrow ~1.6 m short),
//   - and keep shooting for as long as the fight lasts (reserve ammo).

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint32_t kArcherArrowWeaponId = 24u;

fs::path catalog_root() {
    return fs::path(std::getenv("TEST_SRCDIR")) / std::getenv("TEST_WORKSPACE") / "game_server" /
        "gameplay_catalog";
}

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path = catalog_root() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

std::uint32_t create(KernelHandle* kernel, std::uint32_t template_id, const KernelVec3& position) {
    require(template_id != 0u);
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = template_id;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0u);
    return net_id;
}

std::vector<KernelDamageRecord> query(KernelHandle* kernel) {
    const std::uint32_t count = Kernel_ServerQueryDamageRecords(kernel, nullptr, 0u);
    std::vector<KernelDamageRecord> records(count);
    for (KernelDamageRecord& record : records) record.struct_size = sizeof(record);
    require(Kernel_ServerQueryDamageRecords(kernel, records.data(), count) == count);
    return records;
}

std::size_t slot_of(const KernelServerEntityState& state, std::uint32_t weapon_id) {
    for (std::size_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == weapon_id) return slot;
    }
    return KERNEL_MAX_WEAPON_SLOTS;
}

}  // namespace

int main() {
    gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file((catalog_root() / "gameplay_catalog.yaml").string());
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.preload_director_template_ids.clear();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 8050));
    gs::GameServer game_server(kernel, config);
    std::array<KernelEvent, 256> events{};
    const auto step = [&]() {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            game_server.handle_event(events[index]);
        }
    };
    const auto state_of = [&](std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state;
    };

    // Agents face -x until they turn: the player 15 m that way, inside the
    // archer's stop distance, so it shoots from where it stands. Both stand
    // well clear of the origin: the game server puts the start scene's camp
    // prop there, and its collider is a static obstacle -- an archer standing
    // in it would put every arrow into the camp on the tick it is loosed.
    const std::uint32_t archer =
        create(kernel, template_id_of(config, "gingerbread_archer"), {40.0f, 0.0f, 0.0f});
    const std::uint32_t player =
        create(kernel, template_id_of(config, "player"), {25.0f, 0.0f, 0.0f});
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    const std::uint16_t full_hp = state_of(player).max_hp;

    step();
    KernelServerEntityState before = state_of(archer);
    const std::size_t slot = slot_of(before, kArcherArrowWeaponId);
    require(slot < KERNEL_MAX_WEAPON_SLOTS);
    require(before.ammo[slot] == 6u);

    std::vector<int> shot_ticks;
    std::uint32_t arrow_hits = 0;
    // 80 s: past the 42 arrows a default reserve of 6 magazines would hold
    // (~38 s of shooting), so an archer that runs dry fails the check below.
    constexpr int kTicks = 2400;
    for (int tick = 0; tick < kTicks; ++tick) {
        // Kept alive, so the fight lasts the whole window.
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        step();
        const KernelServerEntityState after = state_of(archer);
        if (after.ammo[slot] < before.ammo[slot]) {
            // One arrow per commit, and commits are 6 ticks apart.
            require(before.ammo[slot] - after.ammo[slot] == 1u);
            shot_ticks.push_back(tick);
        }
        before = after;
        for (const KernelDamageRecord& record : query(kernel)) {
            if (record.attacker_net_id == archer && record.victim_net_id == player) {
                ++arrow_hits;
            }
        }
    }

    std::printf("[archer] %d ticks: %zu arrows, %u hits\n", kTicks, shot_ticks.size(), arrow_hits);
    // Whole volleys only, and at least two of them (one magazine).
    require(shot_ticks.size() >= 6u);
    require(shot_ticks.size() % 3u == 0u);
    for (std::size_t volley = 0; volley + 2 < shot_ticks.size(); volley += 3) {
        std::printf("[archer] volley at ticks %d %d %d\n",
                    shot_ticks[volley], shot_ticks[volley + 1], shot_ticks[volley + 2]);
        require(shot_ticks[volley + 1] - shot_ticks[volley] == 6);
        require(shot_ticks[volley + 2] - shot_ticks[volley + 1] == 6);
    }
    require(arrow_hits > 0u);
    // Still shooting at the end of the fight: an agent cannot pick up ammo, so
    // a finite reserve leaves it standing in range doing nothing.
    std::printf("[archer] last arrow at tick %d\n", shot_ticks.back());
    require(shot_ticks.back() > kTicks - 300);

    Kernel_Destroy(kernel);
    std::printf("ai_archer_volley_test passed\n");
    return 0;
}
