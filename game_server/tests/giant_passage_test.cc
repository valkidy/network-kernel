// A player can walk between a gingerbread giant's legs and out the other side.
//
// Measured through real movement on flat ground, with the session blocking
// actors as the app does, and the player's own authored collision mask. Every
// box counts, not only the legs': GEO_Hip hangs between them, and a stance
// crouch brings it down to the player's head.

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
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr float kApproach = 15.0f;
constexpr int kWalkTicks = 240;  // 40 m of walking at the player's 5 m/s.
// A line that deflects further than this slid round a box rather than passing.
constexpr float kStraightLine = 0.02f;

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    std::ifstream file(fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog" /
                           "mesh_assets" / "jolt" / "plane_200x200.joltmesh",
                       std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

struct World {
    KernelHandle* kernel = nullptr;
    std::uint32_t giant = 0;
    std::uint32_t player = 0;

    KernelServerEntityState state(std::uint32_t net_id) const {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel, net_id, &out));
        return out;
    }
};

std::uint32_t spawn(KernelHandle* kernel, std::uint32_t template_id, KernelActorType type,
                    std::uint32_t owner_peer, KernelVec3 position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = type;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.owner_peer = owner_peer;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0u);
    return net_id;
}

struct Walked {
    float along = 0.0f;  // Distance covered along the walk's direction.
    float deflection = 0.0f;  // Largest sideways drift off the line.
};

// Walks the player along z from (line_x, start_z), one step per tick. `each_tick`
// runs before each update, so a giant can be walking at the same time.
template <typename EachTick>
Walked walk(World& world, float line_x, float start_z, float direction, std::uint32_t* seq,
            EachTick each_tick) {
    const KernelVec3 start{line_x, 0.0f, start_z};
    const KernelQuat upright{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(world.kernel, world.player, &start, &upright));
    Walked out;
    for (int tick = 0; tick < kWalkTicks; ++tick) {
        KernelPlayerInput input{};
        input.input_seq = ++*seq;
        input.move = KernelVec2{0.0f, direction};
        require(Kernel_ServerSubmitEntityInput(world.kernel, world.player, &input));
        each_tick();
        Kernel_Update(world.kernel, kTickSeconds);
        const KernelVec3 at = world.state(world.player).position;
        out.along = std::max(out.along, (at.z - start_z) * direction);
        out.deflection = std::max(out.deflection, std::abs(at.x - line_x));
    }
    return out;
}

}  // namespace

int main() {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    const std::uint32_t giant_template = template_id_of(config, "gingerbread_giant");
    require(giant_template != 0u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
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

    // What the app runs; the engine's default disables actor blocking altogether.
    KernelSessionRulesConfig rules{};
    rules.struct_size = sizeof(rules);
    rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &rules));
    require(Kernel_StartDedicatedServer(kernel, 8211));

    // No GameServer: without its AI the giant stands still and swings at nobody.
    World world;
    world.kernel = kernel;
    world.giant = spawn(kernel, giant_template, KernelActorType_Agent, 0u, {0.0f, 1.0f, 0.0f});
    world.player = spawn(kernel, 1u, KernelActorType_Player, 11u, {30.0f, 1.0f, -kApproach});
    for (int tick = 0; tick < 120; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
    }

    // Grounded, with its rig solved: no ground or no feet would pass everything.
    require(std::abs(world.state(world.player).position.y) < 0.2f);
    KernelColliderShapeQuery query{};
    query.struct_size = sizeof(query);
    query.entity_net_id = world.giant;
    query.purpose_mask = KernelColliderPurpose_Limb;
    std::array<KernelColliderShapeView, 32> shapes{};
    require(Kernel_QueryColliderShapes(kernel, &query, shapes.data(),
                                       static_cast<std::uint32_t>(shapes.size())) == 8u);

    const auto nothing = []() {};
    std::uint32_t player_seq = 0;

    // The control: a line through a foot does not stay a line, so limbs really
    // are in the way. Distance would not show it: the boxes are slanted, and a
    // player slides off them and carries on.
    const Walked into_foot = walk(world, 2.7f, -kApproach, 1.0f, &player_seq, nothing);
    std::printf("into a foot: %.3f m (off line %.3f)\n", into_foot.along, into_foot.deflection);
    require(into_foot.deflection > 0.5f);

    // Between the legs, both ways, without being pushed off the line.
    const Walked forward = walk(world, 0.0f, -kApproach, 1.0f, &player_seq, nothing);
    const Walked backward = walk(world, 0.0f, kApproach, -1.0f, &player_seq, nothing);
    std::printf("standing: %.3f m forward (off line %.3f), %.3f m backward (off line %.3f)\n",
                forward.along, forward.deflection, backward.along, backward.deflection);
    require(forward.along > 2.0f * kApproach);
    require(forward.deflection < kStraightLine);
    require(backward.along > 2.0f * kApproach);
    require(backward.deflection < kStraightLine);

    // And while it walks: a stride moves the feet, so this is the tight case.
    std::uint32_t giant_seq = 0;
    const auto giant_steps = [&]() {
        KernelPlayerInput input{};
        input.input_seq = ++giant_seq;
        input.move = KernelVec2{0.0f, 1.0f};
        require(Kernel_ServerSubmitEntityInput(kernel, world.giant, &input));
    };
    for (int tick = 0; tick < 60; ++tick) {
        giant_steps();
        Kernel_Update(kernel, kTickSeconds);
    }
    const KernelVec3 giant_before = world.state(world.giant).position;
    const float start_z = giant_before.z + kApproach;
    const Walked oncoming =
        walk(world, giant_before.x, start_z, -1.0f, &player_seq, giant_steps);
    const KernelVec3 giant_after = world.state(world.giant).position;
    const KernelVec3 player_after = world.state(world.player).position;
    std::printf("walking: giant %.2f m on, player %.3f m (off line %.3f), ends %.2f m past it\n",
                giant_after.z - giant_before.z, oncoming.along, oncoming.deflection,
                giant_after.z - player_after.z);
    require(giant_after.z - giant_before.z > 10.0f);
    require(giant_after.z - player_after.z > 5.0f);
    require(oncoming.deflection < kStraightLine);

    Kernel_Destroy(kernel);
    return 0;
}
