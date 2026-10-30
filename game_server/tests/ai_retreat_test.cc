// A ranged agent backs off from a target that comes too close
// (ai.locomotion.retreat, docs/AI_RANGED_RETREAT_PLAN.md R1).
//
// The catalog half edits a copy of the mage's template and checks which
// retreats load: the band has to end inside the hold band, within the weapon
// and the eyes, and sentries and legged rigs are refused one outright.
//
// The behavior half runs on flat ground with the tuning set in code, so it
// does not depend on what the shipped templates say:
//   A  a player standing 5 m from a mage: it backs off to its target distance,
//      then holds there and keeps firing;
//   B  a player who keeps coming faster than it can back off: the retreat
//      gives up within max_ticks and it fights where it stands;
//   C  fire_while_retreating false: nothing is fired while backing off;
//   D  a hovering drone backs off the same way.
// The player is pinned in place every tick (or walked, in B), and healed, so
// a grenade's knockback or damage cannot move the goalposts.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

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

fs::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

// Edits one catalog file in a copy; the config it loads to, or nothing when
// it is refused.
std::optional<gs::GameServerGameplayConfig> load_after(
    const char* label, const std::string& relative,
    const std::function<void(YAML::Node&)>& edit) {
    const fs::path root = fs::path(std::getenv("TEST_TMPDIR")) / "edited";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    const fs::path file = root / relative;
    YAML::Node node = YAML::LoadFile(file.string());
    edit(node);
    YAML::Emitter out;
    out << node;
    std::ofstream(file) << out.c_str() << "\n";
    try {
        gs::GameServerGameplayConfig config =
            gs::load_gameplay_config_from_catalog_file((root / "gameplay_catalog.yaml").string());
        std::printf("[loads] %s\n", label);
        return config;
    } catch (const std::exception& error) {
        std::printf("[refused] %s: %s\n", label, error.what());
        return std::nullopt;
    }
}

const char* kMage = "entity_templates/31_gingerbread_mage.yaml";

std::function<void(YAML::Node&)> retreat(const std::string& yaml) {
    return [yaml](YAML::Node& node) { node["ai"]["locomotion"]["retreat"] = YAML::Load(yaml); };
}

void catalog_checks() {
    // The mage holds from 16 m (stop) to 20 m (resume); its grenade reaches
    // 22 m and its eyes 30 m.
    const auto valid = load_after("mage retreat 8 -> 19", kMage,
        retreat("{trigger_distance_meters: 8, target_distance_meters: 19}"));
    require(valid.has_value());
    const gs::ActorTemplateConfig* mage = gs::find_actor_template(*valid, 31u);
    require(mage != nullptr);
    require(mage->chaser.retreat.trigger_distance_meters == 8.0f);
    require(mage->chaser.retreat.target_distance_meters == 19.0f);
    require(mage->chaser.retreat.max_ticks == 90u);
    require(mage->chaser.retreat.cooldown_ticks == 150u);
    require(mage->chaser.retreat.fire_while_retreating);

    // Left out, the target is 90% of the furthest weapon, held to resume.
    const auto defaulted = load_after("mage retreat, target defaulted", kMage,
        retreat("{trigger_distance_meters: 8}"));
    require(defaulted.has_value());
    require(std::abs(gs::find_actor_template(*defaulted, 31u)
                         ->chaser.retreat.target_distance_meters - 19.8f) < 1e-4f);

    // Off is the default: the shipped mage has none.
    const auto shipped = load_after("mage as shipped", kMage, [](YAML::Node&) {});
    require(shipped.has_value());
    require(gs::find_actor_template(*shipped, 31u)->chaser.retreat.trigger_distance_meters ==
            0.0f);

    require(!load_after("trigger at the stop distance", kMage,
        retreat("{trigger_distance_meters: 16, target_distance_meters: 19}")));
    require(!load_after("target inside the stop distance", kMage,
        retreat("{trigger_distance_meters: 8, target_distance_meters: 15}")));
    require(!load_after("target past resume", kMage,
        retreat("{trigger_distance_meters: 8, target_distance_meters: 21}")));
    require(!load_after("no trigger", kMage, retreat("{target_distance_meters: 19}")));
    require(!load_after("unknown key", kMage,
        retreat("{trigger_distance_meters: 8, distance: 19}")));
    require(!load_after("input_magnitude 0", kMage,
        retreat("{trigger_distance_meters: 8, input_magnitude: 0}")));
    require(!load_after("max_ticks 0", kMage,
        retreat("{trigger_distance_meters: 8, max_ticks: 0}")));
    // A sentry takes no locomotion at all, so no retreat either.
    require(!load_after("sentry", "entity_templates/2_sentry_grunt.yaml",
        retreat("{trigger_distance_meters: 1}")));
    // A legged body turns to the way it walks: backing off turns its back.
    require(!load_after("legged giant", "entity_templates/39_gingerbread_giant.yaml",
        retreat("{trigger_distance_meters: 3, target_distance_meters: 5.5}")));
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

struct Outcome {
    float final_distance = 0.0f;
    // The longest unbroken run of retreating ticks, and how many there were.
    std::uint32_t longest_retreat = 0;
    std::uint32_t retreat_ticks = 0;
    std::uint32_t retreats = 0;
    std::uint32_t shots = 0;
    std::uint32_t shots_while_retreating = 0;
    // Distance over the last second: does it hold once it got there.
    float last_second_min = 1e9f;
    float last_second_max = 0.0f;
};

// `follow_speed` zero pins the player where it starts; otherwise it walks
// after the agent at that speed, keeping no closer than `start_distance`.
Outcome run(const char* label, const std::string& unit, const gs::AgentRetreatTuning& tuning,
            float start_distance, float follow_speed, int ticks, std::uint16_t port) {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    const std::uint32_t unit_id = template_id_of(config, unit);
    require(unit_id != 0u);
    for (auto* templates : {&config.actor_templates, &config.entity_templates}) {
        for (gs::ActorTemplateConfig& actor : *templates) {
            if (actor.actor_template_id == unit_id) actor.chaser.retreat = tuning;
        }
    }
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
    require(Kernel_StartDedicatedServer(kernel, port));
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
    const auto create = [&](std::uint32_t template_id, const KernelVec3& position) {
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_template_id = template_id;
        create_info.position = position;
        create_info.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
        return net_id;
    };

    const std::uint32_t agent_id = create(unit_id, {0.0f, 1.0f, 0.0f});
    for (int i = 0; i < 20; ++i) step();
    // In front of it: an agent looks down its local -x.
    const KernelVec3 agent_start = state_of(agent_id).position;
    KernelVec3 player_at{agent_start.x - start_distance, agent_start.y + 0.5f, agent_start.z};
    const std::uint32_t player = create(template_id_of(config, "player"), player_at);
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    const std::uint16_t full_hp = state_of(player).max_hp;

    Outcome outcome;
    std::uint32_t run_length = 0, last_projectile = 0;
    bool was_retreating = false;
    for (int tick = 0; tick < ticks; ++tick) {
        const KernelServerEntityState agent_state = state_of(agent_id);
        KernelServerEntityState player_state = state_of(player);
        if (follow_speed > 0.0f) {
            const float dx = agent_state.position.x - player_at.x;
            const float dz = agent_state.position.z - player_at.z;
            const float distance = std::hypot(dx, dz);
            const float stride = std::min(follow_speed * kTickSeconds, distance - start_distance);
            if (stride > 0.0f) {
                player_at.x += dx / distance * stride;
                player_at.z += dz / distance * stride;
            }
        }
        require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &player_state.rotation));
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        step();

        const gs::AgentRuntimeState* agent = nullptr;
        for (const gs::AgentRuntimeState& candidate : game_server.agent_runtime_manager().agents()) {
            if (candidate.net_id == agent_id) agent = &candidate;
        }
        require(agent != nullptr);
        std::array<KernelServerEntityState, 64> projectiles{};
        for (KernelServerEntityState& projectile : projectiles) {
            projectile.struct_size = sizeof(projectile);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, KernelEntityType_Projectile, projectiles.data(),
            static_cast<std::uint32_t>(projectiles.size()));
        for (std::uint32_t index = 0; index < count && index < projectiles.size(); ++index) {
            if (projectiles[index].net_id > last_projectile) {
                last_projectile = projectiles[index].net_id;
                ++outcome.shots;
                if (agent->retreating) ++outcome.shots_while_retreating;
            }
        }
        if (agent->retreating) {
            ++outcome.retreat_ticks;
            outcome.longest_retreat = std::max(outcome.longest_retreat, ++run_length);
            if (!was_retreating) ++outcome.retreats;
        } else {
            run_length = 0;
        }
        was_retreating = agent->retreating;
        const KernelVec3 now = state_of(agent_id).position;
        const float distance = std::hypot(now.x - player_at.x, now.z - player_at.z);
        outcome.final_distance = distance;
        if (tick >= ticks - 30) {
            outcome.last_second_min = std::min(outcome.last_second_min, distance);
            outcome.last_second_max = std::max(outcome.last_second_max, distance);
        }
    }
    std::printf(
        "[%s] final %.2f m (last second %.2f-%.2f) | %u retreats, %u ticks, longest %u | "
        "shots %u, %u while retreating\n",
        label, outcome.final_distance, outcome.last_second_min, outcome.last_second_max,
        outcome.retreats, outcome.retreat_ticks, outcome.longest_retreat, outcome.shots,
        outcome.shots_while_retreating);
    Kernel_Destroy(kernel);
    return outcome;
}

}  // namespace

int main() {
    catalog_checks();

    gs::AgentRetreatTuning mage;
    mage.trigger_distance_meters = 8.0f;
    mage.target_distance_meters = 19.0f;
    // Long enough to cover the 14 m at 1.8 m/s in one go.
    mage.max_ticks = 600u;

    // A: backs off to 19 m and holds there firing. Inside the hold band
    // (16-20 m) it does not close again.
    const Outcome a = run("A mage, player standing at 5 m", "gingerbread_mage", mage, 5.0f, 0.0f,
                          420, 7972);
    require(a.retreats == 1u);
    require(a.last_second_min >= 18.5f && a.last_second_max <= 20.0f);
    require(a.shots_while_retreating > 0u);
    require(a.shots > a.shots_while_retreating);

    // B: the player keeps 5 m behind at 3 m/s, faster than the mage's 1.8.
    // Each retreat gives up at max_ticks, and between them it stands and
    // fires; cooldown_ticks apart.
    gs::AgentRetreatTuning chased = mage;
    chased.max_ticks = 90u;
    chased.cooldown_ticks = 150u;
    const Outcome b = run("B mage, player keeps coming", "gingerbread_mage", chased, 5.0f, 3.0f,
                          600, 7973);
    require(b.retreats >= 2u);
    require(b.longest_retreat <= chased.max_ticks + 1u);
    require(b.final_distance < chased.trigger_distance_meters);
    require(b.shots > b.shots_while_retreating);

    // C: told not to, it fires nothing while backing off -- but still does
    // once it holds.
    gs::AgentRetreatTuning quiet = mage;
    quiet.fire_while_retreating = false;
    const Outcome c = run("C mage, no fire while retreating", "gingerbread_mage", quiet, 5.0f,
                          0.0f, 420, 7978);
    require(c.retreats == 1u);
    require(c.shots_while_retreating == 0u);
    require(c.shots > 0u);

    // D: the drone holds 8-10 m; it hovers rather than walks.
    gs::AgentRetreatTuning drone;
    drone.trigger_distance_meters = 4.0f;
    drone.target_distance_meters = 9.5f;
    drone.max_ticks = 300u;
    const Outcome d = run("D drone, player standing at 3 m", "beam_drone", drone, 3.0f, 0.0f, 240,
                          7979);
    require(d.retreats == 1u);
    require(d.last_second_min >= 9.0f && d.last_second_max <= 10.0f);
    require(d.shots > 0u);

    std::printf("ai_retreat_test passed\n");
    return 0;
}
