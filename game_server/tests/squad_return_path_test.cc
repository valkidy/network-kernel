// A squad member below a cliff from its squad, on the shipping map: the spot and
// the squad's position are the ones clip_20261106_141700 was stuck at. It chased
// down a 3 m riser it cannot climb, and the straight line to its slot runs into
// that riser for good.
//
//   A  with the navmesh it walks round and reaches its slot;
//   A' with none it stays pinned, which is what proves A is the path's doing and
//      that this spot reproduces the report.
//
// And the whole of it from a real chase, with the player luring it off the top:
//   B  it goes down after the player, turns back, and rejoins its squad, never
//      refusing to fight for longer than a short walk takes;
//   B' with no navmesh it cannot get back, and must not stay deaf to a player
//      who walks up to it, as the report's did at 13 m.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_navigation.h"
#include "kernel/public/kernel_api.h"

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "squad_return_path_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// Where the report's members stood, and where the rest of their squad was.
constexpr KernelVec3 kBelowTheCliff{100.87f, 15.0f, 43.21f};
constexpr KernelVec3 kSquadOnTop{118.0f, 25.5f, 50.5f};

fs::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::ActorTemplateConfig& actor : config.entity_templates) {
        if (actor.name == name) return actor.actor_template_id;
    }
    return 0u;
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

struct Rig {
    KernelHandle* kernel = nullptr;
    gs::GameServerGameplayConfig config;
    std::unique_ptr<gs::GameServer> server;
    std::array<KernelEvent, 256> events{};

    Rig(bool with_navmesh, std::uint16_t port) {
        config = gs::default_game_server_gameplay_config();
        config.game_rules.clear();
        config.world_rule_spawns.clear();
        config.patrols.clear();
        config.spawner_carriers.clear();
        if (with_navmesh) {
            config.navigation_mesh.artifact =
                read_file(catalog_root() / config.navigation_mesh.entry_path);
        } else {
            config.navigation_mesh.artifact.clear();
        }
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel_config.max_events = 256;
        kernel_config.max_render_states = 64;
        kernel = Kernel_Create(&kernel_config);
        require(kernel != nullptr);
        require(gs::load_kernel_gameplay_catalog(kernel, config));
        const std::vector<std::uint8_t> scene =
            read_file(catalog_root() / config.static_collision_scene.entry_path);
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = config.static_collision_scene.scene_id;
        scene_config.collider_id = config.static_collision_scene.collider_id;
        scene_config.collision_layer = config.static_collision_scene.collision_layer;
        require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
        require(Kernel_StartDedicatedServer(kernel, port));
        server = std::make_unique<gs::GameServer>(kernel, config);
    }

    ~Rig() {
        server.reset();
        Kernel_Destroy(kernel);
    }

    void step() {
        server->tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            server->handle_event(events[index]);
        }
    }

    std::uint32_t create(const std::string& name, const KernelVec3& position) {
        KernelServerEntityCreateInfo info{};
        info.struct_size = sizeof(info);
        info.entity_template_id = template_id_of(config, name);
        require(info.entity_template_id != 0u);
        info.position = position;
        info.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel, &info, &net_id));
        return net_id;
    }

    KernelVec3 position_of(std::uint32_t net_id) const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state.position;
    }

    const gs::AgentRuntimeState* agent(std::uint32_t net_id) const {
        for (const gs::AgentRuntimeState& candidate : server->agent_runtime_manager().agents()) {
            if (candidate.net_id == net_id) return &candidate;
        }
        return nullptr;
    }

    // A squad standing still at `cursor`, with this one member in it.
    void make_squad(std::uint32_t member, const KernelVec3& cursor) {
        gs::PatrolGroupTuning tuning;
        tuning.advance_speed_meters_per_second = 0.9f;
        require(server->agent_runtime_manager().patrol_groups().create_group(
                    1u, {cursor}, cursor, {member}, {KernelVec3{0.0f, 0.0f, 0.0f}}, tuning) != 0u);
    }
};

struct Walk {
    KernelVec3 start{};
    KernelVec3 end{};
    float best_slot_distance = 1e9f;
    float travelled = 0.0f;
    int ticks_to_arrive = -1;
};

Walk walk_back_to_squad(bool with_navmesh, std::uint16_t port, int ticks) {
    Rig rig(with_navmesh, port);
    const std::uint32_t member = rig.create("gingerbread", kBelowTheCliff);
    for (int i = 0; i < 30; ++i) rig.step();
    rig.make_squad(member, kSquadOnTop);
    Walk walk;
    walk.start = rig.position_of(member);
    KernelVec3 previous = walk.start;
    for (int tick = 0; tick < ticks; ++tick) {
        rig.step();
        const KernelVec3 now = rig.position_of(member);
        walk.travelled += horizontal(previous, now);
        previous = now;
        const float distance = horizontal(now, kSquadOnTop);
        if (distance < walk.best_slot_distance) walk.best_slot_distance = distance;
        if (walk.ticks_to_arrive < 0 && distance <= 3.0f && now.y > 22.0f) {
            walk.ticks_to_arrive = tick;
        }
    }
    walk.end = rig.position_of(member);
    return walk;
}

struct Chase {
    bool went_down = false;
    bool returned = false;
    bool rejoined = false;
    int ticks_to_rejoin = -1;
    int longest_return_ticks = 0;
    bool engaged_again = false;
    KernelVec3 end{};
    gs::AgentSentryState end_state = gs::AgentSentryState::kIdle;
};

// Where the report's member stood when it broke off, with the player below.
constexpr KernelVec3 kOnTheEdge{110.9f, 25.0f, 49.5f};
constexpr KernelVec3 kPlayerBelow{88.4f, 12.0f, 41.9f};
constexpr KernelVec3 kPlayerGone{20.0f, 40.0f, -60.0f};

Chase chase_down_and_back(bool with_navmesh, std::uint16_t port, int ticks) {
    Rig rig(with_navmesh, port);
    const std::uint32_t member = rig.create("gingerbread", kOnTheEdge);
    for (int i = 0; i < 30; ++i) rig.step();
    const KernelVec3 slot = rig.position_of(member);
    rig.make_squad(member, slot);
    for (int i = 0; i < 5; ++i) rig.step();

    const std::uint32_t player = rig.create("player", kPlayerBelow);
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(rig.kernel, player, &vision));
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
    const std::uint16_t full_hp = player_state.max_hp;

    Chase chase;
    KernelVec3 player_at = kPlayerBelow;
    int return_streak = 0;
    int stranded_for = 0;
    bool came_back_to_it = false;
    for (int tick = 0; tick < ticks; ++tick) {
        require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
        require(Kernel_ServerSetEntityTransform(rig.kernel, player, &player_at, &player_state.rotation));
        require(Kernel_ServerSetEntityHealth(rig.kernel, player, full_hp));
        rig.step();

        const gs::AgentRuntimeState* agent = rig.agent(member);
        require(agent != nullptr);
        const KernelVec3 now = rig.position_of(member);
        if (now.y < 20.0f) chase.went_down = true;
        const bool in_return = agent->sentry.state == gs::AgentSentryState::kReturn;
        return_streak = in_return ? return_streak + 1 : 0;
        if (return_streak > chase.longest_return_ticks) chase.longest_return_ticks = return_streak;
        if (in_return && !chase.returned) {
            chase.returned = true;
            // Out of the way: from here it has only the walk back to do.
            player_at = kPlayerGone;
        }
        if (chase.returned && !came_back_to_it && chase.ticks_to_rejoin < 0 &&
            horizontal(now, slot) <= 3.0f && now.y > 20.0f) {
            chase.rejoined = true;
            chase.ticks_to_rejoin = tick;
        }
        // Still below after a good while: walk up to it, as the report's player did.
        if (chase.returned && !chase.rejoined && now.y < 20.0f) {
            if (++stranded_for == 600 && !with_navmesh) {
                player_at = KernelVec3{now.x - 8.0f, now.y + 1.0f, now.z};
                came_back_to_it = true;
            }
        }
        if (came_back_to_it && gs::is_engaged(agent->sentry.state)) {
            chase.engaged_again = true;
        }
    }
    chase.end = rig.position_of(member);
    chase.end_state = rig.agent(member)->sentry.state;
    return chase;
}

void print(const char* label, const Chase& chase) {
    std::printf(
        "%s: went down %d, returned %d, rejoined %d at %.1f s, longest Return %.1f s, "
        "engaged again %d, end (%.1f, %.1f, %.1f) state %d\n",
        label, chase.went_down, chase.returned, chase.rejoined,
        static_cast<float>(chase.ticks_to_rejoin) * kTickSeconds,
        static_cast<float>(chase.longest_return_ticks) * kTickSeconds,
        chase.engaged_again, chase.end.x, chase.end.y, chase.end.z,
        static_cast<int>(chase.end_state));
}

void print(const char* label, const Walk& walk) {
    std::printf(
        "%s: start (%.1f, %.1f, %.1f) end (%.1f, %.1f, %.1f) closest %.1f m, walked %.1f m, arrived %s %.1f s\n",
        label, walk.start.x, walk.start.y, walk.start.z, walk.end.x, walk.end.y, walk.end.z,
        walk.best_slot_distance, walk.travelled,
        walk.ticks_to_arrive >= 0 ? "at" : "never,",
        static_cast<float>(walk.ticks_to_arrive) * kTickSeconds);
}

}  // namespace

int main() {
    const Walk pinned = walk_back_to_squad(false, 8011, 1800);
    print("A' no navmesh", pinned);
    // The rig reproduces the report: at the riser's foot, and it stays there.
    require(pinned.start.y < 16.0f);
    require(pinned.ticks_to_arrive < 0);

    const Walk home = walk_back_to_squad(true, 8012, 1800);
    print("A  navmesh", home);
    require(home.ticks_to_arrive >= 0);

    {
        // What one route costs here, against a server tick's 33 ms; update_squad_returns
        // spends at most 8 of these a tick.
        gs::PatrolNavigation navigation;
        std::string error;
        const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
        require(navigation.load(read_file(catalog_root() / config.navigation_mesh.entry_path), &error));
        gs::PatrolRouteRequest request;
        request.start = kBelowTheCliff;
        request.end = kSquadOnTop;
        request.snap_half_extent_meters = 2.0f;
        std::vector<KernelVec3> corners;
        const auto began = std::chrono::steady_clock::now();
        constexpr int kRoutes = 200;
        for (int i = 0; i < kRoutes; ++i) require(navigation.find_route(request, &corners));
        const std::chrono::duration<double, std::micro> took = std::chrono::steady_clock::now() - began;
        std::printf("route: %zu corners, %.0f us each\n", corners.size(), took.count() / kRoutes);
    }

    const Chase chased = chase_down_and_back(true, 8013, 2700);
    print("B  navmesh", chased);
    require(chased.went_down);
    require(chased.returned);
    require(chased.rejoined);
    require(chased.longest_return_ticks <= 301);

    const Chase stranded = chase_down_and_back(false, 8014, 2700);
    print("B' no navmesh", stranded);
    require(stranded.went_down);
    require(stranded.returned);
    require(!stranded.rejoined);
    require(stranded.longest_return_ticks <= 301);
    require(stranded.engaged_again);

    std::puts("squad_return_path_test: ok");
    return 0;
}
