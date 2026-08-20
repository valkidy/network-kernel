// What a followed legged rig costs the client's prediction world.
//
// Limbs multiply object count: every other actor contributes one movement
// capsule, a quadruped contributes nine boxes. The prediction world is
// query-only, so Jolt never rebuilds its broad phase on its own and
// optimize_broad_phase() is the only thing that hands retired nodes back --
// which is why adding a per-frame body set to it was worth measuring rather
// than assuming.
//
// Three numbers per rig count: what one sync_prediction_limb_proxies() costs
// (it moves every limb in place), what one character step costs (that is the
// query that now has more to sift through), and what an optimize pass costs.
//
// Reported as wall clock on this host, deliberately not asserted against a
// threshold: a timing bound would be flaky in CI and would not survive a
// different machine. Run it with
//   bazel run -c opt //game_server:limb_broad_phase_bench

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "physics/public/collision_types.h"
#include "physics/public/physics_world.h"
#include "protocol/public/network_packets.h"
#include "simulation/public/movement_solver.h"
#include "sync/public/snapshot.h"
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

double micros_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// One followed rig, placed on its own patch of ground so the rigs do not all
// land in the same broad phase cell -- a pile in one cell would flatter the
// numbers by keeping every query in one node.
struct Follower {
    network_example::NetId net_id = 0;
    glm::vec3 position{0.0f};
};

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

    // One authority, run once, purely to obtain a plausible set of planted feet
    // to hand every follower as its baseline. Solving legs is not what this
    // measures.
    KernelConfig authority_config{};
    authority_config.mode = KernelMode_DedicatedServer;
    authority_config.tick.server_tick_rate = 30;
    authority_config.tick.snapshot_rate = 30;
    authority_config.max_render_states = 64;
    authority_config.max_events = 128;
    network_example::KernelEngine authority(authority_config);
    bool scene_rejected = true;
    require(authority.load_gameplay_catalog_with_static_collision_scene(
        storage.definition, scene, &scene_rejected));
    require(!scene_rejected);
    require(authority.start_dedicated_server(7911));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = 21u;
    create.actor_template_id = 21u;
    create.position = KernelVec3{0.0f, 10.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t reference_net_id = 0u;
    require(authority.server_create_entity(create, &reference_net_id));
    for (std::uint32_t frame = 0u; frame < 90u; ++frame) {
        authority.update(1.0f / 30.0f);
    }
    const auto reference_locomotion =
        authority.locomotion_states_.find(reference_net_id);
    require(reference_locomotion != authority.locomotion_states_.end());
    KernelServerEntityState reference_state{};
    reference_state.struct_size = sizeof(reference_state);
    require(authority.server_get_entity_state(
        reference_net_id, &reference_state));
    const glm::vec3 reference_position{
        reference_state.position.x,
        reference_state.position.y,
        reference_state.position.z};

    const network_example::game_server::ColliderTemplateConfig* player_capsule =
        nullptr;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.name == "player_movement_capsule") {
            player_capsule = &collider;
        }
    }
    require(player_capsule != nullptr);

    std::printf(
        "%6s %9s %14s %16s %14s\n",
        "rigs",
        "bodies",
        "limb sync us",
        "character step us",
        "optimize us");

    for (const std::uint32_t rig_count : {0u, 1u, 4u, 16u, 64u}) {
        KernelConfig client_config{};
        client_config.mode = KernelMode_Client;
        client_config.tick.server_tick_rate = 30;
        client_config.tick.snapshot_rate = 15;
        client_config.max_render_states = 256;
        client_config.max_events = 128;
        network_example::KernelEngine client(client_config);
        client.reset_runtime_state(KernelMode_Client);
        bool client_scene_rejected = true;
        require(client.load_gameplay_catalog_with_static_collision_scene(
            storage.definition, scene, &client_scene_rejected));
        require(!client_scene_rejected);
        require(client.prepare_prediction_physics());

        std::vector<Follower> followers;
        followers.reserve(rig_count);
        for (std::uint32_t index = 0u; index < rig_count; ++index) {
            // Spread on a grid wide enough that the rigs do not overlap: a
            // quadruped's stance is about 14 m across.
            const float column = static_cast<float>(index % 8u);
            const float row = static_cast<float>(index / 8u);
            const glm::vec3 offset{column * 20.0f, 0.0f, row * 20.0f};
            Follower follower{};
            follower.net_id = 5000u + index;
            follower.position = reference_position + offset;
            followers.push_back(follower);

            network_example::EntitySpawnPacket spawn{};
            spawn.net_id = follower.net_id;
            spawn.entity_type = network_example::EntityType::kActor;
            spawn.actor_type = network_example::ActorType::kAgent;
            spawn.actor_template_id = 21u;
            spawn.entity_template_id = 21u;
            spawn.position = follower.position;
            client.handle_client_spawn(spawn);

            network_example::LocomotionStepBatchPacket baseline{};
            baseline.server_tick = authority.tick_loop_.current_tick();
            for (std::uint32_t leg_index = 0u;
                 leg_index < reference_locomotion->second.legs.size();
                 ++leg_index) {
                const network_example::LegLocomotionState& leg =
                    reference_locomotion->second.legs[leg_index];
                require(leg.foot_initialized);
                network_example::LocomotionStepRecord record{};
                record.net_id = follower.net_id;
                record.leg_index = static_cast<std::uint8_t>(leg_index);
                record.start_tick_delta = UINT8_MAX;
                record.landing_target_world = leg.foot_target_world + offset;
                baseline.records.push_back(record);
            }
            client.handle_client_locomotion_step_batch(baseline);
        }

        for (const std::uint32_t tick_offset : {0u, 2u}) {
            network_example::WorldSnapshot snapshot;
            snapshot.header.server_tick =
                authority.tick_loop_.current_tick() + tick_offset;
            for (const Follower& follower : followers) {
                network_example::EntitySnapshot entity;
                entity.net_id = follower.net_id;
                entity.type = network_example::EntityType::kActor;
                entity.actor_type = network_example::ActorType::kAgent;
                entity.position = follower.position;
                entity.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
                snapshot.entities.push_back(entity);
            }
            client.handle_client_snapshot(snapshot);
        }
        for (std::uint32_t frame = 0u; frame < 8u; ++frame) {
            client.update(1.0f / 30.0f);
        }
        client.sync_prediction_limb_proxies();
        require(
            client.prediction_limb_collider_ids_.size() == followers.size());

        std::uint32_t bodies = 0u;
        for (const auto& [net_id, ids] : client.prediction_limb_collider_ids_) {
            (void)net_id;
            bodies += static_cast<std::uint32_t>(ids.size());
        }

        constexpr std::uint32_t kIterations = 200u;
        auto start = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0u; iteration < kIterations;
             ++iteration) {
            client.sync_prediction_limb_proxies();
        }
        const double sync_us = micros_since(start) / kIterations;

        // The query that actually pays for the extra bodies. Walked through
        // open ground beside the rigs rather than into one, so the cost being
        // measured is the broad phase sifting rather than contact resolution.
        network_example::movement_solver::CharacterMovementConfig walk{};
        walk.character_id = 990001u;
        walk.shape.type = network_example::physics::CollisionShapeType::kCapsule;
        walk.shape.local_center = glm::vec3{
            player_capsule->definition.center.x,
            player_capsule->definition.center.y,
            player_capsule->definition.center.z};
        walk.shape.capsule_half_height = player_capsule->definition.shape_params.x;
        walk.shape.radius = player_capsule->definition.shape_params.y;
        walk.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        walk.max_slope_degrees = 50.0f;
        walk.step_height = 0.4f;
        walk.ground_snap_distance = 0.5f;
        walk.filter.collision_mask = KERNEL_MOVEMENT_MASK_SUPPORTED;
        network_example::movement_solver::CharacterMovementState walk_state{};
        walk_state.position = reference_position + glm::vec3{0.0f, 0.0f, -12.0f};
        std::string walk_error;
        start = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0u; iteration < kIterations;
             ++iteration) {
            require(network_example::movement_solver::step_character(
                *client.prediction_physics_world_,
                walk,
                glm::vec3{1.0f, 0.0f, 0.0f},
                1.0f / 30.0f,
                &walk_state,
                &walk_error));
        }
        const double step_us = micros_since(start) / kIterations;

        start = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0u; iteration < kIterations;
             ++iteration) {
            client.prediction_physics_world_->optimize_broad_phase();
        }
        const double optimize_us = micros_since(start) / kIterations;

        std::printf(
            "%6u %9u %14.2f %16.2f %14.2f\n",
            rig_count,
            bodies,
            sync_us,
            step_us,
            optimize_us);
    }

    return 0;
}
