// Portals (docs/PORTAL_PLAN.md P2: K3 portal block, K4 teleport; ABI 103), on
// the shipped catalog's portal_entrance and ground.
//
// What this pins:
//   - an entrance brings its exit, standing at the destination;
//   - walking into the gate sends a player to the destination, facing the
//     portal's forward;
//   - arrival grace: a portal standing on the destination does not take the
//     player back until the grace is over, and then does;
//   - arrival immunity: a volume that hurts on entry, standing on the
//     destination, runs on arrival and hurts nothing; the same volume later
//     does (the control);
//   - the exit goes when its entrance goes, for the same despawn reason;
//   - thrown, the kit's portal faces the way it was thrown, its gate toward
//     the thrower.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

namespace fs = std::filesystem;

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

using network_example::game_server::GameServerGameplayConfig;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint16_t kPort = 8475;
constexpr std::uint32_t kPeer = 7;
// portal_entrance's, from the shipped catalog.
const KernelVec3 kDestination{50.0f, 0.0f, 50.0f};
constexpr std::uint16_t kEntranceMaxHp = 600;
constexpr std::uint32_t kGraceTicks = 30;
constexpr std::uint32_t kImmunityTicks = 60;
constexpr std::uint16_t kGateDamage = 100;

// A volume that hurts whoever walks in: the arrival immunity's probe.
constexpr const char* kHurtVolume = R"(id: 900
name: test_hurt_volume
shape: oriented_box
center: {x: 0.0, y: 1.0, z: 0.0}
half_extents: {x: 1.0, y: 1.0, z: 1.0}
purpose: trigger
layer: neutral
)";
constexpr const char* kHurtGate = R"(id: 900
name: test_hurt_gate
entity_type: prop
physics:
  trigger_collider_template: test_hurt_volume
triggers:
  on_enter:
    collision_mask: player_side
    instigators: players
    action_graph: action_apply_damage
    parameters:
      target: event.instigator
      amount: 100
)";

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good());
    stream << text;
}

GameServerGameplayConfig load_catalog() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path root = fs::path(tmp != nullptr ? tmp : "/tmp") / "portal_catalog";
    fs::remove_all(root);
    fs::copy("game_server/gameplay_catalog", root, fs::copy_options::recursive);
    write_file(root / "collider_templates/900_collider_test_hurt_volume.yaml", kHurtVolume);
    write_file(root / "entity_templates/900_prop_test_hurt_gate.yaml", kHurtGate);
    return network_example::game_server::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

std::uint32_t entity_template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

std::uint32_t item_template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.item_templates) {
        if (candidate.name == name) {
            return candidate.definition.item_template_id;
        }
    }
    return 0;
}

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const fs::path path = fs::path(test_srcdir) / test_workspace /
        "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" /
        "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

float horizontal_distance(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// Where local +z points under `rotation`, on the ground plane.
KernelVec3 forward_of(const KernelQuat& q) {
    // Third column of the rotation matrix.
    return KernelVec3{
        2.0f * (q.x * q.z + q.w * q.y),
        2.0f * (q.y * q.z - q.w * q.x),
        1.0f - 2.0f * (q.x * q.x + q.y * q.y),
    };
}

class World {
public:
    explicit World(const GameServerGameplayConfig& config) : config_(config) {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel_config.max_events = 256;
        kernel_config.max_render_states = 64;
        kernel_ = Kernel_Create(&kernel_config);
        require(kernel_ != nullptr);
        require(network_example::game_server::load_kernel_gameplay_catalog(
            kernel_, config));
        const std::vector<std::uint8_t> scene = read_ground_scene();
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = config.static_collision_scene.scene_id;
        scene_config.collider_id = config.static_collision_scene.collider_id;
        scene_config.collision_layer = config.static_collision_scene.collision_layer;
        require(Kernel_SetStaticCollisionScene(kernel_, &scene_config));
        require(Kernel_StartDedicatedServer(kernel_, kPort));
    }
    ~World() { Kernel_Destroy(kernel_); }

    // Every EntityDestroyed seen, as (net_id, reason).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> destroyed;

    void step(int ticks) {
        std::array<KernelEvent, 256> events{};
        for (int tick = 0; tick < ticks; ++tick) {
            Kernel_Update(kernel_, kTickSeconds);
            const std::uint32_t count = Kernel_PollEvents(
                kernel_, events.data(), static_cast<std::uint32_t>(events.size()));
            for (std::uint32_t index = 0; index < count; ++index) {
                if (events[index].type == KernelEventType_EntityDestroyed) {
                    destroyed.emplace_back(events[index].net_id, events[index].code);
                }
            }
        }
    }

    std::uint32_t create(const std::string& name, const KernelVec3& position,
                         const KernelQuat& rotation = kIdentityRotation) {
        const std::uint32_t template_id = entity_template_id_of(config_, name);
        require(template_id != 0u);
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_template_id = template_id;
        create.owner_peer = kPeer;
        create.position = position;
        create.rotation = rotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel_, &create, &net_id));
        require(net_id != 0u);
        return net_id;
    }

    std::uint32_t player(const KernelVec3& position) {
        const std::uint32_t template_id = config_.player.actor_template_id;
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Actor;
        create.actor_type = KernelActorType_Player;
        create.entity_template_id = template_id;
        create.actor_template_id = template_id;
        create.owner_peer = kPeer;
        create.position = position;
        create.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel_, &create, &net_id));
        require(Kernel_ServerSetEntityActorTemplate(kernel_, net_id, template_id));
        return net_id;
    }

    void move(std::uint32_t net_id, const KernelVec3& to) {
        require(Kernel_ServerSetEntityTransform(kernel_, net_id, &to, &kIdentityRotation));
    }

    void walk(std::uint32_t net_id, const KernelVec2& move) {
        KernelPlayerInput input{};
        input.move = move;
        input.aim_dir = KernelVec3{move.x, 0.0f, move.y};
        require(Kernel_ServerEnqueueEntityInput(
            kernel_, KernelCommandSource_Test, net_id, &input));
        step(1);
    }

    bool exists(std::uint32_t net_id) {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        return Kernel_ServerGetEntityState(kernel_, net_id, &out);
    }

    KernelServerEntityState state(std::uint32_t net_id) {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel_, net_id, &out));
        return out;
    }

    // Props within `radius` of `at` (horizontally) with this max_hp.
    std::vector<KernelServerEntityState> props_near(
        const KernelVec3& at, float radius, std::uint16_t max_hp) {
        std::array<KernelServerEntityState, 256> states{};
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(state);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel_, KernelEntityType_Prop, states.data(),
            static_cast<std::uint32_t>(states.size()));
        std::vector<KernelServerEntityState> found;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].max_hp == max_hp &&
                horizontal_distance(states[index].position, at) < radius) {
                found.push_back(states[index]);
            }
        }
        return found;
    }

    KernelHandle* kernel() { return kernel_; }

private:
    GameServerGameplayConfig config_;
    KernelHandle* kernel_ = nullptr;
};

}  // namespace

int main() {
    const GameServerGameplayConfig config = load_catalog();
    World world(config);

    // An entrance brings its exit (max_hp 0: no health) to the destination.
    const std::uint32_t entrance = world.create("portal_entrance", KernelVec3{});
    world.step(1);
    const std::vector<KernelServerEntityState> exits =
        world.props_near(kDestination, 0.1f, 0u);
    require(exits.size() == 1u);
    const std::uint32_t exit = exits.front().net_id;
    std::printf("exit %u at (%.1f, %.1f, %.1f)\n", exit,
                exits.front().position.x, exits.front().position.y,
                exits.front().position.z);

    // Waiting at the destination: a second portal, whose gate the arrival
    // stands in (its frame starts 0.6 m on), and a volume that hurts on entry.
    world.create("portal_entrance",
                 KernelVec3{kDestination.x, 0.0f, kDestination.z + 0.6f});
    world.create("test_hurt_gate", kDestination);

    // Walk into the gate: it faces -z, the frame behind it at +z.
    const std::uint32_t player = world.player(KernelVec3{0.0f, 0.0f, -3.0f});
    world.step(10);
    const std::uint16_t full_hp = world.state(player).hp;
    int walked_ticks = 0;
    while (horizontal_distance(world.state(player).position, kDestination) > 1.0f &&
           walked_ticks < 60) {
        world.walk(player, KernelVec2{0.0f, 1.0f});
        ++walked_ticks;
    }
    const KernelServerEntityState arrived = world.state(player);
    std::printf("arrived after %d ticks at (%.2f, %.2f, %.2f), hp %u of %u\n",
                walked_ticks, arrived.position.x, arrived.position.y,
                arrived.position.z, arrived.hp, full_hp);
    require(horizontal_distance(arrived.position, kDestination) < 0.05f);
    // Facing the portal's forward, +z.
    const KernelVec3 facing = forward_of(arrived.rotation);
    require(facing.z > 0.99f);
    // Immunity: the hurt volume runs on arrival -- the tick after, since the
    // teleport happens during that tick's volume pass, which saw the player
    // where it was -- and hurts nothing.
    world.step(2);
    std::printf("two ticks after arriving: hp %u\n", world.state(player).hp);
    require(world.state(player).hp == full_hp);

    // Grace: still in the waiting portal's gate, a step to one side; it does
    // not take the player back until the grace is over.
    const KernelVec3 aside{kDestination.x, 0.0f, kDestination.z + 0.1f};
    world.move(player, aside);
    world.step(5);
    require(horizontal_distance(world.state(player).position, aside) < 0.02f);
    world.step(static_cast<int>(kGraceTicks));
    std::printf("after the grace: z %.3f\n", world.state(player).position.z);
    require(horizontal_distance(world.state(player).position, kDestination) < 0.02f);

    // The control for immunity: once it is over, walking into the hurt volume
    // hurts.
    world.move(player, KernelVec3{kDestination.x + 5.0f, 0.0f, kDestination.z});
    world.step(static_cast<int>(kImmunityTicks));
    world.move(player, KernelVec3{kDestination.x, 0.0f, kDestination.z - 0.5f});
    world.step(2);
    std::printf("hurt volume after the immunity: hp %u\n", world.state(player).hp);
    require(world.state(player).hp == full_hp - kGateDamage);

    // The exit goes with its entrance, for the same reason.
    require(world.exists(exit));
    require(Kernel_ServerDestroyEntity(
        world.kernel(), entrance, KernelDespawnReason_Expired));
    world.step(1);
    require(!world.exists(exit));
    bool exit_reason_seen = false;
    for (const auto& [net_id, reason] : world.destroyed) {
        if (net_id == exit) {
            std::printf("exit destroyed, reason %u\n", reason);
            require(reason == KernelDespawnReason_Expired);
            exit_reason_seen = true;
        }
    }
    require(exit_reason_seen);

    // Thrown along +x: the portal faces +x, its gate toward the thrower.
    const KernelVec3 thrower_at{-20.0f, 0.0f, -20.0f};
    world.move(player, thrower_at);
    world.step(10);
    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(world.kernel(), player, 8, &container));
    KernelItemInstanceId kit = 0;
    require(Kernel_ServerCreateInventoryItem(
        world.kernel(), item_template_id_of(config, "fungible_portal_kit"), 1,
        container, &kit));
    world.step(1);
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = kPeer;
    request.request_id = 1;
    request.instigator_net_id = player;
    request.domain_action = KernelDomainAction_Throw;
    request.selected_item_instance_id = kit;
    request.requested_quantity = 1;
    request.throw_direction = KernelVec3{1.0f, 0.3f, 0.0f};
    require(Kernel_ServerSubmitGameplayRequest(world.kernel(), &request));
    KernelGameplayRequestOutcome outcomes[4]{};
    for (KernelGameplayRequestOutcome& outcome : outcomes) {
        outcome.struct_size = sizeof(outcome);
    }
    std::uint32_t outcome_count = 0;
    for (int tick = 0; outcome_count == 0u && tick < 3; ++tick) {
        world.step(1);
        outcome_count = Kernel_PollGameplayRequestOutcomes(world.kernel(), outcomes, 4);
    }
    require(outcome_count >= 1u);
    std::printf("throw: status %u reason %u kit %u\n", outcomes[0].status,
                outcomes[0].rejection_reason, outcomes[0].prop_entity_id);
    require(outcomes[0].status == KernelGameplayRequestStatus_Committed);
    std::vector<KernelServerEntityState> thrown;
    for (int tick = 0; tick < 240 && thrown.empty(); ++tick) {
        world.step(1);
        // A grenade arc carries it some 35 m; the waiting portal is ~100 m off.
        thrown = world.props_near(thrower_at, 60.0f, kEntranceMaxHp);
    }
    require(thrown.size() == 1u);
    const KernelVec3 thrown_facing = forward_of(thrown.front().rotation);
    std::printf("thrown portal at (%.2f, %.2f, %.2f), facing (%.2f, %.2f)\n",
                thrown.front().position.x, thrown.front().position.y,
                thrown.front().position.z, thrown_facing.x, thrown_facing.z);
    require(thrown.front().position.x > thrower_at.x + 1.0f);
    require(thrown_facing.x > 0.99f);

    std::printf("portal_test: PASS\n");
    return 0;
}
