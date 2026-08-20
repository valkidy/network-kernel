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
    std::printf("limb_collider_test: PASS\n");
    return 0;
}
