// The catalog loader and the kernel validator keep separate tables of what an
// entity-backed trigger may contain, and they have now drifted apart three
// times: the movement mask vocabulary, the limb mask vocabulary, and
// SpawnProjectile. The failure mode is the worst kind -- the game_server loader
// accepts the YAML, the kernel then rejects the catalog, and because a catalog
// loads all-or-nothing the diagnostic points at nothing in particular while
// *every* template goes missing.
//
// So this pins the direction that actually breaks: for every action type the
// loader can compile into an entity trigger, the kernel must accept a
// well-formed instance of it. A new action type that reaches entity triggers
// belongs in `kEntityTriggerActions` below, and if the kernel has no branch for
// it this test says so before a catalog does.

#include <cassert>
#include <cstdint>
#include <cstdlib>

#include "kernel/src/kernel.h"

namespace {

constexpr std::uint32_t kPropTemplateId = 9001u;
constexpr std::uint32_t kSpawnedTemplateId = 9002u;
constexpr std::uint32_t kProjectileTemplateId = 9003u;
constexpr std::uint32_t kStatusEffectId = 9004u;

void require(bool condition) {
    assert(condition);
    if (!condition) {
        std::abort();
    }
}

// Every field any branch might read, all at once. The validator only inspects
// the ones belonging to the action type under test, so one well-formed donor
// serves all of them and the test cannot accidentally pass by leaving the
// interesting field at a value the branch never looks at.
KernelActionDefinition well_formed_action(std::uint8_t action_type) {
    KernelActionDefinition action{};
    action.action_type = action_type;
    action.target_source = KernelEntityRefSource_EventTarget;
    action.owner_source = KernelEntityRefSource_Self;
    action.position_source = KernelEventVec3Source_Position;
    action.direction_source = KernelEventVec3Source_Direction;
    action.damage_amount = 1u;
    action.health_change_amount = -1;
    action.spawn_entity_template_id = kSpawnedTemplateId;
    action.spawn_projectile_template_id = kProjectileTemplateId;
    action.status_effect_id = kStatusEffectId;
    action.impulse_strength = 1.0f;
    action.impulse_strength_mode = KERNEL_IMPULSE_STRENGTH_MODE_RADIAL;
    action.impulse_collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    action.impulse_direction = KernelVec3{1.0f, 0.0f, 0.0f};
    action.modifier_operation = KernelStatModifierOperation_Additive;
    action.modifier_value = 1.0f;
    return action;
}

KernelEntityTemplateDefinition prop_template(
    std::uint32_t entity_template_id,
    const KernelActionTriggerDefinition* collision_trigger) {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = entity_template_id;
    entity_template.entity_type = KernelEntityType_Prop;
    entity_template.actor_type = KernelActorType_Unknown;
    // Nested struct_size is how the ABI spells "this block is present"; the
    // validator rejects an entity template whose ai block is absent before it
    // ever reaches a trigger.
    entity_template.ai.struct_size = sizeof(entity_template.ai);
    entity_template.vision.struct_size = sizeof(entity_template.vision);
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    // Checked in a second pass long after the trigger loop, and required of
    // every entity template regardless of type. A prop may leave the controller
    // at None, which is what skips the movement-collider lookup below it.
    entity_template.movement.struct_size = sizeof(entity_template.movement);
    if (collision_trigger != nullptr) {
        entity_template.collision_trigger = *collision_trigger;
        entity_template.collision_trigger_mask = KERNEL_COLLISION_MASK_ACTOR;
    }
    return entity_template;
}

KernelStatusEffectDefinition status_effect() {
    KernelStatusEffectDefinition status{};
    status.struct_size = sizeof(status);
    status.status_effect_id = kStatusEffectId;
    status.channel_id = 1u;
    status.duration_ticks = 30u;
    status.interval_ticks = 1u;
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Replace;
    return status;
}

bool load_with_collision_trigger(
    const KernelActionTriggerDefinition& collision_trigger) {
    const KernelEntityTemplateDefinition entity_templates[] = {
        prop_template(kPropTemplateId, &collision_trigger),
        // SpawnEntity names a template, so one has to exist to name.
        prop_template(kSpawnedTemplateId, nullptr),
    };
    const KernelStatusEffectDefinition status = status_effect();

    network_example::KernelEngine engine(KernelConfig{});
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1u;
    catalog.catalog_hash = UINT64_C(1);
    catalog.entity_templates = entity_templates;
    catalog.entity_template_count = 2u;
    catalog.status_effects = &status;
    catalog.status_effect_count = 1u;
    return engine.load_gameplay_catalog(catalog);
}

KernelActionTriggerDefinition multi_action_trigger(std::uint8_t action_type) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    trigger.actions[0] = well_formed_action(action_type);
    return trigger;
}

// action_count == 0 selects the legacy single-action mirror, where the fields
// live on the trigger rather than in actions[0]. The catalog loader always sets
// action_count, so this form only ever arrives as hand-built ABI input -- which
// is precisely why a field missing from the mirror goes unnoticed.
KernelActionTriggerDefinition legacy_mirror_trigger(std::uint8_t action_type) {
    const KernelActionDefinition action = well_formed_action(action_type);
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 0u;
    trigger.action_type = action.action_type;
    trigger.target_source = action.target_source;
    trigger.owner_source = action.owner_source;
    trigger.position_source = action.position_source;
    trigger.direction_source = action.direction_source;
    trigger.damage_amount = action.damage_amount;
    trigger.health_change_amount = action.health_change_amount;
    trigger.spawn_entity_template_id = action.spawn_entity_template_id;
    trigger.spawn_projectile_template_id = action.spawn_projectile_template_id;
    trigger.status_effect_id = action.status_effect_id;
    trigger.impulse_strength = action.impulse_strength;
    trigger.impulse_strength_mode = action.impulse_strength_mode;
    trigger.impulse_collision_mask = action.impulse_collision_mask;
    trigger.impulse_direction = action.impulse_direction;
    trigger.modifier_operation = action.modifier_operation;
    trigger.modifier_value = action.modifier_value;
    return trigger;
}

// What game_server/gameplay_config.cc's entity-trigger path can compile.
// apply_speed_modifier is absent because it is not authorable here at all --
// the loader restricts it to a status `on_apply` and the kernel now rejects it
// on an entity trigger to match. That agreement is asserted in
// malformed_actions_are_still_rejected rather than here, since this list is
// the set the kernel must *accept*.
constexpr std::uint8_t kEntityTriggerActions[] = {
    KernelEntityTriggerActionType_ApplyDamage,
    KernelEntityTriggerActionType_ApplyHealthChange,
    KernelEntityTriggerActionType_ApplyImpulse,
    KernelEntityTriggerActionType_ApplyStatus,
    KernelEntityTriggerActionType_RemoveStatus,
    KernelEntityTriggerActionType_SpawnEntity,
    KernelEntityTriggerActionType_SpawnProjectile,
};

void kernel_accepts_every_action_the_loader_can_author() {
    for (const std::uint8_t action_type : kEntityTriggerActions) {
        require(load_with_collision_trigger(multi_action_trigger(action_type)));
    }
}

void the_legacy_mirror_carries_the_same_fields() {
    for (const std::uint8_t action_type : kEntityTriggerActions) {
        require(load_with_collision_trigger(legacy_mirror_trigger(action_type)));
    }
}

// The parity claim above is only worth something if the validator is actually
// inspecting these actions rather than waving the whole trigger through.
void malformed_actions_are_still_rejected() {
    KernelActionTriggerDefinition no_template = multi_action_trigger(
        KernelEntityTriggerActionType_SpawnProjectile);
    no_template.actions[0].spawn_projectile_template_id = 0u;
    require(!load_with_collision_trigger(no_template));

    // spawn_projectile has to fire along the event's direction; a literal one
    // is what the authoring contract forbids here.
    KernelActionTriggerDefinition literal_direction = multi_action_trigger(
        KernelEntityTriggerActionType_SpawnProjectile);
    literal_direction.actions[0].direction_source =
        KernelEventVec3Source_Literal;
    require(!load_with_collision_trigger(literal_direction));

    KernelActionTriggerDefinition unknown_action = multi_action_trigger(200u);
    require(!load_with_collision_trigger(unknown_action));

    // An entity trigger may not carry a speed modifier at all -- not a
    // malformed one, and not a perfectly well-formed one either. A speed
    // modifier belongs to a status effect's lifetime: its status_instance_id
    // comes from provenance that only the status lifecycle fills, the preflight
    // rejects an id of 0, applying it needs a matching active status, and
    // expiry removes it by that id. None of that exists on a collision, so the
    // shape can only fail at runtime -- and take the rest of its batch with it,
    // because the preflight is all-or-nothing.
    //
    // Rejecting it at load is also what makes the two tables agree: the catalog
    // loader already throws on apply_speed_modifier outside a status on_apply.
    // This is the one place where loader-rejects and kernel-rejects, so it is
    // asserted rather than left to the parity loop above, which only covers
    // what the loader accepts.
    for (const std::uint8_t form_is_legacy : {0u, 1u}) {
        const KernelActionTriggerDefinition speed_modifier =
            form_is_legacy != 0u
                ? legacy_mirror_trigger(
                      KernelEntityTriggerActionType_ApplySpeedModifier)
                : multi_action_trigger(
                      KernelEntityTriggerActionType_ApplySpeedModifier);
        require(!load_with_collision_trigger(speed_modifier));
    }
}

}  // namespace

int main() {
    kernel_accepts_every_action_the_loader_can_author();
    the_legacy_mirror_carries_the_same_fields();
    malformed_actions_are_still_rejected();
    return 0;
}
