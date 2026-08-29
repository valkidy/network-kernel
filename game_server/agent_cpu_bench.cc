// What a crowd of agents costs one server tick.
//
// Every CPU claim about agent density so far has been read off the source
// rather than measured. This measures it: N chaser_grunt agents and one player
// inside the same 40 m the relevance sphere uses, driven the way the dedicated
// server drives them, with the tick split into the two halves that can be
// attributed separately.
//
//   ai us      -- the chaser controllers over a plain agent vector
//   kernel us  -- Kernel_Update: movement, vision, actions, snapshot build
//
// The NO AI table drops the controllers, so its kernel column is the cost of
// the agents merely existing. The MANAGER table swaps the bare controllers for
// AgentRuntimeManager, which is the shipping path -- and which stops seeing
// agents past kMaxQueriedAgents.
//
// Two scenarios per table. IDLE parks the player far outside every vision cone,
// so the agents patrol. ENGAGED puts the player at the origin, so whichever
// agents have the player in their cone chase and fire; those agents converge
// over time, so the sample window is kept short enough that the crowd is still
// spread out.
//
// Reported as wall clock on this host, deliberately not asserted against a
// threshold: a timing bound would be flaky in CI and would not survive a
// different machine. Run it with
//   bazel run -c opt //game_server:agent_cpu_bench

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "game_server/agent_chaser_controller.h"
#include "game_server/agent_runtime.h"
#include "game_server/agent_runtime_manager.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "protocol/public/network_packets.h"
#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kServerTickRate = 30;
// One second of settling, then four seconds of samples. Four seconds is short
// enough that chasers starting 40 m out have not yet piled onto the player.
constexpr std::uint32_t kWarmupTicks = 30;
constexpr std::uint32_t kSampleTicks = 120;
constexpr float kRelevanceRadiusMeters = 40.0f;
constexpr std::size_t kQueryCapacity = 1024;

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

double micros_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// The same deterministic spread the snapshot benchmark uses, so the two
// measurements describe the same synthetic crowd.
KernelVec3 agent_position(std::size_t index, std::size_t count) {
    const double golden = 2.399963229728653;
    const double t = static_cast<double>(index) /
        static_cast<double>(count == 0 ? 1 : count);
    const double radius = kRelevanceRadiusMeters * 0.9 * std::sqrt(t);
    const double angle = static_cast<double>(index) * golden;
    return KernelVec3{
        static_cast<float>(radius * std::cos(angle)),
        1.0f,
        static_cast<float>(radius * std::sin(angle))};
}

const network_example::game_server::ActorTemplateConfig* find_template(
    const std::vector<network_example::game_server::ActorTemplateConfig>& templates,
    const std::string& name) {
    for (const network_example::game_server::ActorTemplateConfig& config : templates) {
        if (config.name == name) {
            return &config;
        }
    }
    return nullptr;
}

enum class Drive {
    kNone,
    kController,
    kManager,
};

struct Row {
    std::size_t agent_count = 0;
    double ai_us = 0.0;
    double kernel_us = 0.0;
    std::uint32_t live_entities = 0;
    std::uint32_t agents_driven = 0;
};

struct Catalog {
    network_example::game_server::GameServerGameplayConfig config;
    network_example::game_server::KernelGameplayCatalogStorage storage;
    std::vector<std::uint8_t> scene_bytes;
};

Catalog load_catalog() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip")
            .string());
    Catalog catalog;
    catalog.config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "gameplay_catalog.yaml");
    catalog.storage =
        network_example::game_server::build_kernel_gameplay_catalog(catalog.config);
    catalog.scene_bytes =
        network_example::game_server::load_gameplay_bundle_entry_bytes(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            catalog.config.static_collision_scene.entry_path);
    require(!catalog.scene_bytes.empty());
    return catalog;
}

std::uint32_t spawn_actor(
    KernelHandle* kernel,
    std::uint32_t template_id,
    std::uint16_t actor_type,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = actor_type;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

Row measure(
    const Catalog& catalog,
    std::size_t agent_count,
    Drive drive,
    bool engaged,
    std::uint16_t port) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = kServerTickRate;
    config.tick.snapshot_rate = 15;
    config.max_events = 4096;
    config.max_render_states = 1024;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene));
    require(Kernel_StartDedicatedServer(kernel, port));
    require(Kernel_LoadGameplayCatalog(kernel, &catalog.storage.definition, nullptr));

    const network_example::game_server::ActorTemplateConfig* chaser =
        find_template(catalog.config.actor_templates, "chaser_grunt");
    require(chaser != nullptr);
    const std::uint32_t player_template_id =
        catalog.config.player.actor_template_id;
    require(player_template_id != 0);

    // Far enough that no cone reaches it, without leaving the ground plane.
    const KernelVec3 player_position = engaged
        ? KernelVec3{0.0f, 1.0f, 0.0f}
        : KernelVec3{90.0f, 1.0f, 90.0f};
    spawn_actor(
        kernel,
        player_template_id,
        network_example::game_server::kActorTypePlayer,
        player_position);

    std::vector<network_example::game_server::AgentRuntimeState> agents;
    agents.reserve(agent_count);
    for (std::size_t index = 0; index < agent_count; ++index) {
        const KernelVec3 position = agent_position(index, agent_count);
        network_example::game_server::AgentRuntimeState agent;
        agent.net_id = spawn_actor(
            kernel,
            chaser->actor_template_id,
            network_example::game_server::kActorTypeAgent,
            position);
        agent.actor_template_id = chaser->actor_template_id;
        agent.position = position;
        agent.patrol_anchor = position;
        agent.sentry_config = chaser->sentry;
        agents.push_back(agent);
    }

    network_example::game_server::AgentChaserConfig chaser_config;
    chaser_config.sentry = chaser->sentry;
    chaser_config.chase = chaser->chaser;
    const network_example::game_server::AgentChaserController controller(
        chaser_config);
    network_example::game_server::AgentRuntimeManager manager(
        kernel,
        catalog.config);

    const auto drive_ai = [&]() {
        switch (drive) {
            case Drive::kNone:
                break;
            case Drive::kController:
                controller.tick(kernel, &agents, kTickSeconds);
                break;
            case Drive::kManager:
                manager.tick(kTickSeconds);
                break;
        }
    };

    for (std::uint32_t tick = 0; tick < kWarmupTicks; ++tick) {
        drive_ai();
        Kernel_Update(kernel, kTickSeconds);
    }

    double ai_total_us = 0.0;
    double kernel_total_us = 0.0;
    for (std::uint32_t tick = 0; tick < kSampleTicks; ++tick) {
        const auto ai_start = std::chrono::steady_clock::now();
        drive_ai();
        ai_total_us += micros_since(ai_start);
        const auto kernel_start = std::chrono::steady_clock::now();
        Kernel_Update(kernel, kTickSeconds);
        kernel_total_us += micros_since(kernel_start);
    }

    std::vector<KernelServerEntityState> states(kQueryCapacity);
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    Row row;
    row.agent_count = agent_count;
    row.ai_us = ai_total_us / static_cast<double>(kSampleTicks);
    row.kernel_us = kernel_total_us / static_cast<double>(kSampleTicks);
    row.live_entities = Kernel_ServerQueryEntities(
        kernel,
        0u,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    row.agents_driven = drive == Drive::kManager
        ? static_cast<std::uint32_t>(manager.agent_count())
        : static_cast<std::uint32_t>(agents.size());

    Kernel_Destroy(kernel);
    return row;
}

std::uint16_t next_port = 7920;

void print_table(const Catalog& catalog, const char* title, Drive drive, bool engaged) {
    std::printf("%s\n", title);
    std::printf(
        "%7s %9s %10s %11s %11s %10s\n",
        "agents", "driven", "entities", "ai us", "kernel us", "% of 33ms");
    for (const std::size_t agent_count : {0u, 50u, 100u, 200u, 400u, 800u}) {
        const Row row =
            measure(catalog, agent_count, drive, engaged, next_port++);
        const double total_us = row.ai_us + row.kernel_us;
        std::printf(
            "%7zu %9u %10u %11.1f %11.1f %10.1f\n",
            row.agent_count,
            row.agents_driven,
            row.live_entities,
            row.ai_us,
            row.kernel_us,
            total_us / (1'000'000.0 / kServerTickRate) * 100.0);
    }
    std::printf("\n");
}

// The one cost the tables above cannot see. A dedicated server with no client
// connected never builds a snapshot, so the kernel column excludes the whole
// per-session path -- and that path is the one that walks every entity. Timed
// here against a synthetic session, at the snapshot rate rather than the tick
// rate.
struct SnapshotRow {
    std::size_t agent_count = 0;
    double build_us = 0.0;
    std::size_t sent_entities = 0;
};

SnapshotRow measure_snapshot_build(
    const Catalog& catalog,
    std::size_t agent_count,
    std::size_t session_count,
    std::uint16_t port) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = kServerTickRate;
    config.tick.snapshot_rate = 15;
    config.max_events = 4096;
    config.max_render_states = 1024;
    network_example::KernelEngine engine(config);

    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    bool scene_rejected = true;
    require(engine.load_gameplay_catalog_with_static_collision_scene(
        catalog.storage.definition, scene, &scene_rejected));
    require(!scene_rejected);
    require(engine.start_dedicated_server(port));

    const network_example::game_server::ActorTemplateConfig* chaser =
        find_template(catalog.config.actor_templates, "chaser_grunt");
    require(chaser != nullptr);

    const auto spawn = [&](std::uint32_t template_id,
                           std::uint16_t actor_type,
                           const KernelVec3& position) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = network_example::game_server::kEntityTypeActor;
        create.actor_type = actor_type;
        create.entity_template_id = template_id;
        create.actor_template_id = template_id;
        create.position = position;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(engine.server_create_entity(create, &net_id));
        return net_id;
    };

    std::vector<std::uint32_t> players;
    for (std::size_t index = 0; index < session_count; ++index) {
        // Kept close together, so every session finds every agent relevant.
        // That is the worst case for per-session work and the one a party
        // fighting the same crowd actually produces.
        players.push_back(spawn(
            catalog.config.player.actor_template_id,
            network_example::game_server::kActorTypePlayer,
            KernelVec3{static_cast<float>(index) * 2.0f, 1.0f, 0.0f}));
    }
    for (std::size_t index = 0; index < agent_count; ++index) {
        spawn(
            chaser->actor_template_id,
            network_example::game_server::kActorTypeAgent,
            agent_position(index, agent_count));
    }
    for (std::uint32_t tick = 0; tick < kWarmupTicks; ++tick) {
        engine.update(kTickSeconds);
    }

    std::vector<network_example::KernelEngine::PeerSession> sessions;
    for (std::size_t index = 0; index < players.size(); ++index) {
        sessions.push_back(network_example::KernelEngine::PeerSession{
            static_cast<std::uint32_t>(index + 1), players[index], 0, true, {}});
    }
    double total_us = 0.0;
    std::size_t sent_entities = 0;
    for (std::uint32_t sample = 0; sample < kSampleTicks; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        for (network_example::KernelEngine::PeerSession& session : sessions) {
            const network_example::WorldSnapshot relevant =
                engine.build_relevant_snapshot(session, sample * 66u);
            const network_example::WorldSnapshot send =
                engine.build_snapshot_send_set(session, relevant, 1200u);
            sent_entities = send.entities.size();
        }
        total_us += micros_since(start);
    }

    SnapshotRow row;
    row.agent_count = agent_count;
    row.build_us = total_us / static_cast<double>(kSampleTicks);
    row.sent_entities = sent_entities;
    return row;
}

// KernelHandle is opaque in the public header but is only a unique_ptr to the
// engine (kernel_api.cc). Mirrored here so that one world can be measured on
// both sides of the API at once -- the C API for the shipping AI path, the
// engine for the per-session snapshot path -- rather than running the same
// configuration twice and hoping the two runs matched.
struct KernelHandleLayout {
    std::unique_ptr<network_example::KernelEngine> engine;
};

network_example::KernelEngine& engine_of(KernelHandle* handle) {
    return *reinterpret_cast<KernelHandleLayout*>(handle)->engine;
}

struct ScenarioRow {
    const char* name = "";
    std::size_t world_agents = 0;
    std::size_t relevant_per_session = 0;
    std::size_t packed_per_snapshot = 0;
    double ai_us = 0.0;
    double kernel_us = 0.0;
    double snapshot_us = 0.0;
};

// Both four-player shapes, with the same thing held constant: each player has
// roughly the same number of agents in view. What differs is how many the world
// holds -- one shared crowd, or one crowd per player.
ScenarioRow measure_scenario(
    const Catalog& catalog,
    const char* name,
    const std::vector<KernelVec3>& cluster_centres,
    const std::vector<std::size_t>& agents_per_cluster,
    std::uint16_t port) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = kServerTickRate;
    config.tick.snapshot_rate = 15;
    config.max_events = 8192;
    config.max_render_states = 4096;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene));
    require(Kernel_StartDedicatedServer(kernel, port));
    require(Kernel_LoadGameplayCatalog(kernel, &catalog.storage.definition, nullptr));

    const network_example::game_server::ActorTemplateConfig* chaser =
        find_template(catalog.config.actor_templates, "chaser_grunt");
    require(chaser != nullptr);

    std::vector<std::uint32_t> players;
    std::size_t world_agents = 0;
    for (std::size_t cluster = 0; cluster < cluster_centres.size(); ++cluster) {
        const KernelVec3 centre = cluster_centres[cluster];
        players.push_back(spawn_actor(
            kernel,
            catalog.config.player.actor_template_id,
            network_example::game_server::kActorTypePlayer,
            KernelVec3{centre.x, 1.0f, centre.z}));
        const std::size_t cluster_agents = agents_per_cluster[cluster];
        for (std::size_t index = 0; index < cluster_agents; ++index) {
            // The same spread the other tables use, re-centred on this cluster
            // and kept inside the relevance radius of its own player only.
            const KernelVec3 offset = agent_position(index, cluster_agents);
            spawn_actor(
                kernel,
                chaser->actor_template_id,
                network_example::game_server::kActorTypeAgent,
                KernelVec3{
                    centre.x + offset.x * 0.9f,
                    1.0f,
                    centre.z + offset.z * 0.9f});
            ++world_agents;
        }
    }

    network_example::game_server::AgentRuntimeManager manager(
        kernel, catalog.config);
    network_example::KernelEngine& engine = engine_of(kernel);

    // Held outside the engine's own session list on purpose, so that
    // Kernel_Update does not also build these snapshots and land the same work
    // in two columns.
    std::vector<network_example::KernelEngine::PeerSession> sessions;
    for (std::size_t index = 0; index < players.size(); ++index) {
        sessions.push_back(network_example::KernelEngine::PeerSession{
            static_cast<std::uint32_t>(index + 1), players[index], 0, true, {}});
    }

    for (std::uint32_t tick = 0; tick < kWarmupTicks; ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }

    double ai_us = 0.0;
    double kernel_us = 0.0;
    double snapshot_us = 0.0;
    std::size_t relevant_per_session = 0;
    std::size_t packed_per_snapshot = 0;
    for (std::uint32_t tick = 0; tick < kSampleTicks; ++tick) {
        const auto ai_start = std::chrono::steady_clock::now();
        manager.tick(kTickSeconds);
        ai_us += micros_since(ai_start);
        const auto kernel_start = std::chrono::steady_clock::now();
        Kernel_Update(kernel, kTickSeconds);
        kernel_us += micros_since(kernel_start);

        const auto snapshot_start = std::chrono::steady_clock::now();
        std::size_t relevant_agents = 0;
        std::size_t packed_agents = 0;
        for (network_example::KernelEngine::PeerSession& session : sessions) {
            const network_example::WorldSnapshot relevant =
                engine.build_relevant_snapshot(session, tick * 66u);
            const network_example::WorldSnapshot send =
                engine.build_snapshot_send_set(session, relevant, 1200u);
            if (&session == &sessions.front()) {
                for (const network_example::EntitySnapshot& entity :
                     relevant.entities) {
                    if (entity.actor_type ==
                        network_example::ActorType::kAgent) {
                        ++relevant_agents;
                    }
                }
                for (const network_example::EntitySnapshot& entity :
                     send.entities) {
                    if (entity.actor_type ==
                        network_example::ActorType::kAgent) {
                        ++packed_agents;
                    }
                }
            }
        }
        snapshot_us += micros_since(snapshot_start);
        relevant_per_session = relevant_agents;
        packed_per_snapshot = packed_agents;
    }

    ScenarioRow row;
    row.name = name;
    row.world_agents = world_agents;
    row.relevant_per_session = relevant_per_session;
    row.packed_per_snapshot = packed_per_snapshot;
    row.ai_us = ai_us / static_cast<double>(kSampleTicks);
    row.kernel_us = kernel_us / static_cast<double>(kSampleTicks);
    row.snapshot_us = snapshot_us / static_cast<double>(kSampleTicks);

    Kernel_Destroy(kernel);
    return row;
}

void print_scenario_table(const Catalog& catalog) {
    std::printf("F. FOUR PLAYERS: one shared crowd vs one crowd each\n");
    std::printf(
        "%-26s %8s %10s %9s %10s %11s %12s %10s %11s\n",
        "scenario", "world", "relevant", "packed", "ai us", "kernel us",
        "snapshot us", "% of 33ms", "stale s");
    const std::vector<KernelVec3> together{
        KernelVec3{0.0f, 0.0f, 0.0f},
        KernelVec3{2.0f, 0.0f, 0.0f},
        KernelVec3{0.0f, 0.0f, 2.0f},
        KernelVec3{2.0f, 0.0f, 2.0f}};
    // Far enough apart that no player is inside another cluster's relevance
    // radius, and still on the 200 x 200 ground plane.
    const std::vector<KernelVec3> apart{
        KernelVec3{-70.0f, 0.0f, -70.0f},
        KernelVec3{70.0f, 0.0f, -70.0f},
        KernelVec3{-70.0f, 0.0f, 70.0f},
        KernelVec3{70.0f, 0.0f, 70.0f}};
    const ScenarioRow rows[] = {
        // Four players standing in one crowd of 200: the clusters overlap, so
        // every agent is relevant to every session.
        measure_scenario(
            catalog, "A. party, shared 200",
            together, {50, 50, 50, 50}, next_port++),
        measure_scenario(
            catalog, "B. spread, 200 each",
            apart, {200, 200, 200, 200}, next_port++),
        // A global population cap instead of a per-view one: the world holds
        // 200 however the players are arranged. Split evenly, and then all of
        // it in front of one player, which is the case the cap is written for.
        measure_scenario(
            catalog, "C1. spread, capped, even",
            apart, {50, 50, 50, 50}, next_port++),
        measure_scenario(
            catalog, "C2. spread, capped, all on one",
            apart, {200, 0, 0, 0}, next_port++),
    };
    for (const ScenarioRow& row : rows) {
        const double total_us = row.ai_us + row.kernel_us + row.snapshot_us;
        const double stale = row.packed_per_snapshot == 0
            ? 0.0
            : std::ceil(
                  static_cast<double>(row.relevant_per_session) /
                  static_cast<double>(row.packed_per_snapshot)) /
                15.0;
        std::printf(
            "%-26s %8zu %10zu %9zu %10.1f %11.1f %12.1f %10.1f %11.2f\n",
            row.name,
            row.world_agents,
            row.relevant_per_session,
            row.packed_per_snapshot,
            row.ai_us,
            row.kernel_us,
            row.snapshot_us,
            total_us / (1'000'000.0 / kServerTickRate) * 100.0,
            stale);
    }
    std::printf("\n");
}

void print_snapshot_table(const Catalog& catalog, std::size_t session_count) {
    std::printf(
        "E. SNAPSHOT BUILD, %zu session(s) (not included in the tables above)\n",
        session_count);
    std::printf(
        "%7s %10s %14s %18s\n",
        "agents", "sent", "per build us", "% of 33ms at 15Hz");
    for (const std::size_t agent_count : {0u, 50u, 100u, 200u, 400u, 800u}) {
        const SnapshotRow row = measure_snapshot_build(
            catalog, agent_count, session_count, next_port++);
        // Two snapshots per three ticks at 15 Hz on a 30 Hz tick, so half a
        // build lands on the average tick.
        const double per_tick_us = row.build_us * 15.0 / kServerTickRate;
        std::printf(
            "%7zu %10zu %14.1f %18.1f\n",
            row.agent_count,
            row.sent_entities,
            row.build_us,
            per_tick_us / (1'000'000.0 / kServerTickRate) * 100.0);
    }
    std::printf("\n");
}

}  // namespace

int main() {
    const Catalog catalog = load_catalog();
    std::printf(
        "tick_rate=%u Hz  budget=%.1f ms/tick  warmup=%u ticks  sample=%u ticks\n\n",
        kServerTickRate,
        1000.0 / static_cast<double>(kServerTickRate),
        kWarmupTicks,
        kSampleTicks);
    print_table(catalog, "A. NO AI, idle", Drive::kNone, false);
    print_table(catalog, "B. CONTROLLER, idle", Drive::kController, false);
    print_table(catalog, "C. CONTROLLER, engaged", Drive::kController, true);
    print_table(catalog, "D. MANAGER (shipping path), engaged", Drive::kManager, true);
    print_snapshot_table(catalog, 1);
    print_snapshot_table(catalog, 4);
    print_scenario_table(catalog);
    return 0;
}
