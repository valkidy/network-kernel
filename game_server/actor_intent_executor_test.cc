#include "game_server/actor_intent_executor.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "ai_intent.h"
#include "game_server/gameplay_config.h"

namespace {

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
    return projectile;
}

void load_catalog(KernelHandle* kernel) {
    const KernelColliderTemplateDefinition collider = collider_template();
    const KernelProjectileTemplateDefinition projectile = projectile_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = &collider;
    catalog.collider_template_count = 1;
    catalog.projectile_templates = &projectile;
    catalog.projectile_template_count = 1;
    assert(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

std::uint32_t create_actor(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypeAgent;
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
    combat.active_weapon_slot = 0;
    combat.weapon_slot_count = 1;
    combat.weapon_ids[0] =
        network_example::game_server::kAgentSpammerWeaponId;
    combat.collider_template_id = 1;
    combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat.ammo[0] = ammo;
    combat.reserve_magazines[0] = reserve_magazines;
    assert(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
}

void set_weapon(KernelHandle* kernel, std::uint32_t net_id) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(weapon);
    weapon.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = 2;
    weapon.damage = 1;
    weapon.projectile_template_id = 3;
    assert(Kernel_ServerSetEntityWeaponMechanics(kernel, net_id, &weapon));
}

KernelServerEntityState query_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.valid != 0u);
    return state;
}

network_example::ai::ScopedIntent actor_intent(const char* type, std::uint32_t actor) {
    network_example::ai::ScopedIntent intent;
    intent.scope = network_example::ai::IntentScope::kActor;
    intent.type = type;
    intent.subject = actor;
    return intent;
}

network_example::game_server::SentryPerceptionSnapshot perception(
    KernelHandle* kernel,
    std::uint32_t actor,
    std::uint32_t target,
    const KernelVec3& target_position) {
    network_example::game_server::SentryPerceptionSnapshot snapshot;
    snapshot.self_state = query_state(kernel, actor);
    snapshot.has_self_state = true;
    snapshot.has_visible_target = target != 0;
    snapshot.target_id = target;
    snapshot.has_target_position = target != 0;
    snapshot.target_position = target_position;
    return snapshot;
}

}  // namespace

int main() {
    const KernelVec3 gravity{0.0f, -9.81f, 0.0f};
    const auto low_arc =
        network_example::game_server::solve_low_ballistic_aim(
            {0.0f, 0.0f, 0.0f},
            {20.0f, 0.0f, 0.0f},
            24.0f,
            gravity,
            3.0f);
    assert(low_arc.has_value());
    assert(low_arc->aim_direction.y > 0.0f);
    const float flight_seconds = low_arc->flight_seconds;
    const KernelVec3 solved_position{
        low_arc->aim_direction.x * 24.0f * flight_seconds,
        low_arc->aim_direction.y * 24.0f * flight_seconds +
            0.5f * gravity.y * flight_seconds * flight_seconds,
        low_arc->aim_direction.z * 24.0f * flight_seconds,
    };
    assert(std::fabs(solved_position.x - 20.0f) < 0.001f);
    assert(std::fabs(solved_position.y) < 0.001f);
    assert(std::fabs(solved_position.z) < 0.001f);
    assert(!network_example::game_server::solve_low_ballistic_aim(
                {0.0f, 0.0f, 0.0f},
                {100.0f, 0.0f, 0.0f},
                24.0f,
                gravity,
                3.0f)
                .has_value());
    assert(!network_example::game_server::solve_low_ballistic_aim(
                {0.0f, 0.0f, 0.0f},
                {58.0f, 0.0f, 0.0f},
                24.0f,
                gravity,
                3.0f)
                .has_value());

    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7777));
    load_catalog(kernel);

    const std::uint32_t actor = create_actor(kernel, {0.0f, 0.0f, 0.0f});
    const std::uint32_t target = create_actor(kernel, {5.0f, 0.0f, 0.0f});
    set_weapon(kernel, actor);

    assert(std::fabs(Kernel_GetFixedDeltaSeconds(kernel) - 1.0f / 30.0f) <
           0.0001f);
    KernelVec3 launch_position{};
    assert(Kernel_ServerGetProjectileLaunchPosition(
        kernel,
        actor,
        &launch_position));
    assert(std::fabs(launch_position.x) < 0.0001f);
    assert(std::fabs(launch_position.y - 1.0f) < 0.0001f);
    assert(std::fabs(launch_position.z) < 0.0001f);
    KernelVec3 target_aim_point{};
    assert(Kernel_ServerGetEntityAimPoint(kernel, target, &target_aim_point));
    assert(std::fabs(target_aim_point.x - 5.0f) < 0.0001f);
    assert(std::fabs(target_aim_point.y - 0.8f) < 0.0001f);
    assert(std::fabs(target_aim_point.z) < 0.0001f);

    network_example::game_server::AgentRuntimeState enemy;
    enemy.net_id = actor;
    network_example::game_server::ActorIntentExecutor executor(
        network_example::game_server::ActorIntentExecutorConfig{
            network_example::game_server::kAgentSpammerWeaponId});

    set_combat(kernel, actor, 2, 4);
    auto attack = actor_intent("AttackTarget", actor);
    auto result = executor.execute(
        kernel,
        &enemy,
        attack,
        perception(kernel, actor, target, {5.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kRunning);
    assert(result.submitted_input);
    Kernel_Update(kernel, 1.0f / 30.0f);
    KernelServerEntityState state = query_state(kernel, actor);
    assert(state.ammo[0] == 1);

    set_combat(kernel, actor, 0, 2);
    auto reload = actor_intent("Reload", actor);
    result = executor.execute(
        kernel,
        &enemy,
        reload,
        perception(kernel, actor, target, {5.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kRunning);
    assert(result.submitted_input);
    Kernel_Update(kernel, 1.0f / 30.0f);
    state = query_state(kernel, actor);
    assert(state.is_reloading != 0u);

    set_combat(kernel, actor, 2, 4);
    auto missing_target = actor_intent("AttackTarget", actor);
    result = executor.execute(
        kernel,
        &enemy,
        missing_target,
        perception(kernel, actor, 0, {0.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(!result.submitted_input);
    assert(result.report.missing_data.size() == 1);
    Kernel_Update(kernel, 1.0f / 30.0f);
    state = query_state(kernel, actor);
    assert(state.ammo[0] == 2);

    auto unsupported = actor_intent("FindCover", actor);
    result = executor.execute(
        kernel,
        &enemy,
        unsupported,
        perception(kernel, actor, target, {5.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(!result.submitted_input);
    assert(result.report.missing_actions.size() == 1);
    assert(result.report.missing_executors.size() == 1);

    const std::uint32_t empty_actor = create_actor(kernel, {1.0f, 0.0f, 0.0f});
    set_weapon(kernel, empty_actor);
    set_combat(kernel, empty_actor, 0, 0);
    network_example::game_server::AgentRuntimeState empty_enemy;
    empty_enemy.net_id = empty_actor;
    result = executor.execute(
        kernel,
        &empty_enemy,
        actor_intent("AttackTarget", empty_actor),
        perception(kernel, empty_actor, target, {5.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(!result.submitted_input);
    assert(result.report.missing_actions.size() == 1);
    Kernel_Update(kernel, 1.0f / 30.0f);
    state = query_state(kernel, empty_actor);
    assert(state.ammo[0] == 0);
    assert(state.reserve_magazines[0] == 0);

    network_example::game_server::ActorIntentExecutorConfig ballistic_config;
    ballistic_config.weapon_id =
        network_example::game_server::kAgentSpammerWeaponId;
    ballistic_config.ballistic_aim.enabled = true;
    ballistic_config.ballistic_aim.speed = 24.0f;
    ballistic_config.ballistic_aim.gravity = gravity;
    ballistic_config.ballistic_aim.lifetime_ticks = 90;
    network_example::game_server::ActorIntentExecutor ballistic_executor(
        ballistic_config);

    const std::uint32_t ballistic_actor =
        create_actor(kernel, {0.0f, 0.0f, 0.0f});
    set_weapon(kernel, ballistic_actor);
    set_combat(kernel, ballistic_actor, 2, 0);
    network_example::game_server::AgentRuntimeState ballistic_enemy;
    ballistic_enemy.net_id = ballistic_actor;
    result = ballistic_executor.execute(
        kernel,
        &ballistic_enemy,
        actor_intent("AttackTarget", ballistic_actor),
        perception(kernel, ballistic_actor, target, {20.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kRunning);
    assert(result.submitted_input);
    Kernel_Update(kernel, 1.0f / 30.0f);
    state = query_state(kernel, ballistic_actor);
    assert(state.ammo[0] == 1);

    const std::uint32_t unreachable_actor =
        create_actor(kernel, {0.0f, 0.0f, 0.0f});
    set_weapon(kernel, unreachable_actor);
    set_combat(kernel, unreachable_actor, 2, 0);
    network_example::game_server::AgentRuntimeState unreachable_enemy;
    unreachable_enemy.net_id = unreachable_actor;
    result = ballistic_executor.execute(
        kernel,
        &unreachable_enemy,
        actor_intent("AttackTarget", unreachable_actor),
        perception(kernel, unreachable_actor, target, {100.0f, 0.0f, 0.0f}));
    assert(result.status == network_example::ai::IntentStatus::kFailed);
    assert(!result.submitted_input);
    assert(result.ballistic_solution_unavailable);
    assert(result.report.missing_data.size() == 1);
    assert(result.report.missing_data[0] == "Data.BallisticAimSolution");
    Kernel_Update(kernel, 1.0f / 30.0f);
    state = query_state(kernel, unreachable_actor);
    assert(state.ammo[0] == 2);

    Kernel_Destroy(kernel);
    return 0;
}
