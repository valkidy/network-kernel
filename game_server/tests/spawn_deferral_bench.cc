// What kMaxEntitySpawnsPerSnapshot costs an entity that is born mid-fight.
//
// A session cannot use a snapshot record for a net id it has never been given a
// spawn for -- handle_client_snapshot looks the entity up and skips it -- so
// every newcomer waits for a reliable EntitySpawn first. sync_session_relevance
// sends at most kMaxEntitySpawnsPerSnapshot of those per snapshot, nearest
// first, and defers the rest to later snapshots. A crowd that keeps creating
// entities can therefore produce newcomers faster than the introduction quota
// clears them, and a short-lived entity can expire while still queued.
//
// A grenade line is that crowd: every shot is a shell, every impact is an
// explosion, and the explosion is what the player sees as the impact. It lives
// 45 ticks. Anything spent waiting for an introduction is time it is not drawn.
//
// Measured on the real pipeline rather than on a model of it -- a loopback
// transport in place of the server's own, a welcomed session, and the engine's
// own publish_snapshot driving relevance -- so the numbers come from
// sync_session_relevance itself. The clock is the tick an entity was created in
// the world; the reading is the tick its EntitySpawn reached the wire.
//
// Reported as counts and tick deltas, deliberately not asserted: this measures
// a policy against a workload, and both are meant to be tuned rather than
// pinned. Run it with
//   bazel run -c opt //game_server:spawn_deferral_bench

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "transport/public/loopback_transport.h"
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
constexpr std::uint32_t kSnapshotRate = 15;
// Long enough for every sentry to acquire the player and settle into its fire
// cycle, so the sample window measures a running fight rather than the opening
// burst.
constexpr std::uint32_t kWarmupTicks = 90;
// Ten seconds. The interesting quantity is a steady-state rate, and a grenade
// cycle is a couple of seconds, so this covers several per sentry.
constexpr std::uint32_t kSampleTicks = 300;
// The director's own wave_1 shape: one cluster, radius 20 m, player at origin.
constexpr float kClusterRadiusMeters = 20.0f;

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

// The same deterministic spread the other benches use, so the crowd shape is
// comparable across them.
KernelVec3 agent_position(std::size_t index, std::size_t count) {
    const double golden = 2.399963229728653;
    const double t = static_cast<double>(index) /
        static_cast<double>(count == 0 ? 1 : count);
    const double radius = kClusterRadiusMeters * std::sqrt(t);
    const double angle = static_cast<double>(index) * golden;
    return KernelVec3{
        static_cast<float>(radius * std::cos(angle)),
        1.0f,
        static_cast<float>(radius * std::sin(angle))};
}

const network_example::game_server::ActorTemplateConfig* find_actor_template(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const network_example::game_server::ActorTemplateConfig& actor :
         config.actor_templates) {
        if (actor.name == name) {
            return &actor;
        }
    }
    return nullptr;
}

std::uint32_t find_projectile_template_id(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const network_example::game_server::ProjectileTemplateConfig& projectile :
         config.projectile_templates) {
        if (projectile.name == name) {
            return projectile.definition.projectile_template_id;
        }
    }
    return 0u;
}

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

struct KernelHandleLayout {
    std::unique_ptr<network_example::KernelEngine> engine;
};

network_example::KernelEngine& engine_of(KernelHandle* handle) {
    return *reinterpret_cast<KernelHandleLayout*>(handle)->engine;
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

// What an entity is, for the purpose of reading the table. Shells and
// explosions are separated because only one of them is what the player reads as
// the impact.
enum class Kind {
    kOther,
    kShell,
    kExplosion,
};

struct Life {
    bool ever_relevant = false;
    std::uint32_t created_tick = 0;
    // Distance from the player when it was created. sync_session_relevance
    // sorts newcomers nearest-first before applying the quota, so this is the
    // variable that decides who gets deferred.
    float distance_m = 0.0f;
    std::uint32_t introduced_tick = 0;
    bool introduced = false;
    bool destroyed = false;
    Kind kind = Kind::kOther;
};

// Deferral for one class of entity. Split out because shells and explosions are
// born in different places -- a shell at the sentry that fired it, an explosion
// where that shell landed -- and the introduction queue is ordered by exactly
// that difference.
struct KindStats {
    std::size_t created = 0;
    // The largest number of this kind created in a single tick, and how many
    // ticks created more than the per-snapshot introduction quota can clear.
    // Two kinds with the same mean rate can put very different pressure on a
    // queue if one of them arrives in clumps.
    std::size_t max_created_in_one_tick = 0;
    std::size_t burst_ticks = 0;
    std::size_t unseen = 0;
    std::uint32_t p50 = 0;
    std::uint32_t p90 = 0;
    std::uint32_t max = 0;
    double mean_distance_m = 0.0;
};

struct Row {
    std::size_t agent_count = 0;
    KindStats shell;
    KindStats explosion;
    std::size_t created = 0;
    std::size_t introduced = 0;
    std::size_t unseen = 0;
    double created_per_second = 0.0;
    std::uint32_t deferral_p50 = 0;
    std::uint32_t deferral_p90 = 0;
    std::uint32_t deferral_max = 0;
    std::size_t explosions_created = 0;
    std::size_t explosions_unseen = 0;
    // Of the explosions never introduced, how many were never relevant to the
    // session in the first place. That separates "lost the introduction queue"
    // from "the session was never told it should care".
    std::size_t explosions_unseen_never_relevant = 0;
    // Mean creation distance split by outcome. The relevance radius is 40 m, so
    // if the unseen explosions sit beyond it they were never the session's to
    // see and nothing is being starved.
    double explosion_seen_distance_m = 0.0;
    double explosion_unseen_distance_m = 0.0;
    double explosion_visible_fraction = 0.0;
    // Measured rather than read off kMaxEntitySpawnsPerSnapshot, which lives in
    // an anonymous namespace in kernel.cc. If this column stops rising with the
    // crowd, the quota is what is holding it.
    std::size_t max_introductions_in_one_snapshot = 0;
    // Every EntitySpawn that reached the wire, against the number of distinct
    // entities they introduced. sync_session_relevance rebuilds
    // relevant_entities from scratch each snapshot, so an entity that drops out
    // of the relevant set for one snapshot has to be introduced again. If these
    // two diverge, the quota is being spent on re-introductions rather than on
    // newcomers, and a genuine newcomer is queueing behind that churn.
    std::size_t spawn_packets = 0;
    std::size_t distinct_introduced = 0;
};

std::uint32_t percentile(std::vector<std::uint32_t> values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(
        values.size() - 1,
        static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

Row measure(const Catalog& catalog, std::size_t agent_count, std::uint16_t port) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = kServerTickRate;
    config.tick.snapshot_rate = kSnapshotRate;
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

    network_example::KernelEngine& engine = engine_of(kernel);
    // Swapped in after the server is running, so the engine keeps `running_`
    // but everything it sends lands somewhere this bench can read.
    auto loopback = std::make_unique<network_example::LoopbackTransport>();
    network_example::LoopbackTransport* link = loopback.get();
    engine.transport_ = std::move(loopback);
    require(link->StartServer(port + 1u));

    const network_example::game_server::ActorTemplateConfig* sentry =
        find_actor_template(catalog.config, "grenade_sentry");
    require(sentry != nullptr);
    const std::uint32_t shell_template_id =
        find_projectile_template_id(catalog.config, "grenade_shell_projectile");
    const std::uint32_t explosion_template_id =
        find_projectile_template_id(catalog.config, "rocket_explosion");
    require(shell_template_id != 0u);
    require(explosion_template_id != 0u);

    const std::uint32_t player = spawn_actor(
        kernel,
        catalog.config.player.actor_template_id,
        network_example::game_server::kActorTypePlayer,
        KernelVec3{0.0f, 1.0f, 0.0f});
    for (std::size_t index = 0; index < agent_count; ++index) {
        spawn_actor(
            kernel,
            sentry->actor_template_id,
            network_example::game_server::kActorTypeAgent,
            agent_position(index, agent_count));
    }

    network_example::game_server::AgentRuntimeManager manager(
        kernel, catalog.config);
    engine.peer_sessions_.push_back(
        network_example::KernelEngine::PeerSession{1, player, 0, true, {}});

    std::unordered_map<network_example::NetId, Life> lives;
    std::unordered_set<network_example::NetId> present;
    std::unordered_map<std::uint32_t, std::size_t> shell_creations_by_tick;
    std::unordered_map<std::uint32_t, std::size_t> explosion_creations_by_tick;

    const auto classify = [&](network_example::NetId net_id) {
        const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
        if (!entity.has_value() ||
            !engine.world_.registry().all_of<network_example::ProjectileState>(
                *entity)) {
            return Kind::kOther;
        }
        const std::uint32_t template_id =
            engine.world_.registry()
                .get<network_example::ProjectileState>(*entity)
                .projectile_template_id;
        if (template_id == explosion_template_id) {
            return Kind::kExplosion;
        }
        if (template_id == shell_template_id) {
            return Kind::kShell;
        }
        return Kind::kOther;
    };

    const auto creation_distance = [&](network_example::NetId net_id) {
        const std::optional<entt::entity> subject =
            engine.world_.find_entity(net_id);
        const std::optional<entt::entity> viewer =
            engine.world_.find_entity(player);
        if (!subject.has_value() || !viewer.has_value() ||
            !engine.world_.registry().all_of<network_example::Transform>(*subject) ||
            !engine.world_.registry().all_of<network_example::Transform>(*viewer)) {
            return 0.0f;
        }
        return glm::length(
            engine.world_.registry()
                .get<network_example::Transform>(*subject)
                .position -
            engine.world_.registry()
                .get<network_example::Transform>(*viewer)
                .position);
    };

    // Only entities born inside the sample window are counted. The starting
    // crowd is introduced during warmup, and its deferral is a different
    // question -- what a player walking into a fight pays once, rather than what
    // a running fight keeps paying.
    bool sampling = false;
    std::uint32_t tick = 0;

    // build_relevant_snapshot is const, so asking it what the session can see
    // costs nothing but time and changes nothing the engine does.
    const auto mark_relevant = [&]() {
        if (!sampling || engine.peer_sessions_.empty()) {
            return;
        }
        const network_example::WorldSnapshot relevant =
            engine.build_relevant_snapshot(engine.peer_sessions_.front(), 0u);
        for (const network_example::EntitySnapshot& entity : relevant.entities) {
            const auto life = lives.find(entity.net_id);
            if (life != lives.end()) {
                life->second.ever_relevant = true;
            }
        }
    };

    const auto scan_world = [&]() {
        std::unordered_set<network_example::NetId> now;
        auto view = engine.world_.registry()
                        .view<network_example::NetworkIdentity>();
        for (const entt::entity entity : view) {
            const network_example::NetId net_id =
                view.get<network_example::NetworkIdentity>(entity).net_id;
            now.insert(net_id);
            if (present.insert(net_id).second && sampling) {
                Life life;
                life.created_tick = tick;
                life.kind = classify(net_id);
                life.distance_m = creation_distance(net_id);
                if (life.kind == Kind::kShell) {
                    ++shell_creations_by_tick[tick];
                } else if (life.kind == Kind::kExplosion) {
                    ++explosion_creations_by_tick[tick];
                }
                lives.emplace(net_id, life);
            }
        }
        for (auto iter = present.begin(); iter != present.end();) {
            if (now.find(*iter) != now.end()) {
                ++iter;
                continue;
            }
            const auto life = lives.find(*iter);
            if (life != lives.end()) {
                life->second.destroyed = true;
            }
            iter = present.erase(iter);
        }
    };

    std::size_t max_introductions_in_one_snapshot = 0;
    std::size_t spawn_packets = 0;
    const auto drain_introductions = [&]() {
        std::size_t introduced_now = 0;
        network_example::TransportEvent event;
        while (link->PollClientEvent(event)) {
            if (event.channel != network_example::ChannelId::kReliableEvent) {
                continue;
            }
            network_example::EntitySpawnPacket packet;
            if (!network_example::decode_entity_spawn_packet(
                    event.payload.data(), event.payload.size(), &packet)) {
                continue;
            }
            if (sampling) {
                ++spawn_packets;
            }
            const auto life = lives.find(packet.net_id);
            if (life == lives.end() || life->second.introduced) {
                continue;
            }
            life->second.introduced = true;
            life->second.introduced_tick = tick;
            ++introduced_now;
        }
        max_introductions_in_one_snapshot =
            std::max(max_introductions_in_one_snapshot, introduced_now);
    };

    for (; tick < kWarmupTicks; ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        scan_world();
        drain_introductions();
    }
    sampling = true;
    for (; tick < kWarmupTicks + kSampleTicks; ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        scan_world();
        mark_relevant();
        drain_introductions();
    }
    // One more snapshot interval of draining, so an entity created on the last
    // sampled tick still gets its chance to be introduced rather than being
    // counted unseen because the window ended.
    const std::uint32_t drain_ticks = kServerTickRate / kSnapshotRate * 2u;
    for (std::uint32_t extra = 0; extra < drain_ticks; ++extra, ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        scan_world();
        mark_relevant();
        drain_introductions();
    }

    Row row;
    row.agent_count = agent_count;
    std::vector<std::uint32_t> deferrals;
    std::vector<std::uint32_t> shell_deferrals;
    std::vector<std::uint32_t> explosion_deferrals;
    double shell_distance_total = 0.0;
    double explosion_distance_total = 0.0;
    double explosion_visible_total = 0.0;
    double explosion_seen_distance_total = 0.0;
    double explosion_unseen_distance_total = 0.0;
    for (const auto& [net_id, life] : lives) {
        ++row.created;
        if (life.kind == Kind::kShell) {
            ++row.shell.created;
            shell_distance_total += life.distance_m;
        }
        if (life.kind == Kind::kExplosion) {
            ++row.explosions_created;
            ++row.explosion.created;
            explosion_distance_total += life.distance_m;
        }
        if (!life.introduced) {
            ++row.unseen;
            if (life.kind == Kind::kShell) {
                ++row.shell.unseen;
            }
            if (life.kind == Kind::kExplosion) {
                ++row.explosions_unseen;
                ++row.explosion.unseen;
                if (!life.ever_relevant) {
                    ++row.explosions_unseen_never_relevant;
                }
                explosion_unseen_distance_total += life.distance_m;
            }
            continue;
        }
        ++row.introduced;
        const std::uint32_t deferral = life.introduced_tick - life.created_tick;
        deferrals.push_back(deferral);
        if (life.kind == Kind::kShell) {
            shell_deferrals.push_back(deferral);
        }
        if (life.kind == Kind::kExplosion) {
            explosion_deferrals.push_back(deferral);
            explosion_seen_distance_total += life.distance_m;
            // rocket_explosion lives 45 ticks; whatever it spent queued is time
            // the impact was not on screen.
            const float lifetime = 45.0f;
            explosion_visible_total += std::max(
                0.0,
                1.0 - static_cast<double>(deferral) / lifetime);
        }
    }
    row.created_per_second = static_cast<double>(row.created) /
        (static_cast<double>(kSampleTicks) / kServerTickRate);
    row.deferral_p50 = percentile(deferrals, 0.5);
    row.deferral_p90 = percentile(deferrals, 0.9);
    row.deferral_max = deferrals.empty()
        ? 0u
        : *std::max_element(deferrals.begin(), deferrals.end());
    row.max_introductions_in_one_snapshot = max_introductions_in_one_snapshot;
    row.spawn_packets = spawn_packets;
    row.distinct_introduced = row.introduced;
    row.explosion_visible_fraction = row.explosions_created == 0
        ? 1.0
        : explosion_visible_total / static_cast<double>(row.explosions_created);
    const auto fill = [](KindStats* stats,
                         std::vector<std::uint32_t> values,
                         double distance_total) {
        stats->p50 = percentile(values, 0.5);
        stats->p90 = percentile(values, 0.9);
        stats->max = values.empty()
            ? 0u
            : *std::max_element(values.begin(), values.end());
        stats->mean_distance_m = stats->created == 0
            ? 0.0
            : distance_total / static_cast<double>(stats->created);
    };
    fill(&row.shell, shell_deferrals, shell_distance_total);
    fill(&row.explosion, explosion_deferrals, explosion_distance_total);
    // 16 is what one snapshot can introduce; a tick that creates more than that
    // has already produced a backlog no single snapshot can clear.
    constexpr std::size_t kQuotaPerSnapshot = 16;
    const auto fill_burst = [](KindStats* stats,
                               const std::unordered_map<std::uint32_t, std::size_t>&
                                   by_tick) {
        for (const auto& [creation_tick, count] : by_tick) {
            (void)creation_tick;
            stats->max_created_in_one_tick =
                std::max(stats->max_created_in_one_tick, count);
            if (count > kQuotaPerSnapshot) {
                ++stats->burst_ticks;
            }
        }
    };
    row.explosion_seen_distance_m =
        row.explosion.created == row.explosion.unseen
            ? 0.0
            : explosion_seen_distance_total /
                static_cast<double>(row.explosion.created - row.explosion.unseen);
    row.explosion_unseen_distance_m = row.explosion.unseen == 0
        ? 0.0
        : explosion_unseen_distance_total /
            static_cast<double>(row.explosion.unseen);
    fill_burst(&row.shell, shell_creations_by_tick);
    fill_burst(&row.explosion, explosion_creations_by_tick);

    Kernel_Destroy(kernel);
    return row;
}

}  // namespace

int main() {
    const Catalog catalog = load_catalog();
    std::printf(
        "ENTITY INTRODUCTION DEFERRAL (%u Hz snapshots, %u Hz ticks)\n",
        kSnapshotRate,
        kServerTickRate);
    std::printf(
        "%8s %9s %9s %8s %8s %8s %8s %8s %7s %10s %8s %8s %9s %9s %9s\n",
        "sentries",
        "created",
        "new/s",
        "seen",
        "unseen",
        "p50 tk",
        "p90 tk",
        "max tk",
        "max/ss",
        "explosions",
        "expl x",
        "x norel",
        "visible",
        "seen m",
        "unseen m");
    std::uint16_t port = 7960;
    std::vector<Row> rows;
    for (const std::size_t agent_count : {10u, 25u, 50u, 100u}) {
        const Row row = measure(catalog, agent_count, port);
        rows.push_back(row);
        port = static_cast<std::uint16_t>(port + 2u);
        std::printf(
            "%8zu %9zu %9.1f %8zu %8zu %8u %8u %8u %7zu %10zu %8zu %8zu %8.0f%% %9.1f %9.1f\n",
            row.agent_count,
            row.created,
            row.created_per_second,
            row.introduced,
            row.unseen,
            row.deferral_p50,
            row.deferral_p90,
            row.deferral_max,
            row.max_introductions_in_one_snapshot,
            row.explosions_created,
            row.explosions_unseen,
            row.explosions_unseen_never_relevant,
            row.explosion_visible_fraction * 100.0,
            row.explosion_seen_distance_m,
            row.explosion_unseen_distance_m);
    }
    std::printf("\n");

    std::printf("INTRODUCTION TRAFFIC\n");
    std::printf(
        "%8s %14s %14s %10s\n",
        "sentries",
        "spawn packets",
        "distinct seen",
        "re-intro");
    for (const Row& row : rows) {
        std::printf(
            "%8zu %14zu %14zu %10zu\n",
            row.agent_count,
            row.spawn_packets,
            row.distinct_introduced,
            row.spawn_packets > row.distinct_introduced
                ? row.spawn_packets - row.distinct_introduced
                : 0u);
    }
    std::printf("\n");

    // The ordering question. sync_session_relevance sorts newcomers
    // nearest-first before the quota cuts, so an entity born far from the
    // player queues behind one born near it. A shell is born at the sentry that
    // fired it; the explosion it becomes is born wherever that shell landed. If
    // the mean distances differ and the deferrals differ the same way, the sort
    // is what is choosing between them. If the deferrals differ while the
    // distances do not, something else is.
    std::printf("DEFERRAL BY KIND, AGAINST CREATION DISTANCE\n");
    std::printf(
        "%8s %11s %9s %8s %8s %8s %8s %9s %8s %7s\n",
        "sentries",
        "kind",
        "created",
        "unseen",
        "p50 tk",
        "p90 tk",
        "max tk",
        "mean m",
        "max/tick",
        "bursts");
    for (const Row& row : rows) {
        const KindStats* stats[2] = {&row.shell, &row.explosion};
        const char* names[2] = {"shell", "explosion"};
        for (int index = 0; index < 2; ++index) {
            std::printf(
                "%8zu %11s %9zu %8zu %8u %8u %8u %9.1f %8zu %7zu\n",
                row.agent_count,
                names[index],
                stats[index]->created,
                stats[index]->unseen,
                stats[index]->p50,
                stats[index]->p90,
                stats[index]->max,
                stats[index]->mean_distance_m,
                stats[index]->max_created_in_one_tick,
                stats[index]->burst_ticks);
        }
    }
    std::printf("\n");
    return 0;
}
