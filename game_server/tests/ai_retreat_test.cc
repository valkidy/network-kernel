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
// And on the real 4-zone map, with its navmesh, at a spot found by searching
// the navmesh for an edge -- a cliff or a wall -- just behind a mage (R2):
//   E  it backs off sideways, never off the navmesh and never down a drop;
//   E' the same spot with no navmesh loaded walks straight over the edge, so
//      E proves the navmesh is what kept it on.
// And the shipped mission (R4): the lone mage and drone game_rule places when a
// player arrives are guards, not squad members, and back off from a player.
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
#include "game_server/src/patrol_navigation.h"
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

    // As shipped (R4): the mage and the drone back off; nothing else does.
    const auto shipped = load_after("as shipped", kMage, [](YAML::Node&) {});
    require(shipped.has_value());
    for (const gs::ActorTemplateConfig& actor : shipped->entity_templates) {
        const gs::AgentRetreatTuning& tuning = actor.chaser.retreat;
        if (actor.name == "gingerbread_mage") {
            require(tuning.trigger_distance_meters == 8.0f);
            require(tuning.target_distance_meters == 19.0f);
            require(tuning.max_ticks == 330u && tuning.cooldown_ticks == 150u);
            require(tuning.fire_while_retreating);
        } else if (actor.name == "beam_drone") {
            require(tuning.trigger_distance_meters == 4.0f);
            require(tuning.target_distance_meters == 9.5f);
            require(tuning.max_ticks == 90u && tuning.cooldown_ticks == 150u);
        } else {
            require(tuning.trigger_distance_meters == 0.0f);
        }
    }

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

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_ground_scene() {
    return read_file(catalog_root() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh");
}

enum class Scene { kFlatGround, kMapWithNavmesh, kMapWithoutNavmesh };

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
    // Real map only: the lowest it got relative to where it started, ticks it
    // spent off the navmesh, and which retreat headings it used (bit per index).
    float lowest_drop = 0.0f;
    std::uint32_t off_navmesh_ticks = 0;
    std::uint32_t headings_used = 0;
    float final_distance_from_start = 0.0f;
    // Gone from the world: off the edge and under the kill floor.
    bool lost = false;
};

// `follow_speed` zero pins the player where it starts; otherwise it walks
// after the agent at that speed, keeping no closer than `start_distance`.
Outcome run(const char* label, const std::string& unit, const gs::AgentRetreatTuning& tuning,
            float start_distance, float follow_speed, int ticks, std::uint16_t port,
            Scene where = Scene::kFlatGround, KernelVec3 spawn = {0.0f, 1.0f, 0.0f}) {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    // The map's own navmesh, as a bundle load hands it over; the flat ground
    // has none, and the control run on the map leaves it out on purpose.
    gs::PatrolNavigation navigation;
    if (where != Scene::kFlatGround) {
        const std::vector<std::uint8_t> artifact =
            read_file(catalog_root() / config.navigation_mesh.entry_path);
        std::string error;
        require(navigation.load(artifact, &error));
        if (where == Scene::kMapWithNavmesh) config.navigation_mesh.artifact = artifact;
    }
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
    const std::vector<std::uint8_t> scene = where == Scene::kFlatGround
        ? read_ground_scene()
        : read_file(catalog_root() / config.static_collision_scene.entry_path);
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

    const std::uint32_t agent_id = create(unit_id, spawn);
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
        KernelServerEntityState agent_state{};
        agent_state.struct_size = sizeof(agent_state);
        if (!Kernel_ServerGetEntityState(kernel, agent_id, &agent_state)) {
            outcome.lost = true;
            break;
        }
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
        KernelServerEntityState after{};
        after.struct_size = sizeof(after);
        if (agent == nullptr || !Kernel_ServerGetEntityState(kernel, agent_id, &after)) {
            outcome.lost = true;
            break;
        }
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
        const KernelVec3 now = after.position;
        const float distance = std::hypot(now.x - player_at.x, now.z - player_at.z);
        if (where != Scene::kFlatGround) {
            outcome.lowest_drop = std::min(outcome.lowest_drop, now.y - agent_start.y);
            if (!navigation.walkable_line(now, now, 0.5f).has_value()) ++outcome.off_navmesh_ticks;
            if (agent->retreating) outcome.headings_used |= 1u << agent->retreat_heading;
            outcome.final_distance_from_start =
                std::hypot(now.x - agent_start.x, now.z - agent_start.z);
        }
        outcome.final_distance = distance;
        if (tick >= ticks - 30) {
            outcome.last_second_min = std::min(outcome.last_second_min, distance);
            outcome.last_second_max = std::max(outcome.last_second_max, distance);
        }
    }
    std::printf(
        "[%s] final %.2f m (last second %.2f-%.2f) | %u retreats, %u ticks, longest %u | "
        "shots %u, %u while retreating | drop %.2f m, %u ticks off navmesh, headings 0x%x, "
        "%.1f m from start%s\n",
        label, outcome.final_distance, outcome.last_second_min, outcome.last_second_max,
        outcome.retreats, outcome.retreat_ticks, outcome.longest_retreat, outcome.shots,
        outcome.shots_while_retreating, outcome.lowest_drop, outcome.off_navmesh_ticks,
        outcome.headings_used, outcome.final_distance_from_start, outcome.lost ? ", LOST" : "");
    Kernel_Destroy(kernel);
    return outcome;
}

// A spot on the narrow pass's plateau (~11-13 m up, ringed by cliffs) with
// the navmesh's edge less than 3 m east of it -- behind a mage that faces west
// at a player -- while something to the side is walkable and so are the 6 m
// west. Searched rather than hard-coded, so a rebaked navmesh moves the spot
// instead of breaking the test.
KernelVec3 find_edge_spot(const gs::GameServerGameplayConfig& config) {
    gs::PatrolNavigation navigation;
    std::string error;
    require(navigation.load(read_file(catalog_root() / config.navigation_mesh.entry_path), &error));
    const auto walkable = [&](const KernelVec3& from, float dx, float dz) {
        return navigation.walkable_line(from, {from.x + dx, from.y, from.z + dz}, 0.5f)
            .value_or(false);
    };
    for (float x = 60.0f; x <= 98.0f; x += 1.0f) {
        for (float z = -95.0f; z <= -10.0f; z += 1.0f) {
            KernelVec3 ground{};
            if (!navigation.snap({x, 40.0f, z}, 0.5f, &ground) || ground.y < 8.0f) continue;
            const float r = 3.0f;
            const float d = r * 0.70710678f;
            if (walkable(ground, r, 0.0f)) continue;                     // edge behind
            if (!walkable(ground, -6.0f, 0.0f)) continue;                // room in front
            // A straight edge rather than a corner: room either side.
            if (!walkable(ground, 0.0f, 2.0f * r) || !walkable(ground, 0.0f, -2.0f * r)) continue;
            (void)d;
            std::printf("edge spot (%.1f %.2f %.1f)\n", ground.x, ground.y, ground.z);
            return ground;
        }
    }
    require(false && "no edge spot on the plateau");
    return {};
}

// The shipped mission on the real map: a player arrives, game_rule places the
// lone mage and drone, and each backs off from a player put in front of it.
void shipped_lone_spawns(std::uint16_t port) {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.navigation_mesh.artifact = read_file(catalog_root() / config.navigation_mesh.entry_path);
    const std::vector<std::uint8_t> scene =
        read_file(catalog_root() / config.static_collision_scene.entry_path);
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
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
    require(game_server.preload_directors());
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
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = template_id_of(config, "player");
    create_info.position = config.player.spawn_position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t player = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &player));
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    const std::uint16_t full_hp = state_of(player).max_hp;
    for (int tick = 0; tick < 60; ++tick) step();

    // The lone ones are guards; the warband's mages are squad members.
    const auto lone = [&](const char* name) {
        const std::uint32_t template_id = template_id_of(config, name);
        std::uint32_t found = 0, count = 0;
        for (const gs::AgentRuntimeState& agent : game_server.agent_runtime_manager().agents()) {
            if (agent.actor_template_id != template_id ||
                agent.controller_role != gs::AgentControllerRole::kGuard) continue;
            require(!agent.patrol.has_slot);
            found = agent.net_id;
            ++count;
        }
        require(count == 1u);
        return found;
    };
    const auto retreating = [&](std::uint32_t net_id) {
        for (const gs::AgentRuntimeState& agent : game_server.agent_runtime_manager().agents()) {
            if (agent.net_id == net_id) return agent.retreating;
        }
        return false;
    };
    // A player `distance` in front of the agent -- where its local -x looks --
    // pinned there and kept alive; the furthest the agent then gets from it.
    const auto confront = [&](const char* label, std::uint32_t agent, float distance, int ticks) {
        const KernelServerEntityState start = state_of(agent);
        const KernelQuat& q = start.rotation;
        float fx = -(1.0f - 2.0f * (q.y * q.y + q.z * q.z));
        float fz = -(2.0f * (q.x * q.z - q.w * q.y));
        const float length = std::hypot(fx, fz);
        fx /= length;
        fz /= length;
        const KernelVec3 at{start.position.x + fx * distance, start.position.y + 0.5f,
                            start.position.z + fz * distance};
        bool backed_off = false;
        float final_distance = 0.0f;
        for (int tick = 0; tick < ticks; ++tick) {
            const KernelServerEntityState p = state_of(player);
            require(Kernel_ServerSetEntityTransform(kernel, player, &at, &p.rotation));
            require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
            step();
            backed_off = backed_off || retreating(agent);
            const KernelVec3 now = state_of(agent).position;
            final_distance = std::hypot(now.x - at.x, now.z - at.z);
        }
        std::printf("[H %s] at (%.1f %.1f) | backed off %d | final %.2f m\n", label,
                    start.position.x, start.position.z, backed_off, final_distance);
        require(backed_off);
        return final_distance;
    };
    require(confront("lone mage, player at 5 m", lone("gingerbread_mage"), 5.0f, 330) >= 18.5f);
    require(confront("lone drone, player at 3 m", lone("beam_drone"), 3.0f, 150) >= 9.0f);
    Kernel_Destroy(kernel);
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

    // F, G: the shipped tuning, as the catalog has it (R4).
    const gs::GameServerGameplayConfig shipped = gs::default_game_server_gameplay_config();
    const auto shipped_retreat = [&](const char* name) {
        for (const gs::ActorTemplateConfig& actor : shipped.entity_templates) {
            if (actor.name == name) return actor.chaser.retreat;
        }
        require(false && "template missing");
        return gs::AgentRetreatTuning{};
    };
    const Outcome f = run("F mage as shipped, player standing at 5 m", "gingerbread_mage",
                          shipped_retreat("gingerbread_mage"), 5.0f, 0.0f, 420, 7970);
    require(f.retreats == 1u);
    require(f.last_second_min >= 18.5f && f.last_second_max <= 20.0f);
    const Outcome g = run("G drone as shipped, player standing at 3 m", "beam_drone",
                          shipped_retreat("beam_drone"), 3.0f, 0.0f, 240, 7999);
    require(g.retreats == 1u);
    require(g.last_second_min >= 9.0f && g.last_second_max <= 10.0f);

    // E: on the plateau with the cliff behind it. Long retreats, so it would
    // reach the edge if nothing stopped it.
    const KernelVec3 edge = find_edge_spot(gs::default_game_server_gameplay_config());
    const KernelVec3 spawn{edge.x, edge.y + 1.0f, edge.z};
    gs::AgentRetreatTuning cliff = mage;
    const Outcome e = run("E mage, cliff behind, navmesh", "gingerbread_mage", cliff, 5.0f, 0.0f,
                          300, 7980, Scene::kMapWithNavmesh, spawn);
    require(e.retreats >= 1u);
    require(e.lowest_drop > -1.0f);
    require(e.off_navmesh_ticks == 0u);
    // It went to the side rather than straight back, and still got away.
    require((e.headings_used & ~1u) != 0u);
    require(e.final_distance_from_start > 1.0f);

    // E': nothing to look at, so it backs straight off the edge.
    const Outcome control = run("E' mage, cliff behind, no navmesh", "gingerbread_mage", cliff,
                                5.0f, 0.0f, 300, 7992, Scene::kMapWithoutNavmesh, spawn);
    require(control.lost || control.lowest_drop < -3.0f || control.off_navmesh_ticks > 0u);

    shipped_lone_spawns(8101);

    std::printf("ai_retreat_test passed\n");
    return 0;
}
