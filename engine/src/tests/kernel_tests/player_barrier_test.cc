// Player barriers (docs/LEVEL_AUTHORING_TOOLS_PLAN.md P1): invisible boxes a
// scene puts at its edges that stop only movement, and only of an actor whose
// movement mask names KERNEL_MOVEMENT_LAYER_PLAYER_BARRIER.
//
// Two halves. The worlds are looked at directly first: a ray down through a
// barrier finds it only when it asks for the layer, in every world the kernel
// has, now and in any it builds later. Each check has its control -- the same
// ray without the layer finds the terrain under the barrier -- so a world that
// lost its barriers and one that never had them read differently from a ray
// that skipped them. Then a listen server's player walks at a barrier with and
// without the layer in its mask.

#include <array>
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

constexpr std::uint32_t kSceneId = 1u;
constexpr std::uint32_t kCampColliderId = 2147483649u;
constexpr float kSlotSpacing = 4096.0f;

// A wall across x = 10, 40 m wide and 60 m tall: well above the undulating
// terrain's +-5.4 m relief, so a ray down at x = 10 meets its top first.
constexpr float kWallX = 10.0f;
constexpr float kWallTop = 60.0f;

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
    config.tick.snapshot_rate = 30;
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

const std::array<KernelPlayerBarrierBox, 1> kWall = {
    KernelPlayerBarrierBox{
        KernelVec3{kWallX, kWallTop * 0.5f, 0.0f},
        KernelVec3{0.5f, kWallTop * 0.5f, 20.0f},
        0.0f},
};

KernelStaticScenePlayerBarriersConfig barriers(
    std::uint32_t slot,
    const KernelPlayerBarrierBox* boxes,
    std::uint32_t count) {
    KernelStaticScenePlayerBarriersConfig config{};
    config.struct_size = sizeof(config);
    config.slot = slot;
    config.boxes = boxes;
    config.box_count = count;
    return config;
}

KernelStaticScenePlayerBarriersConfig wall(std::uint32_t slot) {
    return barriers(slot, kWall.data(), static_cast<std::uint32_t>(kWall.size()));
}

constexpr std::uint32_t kBarrierBit =
    ne::physics::collision_layer_bit(ne::physics::CollisionLayer::kPlayerBarrier);

// What a ray straight down at (x, 0) hits first under `mask`.
struct Down {
    bool hit = false;
    float y = NAN;
    ne::physics::CollisionObjectKind kind = ne::physics::CollisionObjectKind::kTerrain;
};

Down down_at(const ne::physics::PhysicsWorld* world, float x, std::uint32_t mask) {
    require(world != nullptr);
    ne::physics::RayCastRequest ray{};
    ray.origin = glm::vec3{x, 200.0f, 0.0f};
    ray.direction = glm::vec3{0.0f, -1.0f, 0.0f};
    ray.max_distance = 400.0f;
    ray.filter.collision_mask = mask;
    ne::physics::CollisionHit hit{};
    Down out;
    if (world->ray_cast_closest(ray, &hit)) {
        out.hit = true;
        out.y = hit.position.y;
        out.kind = hit.identity.kind;
    }
    return out;
}

// The barrier is there for a query that names its layer, and invisible to
// every query that does not: gameplay's catch-all mask and the movement
// default both reach the terrain beneath it instead.
void require_wall(const ne::physics::PhysicsWorld* world, std::uint32_t slot) {
    const float x = kSlotSpacing * static_cast<float>(slot) + kWallX;
    const Down named = down_at(world, x, kBarrierBit | ne::physics::kMovementCollisionMask);
    require(named.hit);
    require(named.kind == ne::physics::CollisionObjectKind::kPlayerBarrier);
    require(std::fabs(named.y - kWallTop) < 1e-3f);

    const Down gameplay = down_at(world, x, ne::physics::kCollisionMaskAll);
    require(gameplay.hit);
    require(gameplay.kind == ne::physics::CollisionObjectKind::kTerrain);
    require(gameplay.y < 10.0f);

    const Down movement = down_at(world, x, ne::physics::kMovementCollisionMask);
    require(movement.hit);
    require(movement.kind == ne::physics::CollisionObjectKind::kTerrain);
}

void require_no_wall(const ne::physics::PhysicsWorld* world, std::uint32_t slot) {
    const float x = kSlotSpacing * static_cast<float>(slot) + kWallX;
    const Down named = down_at(world, x, kBarrierBit | ne::physics::kMovementCollisionMask);
    // The control: something is under the wall's spot (the terrain, when the
    // slot still has it), but never a barrier.
    require(!named.hit || named.kind != ne::physics::CollisionObjectKind::kPlayerBarrier);
}

// A dedicated server: slot 0's barriers set before the camp scene and before
// start reach the world start builds; a level slot's go in at runtime, at its
// offset; replacing and clearing take them out; removing the slot takes its
// barriers with it.
void server_lifecycle(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine server(config_for(KernelMode_DedicatedServer));
    require(server.set_static_scene_player_barriers(wall(0u)));
    require(server.set_static_collision_scene(camp_config(artifact)));

    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(server.prepare_server_physics(&world));
    server.physics_world_ = std::move(world);
    require_wall(server.physics_world_.get(), 0u);

    // A level slot takes barriers only while it is occupied.
    require(!server.set_static_scene_player_barriers(wall(1u)));
    require(server.add_static_scene_slot(slot_config(artifact, 1u)));
    require(server.set_static_scene_player_barriers(wall(1u)));
    require_wall(server.physics_world_.get(), 1u);

    // Replacing: the new set is all there is.
    const std::array<KernelPlayerBarrierBox, 1> moved = {
        KernelPlayerBarrierBox{
            KernelVec3{kWallX + 30.0f, kWallTop * 0.5f, 0.0f},
            KernelVec3{0.5f, kWallTop * 0.5f, 20.0f},
            0.0f},
    };
    require(server.set_static_scene_player_barriers(barriers(1u, moved.data(), 1u)));
    require_no_wall(server.physics_world_.get(), 1u);
    const Down at_moved = down_at(
        server.physics_world_.get(),
        kSlotSpacing + kWallX + 30.0f,
        kBarrierBit);
    require(at_moved.hit && at_moved.kind == ne::physics::CollisionObjectKind::kPlayerBarrier);

    require(server.remove_static_scene_slot(1u));
    const Down gone = down_at(
        server.physics_world_.get(),
        kSlotSpacing + kWallX + 30.0f,
        kBarrierBit);
    require(!gone.hit);
    // The camp's barriers are untouched by another slot leaving.
    require_wall(server.physics_world_.get(), 0u);

    require(server.set_static_scene_player_barriers(barriers(0u, nullptr, 0u)));
    require_no_wall(server.physics_world_.get(), 0u);
    std::printf("server: barriers load at start, at runtime, replace, clear and leave with their slot\n");
}

// A listen host holds two worlds; barriers go into both.
void listen_host_both_worlds(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine host(config_for(KernelMode_ListenServer));
    require(host.set_static_collision_scene(camp_config(artifact)));
    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(host.prepare_server_physics(&world));
    host.physics_world_ = std::move(world);
    require(host.prepare_prediction_physics());

    require(host.set_static_scene_player_barriers(wall(0u)));
    require_wall(host.physics_world_.get(), 0u);
    require_wall(host.prediction_physics_world_.get(), 0u);

    require(host.set_static_scene_player_barriers(barriers(0u, nullptr, 0u)));
    require_no_wall(host.physics_world_.get(), 0u);
    require_no_wall(host.prediction_physics_world_.get(), 0u);
    std::printf("listen host: barriers reach and leave both worlds\n");
}

// A client: barriers set before Welcome land in the prediction world it
// builds, and a second Welcome rebuilds them from the table.
void client_around_welcome(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine client(config_for(KernelMode_Client));
    require(client.add_static_scene_slot(slot_config(artifact, 1u)));
    require(client.set_static_scene_player_barriers(wall(1u)));
    require(client.prediction_physics_world_ == nullptr);

    require(client.prepare_prediction_physics());
    require_wall(client.prediction_physics_world_.get(), 1u);
    require(client.prepare_prediction_physics());
    require_wall(client.prediction_physics_world_.get(), 1u);
    require(client.physics_world_ == nullptr);
    std::printf("client: barriers before Welcome reach the prediction world, and survive a rebuild\n");
}

// Every refusal leaves what was there.
void rejections(const std::vector<std::uint8_t>& artifact) {
    ne::KernelEngine server(config_for(KernelMode_DedicatedServer));
    require(server.set_static_collision_scene(camp_config(artifact)));
    std::unique_ptr<ne::physics::PhysicsWorld> world;
    require(server.prepare_server_physics(&world));
    server.physics_world_ = std::move(world);
    require(server.set_static_scene_player_barriers(wall(0u)));

    require(!server.set_static_scene_player_barriers(wall(KERNEL_STATIC_SCENE_SLOT_COUNT)));
    require(!server.set_static_scene_player_barriers(barriers(0u, nullptr, 1u)));
    std::vector<KernelPlayerBarrierBox> too_many(
        KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT + 1u, kWall[0]);
    require(!server.set_static_scene_player_barriers(
        barriers(0u, too_many.data(), static_cast<std::uint32_t>(too_many.size()))));
    for (const KernelVec3 half :
         {KernelVec3{0.0f, 1.0f, 1.0f},
          KernelVec3{1.0f, -1.0f, 1.0f},
          KernelVec3{1.0f, 1.0f, KERNEL_MAX_PLAYER_BARRIER_HALF_EXTENT * 2.0f},
          KernelVec3{1.0f, NAN, 1.0f}}) {
        const KernelPlayerBarrierBox bad{KernelVec3{0.0f, 0.0f, 0.0f}, half, 0.0f};
        require(!server.set_static_scene_player_barriers(barriers(0u, &bad, 1u)));
    }
    const KernelPlayerBarrierBox bad_yaw{
        KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{1.0f, 1.0f, 1.0f}, INFINITY};
    require(!server.set_static_scene_player_barriers(barriers(0u, &bad_yaw, 1u)));
    require_wall(server.physics_world_.get(), 0u);
    std::printf("rejections: bad slots, counts and boxes change nothing\n");
}

// ---------------------------------------------------------------- walking

constexpr float kFixedDeltaSeconds = 1.0f / 30.0f;
constexpr float kPlayerSpeed = 5.0f;
// As movement_collision_mask_test: the capsule floats 20 m up with no gravity,
// clear of the terrain's relief, so the barrier is the only thing it can meet.
constexpr float kCapsuleHeight = 20.0f;
constexpr float kCapsuleRadius = 0.35f;
constexpr float kWalkWallX = 6.0f;
constexpr float kWalkWallHalfX = 0.5f;
constexpr float kContactX = kWalkWallX - kWalkWallHalfX - kCapsuleRadius;

void load_walk_catalog(KernelHandle* kernel, std::uint32_t player_mask) {
    KernelColliderTemplateDefinition player_hit{};
    player_hit.struct_size = sizeof(player_hit);
    player_hit.template_id = 1u;
    player_hit.shape_type = KernelColliderShapeType_Aabb;
    player_hit.center = KernelVec3{0.0f, 0.9f, 0.0f};
    player_hit.shape_params = KernelVec4{0.35f, 0.9f, 0.35f, 0.0f};
    player_hit.purpose_flags = KernelColliderPurpose_Hit;
    player_hit.layer_mask = KERNEL_COLLISION_LAYER_PLAYER_SIDE;

    KernelColliderTemplateDefinition player_movement{};
    player_movement.struct_size = sizeof(player_movement);
    player_movement.template_id = 11u;
    player_movement.shape_type = KernelColliderShapeType_Capsule;
    player_movement.center = KernelVec3{0.0f, kCapsuleHeight, 0.0f};
    player_movement.shape_params = KernelVec4{0.55f, kCapsuleRadius, 0.0f, 0.0f};
    player_movement.purpose_flags = KernelColliderPurpose_Movement;
    player_movement.layer_mask = KERNEL_COLLISION_LAYER_PLAYER_SIDE;

    const std::array<KernelColliderTemplateDefinition, 2> collider_templates = {
        player_hit,
        player_movement,
    };

    KernelActorTemplateDefinition player_actor{};
    player_actor.struct_size = sizeof(player_actor);
    player_actor.actor_template_id = 1u;
    player_actor.entity_type = KernelEntityType_Actor;
    player_actor.actor_type = KernelActorType_Player;
    player_actor.collider_template_id = 1u;
    player_actor.vision.struct_size = sizeof(KernelAgentVisionConfig);
    player_actor.vision.camp = KernelAgentCamp_PlayerSide;

    KernelEntityTemplateDefinition player_entity{};
    player_entity.struct_size = sizeof(player_entity);
    player_entity.entity_template_id = 101u;
    player_entity.entity_type = KernelEntityType_Actor;
    player_entity.actor_type = KernelActorType_Player;
    player_entity.actor_template_id = 1u;
    player_entity.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    player_entity.collider_template_id = 1u;
    player_entity.combat.struct_size = sizeof(player_entity.combat);
    player_entity.combat.hp = 100;
    player_entity.combat.max_hp = 100;
    player_entity.combat.move_speed_meters_per_second = kPlayerSpeed;
    player_entity.combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    player_entity.combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    player_entity.movement.struct_size = sizeof(KernelMovementDefinition);
    player_entity.movement.controller_type = KernelMovementControllerType_Character;
    player_entity.movement.movement_collider_template_id = 11u;
    player_entity.movement.gravity = KernelVec3{0.0f, 0.0f, 0.0f};
    player_entity.movement.max_slope_degrees = 45.0f;
    player_entity.movement.movement_collision_mask = player_mask;
    player_entity.ai.struct_size = sizeof(KernelEntityAiDefinition);
    player_entity.vision.struct_size = sizeof(KernelAgentVisionConfig);
    player_entity.vision.camp = KernelAgentCamp_PlayerSide;

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count = static_cast<std::uint32_t>(collider_templates.size());
    catalog.actor_templates = &player_actor;
    catalog.actor_template_count = 1u;
    catalog.entity_templates = &player_entity;
    catalog.entity_template_count = 1u;
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

std::uint32_t find_local_player(KernelHandle* kernel) {
    std::array<KernelEvent, 16> events{};
    const std::uint32_t count = Kernel_PollEvents(
        kernel, events.data(), static_cast<std::uint32_t>(events.size()));
    for (std::uint32_t index = 0u; index < count; ++index) {
        if (events[index].type == KernelEventType_PlayerJoined) {
            return events[index].net_id;
        }
    }
    require(false);
    return 0u;
}

float player_x(KernelHandle* kernel, std::uint32_t player_net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, player_net_id, &state));
    require(state.valid != 0u);
    return state.position.x;
}

// Walks the listen server's player at a barrier across x = 6 and reports how
// far it got. Unobstructed the walk covers 15 m.
float walk_at_barrier(
    const std::vector<std::uint8_t>& artifact,
    std::uint16_t port,
    std::uint32_t player_mask) {
    KernelConfig config = config_for(KernelMode_ListenServer);
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    const KernelStaticCollisionSceneConfig scene = camp_config(artifact);
    require(Kernel_SetStaticCollisionScene(kernel, &scene));
    const KernelPlayerBarrierBox box{
        KernelVec3{kWalkWallX, kCapsuleHeight, 0.0f},
        KernelVec3{kWalkWallHalfX, 10.0f, 20.0f},
        0.0f};
    const KernelStaticScenePlayerBarriersConfig set = barriers(0u, &box, 1u);
    require(Kernel_SetStaticScenePlayerBarriers(kernel, &set));

    require(Kernel_StartListenServer(kernel, port));
    load_walk_catalog(kernel, player_mask);

    const std::uint32_t player_net_id = find_local_player(kernel);
    require(Kernel_ServerSetEntityActorTemplate(kernel, player_net_id, 1u));
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.collider_template_id = 1u;
    combat.weapon_slot_count = 1u;
    combat.active_weapon_slot = 0u;
    combat.weapon_ids[0] = 0u;
    combat.move_speed_meters_per_second = kPlayerSpeed;
    combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    require(Kernel_ServerSetEntityCombatState(kernel, player_net_id, &combat));
    // The listen server's own player never goes through the template's create
    // path, so its mask is set the way a game server sets it.
    require(Kernel_ServerSetEntityMovementCollisionMask(kernel, player_net_id, player_mask));

    Kernel_Update(kernel, kFixedDeltaSeconds);
    require(player_x(kernel, player_net_id) == 0.0f);
    for (std::uint32_t tick = 0u; tick < 90u; ++tick) {
        KernelPlayerInput input{};
        input.input_seq = tick + 1u;
        input.move = KernelVec2{1.0f, 0.0f};
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        Kernel_SubmitPlayerInput(kernel, 1, &input);
        Kernel_Update(kernel, kFixedDeltaSeconds);
    }
    const float travelled = player_x(kernel, player_net_id);
    Kernel_Destroy(kernel);
    return travelled;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> artifact = read_artifact();
    server_lifecycle(artifact);
    listen_host_both_worlds(artifact);
    client_around_welcome(artifact);
    rejections(artifact);

    // Naming the layer stops the player against the barrier, well short of
    // the 15 m it asked for.
    const float blocked_x = walk_at_barrier(
        artifact, 8740, KERNEL_MOVEMENT_MASK_DEFAULT | KERNEL_MOVEMENT_LAYER_PLAYER_BARRIER);
    require(blocked_x > 0.0f);
    require(blocked_x < kContactX + 0.1f);
    require(blocked_x > kContactX - 0.5f);

    // The engine default -- what every agent has -- walks straight through.
    const float unblocked_x = walk_at_barrier(artifact, 8741, 0u);
    require(unblocked_x > kWalkWallX + kWalkWallHalfX + 1.0f);

    std::printf(
        "walking: blocked_x=%.3f (contact %.3f) unblocked_x=%.3f\n",
        static_cast<double>(blocked_x),
        static_cast<double>(kContactX),
        static_cast<double>(unblocked_x));
    return 0;
}
