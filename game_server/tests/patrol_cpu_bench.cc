// What the shipping catalog's patrols cost one server tick.
//
// `//game_server:agent_cpu_bench` measures a synthetic crowd of loose agents.
// This measures the population the catalog actually produces: squads spawned by
// PatrolDirector, held in formation by PatrolGroupRuntime and driven by the
// chaser controllers, with the tick split into the phases
// AgentRuntimeManager::tick runs in order.
//
//   director us     -- the tick's first actor snapshot plus PatrolDirector::tick,
//                      which is retirement every tick and a spawn only when a
//                      definition's countdown fires
//   world rule us   -- WorldRuleDirector::tick over that same snapshot, plus the
//                      game rule and spawner directors
//   sync us         -- the post-director actor snapshot and
//                      sync_agents_from_kernel over it
//   groups us       -- PatrolGroupRuntime::tick, slots and casualties
//   controllers us  -- dispatch_controllers, which is the tick's one bulk
//                      vision query plus the per-agent controller work
//   kernel us       -- Kernel_Update: movement, vision, actions, snapshot build
//   charmove us     -- the part of that which is physics::PhysicsWorld's
//                      character solver, read off KernelBenchmarkStats rather
//                      than timed here
//
// Table A runs the catalog unmodified, so its population is whatever
// `patrols:` and `patrol_budget:` currently author -- two squads of 20 to 24.
// Table B lifts `max_live_groups`, the budget and the spawn interval so the
// same code can be measured at other squad counts; the steady state it reaches
// is the same one Table A measures, because a definition at its live ceiling
// spawns nothing.
//
// The tick body is reproduced here rather than calling AgentRuntimeManager::tick
// so the phases can be attributed separately. It has to be kept in step with
// that function; there is nothing that checks it.
//
// Reported as wall clock on this host, deliberately not asserted against a
// threshold. Run it with
//   bazel run -c opt //game_server:patrol_cpu_bench

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_server/src/agent_chaser_controller.h"
#include "game_server/src/agent_runtime.h"
#include "game_server/src/agent_sentry_controller.h"
#include "game_server/src/game_rule_director.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_director.h"
#include "game_server/src/patrol_group_runtime.h"
#include "game_server/src/patrol_navigation.h"
#include "game_server/src/spawner_director.h"
#include "game_server/src/world_rule_director.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
// The phase breakdown is the whole point of this bench, and every phase is a
// private member. Same device agent_cpu_bench uses to reach into the kernel.
#define private public
#include "game_server/src/agent_runtime_manager.h"
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

namespace gs = network_example::game_server;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kServerTickRate = 30;
// Long enough for the authored 900-tick interval to fire twice, plus slack.
constexpr std::uint32_t kMaxWarmupTicks = 4000;
// A second of settling after the player appears, then ten seconds of samples.
constexpr std::uint32_t kSettleTicks = 30;
constexpr std::uint32_t kSampleTicks = 300;

// Out of every vision cone (12 m) and inside every squad's 120 m retirement
// distance, for a patrol area of +-40 m. Both halves matter: closer and the
// squads engage, further and the director retires them mid-sample.
constexpr KernelVec3 kIdlePlayerPosition{0.0f, 1.0f, 70.0f};

// Where the scaling table stops being cheap, so the attribution has something
// to attribute.
constexpr std::uint32_t kAttributionSquads = 8;

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

struct Catalog {
    gs::GameServerGameplayConfig config;
    gs::KernelGameplayCatalogStorage storage;
    std::vector<std::uint8_t> scene_bytes;
};

Catalog load_catalog() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip")
            .string());
    Catalog catalog;
    catalog.config = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(),
        static_cast<std::uint32_t>(bundle.size()),
        "gameplay_catalog.yaml");
    catalog.storage = gs::build_kernel_gameplay_catalog(catalog.config);
    catalog.scene_bytes = gs::load_gameplay_bundle_entry_bytes(
        bundle.data(),
        static_cast<std::uint32_t>(bundle.size()),
        catalog.config.static_collision_scene.entry_path);
    require(!catalog.scene_bytes.empty());
    // The parser records the navmesh's path and leaves the bytes alone, so the
    // apps read them out of the bundle themselves; a bench that skipped this
    // would measure straight chords rather than Detour routes.
    require(!catalog.config.navigation_mesh.entry_path.empty());
    catalog.config.navigation_mesh.artifact = gs::load_gameplay_bundle_entry_bytes(
        bundle.data(),
        static_cast<std::uint32_t>(bundle.size()),
        catalog.config.navigation_mesh.entry_path);
    require(!catalog.config.navigation_mesh.artifact.empty());
    return catalog;
}

struct Phases {
    double director = 0.0;
    double world_rule = 0.0;
    double sync = 0.0;
    double groups = 0.0;
    double controllers = 0.0;
    double kernel = 0.0;
    // Not timed by this bench: the kernel already accumulates it, so this is a
    // slice out of the kernel column rather than a column beside it.
    double character_move = 0.0;
    double character_moves_per_tick = 0.0;

    double ai() const {
        return director + world_rule + sync + groups + controllers;
    }
    double total() const { return ai() + kernel; }
};

KernelBenchmarkStats benchmark_stats(KernelHandle* kernel) {
    KernelBenchmarkStats stats{};
    stats.struct_size = sizeof(stats);
    require(Kernel_GetBenchmarkStats(kernel, &stats));
    return stats;
}

struct Row {
    std::uint32_t groups = 0;
    // Squad members, and every other agent in the world. The catalog's mission
    // rule puts three gingerbread nests out once a player exists and each nest
    // is a spawner, so the second number is not zero and grows across a sample.
    std::uint32_t patrol_agents = 0;
    std::uint32_t other_agents = 0;
    std::uint32_t warmup_ticks = 0;
    Phases per_tick;
};

std::uint32_t patrol_member_count(const gs::PatrolGroupRuntime& groups) {
    std::uint32_t members = 0;
    for (const gs::PatrolGroup& group : groups.groups()) {
        members += static_cast<std::uint32_t>(group.member_net_ids.size());
    }
    return members;
}

// Everything AgentRuntimeManager::tick does, in its order, with each phase
// timed. `out` may be null, which is how the warmup runs.
void tick_manager(
    gs::AgentRuntimeManager* manager,
    KernelHandle* kernel,
    Phases* out) {
    const auto director_start = std::chrono::steady_clock::now();
    gs::ActorStateView actors = manager->refresh_actor_states();
    const std::uint32_t spawned_groups_before =
        manager->patrol_director_.spawned_group_count();
    manager->patrol_director_.tick(
        kernel, &manager->patrol_groups_, &manager->patrol_navigation_, actors);
    if (manager->patrol_director_.spawned_group_count() != spawned_groups_before) {
        actors = manager->refresh_actor_states();
    }
    const double director_us = micros_since(director_start);

    const auto world_rule_start = std::chrono::steady_clock::now();
    manager->world_rule_director_.tick(
        kernel, gs::AgentRuntimeManager::live_agent_count(actors));
    manager->game_rule_director_.tick(kernel);
    manager->spawner_director_.tick(kernel);
    const double world_rule_us = micros_since(world_rule_start);

    const auto sync_start = std::chrono::steady_clock::now();
    actors = manager->refresh_actor_states();
    manager->sync_agents_from_kernel(actors);
    const double sync_us = micros_since(sync_start);

    const auto groups_start = std::chrono::steady_clock::now();
    manager->patrol_groups_.tick(
        &manager->agents_, manager->agent_index_, kTickSeconds);
    const double groups_us = micros_since(groups_start);

    const auto controllers_start = std::chrono::steady_clock::now();
    manager->dispatch_controllers(actors, kTickSeconds);
    const double controllers_us = micros_since(controllers_start);

    const auto kernel_start = std::chrono::steady_clock::now();
    Kernel_Update(kernel, kTickSeconds);
    const double kernel_us = micros_since(kernel_start);

    if (out == nullptr) {
        return;
    }
    out->director += director_us;
    out->world_rule += world_rule_us;
    out->sync += sync_us;
    out->groups += groups_us;
    out->controllers += controllers_us;
    out->kernel += kernel_us;
}

std::uint32_t spawn_player(
    KernelHandle* kernel,
    const Catalog& catalog,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = catalog.config.player.actor_template_id;
    create.actor_template_id = catalog.config.player.actor_template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    return net_id;
}

std::uint16_t next_port = 8120;

// Rebuilds the kernel-side catalog for a mutated config, keeping the scene
// bytes: a variant that changes a template has to reach the kernel too.
Catalog with_config(const Catalog& base, gs::GameServerGameplayConfig config) {
    Catalog variant;
    variant.config = std::move(config);
    variant.storage = gs::build_kernel_gameplay_catalog(variant.config);
    variant.scene_bytes = base.scene_bytes;
    return variant;
}

// Applies `mutate` to every actor and entity template named `name`. They are
// the same struct in two lists, and the kernel reads one while game_server
// reads the other.
template <typename Mutate>
void for_template(
    gs::GameServerGameplayConfig* config,
    const char* name,
    Mutate mutate) {
    bool found = false;
    for (gs::ActorTemplateConfig& actor_template : config->actor_templates) {
        if (actor_template.name == name) {
            mutate(&actor_template);
            found = true;
        }
    }
    for (gs::EntityTemplateConfig& entity_template : config->entity_templates) {
        if (entity_template.name == name) {
            mutate(&entity_template);
            found = true;
        }
    }
    require(found);
}

// `target_groups` is what the warmup waits for; the definition's own
// max_live_groups is what actually caps it, so the caller sets both.
Row measure(
    const Catalog& catalog,
    std::uint32_t target_groups,
    bool engaged) {
    const gs::GameServerGameplayConfig& config = catalog.config;
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = kServerTickRate;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 1024;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);

    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene));
    require(Kernel_StartDedicatedServer(kernel, next_port++));
    require(Kernel_LoadGameplayCatalog(kernel, &catalog.storage.definition, nullptr));

    gs::AgentRuntimeManager manager(kernel, config);

    // No player during the warmup on purpose: with one present the director's
    // distance rule would retire squads the moment a route wandered past 120 m,
    // and the population being warmed up to would never settle.
    Row row;
    std::uint32_t tick = 0;
    for (; tick < kMaxWarmupTicks; ++tick) {
        tick_manager(&manager, kernel, nullptr);
        if (manager.patrol_groups_.groups().size() >= target_groups) {
            break;
        }
    }
    row.warmup_ticks = tick;
    require(manager.patrol_groups_.groups().size() >= target_groups);

    const KernelVec3 player_position = engaged
        ? manager.patrol_groups_.groups().front().cursor
        : kIdlePlayerPosition;
    spawn_player(kernel, catalog, KernelVec3{
        player_position.x, player_position.y + 1.0f, player_position.z});
    for (std::uint32_t settle = 0; settle < kSettleTicks; ++settle) {
        tick_manager(&manager, kernel, nullptr);
    }

    Phases totals;
    // Cumulative since the kernel started, so the window is the difference.
    const KernelBenchmarkStats before = benchmark_stats(kernel);
    for (std::uint32_t sample = 0; sample < kSampleTicks; ++sample) {
        tick_manager(&manager, kernel, &totals);
    }
    const KernelBenchmarkStats after = benchmark_stats(kernel);

    const auto divide = [](double value) {
        return value / static_cast<double>(kSampleTicks);
    };
    row.groups = static_cast<std::uint32_t>(manager.patrol_groups_.groups().size());
    row.patrol_agents = patrol_member_count(manager.patrol_groups_);
    row.other_agents =
        static_cast<std::uint32_t>(manager.agents_.size()) - row.patrol_agents;
    row.per_tick.director = divide(totals.director);
    row.per_tick.world_rule = divide(totals.world_rule);
    row.per_tick.sync = divide(totals.sync);
    row.per_tick.groups = divide(totals.groups);
    row.per_tick.controllers = divide(totals.controllers);
    row.per_tick.kernel = divide(totals.kernel);
    row.per_tick.character_move = divide(static_cast<double>(
        after.character_move_cost_us - before.character_move_cost_us));
    row.per_tick.character_moves_per_tick = divide(static_cast<double>(
        after.character_move_count - before.character_move_count));

    Kernel_Destroy(kernel);
    return row;
}

void print_header(const char* first_column) {
    std::printf(
        "%10s %7s %7s %6s %9s %9s %8s %8s %12s %10s %9s %11s %8s %10s\n",
        first_column,
        "squads",
        "patrol",
        "other",
        "director",
        "worldrule",
        "sync",
        "groups",
        "controllers",
        "ai us",
        "kernel us",
        "charmove us",
        "moves",
        "% of 33ms");
}

void print_row(const char* label, const Row& row) {
    std::printf(
        "%10s %7u %7u %6u %9.1f %9.1f %8.1f %8.1f %12.1f %10.1f %9.1f %11.1f "
        "%8.0f %10.1f\n",
        label,
        row.groups,
        row.patrol_agents,
        row.other_agents,
        row.per_tick.director,
        row.per_tick.world_rule,
        row.per_tick.sync,
        row.per_tick.groups,
        row.per_tick.controllers,
        row.per_tick.ai(),
        row.per_tick.kernel,
        row.per_tick.character_move,
        row.per_tick.character_moves_per_tick,
        row.per_tick.total() / (1'000'000.0 / kServerTickRate) * 100.0);
}

}  // namespace

int main() {
    const Catalog catalog = load_catalog();
    require(!catalog.config.patrols.empty());
    const gs::PatrolDefinitionConfig& authored = catalog.config.patrols.front();
    std::printf(
        "catalog patrols=%zu definition=%s count=%u-%u max_live_groups=%u "
        "interval_ticks=%u budget=%u navmesh=%s\n",
        catalog.config.patrols.size(),
        authored.name.c_str(),
        authored.count_min,
        authored.count_max,
        authored.max_live_groups,
        authored.interval_ticks,
        catalog.config.patrol_budget.max_live_agents,
        catalog.config.navigation_mesh.artifact.empty() ? "absent" : "loaded");
    // The vision view's size is what the tick's one bulk vision query copies
    // per agent -- the cost the per-agent query traded away along with its
    // quadratic scan.
    std::printf(
        "sizeof(AgentRuntimeState)=%zu sizeof(KernelServerEntityState)=%zu "
        "sizeof(KernelVisionStateView)=%zu\n\n",
        sizeof(gs::AgentRuntimeState),
        sizeof(KernelServerEntityState),
        sizeof(KernelVisionStateView));

    std::printf(
        "A. shipping catalog, unmodified -- %u sample ticks at %u Hz\n",
        kSampleTicks,
        kServerTickRate);
    print_header("scenario");
    print_row("idle", measure(catalog, authored.max_live_groups, false));
    print_row("engaged", measure(catalog, authored.max_live_groups, true));
    std::printf("\n");

    std::printf(
        "B. squad-count scaling -- ceilings and interval lifted, idle player\n");
    print_header("squads");
    for (const std::uint32_t squads : {1u, 2u, 4u, 8u}) {
        gs::GameServerGameplayConfig config = catalog.config;
        config.patrol_budget.max_live_agents = 0;
        for (gs::PatrolDefinitionConfig& definition : config.patrols) {
            definition.max_live_groups = squads;
            // Only changes how fast the warmup fills; a definition sitting at
            // its ceiling spawns nothing either way.
            definition.interval_ticks = 30;
            // Routes are ~100 m at 0.9 m/s, so nothing finishes inside a
            // sample -- but a squad that did would be replaced mid-table.
            definition.despawn_linger_ticks = 100000;
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%u", squads);
        print_row(label, measure(with_config(catalog, config), squads, false));
    }
    std::printf("\n");

    // Where the kernel column goes. Both variants are wrong as gameplay -- an
    // agent with no cone never sees anything, and agents that do not collide
    // walk through each other -- so these are attribution, not proposals.
    std::printf(
        "C. kernel cost attribution at %u squads -- idle player\n", kAttributionSquads);
    print_header("variant");
    const auto attribution_config = [&](bool vision, bool actor_collision) {
        gs::GameServerGameplayConfig config = catalog.config;
        config.patrol_budget.max_live_agents = 0;
        for (gs::PatrolDefinitionConfig& definition : config.patrols) {
            definition.max_live_groups = kAttributionSquads;
            definition.interval_ticks = 30;
            definition.despawn_linger_ticks = 100000;
        }
        for (const char* name : {"gingerbread", "gingerbread_warrior"}) {
            for_template(&config, name, [&](gs::ActorTemplateConfig* actor_template) {
                if (!vision) {
                    actor_template->vision.vision_collider_template_id = 0u;
                }
                if (!actor_collision) {
                    actor_template->movement_collision_mask =
                        KERNEL_MOVEMENT_LAYER_TERRAIN |
                        KERNEL_MOVEMENT_LAYER_STATIC_OBSTACLE;
                }
            });
        }
        return config;
    };
    print_row(
        "baseline",
        measure(
            with_config(catalog, attribution_config(true, true)),
            kAttributionSquads,
            false));
    print_row(
        "no vision",
        measure(
            with_config(catalog, attribution_config(false, true)),
            kAttributionSquads,
            false));
    print_row(
        "no push",
        measure(
            with_config(catalog, attribution_config(true, false)),
            kAttributionSquads,
            false));
    print_row(
        "neither",
        measure(
            with_config(catalog, attribution_config(false, false)),
            kAttributionSquads,
            false));
    std::printf("\n");

    // Whether the push cost above is the formation packing its own members
    // together. Spacing is authored, so this is the one lever that is a catalog
    // edit rather than a code change.
    std::printf(
        "D. formation spacing at %u squads -- idle player\n", kAttributionSquads);
    print_header("spacing m");
    for (const float spacing : {1.5f, 3.0f, 6.0f}) {
        gs::GameServerGameplayConfig config = attribution_config(true, true);
        for (gs::PatrolDefinitionConfig& definition : config.patrols) {
            definition.formation_spacing_meters = spacing;
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%.1f", static_cast<double>(spacing));
        print_row(
            label,
            measure(with_config(catalog, config), kAttributionSquads, false));
    }
    std::printf("\n");
    return 0;
}
