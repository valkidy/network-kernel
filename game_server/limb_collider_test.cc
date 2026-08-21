// The authoritative side of per-bone limb colliders, measured through the same
// public ABI the Unity plugin uses.
//
// This is the ground truth the client side is compared against. It runs on a
// dedicated-server kernel deliberately: that (and the listen host) is the
// environment the game actually ships in, so "the legs are where the rig says
// they are" has to be true here first, and anything the client does is then a
// parity claim against these numbers rather than an observation of its own.
//
// The subject is quadruped_actor (21) on simplified_quadruped, whose rig
// declares nine colliders -- eight leg segments plus the body. Their bone
// indices and rest scales come from the skeleton manifest and are asserted
// here by value, because a rig edit that silently drops or renames a GEO_ bone
// would otherwise show up only as legs that stop colliding.

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "game_server/game_server.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "protocol/public/network_packets.h"
#include "physics/public/collision_types.h"
#include "simulation/public/collision_filter.h"
#include "simulation/public/movement_solver.h"
#include "physics/public/physics_world.h"
#include "sync/public/snapshot.h"
// The follower half below reads the engine's own client state -- the follower
// locomotion map and the collider registry -- which no public entry point
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

// GEO_ bone indices in simplified_quadruped, in the order the rig declares its
// colliders: leg0 upper/lower, leg1 upper/lower, leg2, leg3, then the body.
constexpr std::array<std::uint32_t, 9> kExpectedBoneIndices = {
    5u, 7u, 10u, 12u, 15u, 17u, 20u, 22u, 24u};

// Half of each GEO_ bone's rest scale: the upper segments are 1.5 x 6.0 x 1.5,
// the lower ones 1.5 x 19.0 x 1.5, and the body a 6.0 cube.
constexpr float kUpperHalfY = 3.0f;
constexpr float kLowerHalfY = 9.5f;
constexpr float kLegHalfXZ = 0.75f;
constexpr float kBodyHalf = 3.0f;

bool near_value(float lhs, float rhs) {
    const float difference = lhs - rhs;
    return (difference < 0.0f ? -difference : difference) < 0.001f;
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

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_render_states = 64;
    kernel_config.max_events = 128;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);

    KernelGameplayCatalogLoadResult load_result{};
    load_result.struct_size = sizeof(load_result);
    require(Kernel_LoadGameplayCatalogFromMemory(
        kernel,
        bundle.data(),
        static_cast<std::uint32_t>(bundle.size()),
        "legged_locomotion_gameplay_catalog.yaml",
        &load_result));
    require(load_result.status == KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS);
    require(Kernel_StartDedicatedServer(kernel, 7906));
    network_example::game_server::GameServer server(kernel, config);

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = KernelActorType_Agent;
    create.entity_template_id = 21u;
    create.actor_template_id = 21u;
    create.position = KernelVec3{0.0f, 10.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0u;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0u);

    // Long enough for the actor to settle on the ground and for the locomotion
    // solve to plant every foot: the colliders are published from the solved
    // pose, so a rig that never solves publishes nothing at all.
    for (std::uint32_t frame = 0u; frame < 90u; ++frame) {
        Kernel_Update(kernel, 1.0f / 30.0f);
        std::array<KernelEvent, 64> events{};
        const std::uint32_t event_count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0u; index < event_count; ++index) {
            server.handle_event(events[index]);
        }
        server.tick(1.0f / 30.0f);
    }

    // Limbs are their own purpose, which is what keeps them out of every query
    // that has not asked for them. Asking by that flag is the only way to see
    // them, and it must return exactly the rig's nine.
    KernelColliderShapeQuery query{};
    query.struct_size = sizeof(query);
    query.entity_net_id = net_id;
    query.purpose_mask = KernelColliderPurpose_Limb;
    std::array<KernelColliderShapeView, 32> shapes{};
    const std::uint32_t shape_count = Kernel_QueryColliderShapes(
        kernel,
        &query,
        shapes.data(),
        static_cast<std::uint32_t>(shapes.size()));
    require(shape_count == kExpectedBoneIndices.size());

    std::uint32_t body_count = 0u;
    std::uint32_t upper_count = 0u;
    std::uint32_t lower_count = 0u;
    for (std::uint32_t index = 0u; index < shape_count; ++index) {
        const KernelColliderShapeView& shape = shapes[index];
        // A limb belongs to its actor and carries no collider template: the rig
        // is where its geometry comes from, so there is nothing to point at.
        require(shape.entity_net_id == net_id);
        require(shape.collider_template_id == 0u);
        require(shape.shape_type == KernelColliderShapeType_OrientedBox);
        // limb | hit, per quadruped_actor.yaml's collision_flags.
        require((shape.purpose_flags & KernelColliderPurpose_Limb) != 0u);
        require((shape.purpose_flags & KernelColliderPurpose_Hit) != 0u);
        require(shape.layer_mask == KERNEL_COLLISION_LAYER_HOSTILE_SIDE);
        require(shape.collider_id != 0u);

        // Half extents come from the GEO_ bone's rest scale, so each shape must
        // be one of the rig's three sizes rather than a unit box -- the scale
        // being divided out of the solved rotation and not put back is exactly
        // how that regresses.
        if (near_value(shape.shape_params.y, kBodyHalf) &&
            near_value(shape.shape_params.x, kBodyHalf)) {
            ++body_count;
        } else if (near_value(shape.shape_params.y, kUpperHalfY)) {
            require(near_value(shape.shape_params.x, kLegHalfXZ));
            require(near_value(shape.shape_params.z, kLegHalfXZ));
            ++upper_count;
        } else {
            require(near_value(shape.shape_params.y, kLowerHalfY));
            require(near_value(shape.shape_params.x, kLegHalfXZ));
            require(near_value(shape.shape_params.z, kLegHalfXZ));
            ++lower_count;
        }
    }
    require(body_count == 1u);
    require(upper_count == 4u);
    require(lower_count == 4u);

    // Every collider sits at a distinct place: nine boxes stacked on the origin
    // would satisfy the counts above while meaning the solve never ran.
    for (std::uint32_t left = 0u; left < shape_count; ++left) {
        for (std::uint32_t right = left + 1u; right < shape_count; ++right) {
            require(
                !near_value(shapes[left].world_center.x, shapes[right].world_center.x) ||
                !near_value(shapes[left].world_center.y, shapes[right].world_center.y) ||
                !near_value(shapes[left].world_center.z, shapes[right].world_center.z));
        }
    }

    // The legs move with the body rather than staying where they were first
    // published, which is the property a follower has to reproduce.
    std::array<KernelVec3, 9> first_centers{};
    for (std::uint32_t index = 0u; index < shape_count; ++index) {
        first_centers[index] = shapes[index].world_center;
    }
    KernelVec3 moved_position{12.0f, 10.0f, 0.0f};
    KernelQuat upright{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel, net_id, &moved_position, &upright));
    for (std::uint32_t frame = 0u; frame < 30u; ++frame) {
        Kernel_Update(kernel, 1.0f / 30.0f);
        server.tick(1.0f / 30.0f);
    }
    const std::uint32_t moved_count = Kernel_QueryColliderShapes(
        kernel,
        &query,
        shapes.data(),
        static_cast<std::uint32_t>(shapes.size()));
    require(moved_count == kExpectedBoneIndices.size());
    bool any_moved = false;
    for (std::uint32_t index = 0u; index < moved_count; ++index) {
        any_moved = any_moved ||
            !near_value(shapes[index].world_center.x, first_centers[index].x);
    }
    require(any_moved);

    // A query that does not name limbs must not see them. This is what lets
    // every existing weapon and movement query keep its cost and its results
    // while a rig hangs a dozen boxes on itself.
    KernelColliderShapeQuery hit_query{};
    hit_query.struct_size = sizeof(hit_query);
    hit_query.entity_net_id = net_id;
    hit_query.purpose_mask = KernelColliderPurpose_Movement;
    const std::uint32_t movement_count = Kernel_QueryColliderShapes(
        kernel,
        &hit_query,
        shapes.data(),
        static_cast<std::uint32_t>(shapes.size()));
    for (std::uint32_t index = 0u; index < movement_count; ++index) {
        require((shapes[index].purpose_flags & KernelColliderPurpose_Limb) == 0u);
    }

    Kernel_Destroy(kernel);

    // ---------------------------------------------------------------------
    // Client parity.
    //
    // The claim is NOT "the client draws something leg-shaped" -- it is that a
    // client kernel derives the same nine boxes, in the same places, from the
    // replicated root plus replicated steps alone. That is the whole argument
    // for spending zero snapshot bytes on limbs, so it is asserted against the
    // authority's own numbers rather than against constants.
    //
    // Two engines rather than one: an authority to be the reference, and a
    // pure client to reproduce it. The client is fed exactly what the wire
    // would carry -- a spawn, a locomotion baseline, and snapshots.
    network_example::game_server::KernelGameplayCatalogStorage storage =
        network_example::game_server::build_kernel_gameplay_catalog(config);

    KernelConfig authority_config{};
    authority_config.mode = KernelMode_DedicatedServer;
    authority_config.tick.server_tick_rate = 30;
    authority_config.tick.snapshot_rate = 30;
    authority_config.max_render_states = 64;
    authority_config.max_events = 128;
    network_example::KernelEngine authority(authority_config);
    // With the terrain, not without it: a foothold raycast that finds nothing
    // leaves every foot uninitialised, and an actor with no planted feet
    // publishes no limbs at all -- the parity check would then compare two
    // empty sets and pass for the wrong reason.
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
    bool scene_rejected = true;
    require(authority.load_gameplay_catalog_with_static_collision_scene(
        storage.definition, scene, &scene_rejected));
    require(!scene_rejected);
    require(authority.start_dedicated_server(7907));

    KernelServerEntityCreateInfo parity_create = create;
    std::uint32_t parity_net_id = 0u;
    require(authority.server_create_entity(parity_create, &parity_net_id));
    require(parity_net_id != 0u);
    for (std::uint32_t frame = 0u; frame < 90u; ++frame) {
        authority.update(1.0f / 30.0f);
    }

    std::array<KernelColliderShapeView, 32> authority_shapes{};
    KernelColliderShapeQuery parity_query{};
    parity_query.struct_size = sizeof(parity_query);
    parity_query.entity_net_id = parity_net_id;
    parity_query.purpose_mask = KernelColliderPurpose_Limb;
    const std::uint32_t authority_count = authority.query_collider_shapes(
        &parity_query,
        authority_shapes.data(),
        static_cast<std::uint32_t>(authority_shapes.size()));
    require(authority_count == kExpectedBoneIndices.size());

    KernelServerEntityState authority_state{};
    authority_state.struct_size = sizeof(authority_state);
    require(authority.server_get_entity_state(parity_net_id, &authority_state));

    KernelConfig follower_config{};
    follower_config.mode = KernelMode_Client;
    follower_config.tick.server_tick_rate = 30;
    follower_config.tick.snapshot_rate = 15;
    follower_config.max_render_states = 64;
    follower_config.max_events = 128;
    network_example::KernelEngine follower(follower_config);
    follower.reset_runtime_state(KernelMode_Client);
    // No scene on the follower on purpose: it never casts a foothold ray, and
    // proving that is part of the point.
    require(follower.load_gameplay_catalog(storage.definition));

    network_example::EntitySpawnPacket spawn{};
    spawn.net_id = parity_net_id;
    spawn.entity_type = network_example::EntityType::kActor;
    spawn.actor_type = network_example::ActorType::kAgent;
    spawn.actor_template_id = 21u;
    spawn.entity_template_id = 21u;
    spawn.position = glm::vec3{
        authority_state.position.x,
        authority_state.position.y,
        authority_state.position.z};
    follower.handle_client_spawn(spawn);

    // The baseline the server sends beside a spawn: every planted foot as a
    // step that already finished, which the follower applies by planting it
    // outright. Built here the way KernelEngine::send_locomotion_baseline
    // builds it, from the authority's own feet.
    const auto authority_locomotion =
        authority.locomotion_states_.find(parity_net_id);
    require(authority_locomotion != authority.locomotion_states_.end());
    const std::uint32_t authority_tick = authority.tick_loop_.current_tick();
    network_example::LocomotionStepBatchPacket baseline{};
    baseline.server_tick = authority_tick;
    for (std::uint32_t leg_index = 0u;
         leg_index < authority_locomotion->second.legs.size();
         ++leg_index) {
        const network_example::LegLocomotionState& leg =
            authority_locomotion->second.legs[leg_index];
        require(leg.foot_initialized);
        network_example::LocomotionStepRecord record{};
        record.net_id = parity_net_id;
        record.leg_index = static_cast<std::uint8_t>(leg_index);
        record.start_tick_delta = UINT8_MAX;
        record.landing_target_world = leg.foot_target_world;
        baseline.records.push_back(record);
    }
    follower.handle_client_locomotion_step_batch(baseline);

    const auto push_snapshot = [&](std::uint32_t server_tick) {
        network_example::WorldSnapshot snapshot;
        snapshot.header.server_tick = server_tick;
        network_example::EntitySnapshot entity;
        entity.net_id = parity_net_id;
        entity.type = network_example::EntityType::kActor;
        entity.actor_type = network_example::ActorType::kAgent;
        entity.position = glm::vec3{
            authority_state.position.x,
            authority_state.position.y,
            authority_state.position.z};
        entity.rotation = glm::quat{
            authority_state.rotation.w,
            authority_state.rotation.x,
            authority_state.rotation.y,
            authority_state.rotation.z};
        snapshot.entities.push_back(entity);
        follower.handle_client_snapshot(snapshot);
    };
    // Two ticks apart so the buffer has a span to interpolate the root over.
    push_snapshot(authority_tick);
    push_snapshot(authority_tick + 2u);
    for (std::uint32_t frame = 0u; frame < 8u; ++frame) {
        follower.update(1.0f / 30.0f);
    }

    require(follower.follower_locomotion_states_.count(parity_net_id) == 1u);
    std::array<KernelColliderShapeView, 32> follower_shapes{};
    const std::uint32_t follower_count = follower.query_collider_shapes(
        &parity_query,
        follower_shapes.data(),
        static_cast<std::uint32_t>(follower_shapes.size()));
    require(follower_count == authority_count);

    // Same derivation on both sides, so the flags, the shape and the size must
    // be identical rather than merely similar -- they are read out of the same
    // rig by the same function.
    float worst_offset = 0.0f;
    for (std::uint32_t index = 0u; index < follower_count; ++index) {
        const KernelColliderShapeView& lhs = authority_shapes[index];
        const KernelColliderShapeView& rhs = follower_shapes[index];
        require(rhs.entity_net_id == lhs.entity_net_id);
        require(rhs.collider_template_id == 0u);
        require(rhs.shape_type == lhs.shape_type);
        require(rhs.purpose_flags == lhs.purpose_flags);
        require(rhs.layer_mask == lhs.layer_mask);
        require(near_value(rhs.shape_params.x, lhs.shape_params.x));
        require(near_value(rhs.shape_params.y, lhs.shape_params.y));
        require(near_value(rhs.shape_params.z, lhs.shape_params.z));
        const float dx = rhs.world_center.x - lhs.world_center.x;
        const float dy = rhs.world_center.y - lhs.world_center.y;
        const float dz = rhs.world_center.z - lhs.world_center.z;
        const float offset = dx * dx + dy * dy + dz * dz;
        worst_offset = offset > worst_offset ? offset : worst_offset;
    }
    // Guard against the comparison passing on two degenerate sets: the
    // follower's own nine must be as spread out as the authority's, not nine
    // boxes sharing an origin.
    for (std::uint32_t left = 0u; left < follower_count; ++left) {
        for (std::uint32_t right = left + 1u; right < follower_count; ++right) {
            require(
                !near_value(
                    follower_shapes[left].world_center.x,
                    follower_shapes[right].world_center.x) ||
                !near_value(
                    follower_shapes[left].world_center.y,
                    follower_shapes[right].world_center.y) ||
                !near_value(
                    follower_shapes[left].world_center.z,
                    follower_shapes[right].world_center.z));
        }
    }
    worst_offset = std::sqrt(worst_offset);
    std::printf(
        "limb parity: worst limb centre offset %.4f m over %u colliders\n",
        static_cast<double>(worst_offset),
        follower_count);
    // Tight on purpose. The follower reaches these positions from the
    // replicated root and the baseline alone, so anything above a few
    // centimetres means the two sides are deriving the pose differently, not
    // that the wire lost precision.
    require(worst_offset < 0.05f);

    // ---------------------------------------------------------------------
    // The movement layer.
    //
    // Registering limbs in the physics world is not the same as anything
    // colliding with them: kActorLimb is off every default mask, so a query
    // only sees limbs if it names them. Both halves of that are asserted here
    // against the authority's own world, using the same filter a character
    // move builds.
    const network_example::game_server::ActorTemplateConfig* player_template =
        network_example::game_server::find_actor_template(config, 1u);
    require(player_template != nullptr);
    require(
        (player_template->movement_collision_mask &
         KERNEL_MOVEMENT_LAYER_LIMB) != 0u);
    // Authoring the opt-in layer must not have quietly dropped the rest.
    require(
        (player_template->movement_collision_mask &
         KERNEL_MOVEMENT_MASK_DEFAULT) == KERNEL_MOVEMENT_MASK_DEFAULT);

    require(authority.physics_world_ != nullptr);
    // Aim at the longest segment -- a lower leg -- straight through its centre,
    // from far enough out to be unambiguously outside it.
    const KernelColliderShapeView* target = nullptr;
    for (std::uint32_t index = 0u; index < authority_count; ++index) {
        if (near_value(authority_shapes[index].shape_params.y, kLowerHalfY)) {
            target = &authority_shapes[index];
            break;
        }
    }
    require(target != nullptr);

    network_example::physics::RayCastRequest ray{};
    ray.origin = glm::vec3{
        target->world_center.x - 8.0f,
        target->world_center.y,
        target->world_center.z};
    ray.direction = glm::vec3{1.0f, 0.0f, 0.0f};
    ray.max_distance = 16.0f;
    ray.filter.collision_mask = KERNEL_MOVEMENT_MASK_SUPPORTED;
    network_example::physics::CollisionHit limb_hit{};
    require(authority.physics_world_->ray_cast_closest(ray, &limb_hit));
    require(limb_hit.identity.kind == network_example::physics::CollisionObjectKind::kActorLimb);
    require(limb_hit.identity.layer == network_example::physics::CollisionLayer::kActorLimb);
    require(limb_hit.identity.entity_net_id == parity_net_id);

    // The same ray under the engine default finds nothing: the limb is the only
    // thing on that line, and nothing that has not asked for limbs may see it.
    ray.filter.collision_mask = KERNEL_MOVEMENT_MASK_DEFAULT;
    network_example::physics::CollisionHit default_hit{};
    const bool default_found =
        authority.physics_world_->ray_cast_closest(ray, &default_hit);
    require(
        !default_found ||
        default_hit.identity.kind != network_example::physics::CollisionObjectKind::kActorLimb);

    // ---------------------------------------------------------------------
    // The client's prediction world.
    //
    // The registry copy above is geometry: it says where the legs are and is
    // what a debug view reads. Nothing can walk into it. This is the other
    // half -- the same nine boxes as bodies in the world the predicted local
    // player actually moves through -- and it is a separate follower because
    // a prediction world only exists once a client has a verified static
    // collision scene, which the parity follower above deliberately lacks.
    network_example::KernelEngine predicting(follower_config);
    predicting.reset_runtime_state(KernelMode_Client);
    bool predicting_scene_rejected = true;
    require(predicting.load_gameplay_catalog_with_static_collision_scene(
        storage.definition, scene, &predicting_scene_rejected));
    require(!predicting_scene_rejected);
    // The welcome packet is what builds this world in a real session; there is
    // no session here, so the same call is made directly.
    require(predicting.prepare_prediction_physics());
    require(predicting.prediction_physics_world_ != nullptr);

    predicting.handle_client_spawn(spawn);
    predicting.handle_client_locomotion_step_batch(baseline);
    const auto push_predicting_snapshot = [&](std::uint32_t server_tick) {
        network_example::WorldSnapshot snapshot;
        snapshot.header.server_tick = server_tick;
        network_example::EntitySnapshot entity;
        entity.net_id = parity_net_id;
        entity.type = network_example::EntityType::kActor;
        entity.actor_type = network_example::ActorType::kAgent;
        entity.position = glm::vec3{
            authority_state.position.x,
            authority_state.position.y,
            authority_state.position.z};
        entity.rotation = glm::quat{
            authority_state.rotation.w,
            authority_state.rotation.x,
            authority_state.rotation.y,
            authority_state.rotation.z};
        snapshot.entities.push_back(entity);
        predicting.handle_client_snapshot(snapshot);
    };
    push_predicting_snapshot(authority_tick);
    push_predicting_snapshot(authority_tick + 2u);
    for (std::uint32_t frame = 0u; frame < 8u; ++frame) {
        predicting.update(1.0f / 30.0f);
    }
    require(predicting.follower_locomotion_states_.count(parity_net_id) == 1u);

    predicting.sync_prediction_limb_proxies();
    const auto predicted_proxies =
        predicting.prediction_limb_collider_ids_.find(parity_net_id);
    require(predicted_proxies != predicting.prediction_limb_collider_ids_.end());
    require(predicted_proxies->second.size() == kExpectedBoneIndices.size());

    // The same ray as on the authority, in the client's own world: a limb is
    // there to be walked into, and only for a query that names the layer.
    network_example::physics::CollisionHit predicted_hit{};
    ray.filter.collision_mask = KERNEL_MOVEMENT_MASK_SUPPORTED;
    require(predicting.prediction_physics_world_->ray_cast_closest(
        ray, &predicted_hit));
    require(
        predicted_hit.identity.kind ==
        network_example::physics::CollisionObjectKind::kActorLimb);
    require(predicted_hit.identity.entity_net_id == parity_net_id);
    // Within a millimetre of where the authority's ray struck: both worlds hold
    // the same nine boxes in the same places, which is the whole claim.
    require(near_value(predicted_hit.position.x, limb_hit.position.x));
    require(near_value(predicted_hit.position.y, limb_hit.position.y));
    require(near_value(predicted_hit.position.z, limb_hit.position.z));

    ray.filter.collision_mask = KERNEL_MOVEMENT_MASK_DEFAULT;
    network_example::physics::CollisionHit predicted_default_hit{};
    const bool predicted_default_found =
        predicting.prediction_physics_world_->ray_cast_closest(
            ray, &predicted_default_hit);
    require(
        !predicted_default_found ||
        predicted_default_hit.identity.kind !=
            network_example::physics::CollisionObjectKind::kActorLimb);

    // ---------------------------------------------------------------------
    // The thing all of the above exists for: a character that cannot walk
    // through a leg.
    //
    // Every assertion so far has been a ray or an inventory. A ray proves the
    // layer filter; it does not prove that the controller stops, which is the
    // only claim a player can actually observe. Same capsule, same authored
    // mask, run twice with the limb bit as the only difference.
    const network_example::game_server::ColliderTemplateConfig* player_capsule =
        nullptr;
    for (const network_example::game_server::ColliderTemplateConfig& collider :
         config.colliders.templates) {
        if (collider.name == "player_movement_capsule") {
            player_capsule = &collider;
        }
    }
    require(player_capsule != nullptr);

    // Where the ground is under the rig, and where a leg actually is at walking
    // height. Both are found by casting rather than assumed: a quadruped's legs
    // are bent, so the height a segment's centre sits at says little about
    // where it crosses the line a walker travels along.
    network_example::physics::RayCastRequest down{};
    down.origin = glm::vec3{
        authority_state.position.x,
        authority_state.position.y + 60.0f,
        authority_state.position.z};
    down.direction = glm::vec3{0.0f, -1.0f, 0.0f};
    down.max_distance = 200.0f;
    down.filter.collision_mask =
        network_example::physics::collision_layer_bit(
            network_example::physics::CollisionLayer::kTerrain);
    network_example::physics::CollisionHit ground_hit{};
    require(authority.physics_world_->ray_cast_closest(down, &ground_hit));
    const float walk_height = ground_hit.position.y + 0.9f;

    // Sweep along each leg's own line rather than guessing offsets: the stance
    // is metres wide and a line through the middle passes between the legs.
    glm::vec3 contact{0.0f};
    glm::vec3 approach_direction{1.0f, 0.0f, 0.0f};
    glm::vec3 approach_origin{0.0f};
    bool found_contact = false;
    for (std::uint32_t index = 0u; index < authority_count && !found_contact;
         ++index) {
        const KernelColliderShapeView& limb = authority_shapes[index];
        const glm::vec3 origins[2] = {
            glm::vec3{
                authority_state.position.x - 25.0f,
                walk_height,
                limb.world_center.z},
            glm::vec3{
                limb.world_center.x,
                walk_height,
                authority_state.position.z - 25.0f},
        };
        const glm::vec3 directions[2] = {
            glm::vec3{1.0f, 0.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
        };
        for (std::uint32_t axis = 0u; axis < 2u && !found_contact; ++axis) {
            network_example::physics::RayCastRequest across{};
            across.origin = origins[axis];
            across.direction = directions[axis];
            across.max_distance = 50.0f;
            across.filter.collision_mask = KERNEL_MOVEMENT_MASK_SUPPORTED;
            network_example::physics::CollisionHit across_hit{};
            if (!authority.physics_world_->ray_cast_closest(across, &across_hit)) {
                continue;
            }
            if (across_hit.identity.kind !=
                network_example::physics::CollisionObjectKind::kActorLimb) {
                continue;
            }
            contact = across_hit.position;
            approach_origin = origins[axis];
            approach_direction = directions[axis];
            found_contact = true;
        }
    }
    if (!found_contact) {
        std::fprintf(stderr, "DIAG ground=%.3f walk=%.3f root=(%.2f,%.2f,%.2f)\n",
            (double)ground_hit.position.y, (double)walk_height,
            (double)authority_state.position.x,
            (double)authority_state.position.y,
            (double)authority_state.position.z);
        for (std::uint32_t index = 0u; index < authority_count; ++index) {
            std::fprintf(stderr,
                "DIAG limb %u centre=(%.2f,%.2f,%.2f) half=(%.2f,%.2f,%.2f)\n",
                index,
                (double)authority_shapes[index].world_center.x,
                (double)authority_shapes[index].world_center.y,
                (double)authority_shapes[index].world_center.z,
                (double)authority_shapes[index].shape_params.x,
                (double)authority_shapes[index].shape_params.y,
                (double)authority_shapes[index].shape_params.z);
        }
    }
    require(found_contact);

    const auto walk_into_leg = [&](network_example::physics::PhysicsWorld& world,
                                   std::uint32_t collision_mask,
                                   std::uint32_t character_id) {
        network_example::movement_solver::CharacterMovementConfig walk{};
        walk.character_id = character_id;
        walk.shape.type = network_example::physics::CollisionShapeType::kCapsule;
        walk.shape.local_center = glm::vec3{
            player_capsule->definition.center.x,
            player_capsule->definition.center.y,
            player_capsule->definition.center.z};
        walk.shape.capsule_half_height = player_capsule->definition.shape_params.x;
        walk.shape.radius = player_capsule->definition.shape_params.y;
        walk.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        walk.max_slope_degrees = player_template->movement_max_slope_degrees;
        walk.step_height = player_template->movement_step_height;
        walk.ground_snap_distance =
            player_template->movement_ground_snap_distance;
        walk.filter.collision_mask = collision_mask;

        network_example::movement_solver::CharacterMovementState state{};
        // Point blank, on the ground, pushing straight into the leg. A long
        // approach would not settle anything: the ground undulates, so most of
        // the travel difference would be slope drift, and a capsule given room
        // slides around a 1.5 m wide box rather than stopping against it.
        state.position = glm::vec3{
            contact.x - approach_direction.x * 0.6f,
            ground_hit.position.y,
            contact.z - approach_direction.z * 0.6f};
        std::string error;
        for (std::uint32_t step = 0u; step < 12u; ++step) {
            require(network_example::movement_solver::step_character(
                world,
                walk,
                approach_direction * 5.0f,
                1.0f / 30.0f,
                &state,
                &error));
        }
        // Distance travelled along the approach, so the two axes read the same.
        return glm::dot(state.position - approach_origin, approach_direction);
    };

    const float authority_blocked_x = walk_into_leg(
        *authority.physics_world_, KERNEL_MOVEMENT_MASK_SUPPORTED, 900001u);
    const float authority_free_x = walk_into_leg(
        *authority.physics_world_, KERNEL_MOVEMENT_MASK_DEFAULT, 900002u);
    const float contact_distance =
        glm::dot(contact - approach_origin, approach_direction);
    const float start_distance = contact_distance - 0.6f;
    const float blocked_travel = authority_blocked_x - start_distance;
    const float free_travel = authority_free_x - start_distance;
    std::printf(
        "limb blocking: 12 steps of a 2.0 m push -- %.3f m travelled with the "
        "layer, %.3f m without\n",
        static_cast<double>(blocked_travel),
        static_cast<double>(free_travel));
    // Without the layer the capsule covers the whole commanded distance and
    // ends inside the leg. With it, it is stopped against the leg after a
    // fraction of that. The bound is relative so this does not become a test of
    // exactly where Jolt seats a capsule against a rotated box.
    require(free_travel > 1.5f);
    require(blocked_travel < free_travel * 0.5f);

    const float predicted_blocked_x = walk_into_leg(
        *predicting.prediction_physics_world_,
        KERNEL_MOVEMENT_MASK_SUPPORTED,
        900003u);
    std::printf(
        "limb blocking: client travels %.3f m\n",
        static_cast<double>(predicted_blocked_x - start_distance));
    // The client stops where the server does. This is what keeps a predicted
    // player from being corrected back through a leg it should have hit.
    require(near_value(predicted_blocked_x, authority_blocked_x));

    // ---------------------------------------------------------------------
    // What a session that does not block actors does with limbs.
    //
    // Limbs are struck out alongside the movement capsule, never on their own:
    // both say "another actor's body blocks this one". Getting that wrong the
    // other way would stop a player on a leg while they walk through the body
    // it hangs from. The engine default is Disabled, so this is the path a
    // kernel takes unless something opts in.
    {
        network_example::EntitySpawnPacket local_player{};
        local_player.net_id = 7001u;
        local_player.entity_type = network_example::EntityType::kActor;
        local_player.actor_type = network_example::ActorType::kPlayer;
        local_player.actor_template_id = 1u;
        local_player.entity_template_id = 1u;
        predicting.handle_client_spawn(local_player);
        predicting.local_player_net_id_ = local_player.net_id;

        const std::uint32_t limb_bit =
            network_example::physics::collision_layer_bit(
                network_example::physics::CollisionLayer::kActorLimb);
        const std::uint32_t capsule_bit =
            network_example::physics::collision_layer_bit(
                network_example::physics::CollisionLayer::kActorMovement);
        const std::uint32_t terrain_bit =
            network_example::physics::collision_layer_bit(
                network_example::physics::CollisionLayer::kTerrain);

        // A client receives this in the welcome packet rather than setting it;
        // there is no session here, so the field is written directly.
        network_example::movement_solver::CharacterMovementConfig blocking{};
        predicting.session_rules_.actor_blocking_mode =
            KernelActorBlockingMode_Predicted;
        require(predicting.build_local_character_movement_config(&blocking));
        // player.yaml authors limb, so a session that blocks actors keeps it.
        require((blocking.filter.collision_mask & limb_bit) != 0u);
        require((blocking.filter.collision_mask & capsule_bit) != 0u);

        predicting.session_rules_.actor_blocking_mode =
            KernelActorBlockingMode_Disabled;
        require(predicting.build_local_character_movement_config(&blocking));
        require((blocking.filter.collision_mask & limb_bit) == 0u);
        require((blocking.filter.collision_mask & capsule_bit) == 0u);
        // Only the actor layers go. The static world is not up for negotiation.
        require((blocking.filter.collision_mask & terrain_bit) != 0u);

        predicting.local_player_net_id_ = 0u;
    }

    // The same rule on the authoritative path, exercised through the real
    // movement simulation rather than by inspecting a filter: a player pushed
    // into a leg is stopped when the session blocks actors and walks through it
    // when it does not.
    {
        KernelServerEntityCreateInfo player_create{};
        player_create.struct_size = sizeof(player_create);
        player_create.entity_type =
            network_example::game_server::kEntityTypeActor;
        player_create.actor_type = KernelActorType_Player;
        player_create.entity_template_id = 1u;
        player_create.actor_template_id = 1u;
        player_create.owner_peer = 11u;
        player_create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t player_net_id = 0u;
        require(authority.server_create_entity(player_create, &player_net_id));
        require(player_net_id != 0u);

        const auto push_player = [&](std::uint32_t blocking_mode) {
            // set_session_rules refuses once a server is running, which is the
            // right rule for a live session and the wrong one for sweeping the
            // two modes here.
            authority.session_rules_.actor_blocking_mode = blocking_mode;
            const KernelVec3 start{
                contact.x - approach_direction.x * 0.6f,
                ground_hit.position.y,
                contact.z - approach_direction.z * 0.6f};
            const KernelQuat upright_rotation{0.0f, 0.0f, 0.0f, 1.0f};
            require(authority.server_set_entity_transform(
                player_net_id, start, upright_rotation));
            for (std::uint32_t step = 0u; step < 12u; ++step) {
                KernelPlayerInput input{};
                input.input_seq = step + 1u;
                input.move = KernelVec2{
                    approach_direction.x, approach_direction.z};
                require(authority.server_submit_entity_input(
                    player_net_id, input));
                authority.update(1.0f / 30.0f);
            }
            KernelServerEntityState state{};
            state.struct_size = sizeof(state);
            require(authority.server_get_entity_state(player_net_id, &state));
            const glm::vec3 end{state.position.x, state.position.y, state.position.z};
            const glm::vec3 begin{start.x, start.y, start.z};
            return glm::dot(end - begin, approach_direction);
        };

        const float blocked = push_player(KernelActorBlockingMode_Predicted);
        const float unblocked = push_player(KernelActorBlockingMode_Disabled);
        std::printf(
            "limb blocking: server player travels %.3f m blocking, %.3f m with "
            "actor blocking disabled\n",
            static_cast<double>(blocked),
            static_cast<double>(unblocked));
        require(unblocked > blocked + 0.5f);
    }

    // ---------------------------------------------------------------------
    // Authoring limbs as a gameplay target.
    //
    // The layer is reachable from data now, but nothing is pointed at it yet:
    // this covers the plumbing from a written mask to the filter a query runs
    // with, including the trap that a limb without a side matches nothing.
    {
        const std::uint32_t limb_layer =
            network_example::physics::collision_layer_bit(
                network_example::physics::CollisionLayer::kActorLimb);
        const std::uint32_t limb_kind =
            1u << static_cast<std::uint32_t>(
                network_example::physics::CollisionObjectKind::kActorLimb);

        // Naming it turns on both halves. A query is filtered on the layer and
        // on the object kind, and one without the other sees nothing.
        const network_example::physics::CollisionQueryFilter limbs =
            network_example::collision_filter_from_mask(
                KERNEL_COLLISION_LAYER_HOSTILE_SIDE |
                KERNEL_COLLISION_LAYER_LIMB);
        require((limbs.collision_mask & limb_layer) != 0u);
        require((limbs.object_kind_mask & limb_kind) != 0u);
        require(
            (limbs.gameplay_category_mask &
             KERNEL_COLLISION_LAYER_HOSTILE_SIDE) != 0u);

        // Not naming it leaves every existing weapon exactly as it was.
        const network_example::physics::CollisionQueryFilter without =
            network_example::collision_filter_from_mask(
                KERNEL_COLLISION_MASK_DAMAGEABLE |
                KERNEL_COLLISION_MASK_STATIC_WORLD);
        require((without.collision_mask & limb_layer) == 0u);
        require((without.object_kind_mask & limb_kind) == 0u);

        // The trap: gameplay_category_mask is built from the side bits alone,
        // so a mask that names limbs and no side produces an empty category and
        // matches nothing. Authoring has to pair them.
        const network_example::physics::CollisionQueryFilter sideless =
            network_example::collision_filter_from_mask(
                KERNEL_COLLISION_LAYER_LIMB);
        require((sideless.collision_mask & limb_layer) != 0u);
        require(sideless.gameplay_category_mask == 0u);

        // A limb hit resolves to the actor wearing it, not to scenery. The
        // owning net id is asserted on every limb further up, so together with
        // this the thrown-prop path reaches the monster: its overlap names the
        // layer, the hit classifies as an actor, and the target it carries is
        // the rig.
        require(network_example::is_actor_hit(
            network_example::physics::CollisionObjectKind::kActorLimb));
        // The movement capsule is still not something you can hit.
        require(!network_example::is_actor_hit(
            network_example::physics::CollisionObjectKind::kActorMovement));

        // The one prop that asks for limbs, read back from the shipped
        // catalog rather than restated here.
        const network_example::game_server::EntityTemplateConfig* damage_prop =
            nullptr;
        for (const network_example::game_server::EntityTemplateConfig& entity :
             config.entity_templates) {
            if (entity.name == "collision_damage_prop") {
                damage_prop = &entity;
            }
        }
        require(damage_prop != nullptr);
        require(
            (damage_prop->collision_trigger_mask &
             KERNEL_COLLISION_LAYER_LIMB) != 0u);
        require(
            (damage_prop->collision_trigger_mask &
             KERNEL_COLLISION_MASK_ACTOR) != 0u);
        const network_example::physics::CollisionQueryFilter prop_filter =
            network_example::collision_filter_from_mask(
                damage_prop->collision_trigger_mask);
        require((prop_filter.collision_mask & limb_layer) != 0u);
        require((prop_filter.object_kind_mask & limb_kind) != 0u);

        // hit_zone is authored per bone on the rig as a decimal and stored as
        // hundredths. The legs take a shot poorly and the body takes one well,
        // which is a property of which bone it is rather than of the actor.
        const network_example::game_server::SkeletonAssetConfig* rig = nullptr;
        for (const network_example::game_server::SkeletonAssetConfig& asset :
             config.skeleton_assets) {
            if (asset.name == "simplified_quadruped") {
                rig = &asset;
            }
        }
        require(rig != nullptr);
        require(rig->colliders.size() == kExpectedBoneIndices.size());
        for (const network_example::game_server::RigColliderConfig& collider :
             rig->colliders) {
            require(collider.has_hit_zone);
            require(
                collider.hit_zone == (collider.bone == "GEO_Body" ? 150u : 50u));
        }

        // And it survives the merge into the catalog the kernel loads, which is
        // the half a rig edit cannot verify on its own.
        const KernelEntityTemplateDefinition* quadruped = nullptr;
        for (const KernelEntityTemplateDefinition& entity :
             storage.entity_templates) {
            if (entity.entity_template_id == 21u) {
                quadruped = &entity;
            }
        }
        require(quadruped != nullptr);
        require(
            quadruped->skeleton.collider_count == kExpectedBoneIndices.size());
        std::uint32_t body_zones = 0u;
        std::uint32_t leg_zones = 0u;
        for (std::uint32_t index = 0u;
             index < quadruped->skeleton.collider_count;
             ++index) {
            const std::uint16_t zone =
                quadruped->skeleton.colliders[index].hit_zone;
            require(zone == 150u || zone == 50u);
            if (zone == 150u) {
                ++body_zones;
            } else {
                ++leg_zones;
            }
        }
        require(body_zones == 1u);
        require(leg_zones == 8u);
        // Never zero by omission: that is the value that would make a volume
        // nobody authored immune.
        require(KERNEL_HIT_ZONE_UNSCALED == 100u);

        // And what the multiplier does to a number. Rounds half away from zero,
        // so a halved hit is not quietly cheaper than the number says.
        require(network_example::scale_damage_by_hit_zone(45u, 100u) == 45u);
        require(network_example::scale_damage_by_hit_zone(45u, 50u) == 23u);
        require(network_example::scale_damage_by_hit_zone(45u, 150u) == 68u);
        // Authored harmlessness is a real answer, and it is the only way to
        // reach zero.
        require(network_example::scale_damage_by_hit_zone(45u, 0u) == 0u);
        // Small damage survives being halved rather than vanishing.
        require(network_example::scale_damage_by_hit_zone(1u, 50u) == 1u);
        // A multiplier small enough to round a hit away does so, which is what
        // "0.01 times 45 is nothing" means.
        require(network_example::scale_damage_by_hit_zone(45u, 1u) == 0u);
        // Saturates rather than wrapping.
        require(
            network_example::scale_damage_by_hit_zone(60000u, 60000u) ==
            std::numeric_limits<std::uint16_t>::max());

        // The one projectile that asks. A rocket strikes the leg in its path
        // instead of sailing between them, and the hit resolves to the actor
        // wearing it -- projectile hit records carry
        // hit.identity.entity_net_id, which every limb above asserts is the rig.
        const network_example::game_server::ProjectileTemplateConfig* rocket =
            nullptr;
        for (const network_example::game_server::ProjectileTemplateConfig&
                 projectile : config.projectile_templates) {
            if (projectile.name == "rocket_projectile") {
                rocket = &projectile;
            }
        }
        require(rocket != nullptr);
        const std::uint32_t rocket_mask =
            rocket->definition.mechanics.collision_mask;
        require((rocket_mask & KERNEL_COLLISION_LAYER_LIMB) != 0u);
        // Still a side-filtered weapon: the limb bit is not a side, so it can
        // neither widen nor narrow who the rocket is allowed to damage.
        require((rocket_mask & KERNEL_COLLISION_MASK_DAMAGEABLE) != 0u);
        const network_example::physics::CollisionQueryFilter rocket_filter =
            network_example::collision_filter_from_mask(rocket_mask);
        require((rocket_filter.collision_mask & limb_layer) != 0u);
        require((rocket_filter.object_kind_mask & limb_kind) != 0u);

        // The rifle is the one hitscan that asks for limbs, which is what makes
        // a rewound shot able to resolve against a leg at all: the rewound path
        // never touches the physics world, so its opt-in is this mask reaching
        // raycast_history_frame rather than a collision filter.
        const std::uint8_t rifle_id =
            network_example::game_server::kWeaponRifle;
        require(config.weapons.configured[rifle_id]);
        require(config.weapons.names[rifle_id] == "Rifle");
        const std::uint32_t rifle_mask =
            config.weapons.definitions[rifle_id].collision_mask;
        require((rifle_mask & KERNEL_COLLISION_LAYER_LIMB) != 0u);
        require((rifle_mask & KERNEL_COLLISION_MASK_DAMAGEABLE) != 0u);
        // Walls still stop it, which the engine default did for free before the
        // mask existed and would silently stop doing if it were dropped.
        require((rifle_mask & KERNEL_COLLISION_MASK_STATIC_WORLD) != 0u);

        // The shotgun does not ask, and its pellets go through the same
        // resolver -- so this is also the assertion that the opt-in is
        // per weapon rather than per code path.
        const std::uint8_t shotgun_id =
            network_example::game_server::kWeaponShotgun;
        require(config.weapons.configured[shotgun_id]);
        require(
            (config.weapons.definitions[shotgun_id].collision_mask &
             KERNEL_COLLISION_LAYER_LIMB) == 0u);

        // The rocket into a quadruped's leg, which is what all of this was for:
        // 45 damage on a 0.5 leg is 23.
        require(
            network_example::scale_damage_by_hit_zone(
                static_cast<std::uint16_t>(
                    rocket->definition.mechanics.damage),
                quadruped->skeleton.colliders[0].hit_zone) == 23u);

        // Everything that did not ask is untouched, including the default any
        // query starts from -- that exclusion is what keeps hitscan, vision and
        // every unnamed query at the cost and the results they had.
        require(
            (network_example::physics::kCollisionMaskAll & limb_layer) == 0u);
        const network_example::game_server::ProjectileTemplateConfig* spammer =
            nullptr;
        for (const network_example::game_server::ProjectileTemplateConfig&
                 projectile : config.projectile_templates) {
            if (projectile.name == "spammer_projectile") {
                spammer = &projectile;
            }
        }
        require(spammer != nullptr);
        require(
            (spammer->definition.mechanics.collision_mask &
             KERNEL_COLLISION_LAYER_LIMB) == 0u);
        const network_example::physics::CollisionQueryFilter spammer_filter =
            network_example::collision_filter_from_mask(
                spammer->definition.mechanics.collision_mask);
        require((spammer_filter.collision_mask & limb_layer) == 0u);
        require((spammer_filter.object_kind_mask & limb_kind) == 0u);
    }

    // Retiring the entity retires its bodies. A leg left behind is an invisible
    // wall standing where the rig no longer is.
    network_example::EntityDespawnPacket despawn{};
    despawn.net_id = parity_net_id;
    predicting.handle_client_despawn(despawn);
    require(
        predicting.prediction_limb_collider_ids_.count(parity_net_id) == 0u);
    ray.filter.collision_mask = KERNEL_MOVEMENT_MASK_SUPPORTED;
    network_example::physics::CollisionHit after_despawn_hit{};
    const bool after_despawn_found =
        predicting.prediction_physics_world_->ray_cast_closest(
            ray, &after_despawn_hit);
    require(
        !after_despawn_found ||
        after_despawn_hit.identity.kind !=
            network_example::physics::CollisionObjectKind::kActorLimb);

    std::printf("limb_collider_test: PASS\n");
    return 0;
}
