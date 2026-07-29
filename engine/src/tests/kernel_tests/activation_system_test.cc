#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at line %d: %s\n",
            line,
            expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

KernelServerEntityCreateInfo create_info(
    std::uint32_t entity_template_id,
    KernelVec3 position) {
    KernelServerEntityCreateInfo info{};
    info.struct_size = sizeof(info);
    info.entity_template_id = entity_template_id;
    info.position = position;
    info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    return info;
}

KernelColliderTemplateDefinition box_collider(
    std::uint32_t id,
    std::uint32_t layer_mask) {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = id;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.5f, 0.0f};
    collider.shape_params = KernelVec4{0.5f, 0.5f, 0.5f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = layer_mask;
    return collider;
}

void activated_prop_applies_damage_exactly_once() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelEntityTemplateDefinition prop_template{};
    prop_template.struct_size = sizeof(prop_template);
    prop_template.entity_template_id = 200;
    prop_template.entity_type = KernelEntityType_Prop;
    prop_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HEALTH;
    prop_template.combat.hp = 100;
    prop_template.combat.max_hp = 100;
    prop_template.ai.struct_size = sizeof(prop_template.ai);
    prop_template.movement.struct_size = sizeof(prop_template.movement);
    prop_template.activated_trigger.struct_size =
        sizeof(prop_template.activated_trigger);
    prop_template.activated_trigger.action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.activated_trigger.target_source =
        KernelEntityRefSource_EventTarget;
    prop_template.activated_trigger.damage_amount = 25;
    prop_template.activated_trigger.action_count = 2;
    prop_template.activated_trigger.actions[0].action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.activated_trigger.actions[0].target_source =
        KernelEntityRefSource_Self;
    prop_template.activated_trigger.actions[0].damage_amount = 1;
    prop_template.activated_trigger.actions[1].action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.activated_trigger.actions[1].target_source =
        KernelEntityRefSource_EventTarget;
    prop_template.activated_trigger.actions[1].damage_amount = 25;
    engine.entity_templates_.push_back(prop_template);

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t instigator = 0;
    require(engine.server_create_entity(actor_info, &instigator));

    std::uint32_t prop = 0;
    require(engine.server_create_entity(
        create_info(200, KernelVec3{1.0f, 0.0f, 0.0f}), &prop));
    const auto prop_entity = engine.world_.find_entity(prop);
    require(prop_entity.has_value());
    require((engine.world_.registry().all_of<
        network_example::OnActivatedTriggerTag,
        network_example::ActionGraphActivatedBinding>(*prop_entity)));

    const network_example::NetId target =
        engine.world_.spawn_enemy(glm::vec3{2.0f, 0.0f, 0.0f});
    const auto target_entity = engine.world_.find_entity(target);
    require(target_entity.has_value());
    engine.world_.registry().replace<network_example::Health>(
        *target_entity,
        network_example::Health{100, 100});
    const std::uint16_t hp_before =
        engine.world_.registry().get<network_example::Health>(*target_entity).hp;

    KernelServerEntityActivateInfo activation{};
    activation.struct_size = sizeof(activation);
    activation.subject_net_id = prop;
    activation.instigator_net_id = instigator;
    activation.target_net_id = target;
    activation.action_instance_id = 9;
    activation.request_id = 1234;
    require(engine.server_activate_entity(activation));
    require(engine.damage_pipeline_.pending_count() == 2);
    require(engine.world_.action_graph_batch_processed(
        activation.request_id,
        network_example::TriggerEventType::kActivated,
        0));
    require(!engine.world_.action_graph_batch_processed(
        activation.request_id,
        network_example::TriggerEventType::kCollision,
        0));
    require(!engine.world_.action_graph_batch_processed(
        activation.request_id,
        network_example::TriggerEventType::kActivated,
        1));
    require(engine.server_activate_entity(activation));
    require(engine.damage_pipeline_.pending_count() == 2);

    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    require(ready.size() == 2);
    require(ready[0].source_net_id == prop);
    require(ready[0].target_net_id == prop);
    require(ready[0].damage == 1);
    require(ready[1].source_net_id == prop);
    require(ready[1].target_net_id == target);
    require(ready[1].damage == 25);
    require(ready[1].source_peer == 0);
    network_example::apply_damage_applications(
        engine.world_, ready, 1, nullptr);
    require(engine.world_.registry()
               .get<network_example::Health>(*target_entity)
               .hp == hp_before - 25);
    require(engine.world_.registry()
               .get<network_example::Health>(*prop_entity)
               .hp == 99);

    actor_info.owner_peer = 8;
    std::uint32_t second_instigator = 0;
    require(engine.server_create_entity(actor_info, &second_instigator));
    activation.instigator_net_id = second_instigator;
    activation.action_instance_id = 10;
    require(engine.server_activate_entity(activation));
    require(engine.damage_pipeline_.pending_count() == 2);
    require(engine.world_.action_graph_batch_processed(
        8,
        activation.request_id,
        network_example::TriggerEventType::kActivated,
        0));
}

void collision_prop_applies_damage_on_contact_enter() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    if (engine.physics_world_ == nullptr) {
        engine.physics_world_ =
            std::make_unique<network_example::physics::PhysicsWorld>();
        engine.world_.set_collision_world(engine.physics_world_.get());
    }
    engine.collider_templates_.push_back(
        box_collider(10, KERNEL_COLLISION_LAYER_NEUTRAL));
    engine.collider_templates_.push_back(
        box_collider(20, KERNEL_COLLISION_LAYER_PLAYER_SIDE));

    KernelEntityTemplateDefinition prop_template{};
    prop_template.struct_size = sizeof(prop_template);
    prop_template.entity_template_id = 201;
    prop_template.entity_type = KernelEntityType_Prop;
    prop_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    prop_template.collider_template_id = 10;
    prop_template.ai.struct_size = sizeof(prop_template.ai);
    prop_template.movement.struct_size = sizeof(prop_template.movement);
    prop_template.collision_trigger.struct_size =
        sizeof(prop_template.collision_trigger);
    prop_template.collision_trigger_mask = KERNEL_COLLISION_MASK_ACTOR;
    prop_template.collision_trigger.action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.collision_trigger.target_source =
        KernelEntityRefSource_EventTarget;
    prop_template.collision_trigger.damage_amount = 25;
    engine.entity_templates_.push_back(prop_template);

    std::uint32_t prop = 0;
    require(engine.server_create_entity(
        create_info(201, KernelVec3{0.0f, 0.0f, 0.0f}), &prop));
    const auto prop_entity = engine.world_.find_entity(prop);
    require(prop_entity.has_value());
    engine.world_.registry().replace<network_example::PropWorldMode>(
        *prop_entity,
        network_example::PropWorldMode{network_example::PropMode::kInFlight});
    require((engine.world_.registry().all_of<
        network_example::OnCollisionTriggerTag,
        network_example::ActionGraphCollisionBinding>(*prop_entity)));

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t target = 0;
    require(engine.server_create_entity(actor_info, &target));
    const auto target_entity = engine.world_.find_entity(target);
    require(target_entity.has_value());
    engine.world_.registry().replace<network_example::Health>(
        *target_entity,
        network_example::Health{100, 100});
    engine.world_.registry().replace<network_example::Hitbox>(
        *target_entity,
        network_example::Hitbox{
            {0.0f, 0.5f, 0.0f},
            {0.5f, 0.5f, 0.5f},
            20});
    engine.materialize_entity_collider(target);
    engine.sync_entity_colliders_from_world();

    network_example::CollisionTriggerSystem{}.update(engine, 1000);
    require(engine.damage_pipeline_.pending_count() == 1);
    network_example::CollisionTriggerSystem{}.update(engine, 2000);
    require(engine.damage_pipeline_.pending_count() == 1);

    require(engine.server_set_entity_transform(
        target,
        KernelVec3{10.0f, 0.0f, 0.0f},
        KernelQuat{0.0f, 0.0f, 0.0f, 1.0f}));
    engine.sync_entity_colliders_from_world();
    network_example::CollisionTriggerSystem{}.update(engine, 3000);
    require(engine.server_set_entity_transform(
        target,
        KernelVec3{0.0f, 0.0f, 0.0f},
        KernelQuat{0.0f, 0.0f, 0.0f, 1.0f}));
    engine.sync_entity_colliders_from_world();
    network_example::CollisionTriggerSystem{}.update(engine, 4000);
    require(engine.damage_pipeline_.pending_count() == 2);

    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    require(ready.size() == 2);
    require(ready[0].source_net_id == prop);
    require(ready[0].target_net_id == target);
    network_example::apply_damage_applications(
        engine.world_, ready, 1, nullptr);
    require(engine.world_.registry()
               .get<network_example::Health>(*target_entity)
               .hp == 50);
}

void lifecycle_triggers_capture_context_before_prop_destruction() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelEntityTemplateDefinition prop_template{};
    prop_template.struct_size = sizeof(prop_template);
    prop_template.entity_template_id = 202;
    prop_template.entity_type = KernelEntityType_Prop;
    prop_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HEALTH;
    prop_template.combat.hp = 10;
    prop_template.combat.max_hp = 10;
    prop_template.ai.struct_size = sizeof(prop_template.ai);
    prop_template.movement.struct_size = sizeof(prop_template.movement);
    prop_template.health_depleted_trigger.struct_size =
        sizeof(prop_template.health_depleted_trigger);
    prop_template.health_depleted_trigger.action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.health_depleted_trigger.target_source =
        KernelEntityRefSource_EventInstigator;
    prop_template.health_depleted_trigger.damage_amount = 7;
    prop_template.destroy_entity_trigger.struct_size =
        sizeof(prop_template.destroy_entity_trigger);
    prop_template.destroy_entity_trigger.action_type =
        KernelEntityTriggerActionType_SpawnEntity;
    prop_template.destroy_entity_trigger.spawn_entity_template_id = 203;
    prop_template.destroy_entity_trigger.position_source =
        KernelEventVec3Source_Position;
    prop_template.destroy_entity_trigger.owner_source =
        KernelEntityRefSource_EventInstigator;
    engine.entity_templates_.push_back(prop_template);

    KernelEntityTemplateDefinition spawned_template{};
    spawned_template.struct_size = sizeof(spawned_template);
    spawned_template.entity_template_id = 203;
    spawned_template.entity_type = KernelEntityType_Prop;
    spawned_template.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
    spawned_template.ai.struct_size = sizeof(spawned_template.ai);
    spawned_template.movement.struct_size = sizeof(spawned_template.movement);
    engine.entity_templates_.push_back(spawned_template);

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t instigator = 0;
    require(engine.server_create_entity(actor_info, &instigator));
    const auto instigator_entity = engine.world_.find_entity(instigator);
    require(instigator_entity.has_value());
    engine.world_.registry().replace<network_example::Health>(
        *instigator_entity,
        network_example::Health{100, 100});

    std::uint32_t prop = 0;
    require(engine.server_create_entity(
        create_info(202, KernelVec3{1.0f, 0.0f, 0.0f}), &prop));
    const auto prop_entity = engine.world_.find_entity(prop);
    require(prop_entity.has_value());
    require((engine.world_.registry().all_of<
        network_example::OnHealthDepletedTriggerTag,
        network_example::ActionGraphHealthDepletedBinding,
        network_example::OnDestroyEntityTriggerTag,
        network_example::ActionGraphDestroyEntityBinding>(*prop_entity)));

    network_example::ConfirmedDamage lethal_damage{};
    lethal_damage.server_tick = 1;
    lethal_damage.sequence_id = 4;
    lethal_damage.source_net_id = instigator;
    lethal_damage.target_net_id = prop;
    lethal_damage.damage = 10;
    lethal_damage.hit_time_us = 1000;
    lethal_damage.hit_position = glm::vec3{1.0f, 0.5f, 0.0f};
    const std::vector<network_example::ConfirmedDamage> health_depleted =
        network_example::apply_damage_applications(
            engine.world_, {lethal_damage}, 1, nullptr);
    require(health_depleted.size() == 1);

    network_example::EntityLifecycleSystem lifecycle;
    lifecycle.process_health_depleted(engine, health_depleted, 1000);
    require(engine.damage_pipeline_.pending_count() == 1);
    lifecycle.destroy_dead_entities(engine, health_depleted);
    require(!engine.world_.find_entity(prop).has_value());
    require(engine.damage_pipeline_.pending_count() == 1);

    network_example::NetId spawned = 0;
    auto spawned_view = engine.world_.registry().view<
        const network_example::NetworkIdentity,
        const network_example::EntityKind,
        const network_example::Transform>();
    for (const entt::entity entity : spawned_view) {
        const auto& identity =
            spawned_view.get<const network_example::NetworkIdentity>(entity);
        const auto& kind =
            spawned_view.get<const network_example::EntityKind>(entity);
        if (kind.type == network_example::EntityType::kProp) {
            spawned = identity.net_id;
            require(identity.owner_peer == 7);
            const glm::vec3 position =
                spawned_view.get<const network_example::Transform>(entity)
                    .position;
            require(position == lethal_damage.hit_position);
        }
    }
    require(spawned != 0u);

    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    require(ready.size() == 1);
    require(ready.front().source_net_id == prop);
    require(ready.front().target_net_id == instigator);
    require(ready.front().damage == 7);
}

void health_change_applies_signed_clamped_delta() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelEntityTemplateDefinition healer{};
    healer.struct_size = sizeof(healer);
    healer.entity_template_id = 204;
    healer.entity_type = KernelEntityType_Prop;
    healer.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
    healer.ai.struct_size = sizeof(healer.ai);
    healer.movement.struct_size = sizeof(healer.movement);
    healer.activated_trigger.struct_size = sizeof(healer.activated_trigger);
    healer.activated_trigger.action_type =
        KernelEntityTriggerActionType_ApplyHealthChange;
    healer.activated_trigger.target_source = KernelEntityRefSource_EventTarget;
    healer.activated_trigger.health_change_amount = 30;
    engine.entity_templates_.push_back(healer);

    KernelEntityTemplateDefinition drain = healer;
    drain.entity_template_id = 205;
    drain.activated_trigger.health_change_amount = -30;
    engine.entity_templates_.push_back(drain);

    std::uint32_t healer_id = 0;
    std::uint32_t drain_id = 0;
    require(engine.server_create_entity(
        create_info(204, KernelVec3{0.0f, 0.0f, 0.0f}), &healer_id));
    require(engine.server_create_entity(
        create_info(205, KernelVec3{0.0f, 0.0f, 0.0f}), &drain_id));
    const network_example::NetId target =
        engine.world_.spawn_enemy(glm::vec3{1.0f, 0.0f, 0.0f});
    const auto target_entity = engine.world_.find_entity(target);
    require(target_entity.has_value());

    KernelServerEntityActivateInfo activation{};
    activation.struct_size = sizeof(activation);
    activation.subject_net_id = healer_id;
    activation.instigator_net_id = target;
    activation.target_net_id = target;
    activation.action_instance_id = 1;
    activation.request_id = 2001;

    engine.world_.registry().replace<network_example::Health>(
        *target_entity, network_example::Health{70, 100});
    engine.events_.clear();
    require(engine.server_activate_entity(activation));
    require(engine.world_.registry().get<network_example::Health>(*target_entity).hp ==
        100);
    require(engine.events_.size() == 1u);
    require(engine.events_.front().type == KernelEventType_HealthChanged);
    require(engine.events_.front().health_delta == 30);

    engine.world_.registry().replace<network_example::Health>(
        *target_entity, network_example::Health{90, 100});
    engine.events_.clear();
    activation.action_instance_id = 2;
    activation.request_id = 2002;
    require(engine.server_activate_entity(activation));
    require(engine.world_.registry().get<network_example::Health>(*target_entity).hp ==
        100);
    require(engine.events_.size() == 1u);
    require(engine.events_.front().health_delta == 10);

    engine.events_.clear();
    activation.action_instance_id = 3;
    activation.request_id = 2003;
    require(engine.server_activate_entity(activation));
    require(engine.events_.empty());

    engine.world_.registry().replace<network_example::Health>(
        *target_entity, network_example::Health{0, 100});
    activation.action_instance_id = 4;
    activation.request_id = 2004;
    require(engine.server_activate_entity(activation));
    require(engine.world_.registry().get<network_example::Health>(*target_entity).hp ==
        0);
    require(engine.events_.empty());

    engine.world_.registry().replace<network_example::Health>(
        *target_entity, network_example::Health{20, 100});
    activation.subject_net_id = drain_id;
    activation.action_instance_id = 5;
    activation.request_id = 2005;
    require(engine.server_activate_entity(activation));
    require(engine.damage_pipeline_.pending_count() == 1u);
    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    require(ready.size() == 1u);
    require(ready.front().damage == 30u);
    const auto depleted = network_example::apply_damage_applications(
        engine.world_, ready, 1, &engine.events_);
    require(depleted.size() == 1u);
    require(engine.world_.registry().get<network_example::Health>(*target_entity).hp ==
        0);
    require(engine.events_.size() == 3u);
    require(engine.events_[0].type == KernelEventType_HitConfirmed);
    require(engine.events_[1].type == KernelEventType_DamageApplied);
    require(engine.events_[2].type == KernelEventType_HealthChanged);
    require(engine.events_[2].health_delta == -20);
}

void static_collision_runs_once_for(
    network_example::physics::CollisionObjectKind impact_kind,
    network_example::physics::CollisionLayer impact_layer) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    if (engine.physics_world_ == nullptr) {
        engine.physics_world_ =
            std::make_unique<network_example::physics::PhysicsWorld>();
        engine.world_.set_collision_world(engine.physics_world_.get());
    }
    engine.collider_templates_.push_back(
        box_collider(30, KERNEL_COLLISION_LAYER_NEUTRAL));

    KernelEntityTemplateDefinition bottle{};
    bottle.struct_size = sizeof(bottle);
    bottle.entity_template_id = 206;
    bottle.entity_type = KernelEntityType_Prop;
    bottle.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    bottle.collider_template_id = 30;
    bottle.combat.hp = 1;
    bottle.combat.max_hp = 1;
    bottle.ai.struct_size = sizeof(bottle.ai);
    bottle.movement.struct_size = sizeof(bottle.movement);
    bottle.collision_trigger.struct_size =
        sizeof(bottle.collision_trigger);
    bottle.collision_trigger_mask =
        KERNEL_COLLISION_MASK_STATIC_WORLD;
    bottle.collision_trigger.action_count = 3;
    bottle.collision_trigger.actions[0].action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    bottle.collision_trigger.actions[0].target_source =
        KernelEntityRefSource_Self;
    bottle.collision_trigger.actions[0].damage_amount = 1;
    bottle.collision_trigger.actions[1].action_type =
        KernelEntityTriggerActionType_SpawnEntity;
    bottle.collision_trigger.actions[1].spawn_entity_template_id = 207;
    bottle.collision_trigger.actions[1].position_source =
        KernelEventVec3Source_Position;
    bottle.collision_trigger.actions[1].owner_source =
        KernelEntityRefSource_Self;
    bottle.collision_trigger.actions[2].action_type =
        KernelEntityTriggerActionType_ApplyHealthChange;
    bottle.collision_trigger.actions[2].target_source =
        KernelEntityRefSource_EventTarget;
    bottle.collision_trigger.actions[2].health_change_amount = 30;
    bottle.collision_trigger.actions[2].condition_type =
        KernelActionConditionType_EventHasTarget;
    engine.entity_templates_.push_back(bottle);

    KernelEntityTemplateDefinition ice{};
    ice.struct_size = sizeof(ice);
    ice.entity_template_id = 207;
    ice.entity_type = KernelEntityType_Prop;
    ice.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HEALTH;
    ice.combat.hp = 100;
    ice.combat.max_hp = 100;
    ice.ai.struct_size = sizeof(ice.ai);
    ice.movement.struct_size = sizeof(ice.movement);
    engine.entity_templates_.push_back(ice);

    std::uint32_t bottle_id = 0;
    require(engine.server_create_entity(
        create_info(206, KernelVec3{0.0f, 0.0f, 0.0f}), &bottle_id));
    const auto bottle_entity = engine.world_.find_entity(bottle_id);
    require(bottle_entity.has_value());
    engine.world_.registry().replace<network_example::PropWorldMode>(
        *bottle_entity,
        network_example::PropWorldMode{network_example::PropMode::kInFlight});
    engine.world_.registry().replace<network_example::Velocity>(
        *bottle_entity,
        network_example::Velocity{glm::vec3{1.0f, 0.0f, 0.0f}});
    engine.sync_entity_colliders_from_world();

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t actor = 0;
    require(engine.server_create_entity(actor_info, &actor));
    const auto actor_entity = engine.world_.find_entity(actor);
    require(actor_entity.has_value());
    engine.world_.registry().replace<network_example::Hitbox>(
        *actor_entity,
        network_example::Hitbox{
            {0.0f, 0.5f, 0.0f},
            {0.5f, 0.5f, 0.5f},
            31});
    engine.materialize_entity_collider(actor);
    engine.sync_entity_colliders_from_world();
    network_example::CollisionTriggerSystem{}.update(engine, 1000);
    require(engine.world_.registry()
                .get<network_example::PropWorldMode>(*bottle_entity)
                .mode == network_example::PropMode::kInFlight);
    require(engine.damage_pipeline_.pending_count() == 0u);

    network_example::physics::CollisionObjectDescriptor obstacle{};
    obstacle.identity = network_example::physics::CollisionObjectIdentity{
        0,
        900,
        0,
        impact_kind,
        impact_layer,
    };
    obstacle.shape.type = network_example::physics::CollisionShapeType::kBox;
    obstacle.shape.half_extents = glm::vec3{0.5f, 0.5f, 0.5f};
    obstacle.position = glm::vec3{0.0f, 0.5f, 0.0f};
    std::string error;
    require(engine.physics_world_->upsert_object(obstacle, &error));
    obstacle.identity.collider_id = 901;
    obstacle.position = glm::vec3{0.1f, 0.5f, 0.0f};
    require(engine.physics_world_->upsert_object(obstacle, &error));

    network_example::CollisionTriggerSystem{}.update(engine, 2000);
    require(engine.world_.registry()
                .get<network_example::PropWorldMode>(*bottle_entity)
                .mode == network_example::PropMode::kPlaced);
    require(engine.damage_pipeline_.pending_count() == 1u);
    std::uint32_t ice_count = 0;
    auto ice_view = engine.world_.registry().view<
        const network_example::EntityTemplateRef,
        const network_example::Health>();
    for (const entt::entity entity : ice_view) {
        if (ice_view.get<const network_example::EntityTemplateRef>(entity)
                .entity_template_id == 207u) {
            ++ice_count;
            require(ice_view.get<const network_example::Health>(entity).hp == 100u);
        }
    }
    require(ice_count == 1u);

    network_example::CollisionTriggerSystem{}.update(engine, 3000);
    require(engine.damage_pipeline_.pending_count() == 1u);
    ice_count = 0;
    for (const entt::entity entity : ice_view) {
        if (ice_view.get<const network_example::EntityTemplateRef>(entity)
                .entity_template_id == 207u) {
            ++ice_count;
        }
    }
    require(ice_count == 1u);

    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    const auto depleted = network_example::apply_damage_applications(
        engine.world_, ready, 1, nullptr);
    require(depleted.size() == 1u);
    network_example::EntityLifecycleSystem lifecycle;
    lifecycle.process_health_depleted(engine, depleted, 3000);
    lifecycle.destroy_dead_entities(engine, depleted);
    require(!engine.world_.find_entity(bottle_id).has_value());
}

void static_collision_accepts_terrain_and_static_obstacle() {
    static_collision_runs_once_for(
        network_example::physics::CollisionObjectKind::kTerrain,
        network_example::physics::CollisionLayer::kTerrain);
    static_collision_runs_once_for(
        network_example::physics::CollisionObjectKind::kStaticObstacle,
        network_example::physics::CollisionLayer::kStaticObstacle);
}

}  // namespace

int main() {
    activated_prop_applies_damage_exactly_once();
    collision_prop_applies_damage_on_contact_enter();
    lifecycle_triggers_capture_context_before_prop_destruction();
    health_change_applies_signed_clamped_delta();
    static_collision_accepts_terrain_and_static_obstacle();
    return 0;
}
