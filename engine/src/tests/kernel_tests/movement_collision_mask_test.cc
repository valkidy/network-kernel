// movement.collision_mask has to be symmetric.
//
// The mask filters the owner's own movement sweeps, and that half has always
// worked. What did not is the other half: the owner's movement capsule was
// registered on the actor layer no matter what the mask said, so an actor
// authored terrain-only walked through a player while the player -- sweeping the
// engine default -- still walked into it. On the production monster that is a
// 3 m x 16 m capsule floating 4 m up, and standing under the belly was a wall.
//
// Both scenarios below run against the same wide blocker, spawned in the local
// player's path. The only difference is the blocker's authored mask.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "kernel/public/kernel_api.h"

namespace {

void require_at(bool condition, int line, const char* expression) {
    if (!condition) {
        std::fprintf(
            stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_at((condition), __LINE__, #condition)

constexpr float kFixedDeltaSeconds = 1.0f / 30.0f;
constexpr float kPlayerSpeed = 5.0f;
constexpr float kBlockerX = 6.0f;
constexpr float kBlockerRadius = 2.0f;
// Predicted actor blocking requires a verified static scene, so the fixture
// loads the real undulating terrain. Both capsules are then floated well above
// its +-5.4 m relief (and gravity is zero) so the only thing either one can
// meet is the other.
constexpr float kCapsuleHeight = 20.0f;

// Where the player's capsule comes to rest against the blocker's, if it is
// stopped at all: blocker centre minus both radii.
constexpr float kContactX = kBlockerX - kBlockerRadius - 0.35f;

constexpr std::uint32_t kBlockingTemplateId = 102u;
constexpr std::uint32_t kNonBlockingTemplateId = 103u;

std::vector<std::uint8_t> read_static_collision_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "shipping_catalog" / "mesh_assets" /
        "generated" / "jolt" /
        "undulating.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

void load_catalog(KernelHandle* kernel) {
    KernelColliderTemplateDefinition player_hit{};
    player_hit.struct_size = sizeof(player_hit);
    player_hit.template_id = 1u;
    player_hit.shape_type = KernelColliderShapeType_Aabb;
    player_hit.center = KernelVec3{0.0f, 0.9f, 0.0f};
    player_hit.shape_params = KernelVec4{0.35f, 0.9f, 0.35f, 0.0f};
    player_hit.purpose_flags = KernelColliderPurpose_Hit;
    player_hit.layer_mask = KERNEL_COLLISION_LAYER_PLAYER_SIDE;

    KernelColliderTemplateDefinition blocker_hit = player_hit;
    blocker_hit.template_id = 2u;
    blocker_hit.center = KernelVec3{0.0f, 1.0f, 0.0f};
    blocker_hit.shape_params = KernelVec4{2.0f, 1.0f, 2.0f, 0.0f};
    blocker_hit.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;

    KernelColliderTemplateDefinition player_movement{};
    player_movement.struct_size = sizeof(player_movement);
    player_movement.template_id = 11u;
    player_movement.shape_type = KernelColliderShapeType_Capsule;
    player_movement.center = KernelVec3{0.0f, kCapsuleHeight, 0.0f};
    player_movement.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    player_movement.purpose_flags = KernelColliderPurpose_Movement;
    player_movement.layer_mask =
        KERNEL_COLLISION_LAYER_PLAYER_SIDE |
        KERNEL_COLLISION_LAYER_HOSTILE_SIDE;

    // Wide enough that the player cannot slide past it, and sharing the
    // player's mid-height so the cylinder section is what it meets.
    KernelColliderTemplateDefinition blocker_movement = player_movement;
    blocker_movement.template_id = 12u;
    blocker_movement.shape_params = KernelVec4{1.0f, kBlockerRadius, 0.0f, 0.0f};

    const std::array<KernelColliderTemplateDefinition, 4> collider_templates = {
        player_hit,
        blocker_hit,
        player_movement,
        blocker_movement,
    };

    KernelActorTemplateDefinition player_actor{};
    player_actor.struct_size = sizeof(player_actor);
    player_actor.actor_template_id = 1u;
    player_actor.entity_type = KernelEntityType_Actor;
    player_actor.actor_type = KernelActorType_Player;
    player_actor.collider_template_id = 1u;
    player_actor.vision.struct_size = sizeof(KernelAgentVisionConfig);
    player_actor.vision.camp = KernelAgentCamp_PlayerSide;

    KernelActorTemplateDefinition blocking_actor = player_actor;
    blocking_actor.actor_template_id = 2u;
    blocking_actor.actor_type = KernelActorType_Agent;
    blocking_actor.collider_template_id = 2u;
    blocking_actor.vision.camp = KernelAgentCamp_EnemySide;

    KernelActorTemplateDefinition non_blocking_actor = blocking_actor;
    non_blocking_actor.actor_template_id = 3u;

    const std::array<KernelActorTemplateDefinition, 3> actor_templates = {
        player_actor,
        blocking_actor,
        non_blocking_actor,
    };

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
    player_entity.movement.controller_type =
        KernelMovementControllerType_Character;
    player_entity.movement.movement_collider_template_id = 11u;
    // Nothing falls, so horizontal blocking is the only thing under test.
    player_entity.movement.gravity = KernelVec3{0.0f, 0.0f, 0.0f};
    player_entity.movement.max_slope_degrees = 45.0f;
    player_entity.ai.struct_size = sizeof(KernelEntityAiDefinition);
    player_entity.vision.struct_size = sizeof(KernelAgentVisionConfig);
    player_entity.vision.camp = KernelAgentCamp_PlayerSide;

    KernelEntityTemplateDefinition blocking_entity = player_entity;
    blocking_entity.entity_template_id = kBlockingTemplateId;
    blocking_entity.actor_type = KernelActorType_Agent;
    blocking_entity.actor_template_id = 2u;
    blocking_entity.collider_template_id = 2u;
    blocking_entity.combat.move_speed_meters_per_second = 0.0f;
    blocking_entity.combat.hitbox_center = KernelVec3{0.0f, 1.0f, 0.0f};
    blocking_entity.combat.hitbox_half_extents = KernelVec3{2.0f, 1.0f, 2.0f};
    blocking_entity.movement.movement_collider_template_id = 12u;
    blocking_entity.vision.camp = KernelAgentCamp_EnemySide;
    // Zero is "engine default", which includes the actor layer.
    blocking_entity.movement.movement_collision_mask = 0u;

    KernelEntityTemplateDefinition non_blocking_entity = blocking_entity;
    non_blocking_entity.entity_template_id = kNonBlockingTemplateId;
    non_blocking_entity.actor_template_id = 3u;
    non_blocking_entity.movement.movement_collision_mask =
        KERNEL_MOVEMENT_LAYER_TERRAIN;

    const std::array<KernelEntityTemplateDefinition, 3> entity_templates = {
        player_entity,
        blocking_entity,
        non_blocking_entity,
    };

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(collider_templates.size());
    catalog.actor_templates = actor_templates.data();
    catalog.actor_template_count =
        static_cast<std::uint32_t>(actor_templates.size());
    catalog.entity_templates = entity_templates.data();
    catalog.entity_template_count =
        static_cast<std::uint32_t>(entity_templates.size());
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

// Walks the local player at the blocker from the origin and reports how far it
// got. Unobstructed the walk covers 15 m, well past the blocker at 6 m.
float walk_into_blocker(std::uint16_t port, std::uint32_t blocker_template_id) {
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));

    const std::vector<std::uint8_t> scene = read_static_collision_scene();
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = 1u;
    scene_config.collider_id = 1u;
    scene_config.collision_layer = KERNEL_STATIC_COLLISION_LAYER_TERRAIN;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));

    require(Kernel_StartListenServer(kernel, port));
    load_catalog(kernel);

    const std::uint32_t player_net_id = find_local_player(kernel);
    require(Kernel_ServerSetEntityActorTemplate(kernel, player_net_id, 1u));
    // Movement speed reaches MovementState through the combat state, not the
    // template, so the listen server's own player needs it set explicitly.
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

    KernelServerEntityCreateInfo blocker{};
    blocker.struct_size = sizeof(blocker);
    blocker.entity_template_id = blocker_template_id;
    blocker.position = KernelVec3{kBlockerX, 0.0f, 0.0f};
    blocker.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t blocker_net_id = 0u;
    require(Kernel_ServerCreateEntity(kernel, &blocker, &blocker_net_id));
    require(blocker_net_id != 0u);

    // One tick to seat both bodies before the walk starts.
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
    // The default mask keeps the old behaviour: the blocker stops the player,
    // resting against it rather than anywhere near the 15 m the walk asked for.
    const float blocked_x = walk_into_blocker(7811, kBlockingTemplateId);
    require(blocked_x > 0.0f);
    require(blocked_x < kContactX + 0.1f);
    require(blocked_x > kContactX - 0.5f);

    // Dropping the actor layer takes the capsule out of the player's sweep too,
    // so the walk runs to completion straight through where it stood.
    const float unblocked_x = walk_into_blocker(7812, kNonBlockingTemplateId);
    require(unblocked_x > kBlockerX + kBlockerRadius);

    std::printf(
        "movement_collision_mask: blocked_x=%.3f unblocked_x=%.3f\n",
        static_cast<double>(blocked_x),
        static_cast<double>(unblocked_x));
    return 0;
}
