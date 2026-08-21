#include <cstdlib>
#include <cstdio>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"

namespace network_example {
// Defined in simulation/src/systems.cc; not surfaced in a public header.
bool execute_action_graph_command_batch(
    KernelEngine& engine,
    const ActionGraphCommandBatch& batch,
    std::uint64_t server_time_us);
}  // namespace network_example

namespace {

void require(
    bool condition,
    const std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

KernelItemTemplateDefinition item_template() {
    KernelItemTemplateDefinition item{};
    item.struct_size = sizeof(item);
    item.item_template_id = 10;
    item.item_mode = KernelItemMode_Fungible;
    item.max_stack = 10;
    item.capability_flags =
        KernelItemCapability_Pickupable |
        KernelItemCapability_Deployable |
        KernelItemCapability_Carryable |
        KernelItemCapability_Consumable |
        KernelItemCapability_Throwable;
    item.entity_template_id = 200;
    item.interaction_range = 3.0f;
    item.throw_policy.struct_size = sizeof(item.throw_policy);
    item.throw_policy.mode = KernelItemThrowMode_IdentityPreserving;
    item.throw_policy.trajectory_projectile_template_id = 7;
    item.use_policy.struct_size = sizeof(item.use_policy);
    item.use_policy.quantity_cost = 1;
    item.use_policy.cooldown_ticks = 0;
    item.item_used_trigger.struct_size = sizeof(item.item_used_trigger);
    return item;
}

KernelEntityTemplateDefinition prop_template() {
    KernelEntityTemplateDefinition prop{};
    prop.struct_size = sizeof(prop);
    prop.entity_template_id = 200;
    prop.entity_type = KernelEntityType_Prop;
    prop.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY;
    prop.ai.struct_size = sizeof(prop.ai);
    prop.movement.struct_size = sizeof(prop.movement);
    prop.prop.struct_size = sizeof(prop.prop);
    prop.prop.interaction.struct_size = sizeof(prop.prop.interaction);
    return prop;
}

void install_throw_trajectory(network_example::KernelEngine& engine) {
    network_example::RuntimeProjectileTemplate trajectory{};
    trajectory.projectile_template_id = 7;
    trajectory.projectile_type = network_example::ProjectileType::kStandard;
    trajectory.motion_model =
        network_example::ProjectileMotionModel::kParabolic;
    trajectory.speed = 24.0f;
    trajectory.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
    engine.world_.set_projectile_templates({trajectory});
}

KernelGameplayRequest request(
    std::uint64_t request_id,
    std::uint32_t actor,
    std::uint8_t domain_action) {
    KernelGameplayRequest value{};
    value.struct_size = sizeof(value);
    value.requester_peer = 7;
    value.request_id = request_id;
    value.instigator_net_id = actor;
    value.domain_action = domain_action;
    value.requested_quantity = 1;
    value.throw_direction = KernelVec3{1.0f, 0.0f, 0.0f};
    return value;
}

void semantic_requests_preserve_identity_and_dedupe() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    install_throw_trajectory(engine);
    engine.entity_templates_.push_back(prop_template());
    KernelEntityTemplateDefinition impact_entity = prop_template();
    impact_entity.entity_template_id = 201;
    engine.entity_templates_.push_back(impact_entity);
    engine.item_templates_.push_back(item_template());
    KernelItemTemplateDefinition deployable = item_template();
    deployable.item_template_id = 12;
    engine.item_templates_.push_back(deployable);
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));

    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 2, &container));
    KernelItemInstanceId inventory_item = 0;
    require(engine.server_create_inventory_item(10, 3, container, &inventory_item));

    KernelItemInstanceId deployable_item = 0;
    require(engine.server_create_inventory_item(
        12, 3, container, &deployable_item));
    KernelGameplayRequest place = request(
        99,
        actor,
        KernelDomainAction_Place);
    place.selected_item_instance_id = deployable_item;
    place.requested_quantity = 1;
    place.placement_position = KernelVec3{2.0f, 0.0f, 0.0f};
    require(engine.server_submit_gameplay_request(place));
    require(engine.item_store_.find_item(deployable_item)->quantity == 2);

    KernelGameplayRequest consume = request(
        100,
        actor,
        KernelDomainAction_Consume);
    consume.selected_item_instance_id = inventory_item;
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(inventory_item)->quantity == 2);
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(inventory_item)->quantity == 2);

    KernelGameplayRequest throw_item = request(
        101,
        actor,
        KernelDomainAction_Throw);
    throw_item.selected_item_instance_id = inventory_item;
    require(engine.server_submit_gameplay_request(throw_item));
    require(engine.item_store_.find_item(inventory_item)->quantity == 1);

    KernelGameplayRequestOutcome outcomes[4]{};
    require(engine.poll_gameplay_request_outcomes(outcomes, 4) == 4);
    require(outcomes[0].domain_action == KernelDomainAction_Place);
    require(outcomes[1].status == KernelGameplayRequestStatus_Committed);
    require(outcomes[1].graph_outcome == KernelGameplayGraphOutcome_Succeeded);
    require(outcomes[2].request_id == outcomes[1].request_id);
    require(outcomes[3].domain_action == KernelDomainAction_Throw);
    const KernelItemInstanceId thrown_item = outcomes[3].item_instance_id;
    const std::uint32_t thrown_prop = outcomes[3].prop_entity_id;
    require(thrown_item != inventory_item);
    require(engine.item_store_.find_item(thrown_item)->residency.world_mode ==
        KernelWorldItemMode_InFlight);
    const auto thrown_entity = engine.world_.find_entity(thrown_prop);
    require(thrown_entity.has_value());
    const network_example::ThrownPropMotion& motion =
        engine.world_.registry().get<network_example::ThrownPropMotion>(
            *thrown_entity);
    require(motion.motion_model ==
        network_example::ProjectileMotionModel::kParabolic);
    require(motion.spawn_position == glm::vec3(0.0f, 1.0f, 0.0f));
    require(motion.initial_velocity == glm::vec3(24.0f, 0.0f, 0.0f));
    require(motion.gravity == glm::vec3(0.0f, -9.81f, 0.0f));
    network_example::simulate_velocity_movement(engine.world_, 1.0f / 30.0f);
    const network_example::Transform& moved =
        engine.world_.registry().get<network_example::Transform>(*thrown_entity);
    require(moved.position.x > 0.79f && moved.position.x < 0.81f);
    require(moved.position.y > 0.99f && moved.position.y < 1.0f);

    if (engine.physics_world_ == nullptr) {
        engine.physics_world_ =
            std::make_unique<network_example::physics::PhysicsWorld>();
        engine.world_.set_collision_world(engine.physics_world_.get());
    }
    network_example::ColliderInstance thrown_collider{};
    thrown_collider.entity_net_id = thrown_prop;
    thrown_collider.entity_type = network_example::EntityType::kProp;
    thrown_collider.shape_type = network_example::ColliderShapeType::kAabb;
    thrown_collider.purpose_flags = KernelColliderPurpose_Hit;
    thrown_collider.layer_mask = KERNEL_COLLISION_LAYER_NEUTRAL;
    thrown_collider.half_extents = glm::vec3{0.25f};
    thrown_collider.world_center = moved.position;
    thrown_collider.enabled = true;
    engine.world_.collider_registry().upsert_entity_collider(
        thrown_prop, 900, thrown_collider);
    KernelActionTriggerDefinition collision_trigger{};
    collision_trigger.struct_size = sizeof(collision_trigger);
    collision_trigger.action_type =
        KernelEntityTriggerActionType_SpawnEntity;
    collision_trigger.spawn_entity_template_id = 201;
    collision_trigger.position_source = KernelEventVec3Source_Position;
    collision_trigger.owner_source = KernelEntityRefSource_Self;
    const auto collision_binding =
        network_example::compile_action_trigger_definition(
            network_example::TriggerEventType::kCollision,
            collision_trigger);
    require(collision_binding.has_value());
    engine.world_.registry().emplace_or_replace<
        network_example::OnCollisionTriggerTag>(*thrown_entity);
    engine.world_.registry().emplace_or_replace<
        network_example::ActionGraphCollisionBinding>(
        *thrown_entity,
        network_example::ActionGraphCollisionBinding{
            *collision_binding,
            KERNEL_COLLISION_MASK_STATIC_WORLD,
        });
    network_example::physics::CollisionObjectDescriptor obstacle{};
    obstacle.identity = network_example::physics::CollisionObjectIdentity{
        0u,
        901u,
        0u,
        network_example::physics::CollisionObjectKind::kStaticObstacle,
        network_example::physics::CollisionLayer::kStaticObstacle,
    };
    obstacle.shape.type =
        network_example::physics::CollisionShapeType::kBox;
    obstacle.shape.half_extents = glm::vec3{0.1f, 0.5f, 0.5f};
    obstacle.position = glm::vec3{2.0f, 1.0f, 0.0f};
    std::string physics_error;
    require(engine.physics_world_->upsert_object(obstacle, &physics_error));
    network_example::ThrownPropMotion& swept_motion =
        engine.world_.registry().get<network_example::ThrownPropMotion>(
            *thrown_entity);
    swept_motion.previous_position = moved.position;
    engine.world_.registry().get<network_example::Transform>(*thrown_entity)
        .position = glm::vec3{4.0f, moved.position.y, 0.0f};
    engine.sync_entity_colliders_from_world();
    network_example::CollisionTriggerSystem{}.update(engine, 1000);
    require(engine.world_.registry().get<network_example::PropWorldMode>(
        *thrown_entity).mode == network_example::PropMode::kPlaced);
    require(!engine.world_.registry().all_of<
        network_example::ThrownPropMotion>(*thrown_entity));
    const float settled_x =
        engine.world_.registry().get<network_example::Transform>(*thrown_entity)
            .position.x;
    require(settled_x > 1.0f && settled_x < 2.0f);
    bool found_impact_entity = false;
    const auto impact_view = engine.world_.registry().view<
        const network_example::EntityTemplateRef,
        const network_example::Transform>();
    for (const entt::entity entity : impact_view) {
        if (impact_view.get<const network_example::EntityTemplateRef>(entity)
                .entity_template_id != 201u) {
            continue;
        }
        found_impact_entity = true;
        require(
            impact_view.get<const network_example::Transform>(entity)
                .position.x > 1.0f);
    }
    require(found_impact_entity);

    require(engine.item_store_.set_world_mode(
        thrown_item,
        KernelWorldItemMode_Placed));
    const auto prop_entity = engine.world_.find_entity(thrown_prop);
    require(prop_entity.has_value());
    engine.world_.registry().replace<network_example::PropWorldMode>(
        *prop_entity,
        network_example::PropWorldMode{network_example::PropMode::kPlaced});
    KernelGameplayRequest pickup = request(
        102,
        actor,
        KernelDomainAction_Pickup);
    pickup.target_net_id = thrown_prop;
    require(engine.server_submit_gameplay_request(pickup));
    require(engine.item_store_.find_item(thrown_item)->terminal);
    require(engine.item_store_.find_item(inventory_item)->quantity == 2);

    KernelItemInstanceId world_item = 0;
    std::uint32_t world_prop = 0;
    require(engine.server_create_world_item(
        10,
        1,
        KernelVec3{1.0f, 0.0f, 0.0f},
        &world_item,
        &world_prop));
    KernelGameplayRequest carry = request(
        103,
        actor,
        KernelDomainAction_Carry);
    carry.target_net_id = world_prop;
    require(engine.server_submit_gameplay_request(carry));
    require(engine.item_store_.find_item(world_item)->residency.world_mode ==
        KernelWorldItemMode_Carrying);

    KernelGameplayRequest carried_throw = request(
        104,
        actor,
        KernelDomainAction_Throw);
    carried_throw.selected_item_instance_id = world_item;
    require(engine.server_submit_gameplay_request(carried_throw));
    require(engine.item_store_.find_item(world_item)->residency.world_mode ==
        KernelWorldItemMode_InFlight);
}

void direct_actions_reject_invalid_contexts_and_capabilities() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    install_throw_trajectory(engine);

    engine.entity_templates_.push_back(prop_template());
    KernelEntityTemplateDefinition pure_prop_template = prop_template();
    pure_prop_template.entity_template_id = 201;
    pure_prop_template.prop.interaction.capability_flags =
        KernelItemCapability_Carryable |
        KernelItemCapability_Throwable;
    pure_prop_template.prop.interaction.interaction_range = 3.0f;
    pure_prop_template.prop.throw_trajectory_projectile_template_id = 7;
    engine.entity_templates_.push_back(pure_prop_template);
    engine.item_templates_.push_back(item_template());
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));

    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 1, &container));
    KernelItemInstanceId inventory_item = 0;
    require(engine.server_create_inventory_item(
        10, 1, container, &inventory_item));

    KernelItemInstanceId world_item = 0;
    std::uint32_t world_prop = 0;
    require(engine.server_create_world_item(
        10,
        1,
        KernelVec3{1.0f, 0.0f, 0.0f},
        &world_item,
        &world_prop));

    KernelServerEntityCreateInfo prop_info{};
    prop_info.struct_size = sizeof(prop_info);
    prop_info.entity_type = KernelEntityType_Prop;
    prop_info.entity_template_id = 201;
    prop_info.position = KernelVec3{2.0f, 0.0f, 0.0f};
    prop_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t pure_prop = 0;
    require(engine.server_create_entity(prop_info, &pure_prop));

    KernelGameplayRequest unknown = request(600, actor, 255u);
    unknown.selected_item_instance_id = inventory_item;
    require(engine.server_submit_gameplay_request(unknown));

    KernelGameplayRequest inventory_pickup =
        request(601, actor, KernelDomainAction_Pickup);
    inventory_pickup.selected_item_instance_id = inventory_item;
    require(engine.server_submit_gameplay_request(inventory_pickup));

    KernelGameplayRequest placed_throw =
        request(602, actor, KernelDomainAction_Throw);
    placed_throw.target_net_id = world_prop;
    require(engine.server_submit_gameplay_request(placed_throw));

    KernelGameplayRequest pure_pickup =
        request(603, actor, KernelDomainAction_Pickup);
    pure_pickup.target_net_id = pure_prop;
    require(engine.server_submit_gameplay_request(pure_pickup));

    KernelGameplayRequest pure_activate =
        request(604, actor, KernelDomainAction_Activate);
    pure_activate.target_net_id = pure_prop;
    require(engine.server_submit_gameplay_request(pure_activate));

    KernelGameplayRequest pure_carry =
        request(605, actor, KernelDomainAction_Carry);
    pure_carry.target_net_id = pure_prop;
    require(engine.server_submit_gameplay_request(pure_carry));

    KernelGameplayRequest pure_place =
        request(606, actor, KernelDomainAction_Place);
    pure_place.target_net_id = pure_prop;
    pure_place.placement_position = KernelVec3{2.0f, 0.0f, 0.0f};
    require(engine.server_submit_gameplay_request(pure_place));

    pure_carry.request_id = 607;
    require(engine.server_submit_gameplay_request(pure_carry));

    KernelGameplayRequest pure_throw =
        request(608, actor, KernelDomainAction_Throw);
    pure_throw.target_net_id = pure_prop;
    require(engine.server_submit_gameplay_request(pure_throw));

    KernelGameplayRequestOutcome outcomes[9]{};
    require(engine.poll_gameplay_request_outcomes(outcomes, 9) == 9);
    require(outcomes[0].status == KernelGameplayRequestStatus_Rejected);
    require(outcomes[0].domain_action == KernelDomainAction_None);
    require(outcomes[0].rejection_reason ==
        KernelGameplayRequestRejection_InvalidRequest);
    for (std::uint32_t index = 1; index <= 3; ++index) {
        require(outcomes[index].status == KernelGameplayRequestStatus_Rejected);
        require(outcomes[index].rejection_reason ==
            KernelGameplayRequestRejection_InvalidContext);
    }
    require(outcomes[4].status == KernelGameplayRequestStatus_Rejected);
    require(outcomes[4].domain_action == KernelDomainAction_Activate);
    require(outcomes[4].rejection_reason ==
        KernelGameplayRequestRejection_MissingCapability);
    for (std::uint32_t index = 5; index < 9; ++index) {
        require(outcomes[index].status == KernelGameplayRequestStatus_Committed);
    }
    require(outcomes[5].domain_action == KernelDomainAction_Carry);
    require(outcomes[6].domain_action == KernelDomainAction_Place);
    require(outcomes[8].domain_action == KernelDomainAction_Throw);
}

void graph_failure_after_commit_does_not_refund_or_retry() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelItemTemplateDefinition consumable = item_template();
    consumable.item_template_id = 11;
    consumable.entity_template_id = 0;
    consumable.capability_flags = KernelItemCapability_Consumable;
    consumable.throw_policy.mode = KernelItemThrowMode_None;
    consumable.throw_policy.trajectory_projectile_template_id = 0;
    consumable.item_used_trigger.action_type =
        KernelEntityTriggerActionType_SpawnProjectile;
    consumable.item_used_trigger.spawn_projectile_template_id = 77;
    consumable.item_used_trigger.position_source =
        KernelEventVec3Source_Position;
    consumable.item_used_trigger.direction_source =
        KernelEventVec3Source_Direction;
    consumable.item_used_trigger.owner_source =
        KernelEntityRefSource_EventInstigator;
    engine.item_templates_.push_back(consumable);
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    network_example::RuntimeProjectileTemplate invalid_projectile{};
    invalid_projectile.projectile_template_id = 77;
    invalid_projectile.projectile_type =
        network_example::ProjectileType::kStandard;
    invalid_projectile.speed = 0.0f;
    engine.world_.set_projectile_templates({invalid_projectile});

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));
    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 1, &container));
    KernelItemInstanceId item = 0;
    require(engine.server_create_inventory_item(11, 2, container, &item));

    KernelGameplayRequest consume = request(
        200,
        actor,
        KernelDomainAction_Consume);
    consume.selected_item_instance_id = item;
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(item)->quantity == 1);
    KernelGameplayRequestOutcome outcomes[2]{};
    require(engine.poll_gameplay_request_outcomes(outcomes, 2) == 1);
    require(outcomes[0].status == KernelGameplayRequestStatus_Committed);
    require(outcomes[0].graph_outcome ==
        KernelGameplayGraphOutcome_FailedAfterCommit);

    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(item)->quantity == 1);
    require(engine.poll_gameplay_request_outcomes(outcomes, 2) == 1);
    require(outcomes[0].graph_outcome ==
        KernelGameplayGraphOutcome_FailedAfterCommit);
}

void consume_graph_spawns_new_item_backed_prop() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    engine.entity_templates_.push_back(prop_template());

    KernelItemTemplateDefinition output = item_template();
    output.item_template_id = 14;
    output.capability_flags = KernelItemCapability_Pickupable;
    output.throw_policy.mode = KernelItemThrowMode_None;
    output.throw_policy.trajectory_projectile_template_id = 0;
    output.use_policy.quantity_cost = 0u;
    output.item_used_trigger = KernelActionTriggerDefinition{};

    KernelItemTemplateDefinition source = item_template();
    source.item_template_id = 15;
    source.entity_template_id = 0u;
    source.capability_flags = KernelItemCapability_Consumable;
    source.throw_policy.mode = KernelItemThrowMode_None;
    source.throw_policy.trajectory_projectile_template_id = 0;
    source.item_used_trigger.struct_size = sizeof(source.item_used_trigger);
    source.item_used_trigger.action_count = 1u;
    KernelActionDefinition& spawn = source.item_used_trigger.actions[0];
    spawn.action_type = KernelEntityTriggerActionType_SpawnEntity;
    spawn.spawn_entity_template_id = 200u;
    spawn.position_source = KernelEventVec3Source_Position;
    spawn.owner_source = KernelEntityRefSource_EventInstigator;
    spawn.spawn_item_template_id = 14u;
    spawn.spawn_item_quantity = 2u;

    engine.item_templates_ = {source, output};
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));
    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 1, &container));
    KernelItemInstanceId source_item = 0;
    require(engine.server_create_inventory_item(15, 1, container, &source_item));

    KernelGameplayRequest consume = request(
        250, actor, KernelDomainAction_Consume);
    consume.selected_item_instance_id = source_item;
    consume.placement_position = KernelVec3{1.0f, 0.0f, 0.0f};
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(source_item)->terminal);

    KernelGameplayRequestOutcome outcome{};
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.graph_outcome == KernelGameplayGraphOutcome_Succeeded);
    KernelItemInstanceId spawned_item = 0;
    std::uint32_t spawned_prop = 0;
    for (const auto& [id, record] : engine.item_store_.items_) {
        if (id != source_item && !record.terminal) {
            spawned_item = id;
            spawned_prop = record.residency.prop_entity_id;
        }
    }
    require(spawned_item != 0u && spawned_item != source_item);
    require(spawned_prop != 0u);
    const auto prop = engine.world_.find_entity(spawned_prop);
    require(prop.has_value());
    require(engine.world_.registry().get<network_example::ItemInstanceRef>(*prop)
        .item_instance_id == spawned_item);
    require(engine.world_.registry().get<network_example::PropWorldMode>(*prop)
        .mode == network_example::PropMode::kPlaced);
}

void semantic_activate_validates_context_range_stale_and_dedupe() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelEntityTemplateDefinition prop = prop_template();
    prop.activated_trigger.struct_size = sizeof(prop.activated_trigger);
    engine.entity_templates_.push_back(prop);
    KernelItemTemplateDefinition activation_item = item_template();
    activation_item.item_template_id = 13;
    activation_item.capability_flags =
        KernelItemCapability_Pickupable | KernelItemCapability_Interactable;
    activation_item.throw_policy.mode = KernelItemThrowMode_None;
    activation_item.throw_policy.trajectory_projectile_template_id = 0;
    activation_item.use_policy.quantity_cost = 0;
    engine.item_templates_.push_back(activation_item);
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));

    KernelItemInstanceId item = 0;
    std::uint32_t prop_id = 0;
    require(engine.server_create_world_item(
        13,
        1,
        KernelVec3{1.0f, 0.0f, 0.0f},
        &item,
        &prop_id));

    KernelGameplayRequest invalid_action = request(
        300, actor, KernelDomainAction_None);
    invalid_action.target_net_id = prop_id;
    require(engine.server_submit_gameplay_request(invalid_action));

    KernelGameplayRequest activate = request(
        301, actor, KernelDomainAction_Activate);
    activate.target_net_id = prop_id;
    require(engine.server_submit_gameplay_request(activate));
    require(engine.server_submit_gameplay_request(activate));

    const auto prop_entity = engine.world_.find_entity(prop_id);
    require(prop_entity.has_value());
    engine.world_.registry().get<network_example::Transform>(*prop_entity).position =
        glm::vec3{10.0f, 0.0f, 0.0f};
    KernelGameplayRequest out_of_range = request(
        302, actor, KernelDomainAction_Activate);
    out_of_range.target_net_id = prop_id;
    require(engine.server_submit_gameplay_request(out_of_range));

    require(engine.server_destroy_entity(
        prop_id, KernelDespawnReason_Destroyed));
    KernelGameplayRequest stale = request(
        303, actor, KernelDomainAction_Activate);
    stale.target_net_id = prop_id;
    require(engine.server_submit_gameplay_request(stale));

    KernelGameplayRequestOutcome outcomes[5]{};
    require(engine.poll_gameplay_request_outcomes(outcomes, 5) == 5);
    require(outcomes[0].status == KernelGameplayRequestStatus_Rejected);
    require(outcomes[0].domain_action == KernelDomainAction_None);
    require(outcomes[0].rejection_reason ==
        KernelGameplayRequestRejection_InvalidRequest);
    require(outcomes[1].status == KernelGameplayRequestStatus_Committed);
    require(outcomes[1].domain_action == KernelDomainAction_Activate);
    require(outcomes[2].request_id == outcomes[1].request_id);
    require(outcomes[2].status == outcomes[1].status);
    require(outcomes[3].status == KernelGameplayRequestStatus_Rejected);
    require(outcomes[3].rejection_reason ==
        KernelGameplayRequestRejection_OutOfRange);
    require(outcomes[4].status == KernelGameplayRequestStatus_Rejected);
    require(outcomes[4].domain_action == KernelDomainAction_Activate);
    require(outcomes[4].rejection_reason ==
        KernelGameplayRequestRejection_UnknownTarget);
}

void stateful_health_round_trips_and_world_destroy_is_terminal() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    install_throw_trajectory(engine);

    KernelEntityTemplateDefinition stateful_prop = prop_template();
    stateful_prop.component_flags |= KERNEL_ENTITY_COMPONENT_HEALTH;
    stateful_prop.combat.hp = 3;
    stateful_prop.combat.max_hp = 3;
    engine.entity_templates_.push_back(stateful_prop);

    KernelItemTemplateDefinition stateful = item_template();
    stateful.item_template_id = 20;
    stateful.item_mode = KernelItemMode_Stateful;
    stateful.max_stack = 1;
    stateful.capability_flags =
        KernelItemCapability_Pickupable | KernelItemCapability_Throwable;
    stateful.use_policy.quantity_cost = 0;
    stateful.portable_state_field_count = 1;
    stateful.portable_state_fields[0].field_id = 7;
    stateful.portable_state_fields[0].type = KernelPortableStateType_Uint32;
    stateful.portable_state_fields[0].world_projection =
        KernelPortableStateProjection_HealthCurrent;
    stateful.portable_state_fields[0].uint32_default = 3;
    engine.item_templates_.push_back(stateful);
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelItemInstanceId server_item = 0;
    std::uint32_t server_prop = 0;
    require(engine.server_create_world_item(
        20,
        1,
        KernelVec3{2.0f, 0.0f, 0.0f},
        &server_item,
        &server_prop));
    const auto server_entity = engine.world_.find_entity(server_prop);
    require(server_entity.has_value());
    require(engine.world_.registry().get<network_example::Health>(*server_entity).hp ==
        3u);
    require(engine.server_destroy_entity(
        server_prop, KernelDespawnReason_Destroyed));
    require(engine.item_store_.find_item(server_item)->terminal);

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));
    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 1, &container));
    KernelItemInstanceId item = 0;
    require(engine.server_create_inventory_item(20, 1, container, &item));

    KernelGameplayRequest throw_item = request(
        400, actor, KernelDomainAction_Throw);
    throw_item.selected_item_instance_id = item;
    require(engine.server_submit_gameplay_request(throw_item));
    KernelGameplayRequestOutcome outcome{};
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.item_instance_id == item);
    const std::uint32_t first_prop = outcome.prop_entity_id;
    const auto first_entity = engine.world_.find_entity(first_prop);
    require(first_entity.has_value());
    require(engine.world_.registry().get<network_example::Health>(*first_entity).hp ==
        3u);

    require(engine.server_set_entity_health(first_prop, 2));
    require(std::find(
                engine.pending_prop_state_changes_.begin(),
                engine.pending_prop_state_changes_.end(),
                first_prop) != engine.pending_prop_state_changes_.end());
    require(engine.item_store_.set_world_mode(item, KernelWorldItemMode_Placed));
    engine.world_.registry().replace<network_example::PropWorldMode>(
        *first_entity,
        network_example::PropWorldMode{network_example::PropMode::kPlaced});
    KernelGameplayRequest pickup = request(
        401, actor, KernelDomainAction_Pickup);
    pickup.target_net_id = first_prop;
    require(engine.server_submit_gameplay_request(pickup));
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    const network_example::ItemInstanceRecord* stored =
        engine.item_store_.find_item(item);
    require(stored != nullptr);
    require(!stored->terminal);
    require(stored->residency.kind == KernelItemResidency_Inventory);
    require(stored->portable_state.size() == 1u);
    require(stored->portable_state[0].uint32_default == 2u);
    require(!engine.world_.find_entity(first_prop).has_value());

    throw_item.request_id = 402;
    require(engine.server_submit_gameplay_request(throw_item));
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.item_instance_id == item);
    const std::uint32_t second_prop = outcome.prop_entity_id;
    require(second_prop != first_prop);
    const auto second_entity = engine.world_.find_entity(second_prop);
    require(second_entity.has_value());
    require(engine.world_.registry().get<network_example::Health>(*second_entity).hp ==
        2u);

    require(engine.server_destroy_entity(
        second_prop, KernelDespawnReason_Destroyed));
    stored = engine.item_store_.find_item(item);
    require(stored != nullptr && stored->terminal);
}

void catalog_cross_validates_health_projection() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    network_example::KernelEngine engine(config);

    KernelEntityTemplateDefinition entity = prop_template();
    entity.component_flags |= KERNEL_ENTITY_COMPONENT_HEALTH;
    entity.combat.hp = 3;
    entity.combat.max_hp = 3;

    KernelItemTemplateDefinition item = item_template();
    item.item_template_id = 21;
    item.item_mode = KernelItemMode_Stateful;
    item.max_stack = 1;
    item.capability_flags = KernelItemCapability_Pickupable;
    item.throw_policy.mode = KernelItemThrowMode_None;
    item.throw_policy.trajectory_projectile_template_id = 0;
    item.use_policy.quantity_cost = 0;
    item.portable_state_field_count = 1;
    item.portable_state_fields[0].field_id = 7;
    item.portable_state_fields[0].type = KernelPortableStateType_Uint32;
    item.portable_state_fields[0].world_projection =
        KernelPortableStateProjection_HealthCurrent;
    item.portable_state_fields[0].uint32_default = 3;

    const auto load = [&](const KernelEntityTemplateDefinition& entity_definition,
                          const KernelItemTemplateDefinition& item_definition) {
        KernelGameplayCatalogDefinition catalog{};
        catalog.struct_size = sizeof(catalog);
        catalog.catalog_version = 1;
        catalog.catalog_hash = 1;
        catalog.entity_templates = &entity_definition;
        catalog.entity_template_count = 1;
        catalog.item_templates = &item_definition;
        catalog.item_template_count = 1;
        return engine.load_gameplay_catalog(catalog);
    };

    require(load(entity, item));

    KernelItemTemplateDefinition invalid_item = item;
    invalid_item.portable_state_fields[0].uint32_default = 4;
    require(!load(entity, invalid_item));

    KernelEntityTemplateDefinition invalid_entity = entity;
    invalid_entity.component_flags &= ~KERNEL_ENTITY_COMPONENT_HEALTH;
    require(!load(invalid_entity, item));

    invalid_item = item;
    invalid_item.item_mode = KernelItemMode_Fungible;
    require(!load(entity, invalid_item));
}

void health_change_no_op_still_consumes_item() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelItemTemplateDefinition potion = item_template();
    potion.item_template_id = 22;
    potion.entity_template_id = 0;
    potion.capability_flags = KernelItemCapability_Consumable;
    potion.throw_policy.mode = KernelItemThrowMode_None;
    potion.throw_policy.trajectory_projectile_template_id = 0;
    potion.item_used_trigger.action_type =
        KernelEntityTriggerActionType_ApplyHealthChange;
    potion.item_used_trigger.target_source =
        KernelEntityRefSource_EventInstigator;
    potion.item_used_trigger.health_change_amount = 30;
    engine.item_templates_.push_back(potion);
    std::string error;
    require(engine.item_store_.set_templates(engine.item_templates_, &error));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));
    const auto actor_entity = engine.world_.find_entity(actor);
    require(actor_entity.has_value());
    engine.world_.registry().replace<network_example::Health>(
        *actor_entity,
        network_example::Health{100, 100});
    KernelInventoryContainerId container = 0;
    require(engine.server_create_inventory_container(actor, 1, &container));
    KernelItemInstanceId item = 0;
    require(engine.server_create_inventory_item(22, 2, container, &item));

    KernelGameplayRequest consume = request(
        500, actor, KernelDomainAction_Consume);
    consume.selected_item_instance_id = item;
    engine.events_.clear();
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(item)->quantity == 1u);
    require(engine.events_.empty());
    KernelGameplayRequestOutcome outcome{};
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.graph_outcome == KernelGameplayGraphOutcome_Succeeded);

    engine.world_.registry().get<network_example::Health>(*actor_entity).hp = 0;
    consume.request_id = 501;
    require(engine.server_submit_gameplay_request(consume));
    require(engine.item_store_.find_item(item)->terminal);
    require(engine.events_.empty());
    require(engine.poll_gameplay_request_outcomes(&outcome, 1) == 1);
    require(outcome.graph_outcome == KernelGameplayGraphOutcome_Succeeded);
}

// CarriedBy and PropWorldMode{kCarrying} are emplaced together and dropped
// together, so a prop nobody is holding has no CarriedBy at all. The impulse
// validation pass admits a prop target only when its mode is NOT kCarrying --
// i.e. exactly the props missing the component -- so execution must not assume
// it is there. It used to erase<CarriedBy> unconditionally, which reads an
// empty sparse-set page: an assert in debug, a null deref in release. A rocket
// blast catching any placed prop took the dedicated server down.
void impulse_on_uncarried_prop_does_not_touch_absent_carrier() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    engine.entity_templates_.push_back(prop_template());

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));

    KernelServerEntityCreateInfo prop_info{};
    prop_info.struct_size = sizeof(prop_info);
    prop_info.entity_type = KernelEntityType_Prop;
    prop_info.owner_peer = 7;
    prop_info.position = KernelVec3{2.0f, 0.0f, 0.0f};
    prop_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    prop_info.entity_template_id = 200;
    std::uint32_t prop = 0;
    require(engine.server_create_entity(prop_info, &prop));

    const auto prop_entity = engine.world_.find_entity(prop);
    require(prop_entity.has_value());
    entt::registry& registry = engine.world_.registry();
    require(registry.get<network_example::PropWorldMode>(*prop_entity).mode ==
            network_example::PropMode::kPlaced);
    require(!registry.all_of<network_example::CarriedBy>(*prop_entity));

    network_example::ActionGraphCommandBatch batch{};
    batch.event.type = network_example::TriggerEventType::kProjectileImpact;
    batch.event.instigator = actor;
    batch.event.target = prop;
    batch.provenance.request_id = 4242;
    batch.provenance.owner_peer = 7;
    batch.provenance.instigator = actor;
    batch.sequence = 1;
    network_example::ActionApplyImpulseCommand impulse{};
    impulse.source = actor;
    impulse.target = prop;
    impulse.strength = 12.0f;
    impulse.direction = glm::vec3{1.0f, 0.0f, 0.0f};
    impulse.collision_mask = KERNEL_COLLISION_MASK_PROP;
    impulse.provenance = batch.provenance;
    batch.commands.emplace_back(impulse);

    require(network_example::execute_action_graph_command_batch(
        engine, batch, 0));

    require(registry.get<network_example::PropWorldMode>(*prop_entity).mode ==
            network_example::PropMode::kInFlight);
    require(registry.all_of<network_example::ThrownPropMotion>(*prop_entity));
    require(!registry.all_of<network_example::CarriedBy>(*prop_entity));
    require(registry.get<network_example::Velocity>(*prop_entity).linear.x >
            11.9f);
}

}  // namespace

int main() {
    semantic_requests_preserve_identity_and_dedupe();
    direct_actions_reject_invalid_contexts_and_capabilities();
    graph_failure_after_commit_does_not_refund_or_retry();
    consume_graph_spawns_new_item_backed_prop();
    semantic_activate_validates_context_range_stale_and_dedupe();
    stateful_health_round_trips_and_world_destroy_is_terminal();
    catalog_cross_validates_health_projection();
    health_change_no_op_still_consumes_item();
    impulse_on_uncarried_prop_does_not_touch_absent_carrier();
    return 0;
}
