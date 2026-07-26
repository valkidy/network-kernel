#include <algorithm>
#include <cassert>

#include "game_server/gameplay_config.h"
#include "kernel/src/kernel.h"

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const auto prop = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 200;
        });
    assert(prop != config.entity_templates.end());
    assert(prop->entity_type == KernelEntityType_Prop);
    assert(
        prop->activated_trigger.action_graph_ref ==
        "action_apply_damage_at_activated");
    assert(
        prop->destroy_entity_trigger.action_graph_ref ==
        "action_spawn_entity_at_destroy_entity");

    const network_example::game_server::KernelGameplayCatalogStorage catalog =
        network_example::game_server::build_kernel_gameplay_catalog(config);
    const auto compiled = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& value) {
            return value.entity_template_id == 200;
        });
    assert(compiled != catalog.entity_templates.end());
    assert(
        compiled->activated_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyDamage);
    assert(
        compiled->activated_trigger.target_source ==
        KernelEntityRefSource_EventTarget);
    assert(compiled->activated_trigger.damage_amount == 25);
    assert(
        compiled->destroy_entity_trigger.action_type ==
        KernelEntityTriggerActionType_SpawnEntity);
    assert(compiled->destroy_entity_trigger.spawn_entity_template_id == 201);
    assert(
        compiled->destroy_entity_trigger.position_source ==
        KernelEventVec3Source_Position);
    assert(
        compiled->destroy_entity_trigger.owner_source ==
        KernelEntityRefSource_EventInstigator);

    const auto collision_prop = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 201;
        });
    assert(collision_prop != config.entity_templates.end());
    assert(
        collision_prop->collision_trigger.action_graph_ref ==
        "action_apply_damage_at_collision");
    const auto compiled_collision = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& value) {
            return value.entity_template_id == 201;
        });
    assert(compiled_collision != catalog.entity_templates.end());
    assert(
        (compiled_collision->component_flags &
         KERNEL_ENTITY_COMPONENT_HEALTH) != 0u);
    assert(compiled_collision->combat.hp == 1);
    assert(compiled_collision->combat.max_hp == 1);
    assert(compiled_collision->collision_trigger.action_count == 2);
    assert(
        compiled_collision->collision_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyDamage);
    assert(
        compiled_collision->collision_trigger.target_source ==
        KernelEntityRefSource_EventTarget);
    assert(compiled_collision->collision_trigger.damage_amount == 1);
    assert(
        compiled_collision->collision_trigger.actions[0].target_source ==
        KernelEntityRefSource_Self);
    assert(compiled_collision->collision_trigger.actions[0].damage_amount == 1);
    assert(
        compiled_collision->collision_trigger.actions[1].target_source ==
        KernelEntityRefSource_EventTarget);
    assert(compiled_collision->collision_trigger.actions[1].damage_amount == 25);

    network_example::game_server::GameServerGameplayConfig invalid = config;
    auto invalid_prop = std::find_if(
        invalid.entity_templates.begin(),
        invalid.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 200;
        });
    assert(invalid_prop != invalid.entity_templates.end());
    invalid_prop->health_depleted_trigger.parameters[0].second = "event.target";
    bool invalid_event_expression_rejected = false;
    try {
        (void)network_example::game_server::build_kernel_gameplay_catalog(
            invalid);
    } catch (const std::runtime_error& error) {
        invalid_event_expression_rejected =
            std::string(error.what()).find("does not provide event.target") !=
            std::string::npos;
    }
    assert(invalid_event_expression_rejected);

    const auto rocket = std::find_if(
        catalog.projectile_templates.begin(),
        catalog.projectile_templates.end(),
        [](const KernelProjectileTemplateDefinition& value) {
            return value.projectile_template_id == 3;
        });
    assert(rocket != catalog.projectile_templates.end());
    assert(
        rocket->mechanics.projectile_impact_trigger.action_type ==
        KernelEntityTriggerActionType_SpawnProjectile);
    assert(
        rocket->mechanics.projectile_impact_trigger
            .spawn_projectile_template_id == 8);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    network_example::KernelEngine kernel(kernel_config);
    assert(kernel.load_gameplay_catalog(catalog.definition));
    return 0;
}
