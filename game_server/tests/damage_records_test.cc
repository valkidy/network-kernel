// K1 of docs/AI_CONTROLLER_PLAN.md: Kernel_ServerQueryDamageRecords reports
// the hits the last Kernel_Update applied -- victim, attacker, where it landed
// -- so an AI can react to being attacked by something it cannot see.
//
// Real hits, not staged ones: a beam drone burns a player (the setup
// flying_units_test proves does damage), and every record has to name that
// drone -- not the beam that dealt it -- and that player. Naming a
// projectile's shooter after the projectile is gone is covered at kernel level
// by damage_record_shooter_test.

#include <array>
#include <cmath>
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

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path = fs::path(std::getenv("TEST_SRCDIR")) / std::getenv("TEST_WORKSPACE") /
        "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
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

}  // namespace

int main() {
    KernelAbiInfo abi{};
    require(Kernel_GetAbiInfo(&abi, sizeof(abi)));
    require((abi.capability_flags & KERNEL_CAPABILITY_SERVER_DAMAGE_RECORDS) != 0);

    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
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
    require(Kernel_StartDedicatedServer(kernel, 7917));
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

    // Before anything has fought, there is nothing to report.
    step();
    require(query(kernel).empty());

    // As flying_units_test: the drone climbs, then a player 12 m ahead of it
    // is inside its cone and gets burned from above.
    const std::uint32_t drone = create(kernel, template_id_of(config, "beam_drone"), {0.0f, 0.0f, 0.0f});
    const std::uint32_t player = create(kernel, template_id_of(config, "player"), {60.0f, 0.0f, 60.0f});
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    for (int tick = 0; tick < 120; ++tick) step();
    KernelServerEntityState drone_state{};
    drone_state.struct_size = sizeof(drone_state);
    require(Kernel_ServerGetEntityState(kernel, drone, &drone_state));
    require(Kernel_ServerSetEntityTransform(kernel, drone, &drone_state.position, &kIdentityRotation));
    const KernelVec3 near_player{-12.0f, 0.0f, 0.0f};
    require(Kernel_ServerSetEntityTransform(kernel, player, &near_player, &kIdentityRotation));

    std::size_t hits = 0;
    bool quiet_after_hits = false;
    for (int tick = 0; tick < 600 && !(hits > 0 && quiet_after_hits); ++tick) {
        step();
        const std::vector<KernelDamageRecord> records = query(kernel);
        if (hits > 0 && records.empty()) quiet_after_hits = true;
        for (const KernelDamageRecord& record : records) {
            require(record.struct_size == sizeof(KernelDamageRecord));
            require(record.attacker_net_id == drone);
            require(record.victim_net_id == player);
            require(record.damage > 0u);
            require(std::isfinite(record.hit_position.x) && std::isfinite(record.hit_position.y));
            ++hits;
        }
        if (hits > 0 && !quiet_after_hits) {
            // Out of sight now: its next updates must stop reporting hits,
            // which shows each update replaces the last one's records.
            const KernelVec3 away{80.0f, 0.0f, 80.0f};
            require(Kernel_ServerSetEntityTransform(kernel, player, &away, &kIdentityRotation));
        }
    }
    std::printf("[damage] %zu hits recorded; a later update reported none: %d\n",
                hits, quiet_after_hits ? 1 : 0);
    require(hits > 0u);
    require(quiet_after_hits);

    // An unsized record is refused rather than written past.
    if (Kernel_ServerQueryDamageRecords(kernel, nullptr, 0u) > 0u) {
        KernelDamageRecord unsized{};
        require(Kernel_ServerQueryDamageRecords(kernel, &unsized, 1u) == 0u);
    }

    Kernel_Destroy(kernel);
    std::printf("damage_records_test passed\n");
    return 0;
}
