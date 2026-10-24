// P5 of docs/AI_CONTROLLER_PLAN.md: an agent uses items on itself (D13,
// D15, D18). A catalog copy gets a speed tonic -- the hover potion pointed at
// speed_up (1003, 4 s) -- and a gingerbread two item skills:
//   - the tonic on entering chasing;
//   - the tonic while chasing whenever speed_up has worn off.
// Facing a player out of reach for 15 s, it has to drink on starting the
// chase and keep drinking each time the 4 s run out, its stock refilled
// every time; and none of it may leave an outcome on the kernel's queue.

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

#include "game_server/src/agent_runtime.h"
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

bool has_status(KernelHandle* kernel, std::uint32_t net_id, std::uint32_t status_id) {
    std::array<KernelStatusEffectView, 8> views{};
    for (KernelStatusEffectView& view : views) view.struct_size = sizeof(view);
    const std::uint32_t count = Kernel_QueryStatusEffects(
        kernel, net_id, views.data(), static_cast<std::uint32_t>(views.size()));
    for (std::uint32_t index = 0; index < count && index < views.size(); ++index) {
        if (views[index].status_effect_id == status_id) return true;
    }
    return false;
}

}  // namespace

int main() {
    constexpr std::uint32_t kSpeedUp = 1003u;
    const fs::path root = fs::path(std::getenv("TEST_TMPDIR")) / "item_skills";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    {
        YAML::Node tonic = YAML::LoadFile(
            (root / "item_templates" / "3036_fungible_hover_potion.yaml").string());
        tonic["id"] = 3090;
        tonic["name"] = "fungible_test_speed_tonic";
        tonic["triggers"]["on_item_used"]["parameters"]["status"] = "speed_up";
        YAML::Emitter out;
        out << tonic;
        std::ofstream(root / "item_templates" / "3090_fungible_test_speed_tonic.yaml")
            << out.c_str() << "\n";
    }
    {
        const fs::path file = root / "entity_templates" / "28_gingerbread.yaml";
        YAML::Node gingerbread = YAML::LoadFile(file.string());
        YAML::Node on_enter;
        on_enter["use"]["type"] = "item";
        on_enter["use"]["value"] = "fungible_test_speed_tonic";
        on_enter["when"]["on_state_enter"] = "chasing";
        gingerbread["ai"]["skills"].push_back(on_enter);
        YAML::Node keep_up;
        keep_up["use"]["type"] = "item";
        keep_up["use"]["value"] = "fungible_test_speed_tonic";
        keep_up["when"]["state"] = "chasing";
        keep_up["when"]["missing_status"] = "speed_up";
        gingerbread["ai"]["skills"].push_back(keep_up);
        YAML::Emitter out;
        out << gingerbread;
        std::ofstream(file) << out.c_str() << "\n";
    }

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
    require(Kernel_StartDedicatedServer(kernel, 7923));
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
    const auto agent_state = [&](std::uint32_t net_id) -> gs::AgentSentryState {
        for (const gs::AgentRuntimeState& agent : game_server.agent_runtime_manager().agents()) {
            if (agent.net_id == net_id) return agent.sentry.state;
        }
        require(false);
        return gs::AgentSentryState::kIdle;
    };

    const std::uint32_t gingerbread =
        create(kernel, template_id_of(config, "gingerbread"), {0.0f, 0.0f, 0.0f});
    // Out of the alert first: nothing to drink yet.
    for (int tick = 0; tick < 30; ++tick) step();
    require(!has_status(kernel, gingerbread, kSpeedUp));

    // A player ahead of it, 8 m off: inside its cone, short of its leap band,
    // so it runs at it. The player is pinned there, so the chase lasts.
    const KernelVec3 ahead{-8.0f, 0.0f, 0.0f};
    const std::uint32_t player = create(kernel, template_id_of(config, "player"), ahead);
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    require(Kernel_ServerGetEntityState(kernel, player, &player_state));
    const std::uint16_t full_hp = player_state.max_hp;

    int chasing_from = -1;
    int first_dose = -1;
    int doses = 0;
    bool had = false;
    constexpr int kTicks = 450;
    for (int tick = 0; tick < kTicks; ++tick) {
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        // Kept just out of reach, moving with it, so it chases throughout.
        KernelServerEntityState self{};
        self.struct_size = sizeof(self);
        require(Kernel_ServerGetEntityState(kernel, gingerbread, &self));
        const KernelVec3 away{self.position.x - 8.0f, 0.0f, self.position.z};
        const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, player, &away, &identity));
        step();
        if (chasing_from < 0 && agent_state(gingerbread) == gs::AgentSentryState::kChasing) {
            chasing_from = tick;
        }
        const bool has = has_status(kernel, gingerbread, kSpeedUp);
        if (has && !had) {
            ++doses;
            if (first_dose < 0) first_dose = tick;
        }
        had = has;
    }
    std::printf("[items] chasing from tick %d, first dose at %d, %d doses in %d ticks\n",
                chasing_from, first_dose, doses, kTicks);
    require(chasing_from >= 0);
    // On entering the chase -- the next tick at the latest, since the kernel
    // applies the request in the following update.
    require(first_dose >= chasing_from && first_dose <= chasing_from + 2);
    // speed_up lasts 120 ticks and is drunk again once it wears off: roughly
    // one dose every 4 s of chase, each from a refilled stock.
    require(doses >= (kTicks - chasing_from) / 121);

    // Agents' requests leave nothing on the outcome queue.
    std::array<KernelGameplayRequestOutcome, 4> outcomes{};
    for (KernelGameplayRequestOutcome& outcome : outcomes) outcome.struct_size = sizeof(outcome);
    require(Kernel_PollGameplayRequestOutcomes(
                kernel, outcomes.data(), static_cast<std::uint32_t>(outcomes.size())) == 0u);

    Kernel_Destroy(kernel);
    std::printf("ai_item_skills_test passed\n");
    return 0;
}
