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
        compiled_collision->collision_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyDamage);
    assert(
        compiled_collision->collision_trigger.target_source ==
        KernelEntityRefSource_EventTarget);
    assert(compiled_collision->collision_trigger.damage_amount == 25);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    network_example::KernelEngine kernel(kernel_config);
    assert(kernel.load_gameplay_catalog(catalog.definition));
    return 0;
}
