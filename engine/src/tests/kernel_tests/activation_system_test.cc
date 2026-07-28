#include <algorithm>
#include <cstdint>
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

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

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
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_HITBOX;
    prop_template.collider_template_id = 10;
    prop_template.ai.struct_size = sizeof(prop_template.ai);
    prop_template.movement.struct_size = sizeof(prop_template.movement);
    prop_template.collision_trigger.struct_size =
        sizeof(prop_template.collision_trigger);
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

}  // namespace

int main() {
    activated_prop_applies_damage_exactly_once();
    collision_prop_applies_damage_on_contact_enter();
    lifecycle_triggers_capture_context_before_prop_destruction();
    return 0;
}
