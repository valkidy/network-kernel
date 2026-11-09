// The static scene table (docs/SCENE_TABLE_PLAN.md S1): slots 1..N-1 sit at
// their own offsets beside the camp's slot 0, come and go at runtime, and reach
// every physics world the kernel has -- now, and any it builds later.
//
// The worlds are looked at directly: a ray straight down finds the slot's
// terrain or it does not. Each check has its control: the camp is hit, and the
// empty strip between two slots is not, so a world that silently lost all its
// terrain cannot pass.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// Everything kernel.h pulls in from the standard library and entt is included
// before `private` is redefined, as the other white-box tests do.
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "physics/public/collision_types.h"
#include "physics/public/physics_world.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

// What the apps pass for the camp (the catalog's static_collision_scene);
// slot i takes the camp's collider id + i.
constexpr std::uint32_t kSceneId = 1u;
constexpr std::uint32_t kCampColliderId = 2147483649u;
constexpr float kSlotSpacing = 4096.0f;

std::vector<std::uint8_t> read_artifact() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog" / "mesh_assets" /
        "jolt" / "undulating.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

KernelConfig config_for(KernelMode mode) {
    KernelConfig config{};
    config.mode = mode;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 256;
    config.max_render_states = 64;
    return config;
}

KernelStaticCollisionSceneConfig camp_config(const std::vector<std::uint8_t>& artifact) {
    KernelStaticCollisionSceneConfig config{};
    config.struct_size = sizeof(config);
    config.artifact_bytes = artifact.data();
    config.artifact_size = static_cast<std::uint32_t>(artifact.size());
    config.scene_id = kSceneId;
    config.collider_id = kCampColliderId;
    config.collision_layer = KERNEL_STATIC_COLLISION_LAYER_TERRAIN;
    return config;
}

KernelStaticSceneSlotConfig slot_config(
    const std::vector<std::uint8_t>& artifact,
    std::uint32_t slot) {
    KernelStaticSceneSlotConfig config{};
    config.struct_size = sizeof(config);
    config.slot = slot;
    config.artifact_bytes = artifact.data();
    config.artifact_size = static_cast<std::uint32_t>(artifact.size());
    config.scene_id = kSceneId;
    config.collider_id = kCampColliderId + slot;
    config.collision_layer = KERNEL_STATIC_COLLISION_LAYER_TERRAIN;
    config.offset = KernelVec3{kSlotSpacing * static_cast<float>(slot), 0.0f, 0.0f};
    return config;
}

// Terrain height straight below (x, z), or NAN when there is none.
float ground_at(const ne::physics::PhysicsWorld* world, float x, float z) {
    require(world != nullptr);
    ne::physics::RayCastRequest ray{};
    ray.origin = glm::vec3{x, 200.0f, z};
    ray.direction = glm::vec3{0.0f, -1.0f, 0.0f};
    ray.max_distance = 400.0f;
    ray.filter.object_kind_mask =
        1u << static_cast<std::uint32_t>(ne::physics::CollisionObjectKind::kTerrain);
    ne::physics::CollisionHit hit{};
    if (!world->ray_cast_closest(ray, &hit)) {
        return NAN;
    }
    return hit.position.y;
}

// The camp's ground under a local point, and that slot's ground under the
// same local point, must agree; the strip between slots must be empty.
void require_slot_present(const ne::physics::PhysicsWorld* world, std::uint32_t slot) {
    const float camp = ground_at(world, 3.0f, -2.0f);
    require(!std::isnan(camp));
    const float here = ground_at(world, kSlotSpacing * static_cast<float>(slot) + 3.0f, -2.0f);
    require(!std::isnan(here));
    require(std::fabs(here - camp) < 1e-3f);
}

void require_slot_absent(const ne::physics::PhysicsWorld* world, std::uint32_t slot) {
    require(std::isnan(ground_at(world, kSlotSpacing * static_cast<float>(slot) + 3.0f, -2.0f)));
}

void require_gap_empty(const ne::physics::PhysicsWorld* world) {
    require(std::isnan(ground_at(world, kSlotSpacing * 0.5f, -2.0f)));
}

// The camp's ground at the local point, measured in a server world; a world
// with no camp of its own (a client that only added level slots) is held to it.
float g_camp_ground = NAN;

void require_slot_present_without_camp(
    const ne::physics::PhysicsWorld* world,
    std::uint32_t slot) {
    require(!std::isnan(g_camp_ground));
    require(std::isnan(ground_at(world, 3.0f, -2.0f)));  // no camp here
    const float here = ground_at(world, kSlotSpacing * static_cast<float>(slot) + 3.0f, -2.0f);
    require(!std::isnan(here));
    require(std::fabs(here - g_camp_ground) < 1e-3f);
    require_gap_empty(world);
}

// A dedicated server: a slot added before start is in the world start builds;
// one added while running goes straight in; removing takes it out.
void server_lifecycle(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine server(config_for(KernelMode_DedicatedServer));
    require(server.set_static_collision_scene(camp_config(artifact)));
    require(server.add_static_scene_slot(slot_config(artifact, 1u)));

    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(server.prepare_server_physics(&world));
    server.physics_world_ = std::move(world);
    g_camp_ground = ground_at(server.physics_world_.get(), 3.0f, -2.0f);
    require_slot_present(server.physics_world_.get(), 1u);
    require_slot_absent(server.physics_world_.get(), 2u);
    require_gap_empty(server.physics_world_.get());

    require(server.add_static_scene_slot(slot_config(artifact, 2u)));
    require_slot_present(server.physics_world_.get(), 2u);
    require(server.prediction_physics_world_ == nullptr);

    require(server.remove_static_scene_slot(2u));
    require_slot_absent(server.physics_world_.get(), 2u);
    require_slot_present(server.physics_world_.get(), 1u);
    std::printf("server: before-start and runtime slots load, removal takes one out\n");
}

// A listen host holds two worlds; a slot goes into both and leaves both.
void listen_host_both_worlds(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine host(config_for(KernelMode_ListenServer));
    require(host.set_static_collision_scene(camp_config(artifact)));
    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(host.prepare_server_physics(&world));
    host.physics_world_ = std::move(world);
    require(host.prepare_prediction_physics());
    require(host.physics_world_ != nullptr && host.prediction_physics_world_ != nullptr);

    require(host.add_static_scene_slot(slot_config(artifact, 3u)));
    require_slot_present(host.physics_world_.get(), 3u);
    require_slot_present(host.prediction_physics_world_.get(), 3u);
    require_gap_empty(host.prediction_physics_world_.get());

    require(host.remove_static_scene_slot(3u));
    require_slot_absent(host.physics_world_.get(), 3u);
    require_slot_absent(host.prediction_physics_world_.get(), 3u);
    std::printf("listen host: a slot reaches and leaves both worlds\n");
}

// A client: a slot added before Welcome is kept and lands in the prediction
// world Welcome builds; one added after goes straight in.
void client_around_welcome(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine client(config_for(KernelMode_Client));
    require(client.add_static_scene_slot(slot_config(artifact, 1u)));
    require(client.prediction_physics_world_ == nullptr);
    require(client.has_static_collision_scene());

    // What apply_welcome does with a static scene registered.
    require(client.prepare_prediction_physics());
    require_slot_present_without_camp(client.prediction_physics_world_.get(), 1u);

    require(client.add_static_scene_slot(slot_config(artifact, 2u)));
    require_slot_present_without_camp(client.prediction_physics_world_.get(), 2u);
    require(client.physics_world_ == nullptr);

    // A second Welcome rebuilds the world from the table: both slots again.
    require(client.prepare_prediction_physics());
    require_slot_present_without_camp(client.prediction_physics_world_.get(), 1u);
    require_slot_present_without_camp(client.prediction_physics_world_.get(), 2u);
    std::printf("client: slots before and after Welcome reach the prediction world\n");
}

void rejections(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine host(config_for(KernelMode_ListenServer));
    require(host.set_static_collision_scene(camp_config(artifact)));
    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(host.prepare_server_physics(&world));
    host.physics_world_ = std::move(world);
    require(host.prepare_prediction_physics());

    require(!host.add_static_scene_slot(slot_config(artifact, 0u)));
    require(!host.add_static_scene_slot(slot_config(artifact, KERNEL_STATIC_SCENE_SLOT_COUNT)));
    require(host.add_static_scene_slot(slot_config(artifact, 1u)));
    require(!host.add_static_scene_slot(slot_config(artifact, 1u)));  // occupied

    KernelStaticSceneSlotConfig clash = slot_config(artifact, 2u);
    clash.collider_id = kCampColliderId + 1u;  // slot 1's
    require(!host.add_static_scene_slot(clash));
    clash.collider_id = kCampColliderId;  // the camp's
    require(!host.add_static_scene_slot(clash));

    KernelStaticSceneSlotConfig bad = slot_config(artifact, 2u);
    bad.collision_layer = 0u;
    require(!host.add_static_scene_slot(bad));
    bad = slot_config(artifact, 2u);
    bad.scene_id = 0u;
    require(!host.add_static_scene_slot(bad));
    bad = slot_config(artifact, 2u);
    bad.offset.x = NAN;
    require(!host.add_static_scene_slot(bad));
    const std::vector<std::uint8_t> garbage(64, 0x5a);
    bad = slot_config(garbage, 2u);
    require(!host.add_static_scene_slot(bad));
    // Nothing a refusal touched is left behind: slot 2 is still free.
    require(host.static_scene_slots_[2].artifact.empty());
    require_slot_absent(host.physics_world_.get(), 2u);
    require_slot_absent(host.prediction_physics_world_.get(), 2u);
    require(host.add_static_scene_slot(slot_config(artifact, 2u)));

    require(!host.remove_static_scene_slot(0u));
    require(!host.remove_static_scene_slot(5u));  // empty
    require(!host.remove_static_scene_slot(KERNEL_STATIC_SCENE_SLOT_COUNT));
    require_slot_present(host.physics_world_.get(), 1u);
    std::printf("rejections: slot 0, past the table, occupied, id clashes, bad config\n");
}

// Jolt reclaims broad phase nodes only in optimize_broad_phase; a slot that
// comes and goes many times must not drain them.
void churn(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine host(config_for(KernelMode_ListenServer));
    require(host.set_static_collision_scene(camp_config(artifact)));
    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(host.prepare_server_physics(&world));
    host.physics_world_ = std::move(world);
    require(host.prepare_prediction_physics());
    for (int round = 0; round < 50; ++round) {
        require(host.add_static_scene_slot(slot_config(artifact, 1u)));
        host.physics_world_->optimize_broad_phase();
        host.prediction_physics_world_->optimize_broad_phase();
        require(host.remove_static_scene_slot(1u));
        host.physics_world_->optimize_broad_phase();
        host.prediction_physics_world_->optimize_broad_phase();
    }
    require(host.add_static_scene_slot(slot_config(artifact, 1u)));
    require_slot_present(host.physics_world_.get(), 1u);
    require_slot_present(host.prediction_physics_world_.get(), 1u);
    std::printf("churn: 50 add/remove rounds, slot still loads\n");
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> artifact = read_artifact();
    server_lifecycle(artifact);
    listen_host_both_worlds(artifact);
    client_around_welcome(artifact);
    rejections(artifact);
    churn(artifact);
    std::printf("static_scene_slot_test passed\n");
    return 0;
}
