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
//   B  it goes down after the player, the player vanishes, and it rejoins its
//      squad, never refusing to fight for longer than a short walk takes;
//   B' with no navmesh it cannot get back, and must not stay deaf to a player
//      who walks up to it, as the report's did at 13 m.
//
// And a player up a ledge (issue_20261106_150830, the spot from it), who a melee
// unit stood under for a minute swinging at air:
//   C  it leaps up and lands on the player's level, where its claw reaches;
//   C' the same distance on the level is too close to leap, as it always was;
//   C" nor up a slope it can walk, however much higher the player stands.
//
// And the squad-mates of a fight (issue_20261106_150830): 40 m from their slots,
// they looped Alert->Return->Idle->Alert every 5 s for a minute, neither home nor
// in the fight, and the alert passed from one to another with nobody in sight.
//   D  a member far from its slot goes home while the fight goes on, and is not
//      told to stand and watch on the way;
//   D' once the fight is over nobody is left holding the squad on hearsay.
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

// The shipping catalog on the composed 216 m map and its cliffs, which the
// camp no longer stands on (templates 2 and 5 still do). The camp's two props
// stay: they land in this map's corner, far from every spot here, and the
// ledge leap (C) does not happen without them -- the entity count shifts
// something the leap depends on.
gs::GameServerGameplayConfig composed_map_config() {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.static_collision_scene.entry_path =
        "mesh_assets/jolt/composed_216m_ground_0001_seed1_collision.joltmesh";
    config.navigation_mesh.entry_path =
        "mesh_assets/recast/composed_216m_ground_0001_seed1_collision.navmesh";
    // The camp's spawn is off this map; template 2's is on it.
    config.player.spawn_position = KernelVec3{106.8f, 23.0f, 77.0f};
    return config;
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
        config = composed_map_config();
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
        if (chase.went_down && player_at.x == kPlayerBelow.x &&
            horizontal(now, slot) > 18.0f) {
            // Past its leash, and now out of its sight: it lets go, and has only
            // the walk back to do.
            player_at = kPlayerGone;
        }
        if (in_return) chase.returned = true;
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

struct Ledge {
    int leaps = 0;
    float best_y = -1e9f;
    int ticks_to_top = -1;
    int first_leap_tick = -1;
    std::uint16_t damage = 0;
};

constexpr KernelVec3 kPlayerOnLedge{76.35f, 25.0f, 117.27f};
constexpr KernelVec3 kAtLedgeFoot{76.46f, 21.5f, 115.48f};

Ledge leap_at_ledge(const KernelVec3& player_spot, const KernelVec3& unit_spot, std::uint16_t port, int ticks) {
    Rig rig(true, port);
    const std::uint32_t player = rig.create("player", player_spot);
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(rig.kernel, player, &vision));
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    for (int i = 0; i < 30; ++i) rig.step();
    const KernelVec3 feet = rig.position_of(player);
    // Facing the player, down its local -x: put it to the player's +x side.
    const std::uint32_t unit = rig.create("gingerbread", unit_spot);
    require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
    const std::uint16_t full_hp = player_state.max_hp;
    Ledge ledge;
    bool was_leaping = false;
    for (int tick = 0; tick < ticks; ++tick) {
        require(Kernel_ServerSetEntityTransform(rig.kernel, player, &feet, &player_state.rotation));
        rig.step();
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(rig.kernel, unit, &state));
        const bool leaping = (state.visual_flags & KERNEL_VISUAL_FLAG_LEAPING) != 0u;
        if (leaping && !was_leaping) {
            ++ledge.leaps;
            if (ledge.first_leap_tick < 0) ledge.first_leap_tick = tick;
        }
        was_leaping = leaping;
        if (state.position.y > ledge.best_y) ledge.best_y = state.position.y;
        if (ledge.ticks_to_top < 0 && !leaping && state.position.y >= feet.y - 1.0f) {
            ledge.ticks_to_top = tick;
        }
        require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
        if (player_state.hp < full_hp) {
            ledge.damage = static_cast<std::uint16_t>(full_hp - player_state.hp);
        }
    }
    std::printf(
        "  player feet (%.1f, %.1f, %.1f) unit started y %.1f\n",
        feet.x, feet.y, feet.z, unit_spot.y);
    return ledge;
}

void print(const char* label, const Ledge& ledge) {
    std::printf(
        "%s: leaps %d, first at %.1f s, highest y %.1f, on the level at %.1f s, damage %u\n",
        label, ledge.leaps, static_cast<float>(ledge.first_leap_tick) * kTickSeconds, ledge.best_y,
        static_cast<float>(ledge.ticks_to_top) * kTickSeconds, ledge.damage);
}

struct SquadFight {
    int far_alerts_on_the_way = 0;
    int far_ticks_to_home = -1;
    int ticks_to_release = -1;
    int holding_ticks_after = 0;
};

// Three members: two under the player's ledge, in a real fight, and one a long way
// from its slot with nothing in sight. Then the player goes, and the squad should
// be free inside one forget.
SquadFight fight_with_one_far_away(std::uint16_t port) {
    Rig rig(true, port);
    const std::uint32_t player = rig.create("player", kPlayerOnLedge);
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(rig.kernel, player, &vision));
    for (int i = 0; i < 30; ++i) rig.step();
    const KernelVec3 feet = rig.position_of(player);
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
    const std::uint16_t full_hp = player_state.max_hp;

    // Slots in open ground a little way off, on the player's level: where the
    // report's squad stood relative to its fight.
    const KernelVec3 cursor{111.24f, 22.5f, 89.96f};
    const std::uint32_t near_a = rig.create("gingerbread", {feet.x + 2.0f, 21.5f, feet.z - 1.0f});
    const std::uint32_t near_b = rig.create("gingerbread", {feet.x + 2.5f, 21.5f, feet.z - 2.5f});
    // Out of its own eyes' 26 m, so what it does is down to the squad's alert.
    gs::PatrolNavigation navigation;
    std::string error;
    require(navigation.load(rig.config.navigation_mesh.artifact, &error));
    KernelVec3 far_ground{};
    require(navigation.snap({80.0f, 21.0f, 84.0f}, 3.0f, &far_ground));
    const std::uint32_t far =
        rig.create("gingerbread", {far_ground.x, far_ground.y + 1.0f, far_ground.z});
    for (int i = 0; i < 30; ++i) {
        require(Kernel_ServerSetEntityTransform(rig.kernel, player, &feet, &player_state.rotation));
        rig.step();
    }
    gs::PatrolGroupTuning tuning;
    tuning.advance_speed_meters_per_second = 0.9f;
    const std::uint32_t group_id = rig.server->agent_runtime_manager().patrol_groups().create_group(
        1u, {cursor, {cursor.x + 60.0f, cursor.y, cursor.z}}, cursor,
        {near_a, near_b, far},
        {KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{0.0f, 0.0f, 1.5f}, KernelVec3{0.0f, 0.0f, -1.5f}},
        tuning);
    require(group_id != 0u);

    SquadFight fight;
    bool player_there = true;
    int gone_at = -1;
    gs::AgentSentryState far_state = gs::AgentSentryState::kIdle;
    KernelVec3 player_at = feet;
    for (int tick = 0; tick < 1800; ++tick) {
        require(Kernel_ServerGetEntityState(rig.kernel, player, &player_state));
        require(Kernel_ServerSetEntityTransform(rig.kernel, player, &player_at, &player_state.rotation));
        require(Kernel_ServerSetEntityHealth(rig.kernel, player, full_hp));
        rig.step();

        const gs::AgentRuntimeState* far_agent = rig.agent(far);
        require(far_agent != nullptr);
        const float far_from_slot = horizontal(far_agent->position, far_agent->patrol.slot);
        if (far_agent->sentry.state == gs::AgentSentryState::kAlert &&
            far_state != gs::AgentSentryState::kAlert && far_agent->sentry.target == 0 &&
            far_from_slot > 3.0f) {
            ++fight.far_alerts_on_the_way;
        }
        far_state = far_agent->sentry.state;
        if (fight.far_ticks_to_home < 0 && far_from_slot <= 3.0f) {
            fight.far_ticks_to_home = tick;
        }
        if (player_there && tick == 900) {
            player_there = false;
            gone_at = tick;
            player_at = kPlayerGone;
        }
        if (!player_there) {
            const gs::PatrolGroup* group =
                rig.server->agent_runtime_manager().patrol_groups().find_group(group_id);
            require(group != nullptr);
            if (group->holding) {
                ++fight.holding_ticks_after;
            } else if (fight.ticks_to_release < 0) {
                fight.ticks_to_release = tick - gone_at;
            }
        }
    }
    return fight;
}

void print(const char* label, const SquadFight& fight) {
    std::printf(
        "%s: far member alerted on its way %d times, home at %.1f s; after the player left, "
        "released at %.1f s, held %.1f s in all\n",
        label, fight.far_alerts_on_the_way,
        static_cast<float>(fight.far_ticks_to_home) * kTickSeconds,
        static_cast<float>(fight.ticks_to_release) * kTickSeconds,
        static_cast<float>(fight.holding_ticks_after) * kTickSeconds);
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
        const gs::GameServerGameplayConfig config = composed_map_config();
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

    const Ledge up = leap_at_ledge(kPlayerOnLedge, kAtLedgeFoot, 8015, 900);
    print("C  ledge", up);
    require(up.leaps >= 1);
    require(up.ticks_to_top >= 0);
    require(up.damage > 0u);

    const KernelVec3 on_level{kPlayerOnLedge.x + 5.0f, kPlayerOnLedge.y, kPlayerOnLedge.z};
    const Ledge level = leap_at_ledge(kPlayerOnLedge, on_level, 8016, 300);
    print("C' level", level);
    require(level.leaps == 0);

    // A walkable slope with a real climb, searched for so a rebake moves it. Its
    // unit is to the player's +x, because it looks down its local -x.
    const gs::GameServerGameplayConfig config = composed_map_config();
    gs::PatrolNavigation navigation;
    std::string error;
    require(navigation.load(read_file(catalog_root() / config.navigation_mesh.entry_path), &error));
    bool found_slope = false;
    KernelVec3 slope_top{}, slope_foot{};
    for (float x = 4.0f; x < 212.0f && !found_slope; x += 3.0f) {
        for (float z = 4.0f; z < 212.0f && !found_slope; z += 3.0f) {
            for (float y = 0.0f; y < 40.0f && !found_slope; y += 4.0f) {
                if (!navigation.snap({x, y, z}, 1.0f, &slope_top)) continue;
                if (!navigation.snap({x + 6.0f, slope_top.y - 2.0f, z}, 1.0f, &slope_foot)) continue;
                if (slope_top.y - slope_foot.y < 1.6f || slope_top.y - slope_foot.y > 3.0f) continue;
                found_slope = navigation.walkable_line(slope_foot, slope_top, 0.5f).value_or(false);
            }
        }
    }
    require(found_slope);
    const Ledge slope = leap_at_ledge(
        {slope_top.x, slope_top.y + 1.0f, slope_top.z},
        {slope_foot.x, slope_foot.y + 1.0f, slope_foot.z},
        8017, 300);
    print("C\" slope", slope);
    require(slope.leaps == 0);

    const SquadFight fight = fight_with_one_far_away(8018);
    print("D  squad fight", fight);
    std::printf("  far member's start: 32 m from the player, 31 m from its slot\n");
    require(fight.far_alerts_on_the_way == 0);
    require(fight.far_ticks_to_home >= 0);
    // D': one forget (5 s) and a little, and not coming back.
    require(fight.ticks_to_release >= 0);
    require(fight.holding_ticks_after <= 240);

    std::puts("squad_return_path_test: ok");
    return 0;
}
