// What a squad's way of getting about costs one server tick, on the shipping map
// with its navmesh. Reports, never asserts; written to compare one build with
// another on this host.
//
//   SHIPPED  the catalog's own patrols and nests, four players standing about,
//            three minutes: whatever play produces.
//   CUT OFF  squads below a cliff from their slots with a player up a ledge: every
//            member asking about its way back at once, the worst the return path
//            and the ledge leap can do.
//
// Run it with
//   bazel run -c opt //game_server:squad_chase_bench
#include <algorithm>
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
#if __has_include("game_server/src/ai_trace.h")
#include "game_server/src/ai_trace.h"
#define SQUAD_BENCH_HAS_TRACE 1
#endif

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

namespace {

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentity{0.0f, 0.0f, 0.0f, 1.0f};

fs::path runfile_root() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    const char* workspace = std::getenv("TEST_WORKSPACE");
    if (srcdir != nullptr && workspace != nullptr) {
        return fs::path(srcdir) / workspace;
    }
    const char* runfiles = std::getenv("RUNFILES_DIR");
    if (runfiles != nullptr) {
        return fs::path(runfiles) / "network-example";
    }
    std::fprintf(stderr, "no runfiles\n");
    std::abort();
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        std::abort();
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct Samples {
    std::vector<double> game;
    std::vector<double> kernel;
    std::size_t agents_sum = 0;

    static void line(const char* name, std::vector<double> values) {
        std::sort(values.begin(), values.end());
        double sum = 0.0;
        for (const double value : values) sum += value;
        std::printf(
            "  %-7s avg %8.1f us   p50 %8.1f   p99 %8.1f   max %8.1f\n",
            name, sum / static_cast<double>(values.size()),
            values[values.size() / 2], values[values.size() * 99 / 100], values.back());
    }

    void print(const char* title) const {
        std::printf("%s (%zu ticks, %.1f agents)\n", title, game.size(),
                    static_cast<double>(agents_sum) / static_cast<double>(game.size()));
        line("game", game);
        line("kernel", kernel);
    }
};

struct Rig {
    KernelHandle* kernel = nullptr;
    gs::GameServerGameplayConfig config;
    std::unique_ptr<gs::GameServer> server;
    std::array<KernelEvent, 512> events{};
    Samples samples;

    Rig(gs::GameServerGameplayConfig loaded, std::uint16_t port) : config(std::move(loaded)) {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel_config.max_events = 512;
        kernel_config.max_render_states = 512;
        kernel = Kernel_Create(&kernel_config);
        if (kernel == nullptr || !gs::load_kernel_gameplay_catalog(kernel, config)) std::abort();
        const std::vector<std::uint8_t> scene =
            read_file(catalog_root() / config.static_collision_scene.entry_path);
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = config.static_collision_scene.scene_id;
        scene_config.collider_id = config.static_collision_scene.collider_id;
        scene_config.collision_layer = config.static_collision_scene.collision_layer;
        if (!Kernel_SetStaticCollisionScene(kernel, &scene_config)) std::abort();
        if (!Kernel_StartDedicatedServer(kernel, port)) std::abort();
        server = std::make_unique<gs::GameServer>(kernel, config);
    }

    ~Rig() {
        server.reset();
        Kernel_Destroy(kernel);
    }

    static fs::path catalog_root() { return runfile_root() / "game_server" / "gameplay_catalog"; }

    void step(bool record) {
        using Clock = std::chrono::steady_clock;
        const auto began = Clock::now();
        server->tick(kTickSeconds);
        const auto ai_done = Clock::now();
        Kernel_Update(kernel, kTickSeconds);
        const auto done = Clock::now();
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            server->handle_event(events[index]);
        }
        if (record) {
            samples.game.push_back(std::chrono::duration<double, std::micro>(ai_done - began).count());
            samples.kernel.push_back(std::chrono::duration<double, std::micro>(done - ai_done).count());
            samples.agents_sum += server->agent_runtime_manager().agent_count();
        }
    }

    std::uint32_t create(std::uint32_t template_id, const KernelVec3& position) {
        KernelServerEntityCreateInfo info{};
        info.struct_size = sizeof(info);
        info.entity_template_id = template_id;
        info.position = position;
        info.rotation = kIdentity;
        std::uint32_t net_id = 0;
        if (!Kernel_ServerCreateEntity(kernel, &info, &net_id)) std::abort();
        return net_id;
    }

    std::uint32_t create_player(const KernelVec3& position) {
        const std::uint32_t player = create(template_of("player"), position);
        KernelAgentVisionConfig vision{};
        vision.struct_size = sizeof(vision);
        vision.camp = KernelAgentCamp_PlayerSide;
        if (!Kernel_ServerSetEntityVisionConfig(kernel, player, &vision)) std::abort();
        return player;
    }

    std::uint32_t template_of(const std::string& name) const {
        for (const gs::ActorTemplateConfig& actor : config.entity_templates) {
            if (actor.name == name) return actor.actor_template_id;
        }
        std::abort();
    }

    void hold(std::uint32_t player, const KernelVec3& at) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        if (!Kernel_ServerGetEntityState(kernel, player, &state)) std::abort();
        Kernel_ServerSetEntityTransform(kernel, player, &at, &state.rotation);
        Kernel_ServerSetEntityHealth(kernel, player, state.max_hp);
    }
};

gs::GameServerGameplayConfig load_config() {
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_catalog_file(
        (Rig::catalog_root() / "gameplay_catalog.yaml").string());
    config.navigation_mesh.artifact = read_file(Rig::catalog_root() / config.navigation_mesh.entry_path);
    return config;
}

// What the two navmesh questions can cost, across the whole map. A tick spends at
// most 8 routes, and one line test per member away from its slot per second.
void run_queries() {
    gs::PatrolNavigation navigation;
    std::string error;
    const gs::GameServerGameplayConfig config = load_config();
    if (!navigation.load(config.navigation_mesh.artifact, &error)) std::abort();
    std::vector<KernelVec3> spots;
    std::uint32_t random = 12345u;
    const auto next = [&random]() {
        random = random * 1664525u + 1013904223u;
        return static_cast<float>((random >> 8) & 0xffffu) / 65535.0f;
    };
    while (spots.size() < 400) {
        KernelVec3 ground{};
        if (navigation.snap({next() * 216.0f, next() * 40.0f, next() * 216.0f}, 5.0f, &ground)) {
            spots.push_back(ground);
        }
    }
    using Clock = std::chrono::steady_clock;
    std::vector<double> routes, lines;
    std::vector<KernelVec3> corners;
    int connected = 0;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        for (std::size_t j = 0; j < 10; ++j) {
            const KernelVec3& to = spots[(i + 1 + j * 37) % spots.size()];
            gs::PatrolRouteRequest request;
            request.start = spots[i];
            request.end = to;
            request.snap_half_extent_meters = 2.0f;
            const auto route_began = Clock::now();
            connected += navigation.find_route(request, &corners) ? 1 : 0;
            routes.push_back(std::chrono::duration<double, std::micro>(Clock::now() - route_began).count());
            const auto line_began = Clock::now();
            (void)navigation.walkable_line(spots[i], to, 2.0f);
            lines.push_back(std::chrono::duration<double, std::micro>(Clock::now() - line_began).count());
        }
    }
    std::printf("NAVMESH QUERIES over %zu pairs, %d connected\n", routes.size(), connected);
    Samples::line("route", routes);
    Samples::line("line", lines);
}

void run_shipped(std::uint16_t port, int seconds) {
    Rig rig(load_config(), port);
    const std::array<KernelVec3, 4> spots{
        KernelVec3{106.8f, 23.0f, 77.0f},
        KernelVec3{110.0f, 23.0f, 80.0f},
        KernelVec3{76.35f, 25.0f, 117.27f},
        KernelVec3{92.6f, 12.0f, 44.4f},
    };
    std::array<std::uint32_t, 4> players{};
    for (std::size_t i = 0; i < spots.size(); ++i) players[i] = rig.create_player(spots[i]);
    for (int tick = 0; tick < 30 * seconds; ++tick) {
        for (std::size_t i = 0; i < spots.size(); ++i) rig.hold(players[i], spots[i]);
        rig.step(tick >= 60);
    }
    rig.samples.print("SHIPPED");
}

void run_cut_off(std::uint16_t port, int seconds, int squads, int members) {
    gs::GameServerGameplayConfig config = load_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    gs::PatrolNavigation navigation;
    std::string error;
    if (!navigation.load(config.navigation_mesh.artifact, &error)) std::abort();
    Rig rig(std::move(config), port);
    const std::uint32_t gingerbread = rig.template_of("gingerbread");
    const KernelVec3 cursor{118.0f, 25.5f, 50.5f};
    const KernelVec3 below{100.87f, 13.1f, 43.21f};
    const KernelVec3 foot{76.46f, 20.0f, 112.0f};
    const KernelVec3 ledge{76.35f, 25.0f, 117.27f};
    // Spots on the ground the map really has at that height. A unit made under
    // the surface falls straight through it to the kill floor.
    const auto spots_around = [&navigation](const KernelVec3& base, std::size_t count) {
        std::vector<KernelVec3> spots;
        for (float radius = 0.0f; radius < 20.0f && spots.size() < count; radius += 1.2f) {
            const int around = radius == 0.0f ? 1 : static_cast<int>(radius * 5.0f);
            for (int step = 0; step < around && spots.size() < count; ++step) {
                const float angle = 6.2832f * static_cast<float>(step) / static_cast<float>(around);
                KernelVec3 ground{};
                if (navigation.snap(
                        {base.x + radius * std::cos(angle), base.y, base.z + radius * std::sin(angle)},
                        0.5f, &ground) &&
                    std::fabs(ground.y - base.y) < 2.0f) {
                    spots.push_back({ground.x, ground.y + 1.0f, ground.z});
                }
            }
        }
        return spots;
    };
    // Half at the report's riser, half under the ledge with a player up it.
    const std::vector<KernelVec3> at_riser =
        spots_around(below, static_cast<std::size_t>(squads * members));
    const std::vector<KernelVec3> at_foot =
        spots_around(foot, static_cast<std::size_t>(squads * members));
    std::size_t next_riser = 0, next_foot = 0;
    std::vector<std::vector<std::uint32_t>> groups;
    for (int squad = 0; squad < squads; ++squad) {
        std::vector<std::uint32_t> ids;
        for (int member = 0; member < members; ++member) {
            const bool riser = squad % 2 == 0;
            const std::vector<KernelVec3>& spots = riser ? at_riser : at_foot;
            std::size_t& next = riser ? next_riser : next_foot;
            if (next < spots.size()) ids.push_back(rig.create(gingerbread, spots[next++]));
        }
        groups.push_back(std::move(ids));
    }
    const std::uint32_t player = rig.create_player(ledge);
    for (int i = 0; i < 30; ++i) {
        rig.hold(player, ledge);
        rig.step(false);
    }
    for (const std::vector<std::uint32_t>& ids : groups) {
        std::vector<KernelVec3> offsets;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            offsets.push_back({-1.5f * static_cast<float>(i / 3), 0.0f,
                               1.5f * (static_cast<float>(i % 3) - 1.0f)});
        }
        gs::PatrolGroupTuning tuning;
        tuning.advance_speed_meters_per_second = 0.9f;
        rig.server->agent_runtime_manager().patrol_groups().create_group(
            1u, {cursor, {60.0f, 21.0f, 100.0f}}, cursor, ids, offsets, tuning);
    }
    for (int tick = 0; tick < 30 * seconds; ++tick) {
        rig.hold(player, ledge);
        rig.step(true);
    }
    char title[64];
    std::snprintf(title, sizeof(title), "CUT OFF %d x %d", squads, members);
    rig.samples.print(title);
    std::array<int, 7> states{};
    int on_top = 0;
    for (const gs::AgentRuntimeState& agent : rig.server->agent_runtime_manager().agents()) {
        ++states[static_cast<std::size_t>(agent.sentry.state)];
        if (agent.position.y > 22.0f) ++on_top;
    }
    std::printf(
        "  at the end: idle %d alert %d attack %d return %d chasing %d search %d, %d on top\n",
        states[0], states[1], states[2], states[3], states[4], states[5], on_top);
}

}  // namespace

int main() {
#ifdef SQUAD_BENCH_HAS_TRACE
    // The AI trace on, as a dedicated server has it; off is what the test runs do.
    if (const char* trace = std::getenv("SQUAD_BENCH_TRACE")) {
        std::string error;
        if (!gs::open_ai_trace_file(trace, 8 * 1024 * 1024, 2, &error)) std::abort();
    }
#endif
    run_queries();
    for (int run = 0; run < 3; ++run) {
        std::printf("--- run %d\n", run);
        run_cut_off(static_cast<std::uint16_t>(8100 + run), 30, 8, 12);
        run_shipped(static_cast<std::uint16_t>(8110 + run), 120);
    }
    return 0;
}
