// A tornado bottle, thrown in the shipped game: where it breaks, one tornado
// rises and carries on the way the bottle was going, level along the ground
// at its 2 m hover, and pulls a gingerbread in its path off its feet without
// hurting it. One throw finds where the bottle lands; the unit is put at that
// point and the same throw is made again.
//
// What this pins that tornado_catalog_test cannot: the bottle's collision
// hands the tornado a heading (the bottle's own velocity, which is mostly
// downward as it lands -- only its level part is kept) and a spawn point on
// the ground itself, which the tornado has to settle up from.

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
constexpr std::uint32_t kTornadoBottleItem = 3012u;

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
    require(Kernel_StartListenServer(kernel, 7998));

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
        kernel, kTornadoBottleItem, 2u, container, &bottle));

    struct Flight {
        bool found = false;
        std::uint32_t net_id = 0;
        KernelVec3 first{};
        KernelVec3 last{};
        int ticks_seen = 0;
        int new_projectiles = 0;
    };
    std::uint32_t request_id = 0;
    // Throws +x from the same spot and follows whatever it leaves behind.
    const auto throw_and_follow = [&](const auto& on_tick) {
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
        Flight flight;
        std::set<std::uint32_t> seen;
        for (int tick = 0; tick < 150; ++tick) {
            harness.step();
            for (const auto& state : harness.query(KernelEntityType_Projectile)) {
                if (before.count(state.net_id) != 0u) continue;
                if (seen.insert(state.net_id).second) ++flight.new_projectiles;
                if (!flight.found) {
                    flight.found = true;
                    flight.net_id = state.net_id;
                    flight.first = state.position;
                }
                if (state.net_id == flight.net_id) {
                    flight.last = state.position;
                    ++flight.ticks_seen;
                    // Level at its hover over the plane, every tick it lives,
                    // and straight on: the throw leaves the hand a little to
                    // one side, so the line is the spawn's, not the player's.
                    require(std::fabs(state.position.y - 2.0f) < 0.05f);
                    require(std::fabs(state.position.z - flight.first.z) < 0.001f);
                }
            }
            on_tick(tick);
        }
        require(flight.found);
        return flight;
    };

    const Flight first = throw_and_follow([](int) {});
    std::printf(
        "tornado from (%.2f, %.2f) to (%.2f, %.2f) over %d ticks\n",
        first.first.x, first.first.z, first.last.x, first.last.z, first.ticks_seen);
    // It broke out on the plane, ahead of the thrower.
    require(first.first.x - player_at.x > 3.0f);
    // One tornado and nothing else for its whole life.
    require(first.new_projectiles == 1);
    // On the way the bottle was going: 150 ticks at 6 m/s is up to 30 m, less
    // the bottle's flight before it broke.
    require(first.last.x - first.first.x > 15.0f);

    // A gingerbread where the bottle broke, which is how the bottle is meant
    // to be used: thrown at someone. It walks at the thrower, so the bottle
    // breaks on it or on the ground by it, and the tornado's first pull -- the
    // tick after it rises -- has it in reach.
    //
    // Not 5 m further on: a gingerbread walking into a 6 m/s funnel is inside
    // its 3 m radius for about 12 ticks, and with a pull every 30 it can be
    // passed through untouched (measured 2026-10-04: in reach ticks 4-16,
    // pulls at 1 and 31). That is the cost of pulling once a second, not a
    // fault this test should paper over.
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = KernelEntityType_Actor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = gingerbread;
    create.actor_template_id = gingerbread;
    create.position = KernelVec3{first.first.x, 0.0f, first.first.z};
    create.rotation = identity;
    std::uint32_t unit = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &unit));
    require(Kernel_ServerSetEntityActorTemplate(kernel, unit, gingerbread));
    harness.step();
    const std::uint16_t hp = harness.state_of(unit).hp;

    bool pulled = false;
    float highest = 0.0f;
    const Flight second = throw_and_follow([&](int) {
        const KernelServerEntityState state = harness.state_of(unit);
        pulled = pulled || state.velocity.y > 1.0f;
        highest = std::max(highest, state.position.y);
    });
    const std::uint16_t hp_after = harness.state_of(unit).hp;
    Kernel_Destroy(kernel);

    std::printf("gingerbread rose %.2f m, hp %u -> %u\n", highest, hp, hp_after);
    require(second.new_projectiles == 1);
    require(pulled);
    require(highest > 0.3f);
    require(hp_after == hp);
    return 0;
}
