#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "game_server/src/gameplay_config.h"
#include "kernel/src/kernel.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const auto prop = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 200;
        });
    require(prop != config.entity_templates.end());
    require(prop->entity_type == KernelEntityType_Prop);
    require(
        prop->activated_trigger.action_graph_ref ==
        "action_apply_damage_at_activated");
    require(
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
    require(compiled != catalog.entity_templates.end());
    require(
        compiled->activated_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyDamage);
    require(
        compiled->activated_trigger.target_source ==
        KernelEntityRefSource_EventTarget);
    require(compiled->activated_trigger.damage_amount == 25);
    require(
        compiled->destroy_entity_trigger.action_type ==
        KernelEntityTriggerActionType_SpawnEntity);
    require(compiled->destroy_entity_trigger.spawn_entity_template_id == 201);
    require(
        compiled->destroy_entity_trigger.position_source ==
        KernelEventVec3Source_Position);
    require(
        compiled->destroy_entity_trigger.owner_source ==
        KernelEntityRefSource_EventInstigator);

    const auto collision_prop = std::find_if(
        config.entity_templates.begin(),
        config.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 201;
        });
    require(collision_prop != config.entity_templates.end());
    require(
        collision_prop->collision_trigger.action_graph_ref ==
        "action_apply_damage_at_collision");
    const auto compiled_collision = std::find_if(
        catalog.entity_templates.begin(),
        catalog.entity_templates.end(),
        [](const KernelEntityTemplateDefinition& value) {
            return value.entity_template_id == 201;
        });
    require(compiled_collision != catalog.entity_templates.end());
    require(
        compiled_collision->collision_trigger_mask ==
        KERNEL_COLLISION_MASK_ACTOR);
    require(
        (compiled_collision->component_flags &
         KERNEL_ENTITY_COMPONENT_HEALTH) != 0u);
    require(compiled_collision->combat.hp == 1);
    require(compiled_collision->combat.max_hp == 1);
    require(compiled_collision->collision_trigger.action_count == 2);
    require(
        compiled_collision->collision_trigger.action_type ==
        KernelEntityTriggerActionType_ApplyDamage);
    require(
        compiled_collision->collision_trigger.target_source ==
        KernelEntityRefSource_EventTarget);
    require(compiled_collision->collision_trigger.damage_amount == 1);
    require(
        compiled_collision->collision_trigger.actions[0].target_source ==
        KernelEntityRefSource_Self);
    require(compiled_collision->collision_trigger.actions[0].damage_amount == 1);
    require(
        compiled_collision->collision_trigger.actions[1].target_source ==
        KernelEntityRefSource_EventTarget);
    require(compiled_collision->collision_trigger.actions[1].damage_amount == 25);

    network_example::game_server::GameServerGameplayConfig invalid = config;
    auto invalid_prop = std::find_if(
        invalid.entity_templates.begin(),
        invalid.entity_templates.end(),
        [](const network_example::game_server::EntityTemplateConfig& value) {
            return value.actor_template_id == 200;
        });
    require(invalid_prop != invalid.entity_templates.end());
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
    require(invalid_event_expression_rejected);

    const auto rocket = std::find_if(
        catalog.projectile_templates.begin(),
        catalog.projectile_templates.end(),
        [](const KernelProjectileTemplateDefinition& value) {
            return value.projectile_template_id == 3;
        });
    require(rocket != catalog.projectile_templates.end());
    require(
        rocket->mechanics.projectile_impact_trigger.action_type ==
        KernelEntityTriggerActionType_SpawnProjectile);
    require(
        rocket->mechanics.projectile_impact_trigger
            .spawn_projectile_template_id == 8);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    network_example::KernelEngine kernel(kernel_config);
    require(kernel.load_gameplay_catalog(catalog.definition));
    return 0;
}
