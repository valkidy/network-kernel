// A pull bottle gathers what it hits: gingerbreads standing 2, 4 and 6 m from
// the blast all come down on a 1 m ring around it, each on the side it came
// from, on the same tick.
//
// That holds for the near one too, which is the point of apply_pull over a
// reversed apply_impulse. It is hostile and walks at the thrower while the
// bottle is in the air; an impulse would have added its walk to the pull and
// flung it ~4.6 m past the centre (measured 2026-09-30 on the impulse
// version). apply_pull replaces the velocity and works the speed out per
// target, so distance and what it was doing both drop out. The printed table
// is the thing to read when retuning action_pull_at_target.
//
// The shipped game, headless, as the dedicated server runs it. One throw finds
// where the bottle lands; the units are placed around that point and the same
// throw is made again.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kPullBottleItem = 3010u;

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog" / "mesh_assets" /
        "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

float horizontal_distance(const KernelVec3& a, const KernelVec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

struct Harness {
    KernelHandle* kernel = nullptr;
    network_example::game_server::GameServer* game_server = nullptr;
    std::array<KernelEvent, 4096> events{};

    void step() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) game_server->handle_event(events[i]);
        game_server->tick(kTickSeconds);
    }

    std::vector<KernelServerEntityState> query(std::uint16_t type) {
        std::vector<KernelServerEntityState> states(1024);
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, type, states.data(), static_cast<std::uint32_t>(states.size()));
        states.resize(count);
        return states;
    }

    KernelServerEntityState state_of(std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state;
    }
};

}  // namespace

int main() {
    const auto config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::uint32_t gingerbread = template_id_of(config, "gingerbread");
    require(gingerbread != 0u);
    const std::vector<std::uint8_t> scene = read_ground_scene();

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));
    require(network_example::game_server::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartListenServer(kernel, 7962));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());
    Harness harness{kernel, &game_server};
    for (int tick = 0; tick < 30; ++tick) harness.step();

    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    // Far out on the plane, away from where the nests put their units.
    const KernelVec3 player_at{60.0f, 0.0f, 60.0f};

    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4, &container));
    KernelItemInstanceId bottle = 0;
    require(Kernel_ServerCreateInventoryItem(
        kernel, kPullBottleItem, 2u, container, &bottle));

    std::uint32_t request_id = 0;
    // Throws from the same spot in the same direction and returns where the
    // pull_blast it leaves appeared.
    const auto throw_and_find_blast = [&](const auto& on_tick) {
        require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
        harness.step();
        std::set<std::uint32_t> before;
        for (const auto& state : harness.query(KernelEntityType_Projectile)) {
            before.insert(state.net_id);
        }
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = local.peer_id != 0u ? local.peer_id : 1u;
        request.request_id = ++request_id;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Throw;
        request.requested_quantity = 1;
        request.selected_item_instance_id = bottle;
        request.throw_direction = KernelVec3{0.97f, -0.24f, 0.0f};
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
        bool found = false;
        KernelVec3 blast{};
        for (int tick = 0; tick < 150; ++tick) {
            harness.step();
            if (!found) {
                for (const auto& state : harness.query(KernelEntityType_Projectile)) {
                    if (before.count(state.net_id) == 0u) {
                        found = true;
                        blast = state.position;
                    }
                }
            }
            on_tick(tick);
        }
        require(found);
        return blast;
    };

    const KernelVec3 centre = throw_and_find_blast([](int) {});
    std::printf("blast at (%.2f, %.2f, %.2f)\n", centre.x, centre.y, centre.z);
    require(horizontal_distance(centre, player_at) > 3.0f);

    // Three units on three sides of it, none on the player's line.
    const std::array<float, 3> distances = {2.0f, 4.0f, 6.0f};
    const std::array<float, 3> bearings = {1.2f, 2.9f, -1.5f};
    std::array<std::uint32_t, 3> units{};
    for (std::size_t index = 0; index < units.size(); ++index) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Actor;
        create.actor_type = KernelActorType_Agent;
        create.entity_template_id = gingerbread;
        create.actor_template_id = gingerbread;
        create.position = KernelVec3{
            centre.x + distances[index] * std::cos(bearings[index]),
            0.0f,
            centre.z + distances[index] * std::sin(bearings[index])};
        create.rotation = identity;
        require(Kernel_ServerCreateEntity(kernel, &create, &units[index]));
        require(Kernel_ServerSetEntityActorTemplate(kernel, units[index], gingerbread));
    }

    struct Track {
        bool pulled = false;
        KernelVec3 at_pull{};
        KernelVec3 pull_velocity{};
        bool airborne = false;
        bool landed = false;
        int landed_tick = 0;
        KernelVec3 at_landing{};
    };
    std::array<Track, 3> tracks{};
    const KernelVec3 second = throw_and_find_blast([&](int tick) {
        for (std::size_t index = 0; index < units.size(); ++index) {
            Track& track = tracks[index];
            const KernelServerEntityState state = harness.state_of(units[index]);
            if (!track.pulled && state.velocity.y > 2.0f) {
                track.pulled = true;
                track.at_pull = state.position;
                track.pull_velocity = state.velocity;
            }
            if (track.pulled && !track.landed) {
                track.airborne = track.airborne || state.position.y > 0.3f;
                if (track.airborne && state.position.y < 0.05f &&
                    (state.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u) {
                    track.landed = true;
                    track.landed_tick = tick;
                    track.at_landing = state.position;
                }
            }
        }
    });
    Kernel_Destroy(kernel);

    // The same throw, but the units are hostile and walk at the thrower while
    // the bottle is in the air, so it can break on one of them short of the
    // first spot. Everything below is measured from where it actually went off.
    std::printf("second blast at (%.2f, %.2f, %.2f), %.2f m from the first\n",
                second.x, second.y, second.z, horizontal_distance(second, centre));
    require(horizontal_distance(second, centre) < 3.0f);
    const KernelVec3 blast = second;

    std::printf("unit  start  at_pull  landed  signed  (m from blast; signed < 0 = past it)\n");
    float farthest_start = 0.0f;
    int first_landing_tick = -1;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const Track& track = tracks[index];
        require(track.pulled);
        require(track.landed);
        // Pulled, not pushed: the first velocity points at the blast.
        const float to_x = blast.x - track.at_pull.x;
        const float to_z = blast.z - track.at_pull.z;
        const float inward = (track.pull_velocity.x * to_x + track.pull_velocity.z * to_z) /
            std::max(0.001f, std::sqrt(to_x * to_x + to_z * to_z));

        const float start = horizontal_distance(track.at_pull, blast);
        const float landed = horizontal_distance(track.at_landing, blast);
        // Which side of the blast it came down on, along the line it was
        // pulled along.
        const float along = ((track.at_landing.x - blast.x) * -to_x +
                             (track.at_landing.z - blast.z) * -to_z) /
            std::max(0.001f, start);
        const float speed = std::sqrt(
            track.pull_velocity.x * track.pull_velocity.x +
            track.pull_velocity.z * track.pull_velocity.z);
        std::printf("#%u  %5.2f  %7.2f  %6.2f  %6.2f  tick %d  "
                    "v_h %.2f (inward %.2f) v_y %.2f\n",
                    units[index], distances[index], start, landed, along,
                    track.landed_tick, speed, inward, track.pull_velocity.y);
        // Every one of them started outside the 1 m ring, so every one is
        // pulled inward -- faster the farther out it was.
        require(inward > 0.0f);
        // On the ring, on its own side of the blast, near one included.
        require(std::fabs(landed - 1.0f) < 0.15f);
        require(along > 0.85f);
        // The airtime is authored, not a consequence of the distance.
        if (first_landing_tick < 0) first_landing_tick = track.landed_tick;
        require(track.landed_tick == first_landing_tick);
        farthest_start = std::max(farthest_start, start);
    }
    require(farthest_start > 5.5f);
    return 0;
}
