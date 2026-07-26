#include <cassert>
#include <cstdint>
#include <limits>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

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
    prop_template.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM;
    prop_template.ai.struct_size = sizeof(prop_template.ai);
    prop_template.movement.struct_size = sizeof(prop_template.movement);
    prop_template.activated_trigger.struct_size =
        sizeof(prop_template.activated_trigger);
    prop_template.activated_trigger.action_type =
        KernelEntityTriggerActionType_ApplyDamage;
    prop_template.activated_trigger.target_source =
        KernelEntityRefSource_EventTarget;
    prop_template.activated_trigger.damage_amount = 25;
    engine.entity_templates_.push_back(prop_template);

    KernelServerEntityCreateInfo actor_info{};
    actor_info.struct_size = sizeof(actor_info);
    actor_info.entity_type = KernelEntityType_Actor;
    actor_info.actor_type = KernelActorType_Player;
    actor_info.owner_peer = 7;
    actor_info.position = KernelVec3{0.0f, 0.0f, 0.0f};
    actor_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t instigator = 0;
    assert(engine.server_create_entity(actor_info, &instigator));

    std::uint32_t prop = 0;
    assert(engine.server_create_entity(
        create_info(200, KernelVec3{1.0f, 0.0f, 0.0f}), &prop));
    const auto prop_entity = engine.world_.find_entity(prop);
    assert(prop_entity.has_value());
    assert((engine.world_.registry().all_of<
        network_example::OnActivatedTriggerTag,
        network_example::ActivatedActionGraphBinding>(*prop_entity)));

    const network_example::NetId target =
        engine.world_.spawn_enemy(glm::vec3{2.0f, 0.0f, 0.0f});
    const auto target_entity = engine.world_.find_entity(target);
    assert(target_entity.has_value());
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
    assert(engine.server_activate_entity(activation));
    assert(engine.damage_pipeline_.pending_count() == 1);
    assert(engine.server_activate_entity(activation));
    assert(engine.damage_pipeline_.pending_count() == 1);

    const auto ready = engine.damage_pipeline_.drain_ready_damage(
        engine.world_, std::numeric_limits<std::uint64_t>::max());
    assert(ready.size() == 1);
    network_example::apply_damage_applications(
        engine.world_, ready, 1, nullptr);
    assert(engine.world_.registry()
               .get<network_example::Health>(*target_entity)
               .hp == hp_before - 25);
}

}  // namespace

int main() {
    activated_prop_applies_damage_exactly_once();
    return 0;
}
