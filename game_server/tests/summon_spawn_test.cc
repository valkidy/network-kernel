// F1 of docs/SUMMON_FOLLOW_PLAN.md, end to end through a thrown bottle.
//
// E1: a player-side actor a thrown bottle spawns belongs to the thrower --
// its owner_peer is the thrower's, though the bottle itself is nobody's --
// while an enemy a bottle spawns stays nobody's.
// E3: a spawn_entity's lifetime_ticks reaches what it spawns, and an actor
// running out despawns as Destroyed, the way an enemy killed does.
//
// The shipped catalog with two test files added to a copy of it: a bottle
// (and its item) that spawns the allied giant for 90 ticks through the shared
// action_spawn_entity_and_damage_self. The shipped test bottle 3005, which spawns a
// chaser_grunt, is the enemy case.
//
// F2: the shipped summon bottle (item 3037) does the same for 120 s, and one
// throw spends one bottle of the stack.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

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
constexpr std::uint32_t kSummonLifetimeTicks = 90;
constexpr std::uint32_t kSummonItemId = 3999;
constexpr std::uint32_t kEnemyItemId = 3005;
constexpr std::uint32_t kShippedSummonItemId = 3037;
constexpr std::uint32_t kShippedSummonLifetimeTicks = 3600;

fs::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_ground_scene() {
    std::ifstream file(
        runfiles_root() / "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" /
            "plane_200x200.joltmesh",
        std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_yaml(const fs::path& path, const YAML::Node& node) {
    YAML::Emitter out;
    out << node;
    std::ofstream file(path);
    file << out.c_str() << "\n";
    require(file.good());
}

// A copy of the shipped catalog with a 90-tick summon bottle and its item
// added.
fs::path catalog_with_summon_bottle() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / "summon_catalog";
    fs::remove_all(root);
    fs::copy(runfiles_root() / "game_server" / "gameplay_catalog", root,
             fs::copy_options::recursive);

    YAML::Node prop = YAML::LoadFile(
        (root / "entity_templates" / "206_prop_test_bottle_spawn_enemy.yaml").string());
    prop["id"] = 299;
    prop["name"] = "test_bottle_summon_ally";
    YAML::Node trigger = prop["triggers"]["on_collision"];
    trigger["parameters"]["template"] = "allied_gingerbread_giant";
    trigger["parameters"]["placement"] = "clear";
    trigger["parameters"]["lifetime_ticks"] = kSummonLifetimeTicks;
    write_yaml(root / "entity_templates" / "299_prop_test_bottle_summon_ally.yaml", prop);

    YAML::Node item = YAML::LoadFile(
        (root / "item_templates" / "3005_stateful_test_bottle_spawn_enemy.yaml").string());
    item["id"] = kSummonItemId;
    item["name"] = "stateful_test_bottle_summon_ally";
    item["entity_template"] = "test_bottle_summon_ally";
    write_yaml(root / "item_templates" / "3999_stateful_test_bottle_summon_ally.yaml", item);
    return root;
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

}  // namespace

int main() {
    auto config = gs::load_gameplay_config_from_catalog_file(
        (catalog_with_summon_bottle() / "gameplay_catalog.yaml").string());
    config.preload_director_template_ids.clear();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    config.scene_props.clear();
    config.scene_items.clear();
    const std::uint32_t allied_id = template_id_of(config, "allied_gingerbread_giant");
    const std::uint32_t grunt_id = template_id_of(config, "chaser_grunt");
    require(allied_id != 0u && grunt_id != 0u);

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
    require(Kernel_StartListenServer(kernel, 8027));
    gs::GameServer game_server(kernel, config);

    // Every entity the server destroyed, and why.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> destroyed;
    std::array<KernelEvent, 4096> events{};
    std::uint32_t ticks = 0;
    const auto step = [&]() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n =
            Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) {
            game_server.handle_event(events[i]);
            if (events[i].type == KernelEventType_EntityDestroyed) {
                destroyed.emplace_back(events[i].net_id, events[i].code);
            }
        }
        game_server.tick(kTickSeconds);
        ++ticks;
    };
    std::vector<KernelServerEntityState> states(1024);
    const auto find_actor = [&](std::uint32_t template_id) -> const KernelServerEntityState* {
        for (KernelServerEntityState& state : states) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(kernel, 0u, states.data(), 1024u);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (states[i].actor_template_id == template_id && states[i].valid != 0u) {
                return &states[i];
            }
        }
        return nullptr;
    };

    for (int tick = 0; tick < 30; ++tick) step();
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    const KernelVec3 player_at{0.0f, 0.0f, 0.0f};
    require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
    step();
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    require(Kernel_ServerGetEntityState(kernel, player, &player_state));
    const std::uint32_t player_peer = player_state.owner_peer;
    require(player_peer != 0u);

    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4, &container));
    const auto throw_item = [&](std::uint32_t item_template_id, std::uint64_t request_id,
                                const KernelVec3& direction) {
        KernelItemInstanceId bottle = 0;
        require(Kernel_ServerCreateInventoryItem(
            kernel, item_template_id, 1u, container, &bottle));
        KernelGameplayRequest request{};
        request.struct_size = sizeof(request);
        request.requester_peer = local.peer_id != 0u ? local.peer_id : 1u;
        request.request_id = request_id;
        request.instigator_net_id = player;
        request.domain_action = KernelDomainAction_Throw;
        request.requested_quantity = 1;
        request.selected_item_instance_id = bottle;
        request.throw_direction = direction;
        require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
    };

    // E1, ally: the summoned giant is the thrower's.
    throw_item(kSummonItemId, 1u, KernelVec3{0.97f, -0.24f, 0.0f});
    std::uint32_t giant = 0;
    std::uint32_t giant_owner_peer = 0;
    std::uint32_t spawned_at = 0;
    for (int tick = 0; tick < 120 && giant == 0u; ++tick) {
        step();
        if (const KernelServerEntityState* found = find_actor(allied_id)) {
            giant = found->net_id;
            giant_owner_peer = found->owner_peer;
            spawned_at = ticks;
        }
    }
    std::printf("[summon] giant %u owner_peer %u (thrower's peer %u) at tick %u\n",
                giant, giant_owner_peer, player_peer, spawned_at);
    require(giant != 0u);
    require(giant_owner_peer == player_peer);

    // E1, enemy: a grunt a bottle spawns stays nobody's.
    throw_item(kEnemyItemId, 2u, KernelVec3{-0.97f, -0.24f, 0.0f});
    std::uint32_t grunt = 0;
    std::uint32_t grunt_owner_peer = 0;
    for (int tick = 0; tick < 120 && grunt == 0u; ++tick) {
        step();
        if (const KernelServerEntityState* found = find_actor(grunt_id)) {
            grunt = found->net_id;
            grunt_owner_peer = found->owner_peer;
        }
    }
    std::printf("[summon] grunt %u owner_peer %u\n", grunt, grunt_owner_peer);
    require(grunt != 0u);
    require(grunt_owner_peer == 0u);

    // E3: the giant lasts its 90 ticks and goes as Destroyed, like a kill.
    bool gone = false;
    while (!gone && ticks < spawned_at + kSummonLifetimeTicks + 30u) {
        step();
        gone = std::any_of(destroyed.begin(), destroyed.end(),
                           [&](const auto& entry) { return entry.first == giant; });
    }
    const auto entry = std::find_if(destroyed.begin(), destroyed.end(),
                                    [&](const auto& candidate) { return candidate.first == giant; });
    require(entry != destroyed.end());
    std::printf("[summon] giant gone after %u ticks, reason %u\n", ticks - spawned_at,
                entry->second);
    require(entry->second == KernelDespawnReason_Destroyed);
    // Seen the tick after it spawned, so it went at most a tick either side of
    // its lifetime.
    require(ticks - spawned_at + 1u >= kSummonLifetimeTicks);
    require(ticks - spawned_at <= kSummonLifetimeTicks + 1u);

    // F2: the shipped bottle. A stack of two; one throw spends one.
    const auto bottles_left = [&]() {
        std::array<KernelItemInstanceView, 8> slots{};
        for (KernelItemInstanceView& slot : slots) slot.struct_size = sizeof(slot);
        const std::uint32_t count = Kernel_CopyInventorySlots(
            kernel, container, slots.data(), static_cast<std::uint32_t>(slots.size()));
        std::uint32_t left = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (slots[i].item_template_id == kShippedSummonItemId) left += slots[i].quantity;
        }
        return left;
    };
    KernelItemInstanceId stack = 0;
    require(Kernel_ServerCreateInventoryItem(
        kernel, kShippedSummonItemId, 2u, container, &stack));
    require(bottles_left() == 2u);
    KernelGameplayRequest summon{};
    summon.struct_size = sizeof(summon);
    summon.requester_peer = local.peer_id != 0u ? local.peer_id : 1u;
    summon.request_id = 3u;
    summon.instigator_net_id = player;
    summon.domain_action = KernelDomainAction_Throw;
    summon.requested_quantity = 1;
    summon.selected_item_instance_id = stack;
    summon.throw_direction = KernelVec3{0.0f, -0.24f, 0.97f};
    require(Kernel_ServerSubmitGameplayRequest(kernel, &summon));
    std::uint32_t shipped = 0;
    std::uint32_t shipped_owner_peer = 0;
    std::uint32_t shipped_at = 0;
    for (int tick = 0; tick < 120 && shipped == 0u; ++tick) {
        step();
        if (const KernelServerEntityState* found = find_actor(allied_id)) {
            shipped = found->net_id;
            shipped_owner_peer = found->owner_peer;
            shipped_at = ticks;
        }
    }
    std::printf("[summon] shipped bottle: giant %u owner_peer %u, %u bottle(s) left\n",
                shipped, shipped_owner_peer, bottles_left());
    require(shipped != 0u);
    require(shipped_owner_peer == player_peer);
    require(bottles_left() == 1u);
    bool shipped_gone = false;
    while (!shipped_gone && ticks < shipped_at + kShippedSummonLifetimeTicks + 30u) {
        step();
        shipped_gone = std::any_of(destroyed.begin(), destroyed.end(),
                                   [&](const auto& e) { return e.first == shipped; });
    }
    const auto shipped_entry = std::find_if(
        destroyed.begin(), destroyed.end(),
        [&](const auto& candidate) { return candidate.first == shipped; });
    require(shipped_entry != destroyed.end());
    std::printf("[summon] shipped giant gone after %u ticks, reason %u\n",
                ticks - shipped_at, shipped_entry->second);
    require(shipped_entry->second == KernelDespawnReason_Destroyed);
    require(ticks - shipped_at + 1u >= kShippedSummonLifetimeTicks);
    require(ticks - shipped_at <= kShippedSummonLifetimeTicks + 1u);
    Kernel_Destroy(kernel);
    std::printf("summon_spawn_test passed\n");
    return 0;
}
