// Trigger volumes (docs/PORTAL_PLAN.md K1, K2; ABI 103), authored in YAML and
// run on the shipped catalog and ground.
//
// A gate prop has a trigger-only collider and on_enter / on_overlap / on_exit,
// each spawning its own marker prop where the actor stands; the markers'
// max_hp tells them apart, so every count below is what one trigger ran. What
// this pins:
//   - an actor coming in runs on_enter once, staying runs on_overlap every
//     tick after that, going out runs on_exit once, and nothing after;
//   - the volume is no body: a player walks straight through it;
//   - instigators: players leaves an agent of a counted camp out, and the
//     camp mask leaves a player out;
//   - dying inside is leaving (on_exit) and coming back to life is coming in
//     (on_enter); while dead, nothing;
//   - a sheltered actor standing in the volume counts for nothing. Note this
//     holds before the kernel's Sheltered rule is reached: entering a building
//     sets a terrain-only movement mask, which takes the capsule out of the
//     physics world, so the query never finds it (checked by removing the
//     rule: still passes). The rule stays for a sheltered actor whose capsule
//     is still there; this case pins the outcome, not the rule;
//   - an actor or a volume that is destroyed runs nothing.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
constexpr std::uint16_t kPort = 8473;
constexpr std::uint16_t kEnterMarkerHp = 11;
constexpr std::uint16_t kOverlapMarkerHp = 22;
constexpr std::uint16_t kExitMarkerHp = 33;

// A two-metre cube, trigger only.
constexpr const char* kGateCollider = R"(id: 900
name: test_gate_volume
shape: oriented_box
center: {x: 0.0, y: 1.0, z: 0.0}
half_extents: {x: 1.0, y: 1.0, z: 1.0}
purpose: trigger
layer: neutral
)";

std::string marker(std::uint32_t id, const std::string& name, std::uint16_t hp) {
    return "id: " + std::to_string(id) + "\nname: " + name +
        "\nentity_type: prop\nhealth:\n  hp: " + std::to_string(hp) +
        "\n  max_hp: " + std::to_string(hp) + "\n";
}

std::string spawn_on(const std::string& trigger,
                     const std::string& mask,
                     const std::string& instigators,
                     const std::string& marker_name) {
    return "  " + trigger + ":\n"
        "    collision_mask: " + mask + "\n"
        "    instigators: " + instigators + "\n"
        "    action_graph: action_spawn_entity_at_destroy_entity\n"
        "    parameters:\n"
        "      template: " + marker_name + "\n"
        "      position: event.position\n"
        "      owner: self\n";
}

std::string gate(std::uint32_t id,
                 const std::string& name,
                 const std::string& mask,
                 const std::string& instigators,
                 bool all_three) {
    std::string text = "id: " + std::to_string(id) + "\nname: " + name +
        "\nentity_type: prop\n"
        "physics:\n  trigger_collider_template: test_gate_volume\n"
        "triggers:\n" +
        spawn_on("on_enter", mask, instigators, "test_enter_marker");
    if (all_three) {
        text += spawn_on("on_overlap", mask, instigators, "test_overlap_marker");
        text += spawn_on("on_exit", mask, instigators, "test_exit_marker");
    }
    return text;
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good());
    stream << text;
}

GameServerGameplayConfig load_catalog_with(
    const std::vector<std::pair<std::string, std::string>>& extra) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path root =
        fs::path(tmp != nullptr ? tmp : "/tmp") / "trigger_volume_catalog";
    fs::remove_all(root);
    fs::copy("game_server/gameplay_catalog", root, fs::copy_options::recursive);
    for (const auto& [path, text] : extra) {
        write_file(root / path, text);
    }
    return network_example::game_server::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

std::uint32_t template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
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

struct Counts {
    int enter = 0;
    int overlap = 0;
    int exit = 0;
    bool operator==(const Counts&) const = default;
};

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

    void step(int ticks) {
        std::array<KernelEvent, 256> events{};
        for (int tick = 0; tick < ticks; ++tick) {
            Kernel_Update(kernel_, kTickSeconds);
            Kernel_PollEvents(
                kernel_, events.data(), static_cast<std::uint32_t>(events.size()));
        }
    }

    std::uint32_t create(const std::string& name, const KernelVec3& position,
                         std::uint32_t owner_peer = 0u) {
        const std::uint32_t template_id = template_id_of(config_, name);
        require(template_id != 0u);
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_template_id = template_id;
        create.owner_peer = owner_peer;
        create.position = position;
        create.rotation = kIdentityRotation;
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel_, &create, &net_id));
        require(net_id != 0u);
        return net_id;
    }

    std::uint32_t player(std::uint32_t peer, const KernelVec3& position) {
        const std::uint32_t template_id = config_.player.actor_template_id;
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Actor;
        create.actor_type = KernelActorType_Player;
        create.entity_template_id = template_id;
        create.actor_template_id = template_id;
        create.owner_peer = peer;
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

    KernelServerEntityState state(std::uint32_t net_id) {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel_, net_id, &out));
        return out;
    }

    Counts counts() {
        std::array<KernelServerEntityState, 512> states{};
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(state);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel_, KernelEntityType_Prop, states.data(),
            static_cast<std::uint32_t>(states.size()));
        Counts out;
        for (std::uint32_t index = 0; index < count; ++index) {
            out.enter += states[index].max_hp == kEnterMarkerHp;
            out.overlap += states[index].max_hp == kOverlapMarkerHp;
            out.exit += states[index].max_hp == kExitMarkerHp;
        }
        return out;
    }

    KernelHandle* kernel() { return kernel_; }

private:
    GameServerGameplayConfig config_;
    KernelHandle* kernel_ = nullptr;
};

void print(const char* what, const Counts& counts) {
    std::printf("%s: enter %d, overlap %d, exit %d\n",
                what, counts.enter, counts.overlap, counts.exit);
}

}  // namespace

int main() {
    const GameServerGameplayConfig config = load_catalog_with({
        {"collider_templates/900_collider_test_gate_volume.yaml", kGateCollider},
        {"entity_templates/901_prop_test_enter_marker.yaml",
         marker(901, "test_enter_marker", kEnterMarkerHp)},
        {"entity_templates/902_prop_test_overlap_marker.yaml",
         marker(902, "test_overlap_marker", kOverlapMarkerHp)},
        {"entity_templates/903_prop_test_exit_marker.yaml",
         marker(903, "test_exit_marker", kExitMarkerHp)},
        {"entity_templates/900_prop_test_gate.yaml",
         gate(900, "test_gate", "player_side | hostile_side", "players", true)},
        {"entity_templates/904_prop_test_hostile_gate.yaml",
         gate(904, "test_hostile_gate", "hostile_side", "actors", false)},
    });

    World world(config);
    const KernelVec3 outside{-10.0f, 0.0f, 0.0f};
    const KernelVec3 at_gate{0.0f, 0.0f, 0.0f};
    world.create("test_gate", at_gate);
    const std::uint32_t player = world.player(7, outside);
    world.step(10);
    require(world.counts() == Counts{});

    // In, stay, out.
    world.move(player, at_gate);
    world.step(1);
    print("after coming in", world.counts());
    require((world.counts() == Counts{1, 0, 0}));
    world.step(3);
    print("three ticks inside", world.counts());
    require((world.counts() == Counts{1, 3, 0}));
    world.move(player, outside);
    world.step(1);
    print("after going out", world.counts());
    require((world.counts() == Counts{1, 3, 1}));
    world.step(5);
    require((world.counts() == Counts{1, 3, 1}));

    // No body: walking from one side of the gate to the other is not stopped.
    world.move(player, KernelVec3{-3.0f, 0.0f, 0.0f});
    world.step(5);
    for (int tick = 0; tick < 45; ++tick) {
        KernelPlayerInput input{};
        input.move = KernelVec2{1.0f, 0.0f};
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        require(Kernel_ServerEnqueueEntityInput(
            world.kernel(), KernelCommandSource_Test, player, &input));
        world.step(1);
    }
    const float walked_to = world.state(player).position.x;
    std::printf("walked through the gate to x %.2f\n", walked_to);
    require(walked_to > 2.0f);
    const Counts walked = world.counts();
    require(walked.enter == 2 && walked.exit == 2);
    world.move(player, outside);
    world.step(2);

    // instigators: players. A hostile agent in the gate counts for nothing;
    // the hostile gate counts the agent and not a player.
    const Counts before_agent = world.counts();
    world.create("test_hostile_gate", KernelVec3{20.0f, 0.0f, 0.0f});
    const std::uint32_t agent = world.create("gingerbread", at_gate);
    world.step(5);
    print("agent in the players-only gate", world.counts());
    require(world.counts() == before_agent);
    world.move(agent, KernelVec3{20.0f, 0.0f, 0.0f});
    world.step(2);
    require((world.counts().enter == before_agent.enter + 1));
    world.move(player, KernelVec3{20.0f, 0.0f, 0.8f});
    world.step(2);
    print("player in the hostile-only gate", world.counts());
    require((world.counts().enter == before_agent.enter + 1));
    require(Kernel_ServerDestroyEntity(world.kernel(), agent, KernelDespawnReason_Destroyed));
    world.move(player, outside);
    world.step(2);

    // Dying inside is leaving; coming back to life is coming in.
    const Counts before_death = world.counts();
    world.move(player, at_gate);
    world.step(1);
    require(Kernel_ServerSetEntityHealth(world.kernel(), player, 0u));
    world.step(1);
    const Counts dead = world.counts();
    print("died inside", dead);
    require(dead.enter == before_death.enter + 1);
    require(dead.exit == before_death.exit + 1);
    world.step(5);
    require(world.counts() == dead);
    KernelServerReviveInfo revive{};
    revive.struct_size = sizeof(revive);
    revive.net_id = player;
    require(Kernel_ServerReviveEntity(world.kernel(), &revive));
    world.step(1);
    print("revived inside", world.counts());
    require(world.counts().enter == dead.enter + 1);
    world.move(player, outside);
    world.step(2);

    // Sheltered: a tent over a second gate, the player sat at its centre --
    // inside the gate -- counts for nothing. (Its capsule has left the physics
    // world; see the header.)
    const KernelVec3 tent_at{0.0f, 0.0f, 20.0f};
    world.create("test_gate", tent_at);
    const std::uint32_t tent = world.create("tent", tent_at, 7u);
    world.move(player, KernelVec3{4.0f, 0.0f, 20.0f});
    world.step(5);
    const Counts before_shelter = world.counts();
    require(Kernel_ServerEnqueueEntityShelter(
        world.kernel(), KernelCommandSource_Test, player, tent));
    world.step(5);
    const KernelVec3 inside_tent = world.state(player).position;
    std::printf("sheltered at (%.2f, %.2f, %.2f)\n",
                inside_tent.x, inside_tent.y, inside_tent.z);
    require(std::abs(inside_tent.x - tent_at.x) < 0.5f &&
            std::abs(inside_tent.z - tent_at.z) < 0.5f);
    require(world.counts() == before_shelter);
    require(Kernel_ServerEnqueueEntityShelter(
        world.kernel(), KernelCommandSource_Test, player, 0u));
    world.step(2);
    world.move(player, outside);
    world.step(2);

    // Destroyed inside: neither the actor nor the volume runs on_exit.
    const std::uint32_t second = world.player(8, outside);
    world.step(5);
    world.move(second, at_gate);
    world.step(2);
    const Counts with_second = world.counts();
    require(Kernel_ServerDestroyEntity(world.kernel(), second, KernelDespawnReason_Destroyed));
    world.step(2);
    require(world.counts().exit == with_second.exit);
    const std::uint32_t lone_gate = world.create("test_gate", KernelVec3{-20.0f, 0.0f, 0.0f});
    world.move(player, KernelVec3{-20.0f, 0.0f, 0.0f});
    world.step(2);
    const Counts in_lone = world.counts();
    require(Kernel_ServerDestroyEntity(world.kernel(), lone_gate, KernelDespawnReason_Destroyed));
    world.step(2);
    print("destroyed inside", world.counts());
    require(world.counts().exit == in_lone.exit);

    std::printf("trigger_volume_test: PASS\n");
    return 0;
}
