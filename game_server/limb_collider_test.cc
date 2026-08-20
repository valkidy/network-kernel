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
#include <string>
#include <vector>

#include "game_server/game_server.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"
#include "protocol/public/network_packets.h"
#include "physics/public/collision_types.h"
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
