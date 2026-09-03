#include "game_server/agent_chaser_controller.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/ai_perception_adapter.h"
#include "game_server/gameplay_config.h"

namespace {

// Every kernel call in this file's setup used to sit inside assert(). Under
// -c opt -- the configuration the suite is actually run in -- NDEBUG expands
// assert(expr) to ((void)0), so the expression is never evaluated: no catalog
// was loaded, no entity was created, no vision was configured, and the test
// reported PASSED having called nothing. A precondition with a side effect has
// to be stated in something that survives NDEBUG.
//
// The setup calls were converted then; the assertions were not, so every check
// in main() was compiled out too, under the one configuration this suite runs
// in. They are require() now and they all pass -- but they had not been running,
// so their history proves nothing. require() reports the line and the
// expression, because a bare abort() leaves a failing test with nothing to
// say.
void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr std::uint32_t kTestFireActionTemplateId = 100;
constexpr std::uint32_t kTestReloadActionTemplateId = 101;
constexpr std::uint32_t kAgentEntityTemplateId = 102;
constexpr float kFixedDelta = 1.0f / 30.0f;
constexpr float kAgentMoveSpeed = 2.5f;

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelColliderTemplateDefinition hit_collider_template() {
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
    collider.shape_params = KernelVec4{20.0f, 90.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Vision;
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    return collider;
}

KernelColliderTemplateDefinition projectile_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 3;
    collider.shape_type = KernelColliderShapeType_Sphere;
    collider.shape_params = KernelVec4{0.2f, 0.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    return collider;
}

KernelColliderTemplateDefinition movement_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 11;
    collider.shape_type = KernelColliderShapeType_Capsule;
    collider.center = KernelVec3{0.0f, 0.9f, 0.0f};
    collider.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Movement;
    collider.layer_mask =
        KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
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
    projectile.mechanics.speed = 35.0f;
    projectile.mechanics.lifetime_ticks = 90;
    projectile.mechanics.collider_template_id = 3;
    projectile.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    projectile.mechanics.max_hit_count = 1;
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

KernelActionTemplateDefinition reload_action_template() {
    KernelActionTemplateDefinition action{};
    action.struct_size = sizeof(action);
    action.action_template_id = kTestReloadActionTemplateId;
    action.trigger_mode = KernelActionTriggerMode_Press;
    action.commit_offset_ticks = 3;
    action.max_commit_count = 1;
    return action;
}

// The agent has to come from an entity template, because only a template
// carries the movement definition that turns a move input into displacement.
KernelEntityTemplateDefinition agent_entity_template() {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = kAgentEntityTemplateId;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_type = KernelActorType_Agent;
    entity_template.actor_template_id = 2u;
    // Velocity is not optional here: the movement solver only sees actors that
    // carry it, so without the flag the agent silently never moves.
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH | KERNEL_ENTITY_COMPONENT_HITBOX |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
    entity_template.collider_template_id = 1u;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 100;
    entity_template.combat.max_hp = 100;
    entity_template.combat.move_speed_meters_per_second = kAgentMoveSpeed;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
    entity_template.movement.controller_type =
        KernelMovementControllerType_Grounded;
    entity_template.movement.movement_collider_template_id = 11u;
    entity_template.movement.max_slope_degrees = 50.0f;
    entity_template.movement.ground_probe_distance = 0.25f;
    entity_template.movement.ground_snap_distance = 0.5f;
    entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
    entity_template.ai.controller_type = KernelAiControllerType_Chaser;
    entity_template.ai.tick_interval = 1u;
    entity_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    entity_template.vision.camp = KernelAgentCamp_EnemySide;
    entity_template.vision.vision_collider_template_id = 2u;
    entity_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    entity_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return entity_template;
}

KernelActorTemplateDefinition agent_actor_template() {
    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = 2u;
    actor_template.entity_type = KernelEntityType_Actor;
    actor_template.actor_type = KernelActorType_Agent;
    actor_template.collider_template_id = 1u;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    actor_template.vision.vision_collider_template_id = 2u;
    actor_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    actor_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return actor_template;
}

void load_catalog(KernelHandle* kernel) {
    const std::array<KernelColliderTemplateDefinition, 4> colliders = {
        hit_collider_template(),
        vision_collider_template(),
        projectile_collider_template(),
        movement_collider_template(),
    };
    const KernelProjectileTemplateDefinition projectile = projectile_template();
    const std::array<KernelActionTemplateDefinition, 2> actions = {
        fire_action_template(),
        reload_action_template(),
    };
    const KernelEntityTemplateDefinition entity_template =
        agent_entity_template();
    const KernelActorTemplateDefinition actor_template = agent_actor_template();
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
    catalog.actor_templates = &actor_template;
    catalog.actor_template_count = 1;
    catalog.entity_templates = &entity_template;
    catalog.entity_template_count = 1;
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

std::uint32_t create_agent(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypeAgent;
    create_info.entity_template_id = kAgentEntityTemplateId;
    create_info.actor_template_id = 2u;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0);
    return net_id;
}

std::uint32_t create_player(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypePlayer;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0);
    return net_id;
}

void set_combat(KernelHandle* kernel, std::uint32_t net_id, std::uint16_t ammo) {
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_slot = 0;
    combat.weapon_slot_count = 1;
    // Combat state owns the movement speed; leaving it zero would silently
    // undo the speed the entity template spawned the agent with.
    combat.move_speed_meters_per_second = kAgentMoveSpeed;
    combat.weapon_ids[0] = network_example::game_server::kAgentSpammerWeaponId;
    combat.collider_template_id = 1;
    combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat.ammo[0] = ammo;
    combat.reserve_magazines[0] = 9;
    require(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
}

void set_weapon_mechanics(KernelHandle* kernel, std::uint32_t net_id) {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(weapon);
    weapon.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    weapon.fire_mode = KernelWeaponFireMode_Projectile;
    weapon.magazine_size = 30;
    weapon.damage = 1;
    weapon.fire_action_template_id = kTestFireActionTemplateId;
    weapon.reload_action_template_id = kTestReloadActionTemplateId;
    weapon.projectile_template_id = 3;
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, net_id, &weapon));
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
    require(Kernel_ServerSetEntityVisionConfig(kernel, net_id, &vision));
}

KernelServerEntityState query_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    require(state.valid != 0u);
    return state;
}

void set_position(
    KernelHandle* kernel,
    std::uint32_t net_id,
    const KernelVec3& position) {
    KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(kernel, net_id, &position, &identity));
}

void run_frame(
    KernelHandle* kernel,
    const network_example::game_server::AgentChaserController& controller,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    controller.tick(kernel, agents, kFixedDelta);
    Kernel_Update(kernel, kFixedDelta);
}

float horizontal_distance(const KernelVec3& lhs, const KernelVec3& rhs) {
    const float delta_x = lhs.x - rhs.x;
    const float delta_z = lhs.z - rhs.z;
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

bool almost_equal(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

network_example::game_server::AgentChaserConfig chaser_config() {
    network_example::game_server::AgentChaserConfig config;
    config.sentry.alert_ticks = 2;
    config.sentry.forget_ticks = 3;
    config.sentry.patrol_rotation_interval_ticks = 2;
    config.sentry.patrol_rotation_min_degrees = 15.0f;
    config.sentry.patrol_rotation_max_degrees = 30.0f;
    config.sentry.ballistic_retry_cooldown_ticks = 3;
    config.sentry.weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    config.sentry.move_speed_meters_per_second = kAgentMoveSpeed;
    config.chase.stop_distance_meters = 2.0f;
    config.chase.resume_distance_meters = 3.0f;
    config.chase.input_magnitude = 1.0f;
    return config;
}

// Patrol tuning layered on the same chase tuning the rest of the file uses, so
// a difference in behaviour below is the squad and not a retuned chase.
network_example::game_server::AgentChaserConfig patrol_config() {
    network_example::game_server::AgentChaserConfig config = chaser_config();
    config.patrol.slot_radius_meters = 1.0f;
    // Full speed: patrolling at half speed is the shipping default but it only
    // doubles how many ticks these cases have to run for.
    config.patrol.input_magnitude = 1.0f;
    config.patrol.leash_meters = 6.0f;
    config.patrol.leash_resume_meters = 2.0f;
    return config;
}

// The sub-tests share one agent and one kernel, so each has to put both back to
// a known state -- including the slot, which otherwise arrives already walked
// to. The agent goes to the origin with the identity rotation, which is where
// its vision cone looks down +X; cases that need a target seen on the first
// tick have to put it there.
void reset_patrol_agent(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents,
    const KernelVec3& slot) {
    set_position(kernel, agent_net_id, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {100.0f, 0.0f, 0.0f});
    set_combat(kernel, agent_net_id, 30);
    network_example::game_server::AgentRuntimeState& agent = (*agents)[0];
    agent.sentry.state = network_example::game_server::AgentSentryState::kIdle;
    agent.sentry.target = 0;
    agent.sentry.state_ticks = 0;
    agent.sentry.lost_target_ticks = 0;
    agent.chase_holding = false;
    agent.patrol = {};
    agent.patrol.group_id = 1;
    agent.patrol.slot = slot;
    agent.patrol.has_slot = true;
    Kernel_Update(kernel, kFixedDelta);
}

// The member walks to where its squad wants it and then stops. Stopping is the
// half worth stating: the slot is republished every tick, so an agent that
// treated "I have a slot" as "I should be moving" would jitter on it forever.
void a_squad_member_walks_to_the_slot_it_is_given(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    const network_example::game_server::AgentChaserController controller(
        patrol_config());
    reset_patrol_agent(
        kernel, agent_net_id, player_net_id, agents, {6.0f, 0.0f, 0.0f});

    for (int tick = 0; tick < 100; ++tick) {
        run_frame(kernel, controller, agents);
    }
    KernelVec3 position = query_state(kernel, agent_net_id).position;
    require(horizontal_distance(position, {6.0f, 0.0f, 0.0f}) <= 1.0f);
    require(almost_equal((*agents)[0].velocity.x, 0.0f));
    require(almost_equal((*agents)[0].velocity.z, 0.0f));

    // The squad moves on, and the member follows it there. A slot that only
    // worked as a spawn-time destination would pass everything above.
    (*agents)[0].patrol.slot = KernelVec3{6.0f, 0.0f, 6.0f};
    for (int tick = 0; tick < 120; ++tick) {
        run_frame(kernel, controller, agents);
    }
    position = query_state(kernel, agent_net_id).position;
    require(horizontal_distance(position, {6.0f, 0.0f, 6.0f}) <= 1.0f);
}

// Losing the target is what a chaser with no squad answers by standing still
// forever. A squad member walks back -- and back is wherever the squad has got
// to by then, not where it left.
void a_pursuit_that_ends_puts_the_agent_back_in_its_slot(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    const network_example::game_server::AgentChaserController controller(
        patrol_config());
    // Already standing in its slot, so what follows is about the pursuit and
    // not about walking into formation.
    reset_patrol_agent(
        kernel, agent_net_id, player_net_id, agents, {0.0f, 0.0f, 0.0f});
    for (int tick = 0; tick < 5; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kIdle);
    require(almost_equal((*agents)[0].velocity.x, 0.0f));

    // Something walks into the cone and pulls it out of formation, but not far
    // enough to hit the leash -- this case is about the pursuit ending.
    set_position(kernel, player_net_id, {12.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    for (int tick = 0; tick < 50; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(
        (*agents)[0].sentry.state !=
        network_example::game_server::AgentSentryState::kIdle);
    const float chased_to = query_state(kernel, agent_net_id).position.x;
    require(chased_to > 1.0f);

    // And it leaves.
    set_position(kernel, player_net_id, {100.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    bool returned = false;
    for (int tick = 0; tick < 10; ++tick) {
        run_frame(kernel, controller, agents);
        returned = returned ||
            (*agents)[0].sentry.state ==
                network_example::game_server::AgentSentryState::kReturn;
    }
    require(returned);
    require(query_state(kernel, agent_net_id).position.x < chased_to);

    // The squad did not wait where the member left it, and the member ends up
    // where the squad is now. This says nothing about which state carried it
    // there -- the return hands over to kIdle partway, and both walk to the
    // slot -- so the trajectory of a return is pinned separately below.
    (*agents)[0].patrol.slot = KernelVec3{0.0f, 0.0f, 5.0f};
    for (int tick = 0; tick < 250; ++tick) {
        run_frame(kernel, controller, agents);
    }
    const KernelVec3 rejoined = query_state(kernel, agent_net_id).position;
    require(horizontal_distance(rejoined, {0.0f, 0.0f, 5.0f}) <= 1.2f);
    require(horizontal_distance(rejoined, {0.0f, 0.0f, 0.0f}) > 3.0f);
}

// A return walks to where the squad is, not back to where the member left it.
// Those are the same point whenever the squad has not moved, which is why this
// case drags them apart first: the assertions in the pursuit case above are
// satisfied by the kIdle rejoin that follows a return, and would pass just as
// well if a return walked to entirely the wrong place.
void a_returning_member_walks_to_the_squad_not_to_where_it_left(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    const network_example::game_server::AgentChaserController controller(
        patrol_config());
    reset_patrol_agent(
        kernel, agent_net_id, player_net_id, agents, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {10.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, agents);
    require(almost_equal((*agents)[0].patrol.leash_anchor.x, 0.0f, 0.5f));

    // Dragged past the leash, so the agent is returning.
    set_position(kernel, agent_net_id, {20.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {25.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, agents);
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kReturn);

    // The squad has walked on, and it is further out than the agent -- so its
    // slot and the point it broke off from are now in opposite directions.
    (*agents)[0].patrol.slot = KernelVec3{40.0f, 0.0f, 0.0f};
    const float broke_off_at = query_state(kernel, agent_net_id).position.x;
    for (int tick = 0; tick < 30; ++tick) {
        run_frame(kernel, controller, agents);
        // Neither exit applies out here: the slot is 20 m ahead and the anchor
        // 20 m behind, so this stays a return the whole way.
        require(
            (*agents)[0].sentry.state ==
            network_example::game_server::AgentSentryState::kReturn);
    }
    require(query_state(kernel, agent_net_id).position.x > broke_off_at + 1.0f);
}

// The leash ends a pursuit the agent is still winning. Nothing else in the
// state machine can do that: every other way out of a chase runs through losing
// sight of the target.
void a_leash_breaks_off_a_pursuit_that_drags_too_far(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    const network_example::game_server::AgentChaserController leashed(
        patrol_config());
    reset_patrol_agent(
        kernel, agent_net_id, player_net_id, agents, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {10.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, leashed, agents);
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAlert);
    // The leash is measured from where the chase started, so that is what has
    // to have been recorded.
    require(almost_equal((*agents)[0].patrol.leash_anchor.x, 0.0f, 0.5f));

    // Dragged well past the leash, with the target still in front of it.
    set_position(kernel, agent_net_id, {20.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {25.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, leashed, agents);
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kReturn);
    require((*agents)[0].sentry.target == 0);

    // The same drag with the leash switched off keeps the pursuit. Without this
    // half the assertion above would also pass for an agent that had merely
    // lost sight of what it was chasing.
    network_example::game_server::AgentChaserConfig unleashed = patrol_config();
    unleashed.patrol.leash_meters = 0.0f;
    const network_example::game_server::AgentChaserController free_chaser(
        unleashed);
    reset_patrol_agent(
        kernel, agent_net_id, player_net_id, agents, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {10.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, free_chaser, agents);
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAlert);
    set_position(kernel, agent_net_id, {20.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {25.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, free_chaser, agents);
    require(
        (*agents)[0].sentry.state !=
        network_example::game_server::AgentSentryState::kReturn);
    require((*agents)[0].sentry.target == player_net_id);
}

// Seeing a target and being able to hit one are different distances. The vision
// cone reaches much further than a melee weapon does, and the controller used
// to commit an attack the moment the sentry half said "visible" -- so a grunt
// carrying a claw swung, and spent ammo, at a target across the room.
//
// Ammo is the observable: the fire action costs one per commit, so a swing that
// happened shows up here.
//
// Takes main's agent vector rather than a fresh one. The kernel keeps only the
// highest input_seq it has seen for a net id, so an AgentRuntimeState that
// starts its sequence over at zero has every input it submits discarded. The
// state machine still advances, because that runs off perception, so the only
// symptom is that nothing the agent decides ever reaches the world.
void a_chaser_attacks_only_inside_its_attack_range(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    network_example::game_server::AgentChaserConfig chaser = chaser_config();
    chaser.chase.attack_range_meters = 2.5f;
    // Parked from the first tick, so the distance under test stays the one set
    // here rather than whatever the chase happens to close to.
    chaser.chase.stop_distance_meters = 20.0f;
    chaser.chase.resume_distance_meters = 21.0f;
    const network_example::game_server::AgentChaserController controller(chaser);

    set_position(kernel, agent_net_id, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {6.0f, 0.0f, 0.0f});
    set_combat(kernel, agent_net_id, 30);
    Kernel_Update(kernel, kFixedDelta);
    for (int tick = 0; tick < 8; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAttack);
    require((*agents)[0].sentry.target == player_net_id);

    // Well inside what it can see, well outside what it can reach.
    const std::uint16_t ammo_out_of_range =
        query_state(kernel, agent_net_id).ammo[0];
    require(ammo_out_of_range != 0);
    for (int tick = 0; tick < 5; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(query_state(kernel, agent_net_id).ammo[0] == ammo_out_of_range);
    // Still hunting it, just not swinging at it.
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAttack);
    require((*agents)[0].sentry.target == player_net_id);

    // The same agent, the same state, the target brought within reach.
    set_position(kernel, player_net_id, {1.5f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    for (int tick = 0; tick < 5; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(query_state(kernel, agent_net_id).ammo[0] < ammo_out_of_range);
}

// The other design the same controller has to serve: a chaser carrying a ranged
// weapon opens fire as soon as the target is inside the weapon's range and
// keeps closing while it shoots. Nothing about the gate is melee-specific --
// it is the same field with a bigger number, and zero turns it off entirely
// for a chaser that should fire at anything it can see.
void a_ranged_chaser_fires_while_it_is_still_closing(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    std::uint32_t player_net_id,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    network_example::game_server::AgentChaserConfig chaser = chaser_config();
    // A firing range rather than a reach, and the chase thresholds it used to
    // have. resume stays inside attack_range, which is what keeps the agent
    // from parking in a band where it neither chases nor shoots.
    chaser.chase.attack_range_meters = 8.0f;
    chaser.chase.stop_distance_meters = 2.0f;
    chaser.chase.resume_distance_meters = 3.0f;
    const network_example::game_server::AgentChaserController controller(chaser);

    set_position(kernel, agent_net_id, {0.0f, 0.0f, 0.0f});
    set_position(kernel, player_net_id, {6.0f, 0.0f, 0.0f});
    set_combat(kernel, agent_net_id, 30);
    Kernel_Update(kernel, kFixedDelta);
    for (int tick = 0; tick < 8; ++tick) {
        run_frame(kernel, controller, agents);
    }
    require(
        (*agents)[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAttack);

    const std::uint16_t ammo_before = query_state(kernel, agent_net_id).ammo[0];
    require(ammo_before != 0);
    for (int tick = 0; tick < 5; ++tick) {
        run_frame(kernel, controller, agents);
    }
    // It fired...
    require(query_state(kernel, agent_net_id).ammo[0] < ammo_before);
    // ...from outside the distance it would have stopped at, so the shooting
    // is not something that only starts once the agent has arrived.
    const KernelServerEntityState agent_state = query_state(kernel, agent_net_id);
    const KernelServerEntityState target_state =
        query_state(kernel, player_net_id);
    require(
        horizontal_distance(agent_state.position, target_state.position) >
        chaser.chase.stop_distance_meters);
    require(!(*agents)[0].chase_holding);
}

}  // namespace

int main() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7787));
    load_catalog(kernel);

    const std::uint32_t agent_net_id = create_agent(kernel, {0.0f, 0.0f, 0.0f});
    const std::uint32_t player_net_id =
        create_player(kernel, {100.0f, 0.0f, 0.0f});
    set_combat(kernel, agent_net_id, 30);
    set_weapon_mechanics(kernel, agent_net_id);
    set_vision(kernel, agent_net_id, KernelAgentCamp_EnemySide, 2);
    set_vision(kernel, player_net_id, KernelAgentCamp_PlayerSide, 0);
    Kernel_Update(kernel, kFixedDelta);

    network_example::game_server::AgentRuntimeState agent;
    agent.net_id = agent_net_id;
    agent.position = KernelVec3{0.0f, 0.0f, 0.0f};
    std::vector<network_example::game_server::AgentRuntimeState> agents{agent};

    const network_example::game_server::AgentChaserController controller(
        chaser_config());

    // Nothing in sight: idle, and no movement is asked for.
    run_frame(kernel, controller, &agents);
    require(
        agents[0].sentry.state ==
        network_example::game_server::AgentSentryState::kIdle);
    require(almost_equal(agents[0].velocity.x, 0.0f));
    require(almost_equal(agents[0].velocity.z, 0.0f));
    require(!agents[0].chase_holding);

    // A visible target puts the agent in alert and starts it closing. The
    // player sits well outside the stop distance.
    set_position(kernel, player_net_id, {10.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, &agents);
    require(
        agents[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAlert);
    require(agents[0].sentry.target == player_net_id);
    require(agents[0].velocity.x > 0.0f);
    require(!agents[0].chase_holding);

    // The chase actually displaces the agent toward the target, tick over tick.
    KernelServerEntityState agent_state = query_state(kernel, agent_net_id);
    KernelServerEntityState player_state = query_state(kernel, player_net_id);
    float distance = horizontal_distance(agent_state.position, player_state.position);
    for (int tick = 0; tick < 20; ++tick) {
        run_frame(kernel, controller, &agents);
        agent_state = query_state(kernel, agent_net_id);
        const float next_distance =
            horizontal_distance(agent_state.position, player_state.position);
        require(next_distance < distance);
        distance = next_distance;
    }
    require(
        agents[0].sentry.state ==
        network_example::game_server::AgentSentryState::kAttack);
    require(agent_state.position.x > 0.5f);

    // Closing inside the stop distance parks the agent, but it keeps shooting.
    set_position(kernel, agent_net_id, {9.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, &agents);
    require(agents[0].chase_holding);
    require(almost_equal(agents[0].velocity.x, 0.0f));
    require(almost_equal(agents[0].velocity.z, 0.0f));
    agent_state = query_state(kernel, agent_net_id);
    const std::uint16_t ammo_while_holding = agent_state.ammo[0];
    run_frame(kernel, controller, &agents);
    agent_state = query_state(kernel, agent_net_id);
    require(agent_state.ammo[0] < ammo_while_holding);

    // Holding survives drifting apart by less than the resume distance, and
    // ends once the gap reopens past it.
    set_position(kernel, agent_net_id, {7.5f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, &agents);
    require(agents[0].chase_holding);
    require(almost_equal(agents[0].velocity.x, 0.0f));
    set_position(kernel, agent_net_id, {6.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, &agents);
    require(!agents[0].chase_holding);
    require(agents[0].velocity.x > 0.0f);

    // Losing sight stops the pursuit on the spot: no movement is commanded, and
    // the agent does not walk toward where the target was.
    set_position(kernel, player_net_id, {100.0f, 0.0f, 0.0f});
    Kernel_Update(kernel, kFixedDelta);
    run_frame(kernel, controller, &agents);
    require(almost_equal(agents[0].velocity.x, 0.0f));
    require(almost_equal(agents[0].velocity.z, 0.0f));
    require(!agents[0].chase_holding);
    agent_state = query_state(kernel, agent_net_id);
    const KernelVec3 stopped_position = agent_state.position;
    for (int tick = 0; tick < 5; ++tick) {
        run_frame(kernel, controller, &agents);
        agent_state = query_state(kernel, agent_net_id);
        require(almost_equal(agent_state.position.x, stopped_position.x, 0.01f));
        require(almost_equal(agent_state.position.z, stopped_position.z, 0.01f));
        require(almost_equal(agents[0].velocity.x, 0.0f));
        require(almost_equal(agents[0].velocity.z, 0.0f));
    }

    // And the sentry forget timers still run the state machine back down.
    require(
        agents[0].sentry.state ==
        network_example::game_server::AgentSentryState::kIdle);

    a_chaser_attacks_only_inside_its_attack_range(
        kernel, agent_net_id, player_net_id, &agents);
    a_ranged_chaser_fires_while_it_is_still_closing(
        kernel, agent_net_id, player_net_id, &agents);
    a_squad_member_walks_to_the_slot_it_is_given(
        kernel, agent_net_id, player_net_id, &agents);
    a_pursuit_that_ends_puts_the_agent_back_in_its_slot(
        kernel, agent_net_id, player_net_id, &agents);
    a_returning_member_walks_to_the_squad_not_to_where_it_left(
        kernel, agent_net_id, player_net_id, &agents);
    a_leash_breaks_off_a_pursuit_that_drags_too_far(
        kernel, agent_net_id, player_net_id, &agents);

    Kernel_Destroy(kernel);
    return 0;
}
