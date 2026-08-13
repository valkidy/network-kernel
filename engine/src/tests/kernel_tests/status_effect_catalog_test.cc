#include <cassert>
#include <cstdint>

#include "kernel/src/kernel.h"

namespace {

KernelActionTriggerDefinition trigger_with_action(
    std::uint8_t action_type,
    std::uint8_t target_source = KernelEntityRefSource_Self) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_count = 1u;
    KernelActionDefinition& action = trigger.actions[0];
    action.action_type = action_type;
    action.target_source = target_source;
    action.damage_amount = 1u;
    action.health_change_amount = -1;
    action.impulse_strength = 1.0f;
    action.impulse_collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    action.impulse_direction = KernelVec3{1.0f, 0.0f, 0.0f};
    action.status_effect_id = 1001u;
    action.modifier_operation = KernelStatModifierOperation_Additive;
    action.modifier_value = 1.0f;
    return trigger;
}

bool load_status(
    const KernelStatusEffectDefinition& status) {
    network_example::KernelEngine engine(KernelConfig{});
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1u;
    catalog.catalog_hash = UINT64_C(1);
    catalog.status_effects = &status;
    catalog.status_effect_count = 1u;
    return engine.load_gameplay_catalog(catalog);
}

KernelStatusEffectDefinition base_status() {
    KernelStatusEffectDefinition status{};
    status.struct_size = sizeof(status);
    status.status_effect_id = 1001u;
    status.channel_id = 1u;
    status.duration_ticks = 30u;
    status.interval_ticks = 1u;
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Replace;
    return status;
}

}  // namespace

int main() {
    KernelStatusEffectDefinition status = base_status();
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier,
        KernelEntityRefSource_EventSubject);
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyDamage,
        KernelEntityRefSource_EventInstigator);
    status.on_expire_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyHealthChange);
    assert(load_status(status));

    status = base_status();
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyImpulse);
    assert(!load_status(status));

    status = base_status();
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyStatus);
    assert(!load_status(status));

    status = base_status();
    status.on_expire_trigger = trigger_with_action(
        KernelEntityTriggerActionType_SpawnEntity);
    assert(!load_status(status));

    status = base_status();
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier);
    assert(!load_status(status));

    status = base_status();
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier,
        KernelEntityRefSource_EventInstigator);
    assert(!load_status(status));
    return 0;
}
