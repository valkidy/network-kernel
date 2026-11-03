// What the shipping warband does to a trio and a loner over three minutes.
// docs/PLAYER_TARGETED_PATROL_PLAN.md P5.
//
// The real bundle, the real AgentRuntimeManager, four players with the
// catalog's player template standing still: three together, one 130 m off.
// Players are healed every second so a death does not end the measurement --
// what is measured is who patrols reach, not whether anyone survives them.
//
// Reported, not asserted. Run it with
//   bazel run -c opt //game_server:patrol_targeting_soak

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_group_runtime.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kTicks = 30 * 180;
// The warband's ring.max_meters: what counts as "a squad near you".
constexpr float kThreatMeters = 60.0f;
// The mage's vision: a squad this close is engaging.
constexpr float kContactMeters = 30.0f;

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
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

float horizontal_distance(const KernelVec3& from, const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

struct Target {
    const char* name;
    std::vector<KernelVec3> members;
    // Ticks with any squad within kThreatMeters of a member, and within
    // kContactMeters.
    std::uint32_t threatened_ticks = 0;
    std::uint32_t contact_ticks = 0;
    std::int64_t first_threat_tick = -1;
    std::int64_t first_contact_tick = -1;
    std::uint32_t max_squads_near = 0;

    float nearest(const KernelVec3& point) const {
        float best = std::numeric_limits<float>::max();
        for (const KernelVec3& member : members) {
            best = std::min(best, horizontal_distance(member, point));
        }
        return best;
    }
};

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip").string());
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), "gameplay_catalog.yaml");
    const gs::KernelGameplayCatalogStorage storage = gs::build_kernel_gameplay_catalog(config);
    const std::vector<std::uint8_t> scene = gs::load_gameplay_bundle_entry_bytes(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()),
        config.static_collision_scene.entry_path);
    require(!scene.empty());
    config.navigation_mesh.artifact = gs::load_gameplay_bundle_entry_bytes(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()),
        config.navigation_mesh.entry_path);
    require(!config.navigation_mesh.artifact.empty());

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 1024;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 8523));
    require(Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr));

    gs::AgentRuntimeManager manager(kernel, config);
    const gs::PatrolNavigation& navigation = manager.patrol_navigation();
    require(navigation.valid());

    // On the ground, wherever the map puts it.
    const auto ground = [&](float x, float z) {
        KernelVec3 point{x, 0.0f, z};
        require(navigation.snap(point, 3.0f, &point));
        point.y += 1.0f;
        return point;
    };
    std::vector<Target> targets = {
        {"trio", {ground(-40.0f, -40.0f), ground(-36.0f, -40.0f), ground(-40.0f, -36.0f)}},
        {"loner", {ground(55.0f, 55.0f)}},
    };
    std::vector<std::uint32_t> players;
    for (const Target& target : targets) {
        for (const KernelVec3& position : target.members) {
            KernelServerEntityCreateInfo create{};
            create.struct_size = sizeof(create);
            create.entity_type = gs::kEntityTypeActor;
            create.actor_type = gs::kActorTypePlayer;
            create.entity_template_id = config.player.actor_template_id;
            create.actor_template_id = config.player.actor_template_id;
            create.position = position;
            create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
            std::uint32_t net_id = 0;
            require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
            players.push_back(net_id);
        }
    }

    std::uint32_t max_live_groups = 0;
    std::uint32_t max_patrol_agents = 0;
    std::uint32_t spawned_before = 0;
    std::printf("tick  t_s  live_groups  patrol_agents  trio_nearest_m  loner_nearest_m\n");
    for (std::uint32_t tick = 0; tick < kTicks; ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        if (tick % 30 == 0) {
            for (const std::uint32_t player : players) {
                (void)Kernel_ServerSetEntityHealth(kernel, player, 100);
            }
        }

        const auto& groups = manager.patrol_groups().groups();
        std::uint32_t patrol_agents = 0;
        for (const gs::PatrolGroup& group : groups) {
            patrol_agents += static_cast<std::uint32_t>(group.member_net_ids.size());
        }
        max_live_groups = std::max(max_live_groups, static_cast<std::uint32_t>(groups.size()));
        max_patrol_agents = std::max(max_patrol_agents, patrol_agents);

        float nearest[2] = {
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        for (std::size_t index = 0; index < targets.size(); ++index) {
            Target& target = targets[index];
            std::uint32_t near = 0;
            bool contact = false;
            for (const gs::PatrolGroup& group : groups) {
                const float distance = target.nearest(group.cursor);
                nearest[index] = std::min(nearest[index], distance);
                near += distance <= kThreatMeters ? 1u : 0u;
                contact = contact || distance <= kContactMeters;
            }
            target.max_squads_near = std::max(target.max_squads_near, near);
            if (near > 0) {
                ++target.threatened_ticks;
                if (target.first_threat_tick < 0) {
                    target.first_threat_tick = tick;
                }
            }
            if (contact) {
                ++target.contact_ticks;
                if (target.first_contact_tick < 0) {
                    target.first_contact_tick = tick;
                }
            }
        }

        const std::uint32_t spawned = manager.patrol_director().spawned_group_count();
        if (spawned != spawned_before && !groups.empty()) {
            const gs::PatrolGroup& group = groups.back();
            // Against where the players are now: a fight moves them, and the
            // ring is drawn against their live positions, not where they began.
            std::vector<KernelServerEntityState> actors(512);
            for (KernelServerEntityState& state : actors) {
                state.struct_size = sizeof(KernelServerEntityState);
            }
            const std::uint32_t count = Kernel_ServerQueryEntities(
                kernel, gs::kEntityTypeActor, actors.data(),
                static_cast<std::uint32_t>(actors.size()));
            float live_nearest = std::numeric_limits<float>::max();
            for (std::uint32_t index = 0; index < count; ++index) {
                if (actors[index].actor_type == gs::kActorTypePlayer) {
                    live_nearest = std::min(
                        live_nearest, horizontal_distance(actors[index].position, group.cursor));
                }
            }
            std::printf("  nearest live player to the new start %.1f m\n", live_nearest);
            std::printf(
                "  spawn at t=%5.1fs group=%u members=%zu trio_m=%.1f loner_m=%.1f\n",
                tick * kTickSeconds, group.group_id, group.member_net_ids.size(),
                targets[0].nearest(group.cursor), targets[1].nearest(group.cursor));
            spawned_before = spawned;
        }
        if (tick % 300 == 0) {
            std::printf(
                "%5u %4.0f  %11zu  %13u  %14.1f  %15.1f\n", tick, tick * kTickSeconds,
                groups.size(), patrol_agents,
                nearest[0] == std::numeric_limits<float>::max() ? -1.0f : nearest[0],
                nearest[1] == std::numeric_limits<float>::max() ? -1.0f : nearest[1]);
        }
    }

    const gs::PatrolDirector& director = manager.patrol_director();
    std::printf("\nseconds %u, squads spawned %u, retired %u, ring failures %u\n",
                kTicks / 30, director.spawned_group_count(),
                director.retired_group_count(), director.route_failure_count());
    std::printf("max live squads %u, max patrol agents %u\n", max_live_groups,
                max_patrol_agents);
    for (const Target& target : targets) {
        std::printf(
            "%-5s  threatened %5.1f%%  first threat %6.1fs  contact %5.1f%%  "
            "first contact %6.1fs  max squads near %u\n",
            target.name, 100.0 * target.threatened_ticks / kTicks,
            target.first_threat_tick < 0 ? -1.0 : target.first_threat_tick * kTickSeconds,
            100.0 * target.contact_ticks / kTicks,
            target.first_contact_tick < 0 ? -1.0 : target.first_contact_tick * kTickSeconds,
            target.max_squads_near);
    }
    Kernel_Destroy(kernel);
    return 0;
}
