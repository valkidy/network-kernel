#include "simulation/public/simulation.h"

#include <algorithm>
#include <unordered_set>

#include <glm/geometric.hpp>

namespace network_example {
namespace {

glm::vec3 normalized_aim(const PlayerInput& input) {
    const glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    return glm::length(aim) > 0.0001f
               ? glm::normalize(aim)
               : glm::vec3{1.0f, 0.0f, 0.0f};
}

entt::entity input_entity(const World& world, const QueuedInput& input) {
    if (input.controlled_net_id != 0u) {
        return world.find_entity(input.controlled_net_id).value_or(entt::null);
    }
    const auto view = world.registry().view<const NetworkIdentity, const PlayerTag>();
    for (const entt::entity entity : view) {
        if (view.get<const NetworkIdentity>(entity).owner_peer == input.owner_peer) {
            return entity;
        }
    }
    return entt::null;
}

const WeaponMechanicsDefinition* weapon_definition(
    const World& world,
    entt::entity entity,
    std::uint8_t weapon_id) {
    if (!world.registry().all_of<WeaponTuning>(entity) ||
        static_cast<std::size_t>(weapon_id) >= kWeaponCount) {
        return nullptr;
    }
    const WeaponTuning& tuning = world.registry().get<WeaponTuning>(entity);
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    return tuning.configured[index] ? &tuning.definitions[index] : nullptr;
}

std::uint32_t template_for_binding(
    const WeaponMechanicsDefinition& weapon,
    std::uint16_t binding_id) {
    if (binding_id == KernelActionBinding_PrimaryFire) {
        return weapon.fire_action_template_id;
    }
    if (binding_id == KernelActionBinding_Reload) {
        return weapon.reload_action_template_id;
    }
    return 0u;
}

void push_outcome(
    const World& world,
    entt::entity entity,
    const ActionRuntimeState& action,
    std::uint32_t current_tick,
    ActionOutcomeType type,
    KernelLocalActionResultReason reason,
    std::vector<ActionOutcome>* outcomes) {
    if (outcomes == nullptr ||
        !world.registry().all_of<NetworkIdentity>(entity)) {
        return;
    }
    const NetworkIdentity& identity =
        world.registry().get<NetworkIdentity>(entity);
    outcomes->push_back(ActionOutcome{
        identity.net_id,
        identity.owner_peer,
        action.action_template_id,
        action.action_instance_id,
        action.binding_id,
        static_cast<std::uint16_t>(
            std::min<std::uint32_t>(UINT16_MAX, action.commit_count)),
        current_tick,
        type,
        reason,
    });
}

void push_rejection(
    const World& world,
    entt::entity entity,
    const ActionIntentState& intent,
    std::uint32_t action_template_id,
    std::uint32_t current_tick,
    KernelLocalActionResultReason reason,
    std::vector<ActionOutcome>* outcomes) {
    ActionRuntimeState rejected{};
    rejected.action_template_id = action_template_id;
    rejected.action_instance_id = intent.action_instance_id;
    rejected.binding_id = intent.binding_id;
    push_outcome(
        world,
        entity,
        rejected,
        current_tick,
        ActionOutcomeType::Rejected,
        reason,
        outcomes);
}

void update_visual_flags(World& world, entt::entity entity) {
    const ActionInputState& input =
        world.registry().get_or_emplace<ActionInputState>(entity);
    ReplicationState& replication =
        world.registry().get_or_emplace<ReplicationState>(entity);
    replication.visual_flags &= ~(kVisualFlagAiming | kVisualFlagFiring);
    if ((input.buttons & InputButton_Aim) != 0u) {
        replication.visual_flags |= kVisualFlagAiming;
    }
}

void reset_action(ActionRuntimeState& action) {
    const std::uint32_t high_water = action.next_generated_instance_id;
    const std::uint32_t last_advanced_tick = action.last_advanced_tick;
    action = ActionRuntimeState{};
    action.next_generated_instance_id = high_water;
    action.last_advanced_tick = last_advanced_tick;
}

void enter_recovery(
    World& world,
    entt::entity entity,
    ActionRuntimeState& action,
    const RuntimeActionTemplate& action_template,
    std::uint32_t current_tick) {
    if (world.registry().all_of<WeaponState>(entity)) {
        WeaponState& weapon = world.registry().get<WeaponState>(entity);
        if (action.binding_id == KernelActionBinding_Reload) {
            weapon.is_reloading = false;
        }
        if (weapon.active_effect_net_id != 0u) {
            world.destroy(weapon.active_effect_net_id);
            weapon.active_effect_net_id = 0u;
        }
    }
    action.recovery_end_tick = current_tick + action_template.recovery_ticks;
    if (action_template.recovery_ticks == 0u) {
        reset_action(action);
    } else {
        action.phase = KernelActionPhase_Recovery;
        action.cancel_after_first_commit = false;
    }
}

void settle_recovery(ActionRuntimeState& action, std::uint32_t current_tick) {
    if (action.phase == KernelActionPhase_Recovery &&
        current_tick >= action.recovery_end_tick) {
        reset_action(action);
    }
}

bool admit_action(
    World& world,
    entt::entity entity,
    const ActionInputState& input,
    std::uint32_t current_tick,
    std::vector<ActionOutcome>* outcomes) {
    const ActionIntentState& intent = input.intent;
    if (intent.action_instance_id == 0u) {
        return false;
    }
    ActionRuntimeState& action =
        world.registry().get_or_emplace<ActionRuntimeState>(entity);
    settle_recovery(action, current_tick);
    const WeaponMechanicsDefinition* weapon_definition_value =
        weapon_definition(world, entity, input.selected_weapon);
    const std::uint32_t action_template_id =
        weapon_definition_value == nullptr
            ? 0u
            : template_for_binding(*weapon_definition_value, intent.binding_id);
    if (intent.flags != 0u || intent.reserved != 0u ||
        (intent.binding_id != KernelActionBinding_PrimaryFire &&
         intent.binding_id != KernelActionBinding_Reload)) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_InvalidActionId,
            outcomes);
        return false;
    }
    if (intent.action_instance_id <= action.next_generated_instance_id - 1u) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_InvalidActionId,
            outcomes);
        return false;
    }
    if (action.phase != KernelActionPhase_None) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_Busy,
            outcomes);
        return false;
    }
    if (weapon_definition_value == nullptr || action_template_id == 0u) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_MissingTemplate,
            outcomes);
        return false;
    }
    const RuntimeActionTemplate* action_template =
        world.find_action_template(action_template_id);
    if (action_template == nullptr ||
        !world.registry().all_of<WeaponState>(entity)) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_MissingTemplate,
            outcomes);
        return false;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0u) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_Dead,
            outcomes);
        return false;
    }
    WeaponState& weapon = world.registry().get<WeaponState>(entity);
    const std::size_t weapon_index = input.selected_weapon;
    if (intent.binding_id == KernelActionBinding_PrimaryFire) {
        if (weapon.is_reloading) {
            push_rejection(
                world,
                entity,
                intent,
                action_template_id,
                current_tick,
                KernelLocalActionResultReason_Reloading,
                outcomes);
            return false;
        }
        if (weapon_index >= weapon.ammo.size() ||
            weapon.ammo[weapon_index] < action_template->ammo_cost_per_commit) {
            push_rejection(
                world,
                entity,
                intent,
                action_template_id,
                current_tick,
                KernelLocalActionResultReason_NoAmmo,
                outcomes);
            return false;
        }
    } else if (
        weapon_index >= weapon.ammo.size() ||
        weapon.ammo[weapon_index] >= weapon_definition_value->magazine_size ||
        weapon.reserve_magazines[weapon_index] == 0u) {
        push_rejection(
            world,
            entity,
            intent,
            action_template_id,
            current_tick,
            KernelLocalActionResultReason_EffectFailed,
            outcomes);
        return false;
    }

    action.next_generated_instance_id = intent.action_instance_id + 1u;
    if (action.next_generated_instance_id == 0u) {
        action.next_generated_instance_id = intent.action_instance_id;
    }
    action.action_template_id = action_template_id;
    action.action_instance_id = intent.action_instance_id;
    action.binding_id = intent.binding_id;
    action.source_weapon_id = input.selected_weapon;
    action.start_tick = current_tick;
    action.next_commit_tick = current_tick + action_template->commit_offset_ticks;
    action.last_commit_tick = 0u;
    action.recovery_end_tick = 0u;
    action.commit_count = 0u;
    action.cancel_after_first_commit = false;
    action.phase = action_template->commit_offset_ticks == 0u
                       ? KernelActionPhase_Active
                       : KernelActionPhase_Windup;
    if (intent.binding_id == KernelActionBinding_Reload) {
        weapon.is_reloading = true;
    }
    push_outcome(
        world,
        entity,
        action,
        current_tick,
        ActionOutcomeType::Admitted,
        KernelLocalActionResultReason_None,
        outcomes);
    return true;
}

void advance_action(
    World& world,
    entt::entity entity,
    std::uint32_t current_tick,
    std::vector<ActionCommit>* commits,
    std::vector<ActionOutcome>* outcomes) {
    ActionRuntimeState& action =
        world.registry().get_or_emplace<ActionRuntimeState>(entity);
    if (action.last_advanced_tick == current_tick) {
        update_visual_flags(world, entity);
        return;
    }
    settle_recovery(action, current_tick);
    action.last_advanced_tick = current_tick;
    if (action.phase == KernelActionPhase_None) {
        update_visual_flags(world, entity);
        return;
    }
    const RuntimeActionTemplate* action_template =
        world.find_action_template(action.action_template_id);
    if (action_template == nullptr ||
        !world.registry().all_of<WeaponState, NetworkIdentity>(entity)) {
        push_outcome(
            world,
            entity,
            action,
            current_tick,
            ActionOutcomeType::Corrected,
            KernelLocalActionResultReason_MissingTemplate,
            outcomes);
        reset_action(action);
        update_visual_flags(world, entity);
        return;
    }
    if (action.phase == KernelActionPhase_Recovery) {
        update_visual_flags(world, entity);
        return;
    }

    const ActionInputState& input =
        world.registry().get_or_emplace<ActionInputState>(entity);
    WeaponState& weapon = world.registry().get<WeaponState>(entity);
    const bool dead = world.registry().all_of<Health>(entity) &&
                      world.registry().get<Health>(entity).hp == 0u;
    const bool matching_control =
        input.continuous.action_instance_id == action.action_instance_id;
    const bool released = matching_control && input.continuous.held == 0u;
    const bool weapon_changed = input.selected_weapon != action.source_weapon_id;
    const bool timed_out =
        action_template->trigger_mode == KernelActionTriggerMode_Hold &&
        action_template->hold_input_timeout_ticks != 0u &&
        current_tick - input.last_input_tick >=
            action_template->hold_input_timeout_ticks;
    KernelLocalActionResultReason cancel_reason =
        KernelLocalActionResultReason_None;
    if (dead &&
        (action_template->flags & KernelActionTemplateFlag_CancelOnDeath) != 0u) {
        cancel_reason = KernelLocalActionResultReason_Dead;
    } else if (
        weapon_changed &&
        (action_template->flags &
         KernelActionTemplateFlag_CancelOnWeaponChange) != 0u) {
        cancel_reason = KernelLocalActionResultReason_WeaponChanged;
    } else if (
        released &&
        (action_template->flags & KernelActionTemplateFlag_CancelOnRelease) != 0u) {
        cancel_reason = KernelLocalActionResultReason_Cancelled;
    } else if (timed_out) {
        cancel_reason = KernelLocalActionResultReason_TimedOut;
    }
    if (cancel_reason != KernelLocalActionResultReason_None &&
        (action.commit_count > 0u ||
         (action_template->flags &
          KernelActionTemplateFlag_CancelBeforeFirstCommit) != 0u)) {
        push_outcome(
            world,
            entity,
            action,
            current_tick,
            ActionOutcomeType::Corrected,
            cancel_reason,
            outcomes);
        enter_recovery(world, entity, action, *action_template, current_tick);
        update_visual_flags(world, entity);
        return;
    }
    if (cancel_reason != KernelLocalActionResultReason_None) {
        action.cancel_after_first_commit = true;
    }
    if (current_tick < action.next_commit_tick) {
        action.phase = KernelActionPhase_Windup;
        update_visual_flags(world, entity);
        return;
    }

    const std::size_t weapon_index = action.source_weapon_id;
    if (weapon_index >= weapon.ammo.size() ||
        (action.binding_id == KernelActionBinding_PrimaryFire &&
         weapon.ammo[weapon_index] < action_template->ammo_cost_per_commit)) {
        push_outcome(
            world,
            entity,
            action,
            current_tick,
            ActionOutcomeType::Corrected,
            KernelLocalActionResultReason_EffectFailed,
            outcomes);
        enter_recovery(world, entity, action, *action_template, current_tick);
        update_visual_flags(world, entity);
        return;
    }

    action.phase = KernelActionPhase_Active;
    action.last_commit_tick = current_tick;
    ++action.commit_count;
    action.next_commit_tick = current_tick + action_template->commit_interval_ticks;
    const bool completes_action =
        (action_template->max_commit_count != 0u &&
         action.commit_count >= action_template->max_commit_count) ||
        action.cancel_after_first_commit;
    const NetworkIdentity& identity =
        world.registry().get<NetworkIdentity>(entity);
    commits->push_back(ActionCommit{
        identity.net_id,
        identity.owner_peer,
        action.source_weapon_id,
        action.binding_id,
        action.action_template_id,
        action.action_instance_id,
        static_cast<std::uint16_t>(
            std::min<std::uint32_t>(UINT16_MAX, action.commit_count)),
        current_tick,
        completes_action,
        input.aim_direction,
    });
    if (completes_action) {
        enter_recovery(world, entity, action, *action_template, current_tick);
    }
    update_visual_flags(world, entity);
}

}  // namespace

std::vector<ActionCommit> simulate_actions(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<ActionOutcome>* outcomes) {
    std::vector<ActionCommit> commits;
    std::unordered_set<entt::entity> touched;
    for (const QueuedInput& queued_input : inputs) {
        const entt::entity entity = input_entity(world, queued_input);
        if (entity == entt::null ||
            !world.registry().all_of<WeaponState>(entity)) {
            continue;
        }
        ActionInputState& input =
            world.registry().get_or_emplace<ActionInputState>(entity);
        input.buttons = queued_input.input.buttons;
        input.intent = ActionIntentState{
            queued_input.input.action_intent.action_instance_id,
            queued_input.input.action_intent.binding_id,
            queued_input.input.action_intent.flags,
            queued_input.input.action_intent.reserved,
        };
        input.selected_weapon = queued_input.input.selected_weapon;
        input.aim_direction = normalized_aim(queued_input.input);
        ActionRuntimeState& action =
            world.registry().get_or_emplace<ActionRuntimeState>(entity);
        if (queued_input.input.action_input.action_instance_id != 0u &&
            queued_input.input.action_input.flags == 0u &&
            queued_input.input.action_input.reserved == 0u &&
            queued_input.input.action_input.action_instance_id ==
                action.action_instance_id) {
            input.continuous = ContinuousActionInputState{
                queued_input.input.action_input.action_instance_id,
                queued_input.input.action_input.held,
                queued_input.input.action_input.flags,
                queued_input.input.action_input.reserved,
            };
            input.last_input_tick = current_tick;
        }
        admit_action(world, entity, input, current_tick, outcomes);
        if (queued_input.input.action_input.action_instance_id != 0u &&
            queued_input.input.action_input.action_instance_id ==
                action.action_instance_id) {
            input.continuous = ContinuousActionInputState{
                queued_input.input.action_input.action_instance_id,
                queued_input.input.action_input.held,
                queued_input.input.action_input.flags,
                queued_input.input.action_input.reserved,
            };
            input.last_input_tick = current_tick;
        }
        touched.insert(entity);
    }

    if (inputs.empty()) {
        const auto view = world.registry().view<ActionRuntimeState>();
        for (const entt::entity entity : view) {
            advance_action(world, entity, current_tick, &commits, outcomes);
        }
    } else {
        for (const entt::entity entity : touched) {
            advance_action(world, entity, current_tick, &commits, outcomes);
        }
    }
    return commits;
}

}  // namespace network_example
