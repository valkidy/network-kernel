#include "game_server/agent_sentry_controller.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "game_server/gameplay_config.h"

namespace {

constexpr std::uint32_t kTestFireActionTemplateId = 100;
constexpr std::uint32_t kTestReloadActionTemplateId = 101;

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelColliderTemplateDefinition collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 1;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    return collider;
}

KernelColliderTemplateDefinition vision_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 2;
    collider.shape_type = KernelColliderShapeType_Cone;
    collider.shape_params = KernelVec4{10.0f, 90.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Vision;
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    return collider;
}

KernelProjectileTemplateDefinition projectile_template() {
    KernelProjectileTemplateDefinition projectile{};
    projectile.struct_size = sizeof(projectile);
    projectile.projectile_template_id = 3;
    projectile.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    projectile.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    projectile.mechanics.projectile_type = KernelProjectileType_Standard;
    projectile.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    projectile.mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    projectile.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    projectile.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    projectile.mechanics.damage = 1;
    projectile.mechanics.speed = 30.0f;
    projectile.mechanics.lifetime_ticks = 30;
    projectile.mechanics.collider_template_id = 1;
    projectile.mechanics.collision_mask = KERNEL_COLLISION_MASK_NONE;
    projectile.mechanics.max_hit_count = 1;
    projectile.mechanics.flags = 1u;
    return projectile;
}

KernelActionTemplateDefinition fire_action_template() {
    KernelActionTemplateDefinition action{};
    action.struct_size = sizeof(action);
    action.action_template_id = kTestFireActionTemplateId;
    action.trigger_mode = KernelActionTriggerMode_Press;
    action.ammo_cost_per_commit = 1;
    action.max_commit_count = 1;
    return action;
}

KernelActionTemplateDefinition reload_action_template(std::uint32_t reload_ticks) {
    KernelActionTemplateDefinition action{};
    action.struct_size = sizeof(action);
    action.action_template_id = kTestReloadActionTemplateId;
    action.trigger_mode = KernelActionTriggerMode_Press;
    action.commit_offset_ticks = reload_ticks;
    action.max_commit_count = 1;
    return action;
}

void load_catalog(KernelHandle* kernel) {
    const std::array<KernelColliderTemplateDefinition, 2> colliders = {
        collider_template(),
        vision_collider_template(),
    };
    const KernelProjectileTemplateDefinition projectile = projectile_template();
    const std::array<KernelActionTemplateDefinition, 2> actions = {
        fire_action_template(),
        reload_action_template(3),
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count = static_cast<std::uint32_t>(colliders.size());
    catalog.projectile_templates = &projectile;
    catalog.projectile_template_count = 1;
    catalog.action_templates = actions.data();
    catalog.action_template_count = static_cast<std::uint32_t>(actions.size());
    assert(Kernel_LoadGameplayCatalog(kernel, &catalog));
}

std::uint32_t create_entity(
    KernelHandle* kernel,
    std::uint16_t actor_type,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = actor_type;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    assert(net_id != 0);
    return net_id;
}

void set_combat(
    KernelHandle* kernel,
    std::uint32_t net_id,
    std::uint16_t ammo,
    std::uint16_t reserve_magazines) {
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    combat.collider_template_id = 1;
    combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat.ammo[network_example::game_server::kAgentSpammerWeaponId] =
        ammo;
    combat.reserve_magazines[network_example::game_server::kAgentSpammerWeaponId] =
        reserve_magazines;
    assert(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
}

void set_spammer_weapon_mechanics(
    KernelHandle* kernel,
    std::uint32_t net_id,
    std::uint16_t magazine_size) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(weapon);
    weapon.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = magazine_size;
    weapon.damage = 1;
    weapon.fire_action_template_id = kTestFireActionTemplateId;
    weapon.reload_action_template_id = kTestReloadActionTemplateId;
    weapon.projectile_template_id = 3;
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, net_id, &weapon));
}

void set_vision(
    KernelHandle* kernel,
    std::uint32_t net_id,
    std::uint8_t camp,
    std::uint32_t vision_collider_template_id) {
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = camp;
    vision.vision_collider_template_id = vision_collider_template_id;
    vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    assert(Kernel_ServerSetEntityVisionConfig(kernel, net_id, &vision));
}

void run_frame(
    KernelHandle* kernel,
    const network_example::game_server::AgentSentryController& controller,
    std::vector<network_example::game_server::AgentRuntimeState>* enemies) {
    controller.tick(kernel, enemies, 1.0f / 30.0f);
    Kernel_Update(kernel, 1.0f / 30.0f);
}

KernelQuat query_rotation(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.valid != 0u);
    return state.rotation;
}

KernelServerEntityState query_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.valid != 0u);
    return state;
}

bool almost_equal(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

float yaw_from_rotation(const KernelQuat& rotation) {
    return 2.0f * std::atan2(-rotation.y, rotation.w);
}

void assert_rotation_faces(const KernelQuat& rotation, float x, float z) {
    const float expected_yaw = std::atan2(z, x);
    assert(almost_equal(rotation.y, -std::sin(expected_yaw * 0.5f)));
    assert(almost_equal(rotation.w, std::cos(expected_yaw * 0.5f)));
}

}  // namespace

int main() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7777));
    load_catalog(kernel);

    const std::uint32_t enemy_net_id =
        create_entity(kernel, network_example::game_server::kActorTypeAgent, {0.0f, 0.0f, 0.0f});
    const std::uint32_t player_net_id =
        create_entity(kernel, network_example::game_server::kActorTypePlayer, {5.0f, 0.0f, 0.0f});
    set_combat(kernel, enemy_net_id, 2, 4);
    set_spammer_weapon_mechanics(kernel, enemy_net_id, 2);
    set_vision(kernel, enemy_net_id, KernelAgentCamp_EnemySide, 2);
    set_vision(kernel, player_net_id, KernelAgentCamp_PlayerSide, 0);
    KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};

    KernelVec3 out_of_range{100.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSetEntityTransform(kernel, player_net_id, &out_of_range, &identity));
    Kernel_Update(kernel, 1.0f / 30.0f);

    network_example::game_server::AgentRuntimeState enemy;
    enemy.net_id = enemy_net_id;
    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    std::vector<network_example::game_server::AgentRuntimeState> enemies{enemy};

    network_example::game_server::AgentSentryConfig sentry_config;
    sentry_config.alert_ticks = 2;
    sentry_config.forget_ticks = 3;
    sentry_config.patrol_rotation_interval_ticks = 2;
    sentry_config.patrol_rotation_min_degrees = 15.0f;
    sentry_config.patrol_rotation_max_degrees = 30.0f;
    sentry_config.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    network_example::game_server::AgentSentryController controller(sentry_config);

    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kIdle);
    KernelQuat rotation = query_rotation(kernel, enemy_net_id);
    assert(almost_equal(yaw_from_rotation(rotation), 0.0f));
    run_frame(kernel, controller, &enemies);
    rotation = query_rotation(kernel, enemy_net_id);
    const float patrol_degrees = std::fabs(yaw_from_rotation(rotation)) * 180.0f /
                                 3.14159265358979323846f;
    assert(patrol_degrees >= 15.0f);
    assert(patrol_degrees <= 30.0f);

    KernelVec3 player_position{5.0f, 0.0f, 2.0f};
    KernelVec3 agent_position{0.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSetEntityTransform(
        kernel,
        enemy_net_id,
        &agent_position,
        &identity));
    assert(Kernel_ServerSetEntityTransform(kernel, player_net_id, &player_position, &identity));
    Kernel_Update(kernel, 1.0f / 30.0f);
    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAlert);
    rotation = query_rotation(kernel, enemy_net_id);
    assert_rotation_faces(rotation, 5.0f, 2.0f);
    KernelServerEntityState state = query_state(kernel, enemy_net_id);
    assert(state.ammo[network_example::game_server::kAgentSpammerWeaponId] == 2);

    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAttack);
    state = query_state(kernel, enemy_net_id);
    assert(state.ammo[network_example::game_server::kAgentSpammerWeaponId] == 1);

    run_frame(kernel, controller, &enemies);
    state = query_state(kernel, enemy_net_id);
    assert(state.ammo[network_example::game_server::kAgentSpammerWeaponId] == 0);

    run_frame(kernel, controller, &enemies);
    state = query_state(kernel, enemy_net_id);
    assert(state.is_reloading != 0u);
    assert(state.reload_remaining_ticks > 0u);
    for (int tick = 0; tick < 3; ++tick) {
        run_frame(kernel, controller, &enemies);
    }
    state = query_state(kernel, enemy_net_id);
    assert(state.is_reloading == 0u);
    assert(state.ammo[network_example::game_server::kAgentSpammerWeaponId] == 2);
    assert(state.reserve_magazines[network_example::game_server::kAgentSpammerWeaponId] == 3);

    assert(Kernel_ServerSetEntityTransform(kernel, player_net_id, &out_of_range, &identity));
    Kernel_Update(kernel, 1.0f / 30.0f);
    for (int tick = 0; tick < 2; ++tick) {
        run_frame(kernel, controller, &enemies);
        assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAttack);
    }
    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAlert);

    for (int tick = 0; tick < 2; ++tick) {
        run_frame(kernel, controller, &enemies);
        assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAlert);
    }
    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kIdle);
    assert(enemies[0].velocity.x == 0.0f);
    assert(enemies[0].velocity.y == 0.0f);
    assert(enemies[0].velocity.z == 0.0f);

    Kernel_Destroy(kernel);
    return 0;
}
