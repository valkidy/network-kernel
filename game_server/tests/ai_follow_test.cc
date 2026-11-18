// F3 of docs/SUMMON_FOLLOW_PLAN.md: the allied giant follows the player who
// summoned it.
//
// Summoned with the shipped bottle (item 3037) on a listen host: it follows
// its summoner, holds still near them and only sets off again once they are
// well away (6 m / 10 m), fights an enemy it meets, holds where it is while
// its owner is dead and follows again once they are back, and is sent away by
// a second summon (one per player). Told to follow someone who then leaves
// the world it turns guard; set_behavior follows only a living player.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

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
constexpr std::uint32_t kSummonItemId = 3037;
constexpr KernelQuat kIdentity{0.0f, 0.0f, 0.0f, 1.0f};

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path = fs::path(std::getenv("TEST_SRCDIR")) / std::getenv("TEST_WORKSPACE") /
        "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

}  // namespace

int main() {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.preload_director_template_ids.clear();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    config.scene_props.clear();
    config.scene_items.clear();
    const std::uint32_t allied_id = template_id_of(config, "allied_gingerbread_giant");
    const std::uint32_t gingerbread_id = template_id_of(config, "gingerbread");
    require(allied_id != 0u && gingerbread_id != 0u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartListenServer(kernel, 8028));
    gs::GameServer server(kernel, config);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> destroyed;
    std::array<KernelEvent, 4096> events{};
    const auto step = [&]() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n =
            Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) {
            server.handle_event(events[i]);
            if (events[i].type == KernelEventType_EntityDestroyed) {
                destroyed.emplace_back(events[i].net_id, events[i].code);
            }
        }
        server.tick(kTickSeconds);
    };
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) step();
    };
    const auto position_of = [&](std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state.position;
    };
    const auto agent_of = [&](std::uint32_t net_id) -> const gs::AgentRuntimeState& {
        for (const gs::AgentRuntimeState& agent : server.agent_runtime_manager().agents()) {
            if (agent.net_id == net_id) return agent;
        }
        require(false);
        return server.agent_runtime_manager().agents().front();
    };
    const auto place = [&](std::uint32_t net_id, const KernelVec3& at) {
        require(Kernel_ServerSetEntityTransform(kernel, net_id, &at, &kIdentity));
    };
    std::vector<KernelServerEntityState> states(1024);
    const auto giants = [&]() {
        std::vector<std::uint32_t> found;
        for (KernelServerEntityState& state : states) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(kernel, 0u, states.data(), 1024u);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (states[i].actor_template_id == allied_id && states[i].valid != 0u) {
                found.push_back(states[i].net_id);
            }
        }
        return found;
    };

    run(30);
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;
    place(player, KernelVec3{0.0f, 0.0f, 0.0f});
    step();
    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4, &container));
    KernelItemInstanceId bottles = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, kSummonItemId, 2u, container, &bottles));
    std::uint64_t request_id = 1;
    const auto summon = [&](const KernelVec3& direction) {
        const std::vector<std::uint32_t> before = giants();
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = local.peer_id != 0u ? local.peer_id : 1u;
        request.request_id = request_id++;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Throw;
        request.requested_quantity = 1;
        request.selected_item_instance_id = bottles;
        request.throw_direction = direction;
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
        for (int tick = 0; tick < 120; ++tick) {
            step();
            for (const std::uint32_t net_id : giants()) {
                if (std::find(before.begin(), before.end(), net_id) == before.end()) {
                    return net_id;
                }
            }
        }
        require(false);
        return 0u;
    };

    // Summoned: it follows its summoner.
    const std::uint32_t giant = summon(KernelVec3{0.0f, -0.24f, 0.97f});
    run(5);
    require(agent_of(giant).controller_role == gs::AgentControllerRole::kFollow);
    require(agent_of(giant).follow_owner == player);

    // The owner 30 m off: it walks over, and holds once within 6 m.
    const KernelVec3 from = position_of(giant);
    place(player, KernelVec3{from.x + 30.0f, 0.0f, from.z});
    // A tick for it to see them gone and set off.
    run(2);
    require(!agent_of(giant).follow_holding);
    float distance = 1.0e9f;
    int walked_ticks = 0;
    for (; walked_ticks < 900 && !agent_of(giant).follow_holding; ++walked_ticks) {
        step();
    }
    distance = horizontal(position_of(giant), position_of(player));
    std::printf("[follow] caught up in %d ticks, %.1f m from its owner\n", walked_ticks, distance);
    require(agent_of(giant).follow_holding);
    require(distance <= 7.0f);
    const KernelVec3 parked = position_of(giant);
    run(60);
    require(horizontal(position_of(giant), parked) < 1.0f);

    // The owner 3 m further: still within 10 m, so it stays put.
    const KernelVec3 owner_at = position_of(player);
    place(player, KernelVec3{owner_at.x + 3.0f, 0.0f, owner_at.z});
    run(60);
    std::printf("[follow] owner 3 m on: moved %.2f m\n",
                horizontal(position_of(giant), parked));
    require(horizontal(position_of(giant), parked) < 1.0f);

    // The owner dead: it holds where it is, wherever they are; back, it
    // follows again. Back well within the respawn delay, which would move them.
    place(player, KernelVec3{owner_at.x + 40.0f, 0.0f, owner_at.z});
    require(Kernel_ServerSetEntityHealth(kernel, player, 0u));
    run(5);
    require(!agent_of(giant).follow_owner_alive);
    const KernelVec3 held = position_of(giant);
    run(60);
    std::printf("[follow] owner dead: moved %.2f m\n", horizontal(position_of(giant), held));
    require(horizontal(position_of(giant), held) < 1.0f);
    require(agent_of(giant).controller_role == gs::AgentControllerRole::kFollow);
    require(Kernel_ServerSetEntityHealth(kernel, player, 1000u));
    run(90);
    std::printf("[follow] owner back: moved %.2f m towards them\n",
                horizontal(held, position_of(player)) -
                    horizontal(position_of(giant), position_of(player)));
    require(agent_of(giant).follow_owner_alive);
    require(horizontal(position_of(giant), position_of(player)) <
            horizontal(held, position_of(player)) - 3.0f);

    // An enemy in its way -- ahead of it, between it and its owner, where its
    // cone faces as it walks: it fights as from any Idle.
    const KernelVec3 at_giant = position_of(giant);
    const std::uint32_t enemy =
        [&]() {
            KernelServerEntityCreateInfo create{};
            create.struct_size = sizeof(create);
            create.entity_template_id = gingerbread_id;
            create.position = KernelVec3{at_giant.x + 10.0f, 0.0f, at_giant.z};
            create.rotation = kIdentity;
            std::uint32_t net_id = 0;
            require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
            return net_id;
        }();
    bool engaged = false;
    for (int tick = 0; tick < 150 && !engaged; ++tick) {
        step();
        engaged = gs::is_engaged(agent_of(giant).sentry.state) &&
            agent_of(giant).sentry.target == enemy;
    }
    std::printf("[follow] engaged the enemy: %d\n", engaged ? 1 : 0);
    require(engaged);
    run(150);

    // A second summon sends the first away, as a summon running out goes.
    const std::uint32_t second = summon(KernelVec3{-0.97f, -0.24f, 0.0f});
    run(3);
    const auto first_entry = std::find_if(
        destroyed.begin(), destroyed.end(),
        [&](const auto& entry) { return entry.first == giant; });
    std::printf("[follow] second summon %u; first %s\n", second,
                first_entry == destroyed.end() ? "still there" : "gone");
    require(first_entry != destroyed.end());
    require(first_entry->second == KernelDespawnReason_Destroyed);
    require(giants().size() == 1u);
    require(agent_of(second).follow_owner == player);

    // set_behavior follows a living player, and nothing else.
    gs::AgentRuntimeManager& manager = server.agent_runtime_manager();
    gs::AgentControllerSpec follow;
    follow.role = gs::AgentControllerRole::kFollow;
    require(manager.set_behavior(second, follow) == gs::BehaviorSwitchResult::kInvalidData);
    follow.owner_net_id = second;
    require(manager.set_behavior(second, follow) == gs::BehaviorSwitchResult::kInvalidData);

    // Told to follow another player, who then leaves the world: a guard.
    KernelServerEntityCreateInfo other_player{};
    other_player.struct_size = sizeof(other_player);
    other_player.entity_type = gs::kEntityTypeActor;
    other_player.actor_type = gs::kActorTypePlayer;
    other_player.entity_template_id = config.player.actor_template_id;
    other_player.actor_template_id = config.player.actor_template_id;
    other_player.position = position_of(second);
    other_player.position.x += 5.0f;
    other_player.rotation = kIdentity;
    std::uint32_t leaver = 0;
    require(Kernel_ServerCreateEntity(kernel, &other_player, &leaver));
    run(3);
    follow.owner_net_id = leaver;
    require(manager.set_behavior(second, follow) == gs::BehaviorSwitchResult::kAccepted);
    require(agent_of(second).follow_owner == leaver);
    require(Kernel_ServerDestroyEntity(kernel, leaver, KernelDespawnReason_Disconnected));
    run(3);
    std::printf("[follow] owner left: role %d\n",
                static_cast<int>(agent_of(second).controller_role));
    require(agent_of(second).controller_role == gs::AgentControllerRole::kGuard);

    Kernel_Destroy(kernel);
    std::printf("ai_follow_test passed\n");
    return 0;
}
