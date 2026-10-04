#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
    require(load_status(status));

    status = base_status();
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyImpulse);
    require(!load_status(status));

    status = base_status();
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyStatus);
    require(!load_status(status));

    status = base_status();
    status.on_expire_trigger = trigger_with_action(
        KernelEntityTriggerActionType_SpawnEntity);
    require(!load_status(status));

    status = base_status();
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier);
    require(!load_status(status));

    status = base_status();
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier,
        KernelEntityRefSource_EventInstigator);
    require(!load_status(status));

    status = base_status();
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Refresh;
    require(load_status(status));

    status = base_status();
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Stack;
    status.max_stacks = 3u;
    status.refresh_on_stack = 1u;
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyDamage);
    require(load_status(status));

    status.max_stacks = 1u;
    require(!load_status(status));
    status.max_stacks = 33u;
    require(!load_status(status));

    status = base_status();
    status.max_stacks = 2u;
    require(!load_status(status));

    status = base_status();
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Stack;
    status.max_stacks = 2u;
    status.on_tick_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplyDamage);
    status.on_tick_trigger.actions[0].damage_amount = 40000u;
    require(!load_status(status));

    status = base_status();
    status.replacement_policy = KernelStatusEffectReplacementPolicy_Stack;
    status.max_stacks = 32u;
    status.on_apply_trigger = trigger_with_action(
        KernelEntityTriggerActionType_ApplySpeedModifier);
    status.on_apply_trigger.actions[0].modifier_operation =
        KernelStatModifierOperation_Multiplier;
    status.on_apply_trigger.actions[0].modifier_value = 1.0e20f;
    require(!load_status(status));
    return 0;
}
