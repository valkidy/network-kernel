// End-to-end cover for the two beam faults the presentation layer surfaced:
//
//  1. A held beam used to be destroyed and respawned under a fresh net_id every
//     other tick, because simulate_projectiles aged it against
//     max_lifetime_ticks while the weapon refresh only moved its origin.
//  2. Its collider was materialized as the authored oriented box placed on the
//     projectile transform: centred on the muzzle, half of it behind the
//     shooter, and world-axis aligned because nothing ever wrote a projectile's
//     rotation.
//
// Both are only observable through a real kernel tick, so this drives one.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "kernel/public/kernel_api.h"

namespace {

constexpr std::uint32_t kBeamColliderTemplateId = 20u;
constexpr std::uint32_t kBeamProjectileTemplateId = 5u;
constexpr std::uint8_t kBeamWeaponId = 5u;
constexpr std::uint32_t kBeamFireActionId = 4101u;
constexpr std::uint32_t kReloadActionId = 4102u;
constexpr std::uint32_t kEntityTemplateId = 101u;
constexpr std::uint32_t kActorTemplateId = 1u;

// beam_sentry_beam_box: length = half_extents.z * 2, radius = max(x, y).
constexpr float kBeamHalfLength = 7.0f;
constexpr float kBeamHalfWidth = 0.3f;
constexpr float kBeamLength = kBeamHalfLength * 2.0f;

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

bool nearly(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.001f;
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

    // The authored beam shape. It is the authority on reach, and it is also the
    // shape that used to be drawn verbatim on the projectile transform.
    KernelColliderTemplateDefinition beam_box{};
    beam_box.struct_size = sizeof(beam_box);
    beam_box.template_id = kBeamColliderTemplateId;
    beam_box.shape_type = KernelColliderShapeType_OrientedBox;
    beam_box.center = KernelVec3{0.0f, 0.0f, 0.0f};
    beam_box.shape_params =
        KernelVec4{kBeamHalfWidth, kBeamHalfWidth, kBeamHalfLength, 0.0f};
    beam_box.purpose_flags = KernelColliderPurpose_Damage;
    beam_box.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;

    KernelColliderTemplateDefinition actor_movement{};
    actor_movement.struct_size = sizeof(actor_movement);
    actor_movement.template_id = 11u;
    actor_movement.shape_type = KernelColliderShapeType_Capsule;
    actor_movement.center = KernelVec3{0.0f, 0.9f, 0.0f};
    actor_movement.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    actor_movement.purpose_flags = KernelColliderPurpose_Movement;
    actor_movement.layer_mask = KERNEL_COLLISION_LAYER_PLAYER_SIDE;

    const std::array<KernelColliderTemplateDefinition, 3> collider_templates = {
        player_hit,
        beam_box,
        actor_movement,
    };

    KernelProjectileTemplateDefinition beam{};
    beam.struct_size = sizeof(beam);
    beam.projectile_template_id = kBeamProjectileTemplateId;
    beam.weapon_id = kBeamWeaponId;
    beam.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    beam.mechanics.projectile_type = KernelProjectileType_Beam;
    beam.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    beam.mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    beam.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    beam.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    // Inert for beams -- beam.damage_per_tick overwrites it -- but the
    // validator still requires a direct-hit projectile to carry damage.
    beam.mechanics.damage = 2;
    beam.mechanics.speed = 0.0f;
    beam.mechanics.lifetime_ticks = 0u;
    beam.mechanics.collider_template_id = kBeamColliderTemplateId;
    beam.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    beam.mechanics.max_hit_count = 1u;
    beam.mechanics.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
    beam.mechanics.beam.length = kBeamLength;
    beam.mechanics.beam.radius = kBeamHalfWidth;
    beam.mechanics.beam.damage_per_tick = 2u;
    // The value that used to expire the entity out from under the refresh.
    beam.mechanics.beam.lifetime_ticks = 2u;
    beam.mechanics.beam.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    const std::array<KernelProjectileTemplateDefinition, 1> projectile_templates = {
        beam,
    };

    KernelActionTemplateDefinition fire_action{};
    fire_action.struct_size = sizeof(fire_action);
    fire_action.action_template_id = kBeamFireActionId;
    fire_action.trigger_mode = KernelActionTriggerMode_Hold;
    fire_action.ammo_cost_per_commit = 1u;
    fire_action.commit_offset_ticks = 0u;
    fire_action.commit_interval_ticks = 1u;
    fire_action.max_commit_count = 0u;
    fire_action.recovery_ticks = 0u;
    fire_action.hold_input_timeout_ticks = 6u;

    KernelActionTemplateDefinition reload_action{};
    reload_action.struct_size = sizeof(reload_action);
    reload_action.action_template_id = kReloadActionId;
    reload_action.trigger_mode = KernelActionTriggerMode_Press;
    reload_action.commit_offset_ticks = 30u;
    reload_action.max_commit_count = 1u;
    const std::array<KernelActionTemplateDefinition, 2> action_templates = {
        fire_action,
        reload_action,
    };

    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = kActorTemplateId;
    actor_template.entity_type = KernelEntityType_Actor;
    actor_template.actor_type = KernelActorType_Player;
    actor_template.collider_template_id = 1u;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_PlayerSide;
    const std::array<KernelActorTemplateDefinition, 1> actor_templates = {
        actor_template,
    };

    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = kEntityTemplateId;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_type = KernelActorType_Player;
    entity_template.actor_template_id = kActorTemplateId;
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    entity_template.collider_template_id = 1u;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 100;
    entity_template.combat.max_hp = 100;
    entity_template.combat.move_speed_meters_per_second = 5.0f;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
    entity_template.movement.controller_type =
        KernelMovementControllerType_Character;
    entity_template.movement.movement_collider_template_id = 11u;
    entity_template.movement.gravity = KernelVec3{0.0f, -9.8f, 0.0f};
    entity_template.movement.max_slope_degrees = 45.0f;
    entity_template.movement.ground_probe_distance = 0.2f;
    entity_template.movement.ground_snap_distance = 0.2f;
    entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
    entity_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    entity_template.vision.camp = KernelAgentCamp_PlayerSide;
    const std::array<KernelEntityTemplateDefinition, 1> entity_templates = {
        entity_template,
    };

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(collider_templates.size());
    catalog.projectile_templates = projectile_templates.data();
    catalog.projectile_template_count =
        static_cast<std::uint32_t>(projectile_templates.size());
    catalog.action_templates = action_templates.data();
    catalog.action_template_count =
        static_cast<std::uint32_t>(action_templates.size());
    catalog.actor_templates = actor_templates.data();
    catalog.actor_template_count =
        static_cast<std::uint32_t>(actor_templates.size());
    catalog.entity_templates = entity_templates.data();
    catalog.entity_template_count =
        static_cast<std::uint32_t>(entity_templates.size());
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

void configure_beam_shooter(KernelHandle* kernel, std::uint32_t net_id) {
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_slot = 0;
    combat.weapon_slot_count = 1;
    combat.weapon_ids[0] = kBeamWeaponId;
    combat.ammo[0] = 200;
    combat.reserve_magazines[0] = 3;
    combat.collider_template_id = 1u;
    combat.move_speed_meters_per_second = 5.0f;
    combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    require(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, kActorTemplateId));

    KernelWeaponMechanicsDefinition beam_weapon{};
    beam_weapon.struct_size = sizeof(beam_weapon);
    beam_weapon.weapon_id = kBeamWeaponId;
    beam_weapon.fire_mode = KernelWeaponFireMode_Projectile;
    beam_weapon.magazine_size = 200;
    beam_weapon.damage = 2;
    beam_weapon.projectile_template_id = kBeamProjectileTemplateId;
    beam_weapon.fire_action_template_id = kBeamFireActionId;
    beam_weapon.reload_action_template_id = kReloadActionId;
    require(Kernel_ServerValidateMechanicsConfig(&beam_weapon));
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, net_id, &beam_weapon));
}

void held_beam_is_one_entity_and_its_collider_starts_at_the_muzzle() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7801));
    load_catalog(kernel);

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_template_id = kEntityTemplateId;
    create.position = KernelVec3{0.0f, 0.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t shooter_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &shooter_net_id));
    require(shooter_net_id != 0);
    configure_beam_shooter(kernel, shooter_net_id);

    constexpr std::uint32_t kTicks = 16u;
    std::uint32_t beam_net_id = 0;
    std::uint32_t projectile_spawns = 0;
    std::uint32_t first_beam_destroys = 0;

    for (std::uint32_t tick = 1; tick <= kTicks; ++tick) {
        KernelPlayerInput input{};
        input.input_seq = tick;
        input.selected_weapon = kBeamWeaponId;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        if (tick == 1u) {
            // The intent opens the hold; every later tick just keeps it held.
            input.action_intent = KernelActionIntent{
                1u, KernelActionBinding_PrimaryFire, 0u, 0u};
        }
        input.action_input = KernelActionInput{1u, 1u, 0u, 0u};
        require(Kernel_ServerSubmitEntityInput(kernel, shooter_net_id, &input));
        Kernel_Update(kernel, 1.0f / 30.0f);

        std::array<KernelEvent, 32> events{};
        const std::uint32_t event_count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            const KernelEvent& event = events[index];
            if (event.type == KernelEventType_EntitySpawned &&
                event.code == KernelEntityType_Projectile) {
                ++projectile_spawns;
                if (beam_net_id == 0) {
                    beam_net_id = event.net_id;
                }
            }
            if (event.type == KernelEventType_EntityDestroyed &&
                beam_net_id != 0 && event.net_id == beam_net_id) {
                ++first_beam_destroys;
            }
        }
    }

    // One beam for the whole hold. Measured before the fix, the same sixteen
    // ticks produced eight spawns -- a fresh net_id every other tick, which is
    // what reached the client as a strobe.
    require(beam_net_id != 0);
    require(projectile_spawns == 1u);
    require(first_beam_destroys == 0u);

    KernelColliderShapeQuery query{};
    query.struct_size = sizeof(query);
    query.entity_net_id = beam_net_id;
    std::array<KernelColliderShapeView, 4> shapes{};
    const std::uint32_t shape_count = Kernel_QueryColliderShapes(
        kernel, &query, shapes.data(), static_cast<std::uint32_t>(shapes.size()));
    require(shape_count == 1u);
    const KernelColliderShapeView& shape = shapes[0];

    // A swept sphere is a segment with a radius, not the authored box.
    require(shape.shape_type == KernelColliderShapeType_Segment);
    // shape_params for a segment is {length, radius}.
    require(nearly(shape.shape_params.x, kBeamLength));
    require(nearly(shape.shape_params.y, kBeamHalfWidth));

    // Aim was +X, so the segment runs the full length along +X and nowhere else.
    require(nearly(shape.segment_end.x - shape.segment_start.x, kBeamLength));
    require(nearly(shape.segment_end.y, shape.segment_start.y));
    require(nearly(shape.segment_end.z, shape.segment_start.z));

    // The muzzle is an endpoint, not the centre. This is the whole fault: the
    // authored box, placed as authored, would have put the muzzle at the centre
    // with half the length behind the shooter.
    require(nearly(
        shape.world_center.x,
        0.5f * (shape.segment_start.x + shape.segment_end.x)));
    require(shape.world_center.x > shape.segment_start.x + 0.001f);
    require(nearly(shape.segment_start.x - shape.world_center.x, -kBeamLength * 0.5f));

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    held_beam_is_one_entity_and_its_collider_starts_at_the_muzzle();
    return 0;
}
