// P4b of docs/AI_CONTROLLER_PLAN.md: a chaser with several weapon skills
// draws between them by weight (D14) -- only when it is free to switch, and
// never a skill out of reach, on cooldown or empty. The shipped mage carries
// one weapon, so this gives a copy of it the rocket as well: grenade launcher
// at weight 3, rocket at weight 1 with a 150-tick cooldown. Facing a player
// 12 m off, inside both reaches, it has to fire both, the grenade more often,
// and the rocket no more often than its cooldown allows.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

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



fs::path catalog_root() {
    return fs::path(std::getenv("TEST_SRCDIR")) / std::getenv("TEST_WORKSPACE") / "game_server" /
        "gameplay_catalog";
}

std::size_t slot_of(const KernelServerEntityState& state, std::uint32_t weapon_id) {
    for (std::size_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == weapon_id) return slot;
    }
    return KERNEL_MAX_WEAPON_SLOTS;
}

}  // namespace

int main() {
    // The mage, with the rocket added to its loadout and its skills.
    const fs::path root = fs::path(std::getenv("TEST_TMPDIR")) / "two_weapon_mage";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    const fs::path mage_file = root / "entity_templates" / "31_gingerbread_mage.yaml";
    YAML::Node mage = YAML::LoadFile(mage_file.string());
    mage["weapon_slots"].push_back(3);
    YAML::Node grenade = mage["ai"]["skills"][0];
    grenade["weight"] = 3;
    YAML::Node rocket;
    rocket["use"]["type"] = "weapon";
    rocket["use"]["value"] = 3;
    rocket["when"]["range_meters"]["min"] = 4.0;
    rocket["when"]["range_meters"]["max"] = 25.0;
    rocket["cooldown_ticks"] = 150;
    rocket["weight"] = 1;
    mage["ai"]["skills"].push_back(rocket);
    YAML::Emitter out;
    out << mage;
    std::ofstream(mage_file) << out.c_str() << "\n";

    gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file((root / "gameplay_catalog.yaml").string());
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
    require(Kernel_StartDedicatedServer(kernel, 7921));
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

    // Agents face -x until they turn: the player 12 m that way, inside both
    // reaches and inside the mage's 16 m stop distance, so it fires from
    // where it stands.
    const std::uint32_t mage_id =
        create(kernel, template_id_of(config, "gingerbread_mage"), {0.0f, 0.0f, 0.0f});
    const std::uint32_t player =
        create(kernel, template_id_of(config, "player"), {-12.0f, 0.0f, 0.0f});
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    const std::uint16_t full_hp = state_of(player).max_hp;

    step();
    KernelServerEntityState before = state_of(mage_id);
    const std::size_t grenade_slot = slot_of(before, 12u);
    const std::size_t rocket_slot = slot_of(before, 3u);
    require(grenade_slot < KERNEL_MAX_WEAPON_SLOTS && rocket_slot < KERNEL_MAX_WEAPON_SLOTS);
    std::uint32_t grenades = 0;
    std::uint32_t rockets = 0;
    constexpr int kTicks = 900;
    for (int tick = 0; tick < kTicks; ++tick) {
        // Kept alive, so the fight lasts the whole window.
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        step();
        const KernelServerEntityState after = state_of(mage_id);
        if (after.ammo[grenade_slot] < before.ammo[grenade_slot]) {
            grenades += before.ammo[grenade_slot] - after.ammo[grenade_slot];
        }
        if (after.ammo[rocket_slot] < before.ammo[rocket_slot]) {
            rockets += before.ammo[rocket_slot] - after.ammo[rocket_slot];
        }
        before = after;
    }
    std::printf("[choice] %d ticks: %u grenades, %u rockets\n", kTicks, grenades, rockets);
    require(grenades > 0u);
    require(rockets > 0u);
    require(grenades > rockets);
    // The rocket's 150-tick cooldown, at most one per window plus the first.
    require(rockets <= kTicks / 150 + 1);

    Kernel_Destroy(kernel);
    std::printf("ai_weapon_choice_test passed\n");
    return 0;
}
