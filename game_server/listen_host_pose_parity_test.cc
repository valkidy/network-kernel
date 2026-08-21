// What a listen host presents for a rig it simulates itself.
//
// A listen host is meant to stand in for the dedicated server it is a
// single-process version of: you run the host scene, watch the monster walk,
// and conclude something about what players will see. That conclusion is only
// worth anything if the host renders the rig the same way a client does --
// from the locomotion steps that went over the wire, not from the
// authoritative solve it happens to have in the same process.
//
// It did not, until the follower reconstruction was allowed to cover entities
// the kernel also simulates. This test pins both halves of that:
//
//   1. a listen host builds follower locomotion state for its OWN simulated
//      rig, which is what makes its presentation the client's answer rather
//      than the authority's. This is the assertion that fails outright on the
//      old behaviour, where the follower deliberately skipped these entities.
//
//   2. a second, genuinely separate client kernel -- fed only what the wire
//      carries, and with no terrain to raycast against -- presents the same
//      bones in the same places. Two engines rather than one: if the host were
//      still drawing its own authoritative pose, there would be no reason for
//      the numbers to agree.
//
// The subject is quadruped_actor (21) on simplified_quadruped, the same rig
// limb_collider_test measures.

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

#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "protocol/public/network_packets.h"
#include "sync/public/snapshot.h"
// Reads the engine's own follower locomotion map, which no public entry point
// exposes. Same access route the other game_server tests take.
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

// Bone locals are metres and unit quaternions built by the same solver on both
// sides, so agreement is exact up to float arithmetic order, not approximate.
constexpr float kBoneEpsilon = 0.001f;

bool near_value(float lhs, float rhs, float epsilon) {
    const float difference = lhs - rhs;
    return (difference < 0.0f ? -difference : difference) < epsilon;
}

// The presented pose for one entity, flattened out of the two parallel arrays
// the skeleton read-back hands over.
struct PresentedPose {
    bool found = false;
    std::uint32_t pose_flags = 0u;
    std::uint32_t skeleton_asset_id = 0u;
    std::uint64_t skeleton_content_hash = 0u;
    std::vector<KernelBoneLocalTransform> bones;
};

PresentedPose read_presented_pose(
    network_example::KernelEngine& engine,
    std::uint32_t net_id) {
    std::array<KernelSkeletonRenderState, 16> states{};
    std::array<KernelBoneLocalTransform, 1024> bones{};
    KernelSkeletonRenderStateResult result{};
    result.struct_size = sizeof(result);
    const std::uint32_t count = engine.get_skeleton_render_states(
        states.data(),
        static_cast<std::uint32_t>(states.size()),
        bones.data(),
        static_cast<std::uint32_t>(bones.size()),
        &result);
    require(result.status == KERNEL_SKELETON_RENDER_STATUS_SUCCESS);
    PresentedPose pose;
    for (std::uint32_t index = 0u; index < count; ++index) {
        if (states[index].entity_net_id != net_id) {
            continue;
        }
        pose.found = true;
        pose.pose_flags = states[index].pose_flags;
        pose.skeleton_asset_id = states[index].skeleton_asset_id;
        pose.skeleton_content_hash = states[index].skeleton_content_hash;
        for (std::uint32_t bone = 0u; bone < states[index].bone_count; ++bone) {
            pose.bones.push_back(
                bones[states[index].first_bone_transform + bone]);
        }
    }
    return pose;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" /
         "bundle.zip")
            .string());
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "legged_locomotion_gameplay_catalog.yaml");
    network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);

    // The host gets the terrain: a foothold raycast that finds nothing leaves
    // every foot uninitialised, and a rig that never plants a foot publishes no
    // steps -- the parity check would then compare two bind poses and pass for
    // the wrong reason.
    const std::vector<std::uint8_t> scene_bytes =
        network_example::game_server::load_gameplay_bundle_entry_bytes(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            config.static_collision_scene.entry_path);
    require(!scene_bytes.empty());
    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(scene_bytes.size());
    scene.scene_id = config.static_collision_scene.scene_id;
    scene.collider_id = config.static_collision_scene.collider_id;
    scene.collision_layer = config.static_collision_scene.collision_layer;

    KernelConfig host_config{};
    host_config.mode = KernelMode_ListenServer;
    host_config.tick.server_tick_rate = 30;
    host_config.tick.snapshot_rate = 30;
    host_config.max_render_states = 64;
    host_config.max_events = 128;
    network_example::KernelEngine host(host_config);
    bool scene_rejected = true;
    require(host.load_gameplay_catalog_with_static_collision_scene(
        storage.definition, scene, &scene_rejected));
    require(!scene_rejected);
    require(host.start_listen_server(7908));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = 21u;
    create.actor_template_id = 21u;
    create.position = KernelVec3{0.0f, 10.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0u;
    require(host.server_create_entity(create, &net_id));
    require(net_id != 0u);

    // Long enough to fall onto the terrain, plant every foot, and for the local
    // session's own snapshots and step baseline to have made the round trip
    // through the loopback transport.
    for (std::uint32_t frame = 0u; frame < 90u; ++frame) {
        host.update(1.0f / 30.0f);
    }

    // (1) The host reconstructs its own simulated rig. Before this behaviour
    // existed the follower skipped anything in locomotion_states_, so this map
    // was empty for exactly the entities a host scene is watched for.
    require(host.locomotion_states_.count(net_id) == 1u);
    require(host.follower_locomotion_states_.count(net_id) == 1u);

    const PresentedPose host_pose = read_presented_pose(host, net_id);
    require(host_pose.found);
    require(host_pose.pose_flags == KERNEL_SKELETON_POSE_FLAG_PROCEDURAL);
    require(!host_pose.bones.empty());

    require(host.follower_locomotion_states_.find(net_id)->second.pose_valid);

    // Two rest poses would satisfy every equality below while meaning no solve
    // ran on either side, so establish up front that the host is presenting
    // something a solve produced: at least one bone is off its bind transform.
    std::vector<KernelBoneLocalTransform> bind_pose(host_pose.bones.size());
    require(host.get_skeleton_bind_pose(
                host_pose.skeleton_asset_id,
                host_pose.skeleton_content_hash,
                bind_pose.data(),
                static_cast<std::uint32_t>(bind_pose.size())) ==
            host_pose.bones.size());
    bool any_bone_moved = false;
    for (std::size_t bone = 0u; bone < host_pose.bones.size(); ++bone) {
        any_bone_moved = any_bone_moved ||
            !near_value(
                host_pose.bones[bone].local_position.y,
                bind_pose[bone].local_position.y,
                kBoneEpsilon) ||
            !near_value(
                host_pose.bones[bone].local_rotation.w,
                bind_pose[bone].local_rotation.w,
                kBoneEpsilon);
    }
    require(any_bone_moved);

    KernelServerEntityState host_state{};
    host_state.struct_size = sizeof(host_state);
    require(host.server_get_entity_state(net_id, &host_state));

    // ------------------------------------------------------------------
    // (2) A separate client kernel, fed only what the wire carries.
    KernelConfig client_config{};
    client_config.mode = KernelMode_Client;
    client_config.tick.server_tick_rate = 30;
    client_config.tick.snapshot_rate = 30;
    client_config.max_render_states = 64;
    client_config.max_events = 128;
    network_example::KernelEngine client(client_config);
    client.reset_runtime_state(KernelMode_Client);
    // No terrain on the client on purpose: it never casts a foothold ray, and
    // proving that is part of the point.
    require(client.load_gameplay_catalog(storage.definition));

    network_example::EntitySpawnPacket spawn{};
    spawn.net_id = net_id;
    spawn.entity_type = network_example::EntityType::kActor;
    spawn.actor_type = network_example::ActorType::kAgent;
    spawn.actor_template_id = 21u;
    spawn.entity_template_id = 21u;
    spawn.position = glm::vec3{
        host_state.position.x, host_state.position.y, host_state.position.z};
    client.handle_client_spawn(spawn);

    // The baseline the server sends beside a spawn: every planted foot as a
    // step that already finished, which the follower applies by planting it
    // outright. Built from the host's own feet, the way
    // KernelEngine::send_locomotion_baseline builds it.
    const auto host_locomotion = host.locomotion_states_.find(net_id);
    require(host_locomotion != host.locomotion_states_.end());
    const std::uint32_t host_tick = host.tick_loop_.current_tick();
    network_example::LocomotionStepBatchPacket baseline{};
    baseline.server_tick = host_tick;
    for (std::uint32_t leg_index = 0u;
         leg_index < host_locomotion->second.legs.size();
         ++leg_index) {
        const network_example::LegLocomotionState& leg =
            host_locomotion->second.legs[leg_index];
        require(leg.foot_initialized);
        network_example::LocomotionStepRecord record{};
        record.net_id = net_id;
        record.leg_index = static_cast<std::uint8_t>(leg_index);
        record.start_tick_delta = UINT8_MAX;
        record.landing_target_world = leg.foot_target_world;
        baseline.records.push_back(record);
    }
    client.handle_client_locomotion_step_batch(baseline);

    const auto push_snapshot = [&](std::uint32_t server_tick) {
        network_example::WorldSnapshot snapshot;
        snapshot.header.server_tick = server_tick;
        network_example::EntitySnapshot entity;
        entity.net_id = net_id;
        entity.type = network_example::EntityType::kActor;
        entity.actor_type = network_example::ActorType::kAgent;
        entity.position = glm::vec3{
            host_state.position.x,
            host_state.position.y,
            host_state.position.z};
        entity.rotation = glm::quat{
            host_state.rotation.w,
            host_state.rotation.x,
            host_state.rotation.y,
            host_state.rotation.z};
        snapshot.entities.push_back(entity);
        client.handle_client_snapshot(snapshot);
    };
    // Two ticks apart so the buffer has a span to interpolate the root over.
    push_snapshot(host_tick);
    push_snapshot(host_tick + 2u);
    for (std::uint32_t frame = 0u; frame < 8u; ++frame) {
        client.update(1.0f / 30.0f);
    }

    require(client.follower_locomotion_states_.count(net_id) == 1u);
    const PresentedPose client_pose = read_presented_pose(client, net_id);
    require(client_pose.found);
    require(client_pose.pose_flags == KERNEL_SKELETON_POSE_FLAG_PROCEDURAL);
    require(client_pose.bones.size() == host_pose.bones.size());

    for (std::size_t bone = 0u; bone < host_pose.bones.size(); ++bone) {
        const KernelBoneLocalTransform& lhs = host_pose.bones[bone];
        const KernelBoneLocalTransform& rhs = client_pose.bones[bone];
        require(near_value(lhs.local_position.x, rhs.local_position.x, kBoneEpsilon));
        require(near_value(lhs.local_position.y, rhs.local_position.y, kBoneEpsilon));
        require(near_value(lhs.local_position.z, rhs.local_position.z, kBoneEpsilon));
        require(near_value(lhs.local_rotation.x, rhs.local_rotation.x, kBoneEpsilon));
        require(near_value(lhs.local_rotation.y, rhs.local_rotation.y, kBoneEpsilon));
        require(near_value(lhs.local_rotation.z, rhs.local_rotation.z, kBoneEpsilon));
        require(near_value(lhs.local_rotation.w, rhs.local_rotation.w, kBoneEpsilon));
    }

    std::printf("listen host pose parity: %zu bones matched\n",
                host_pose.bones.size());
    return 0;
}
