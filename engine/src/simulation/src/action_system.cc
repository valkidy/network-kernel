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

void enter_recovery(
    World& world,
    entt::entity entity,
    ActionRuntimeState& action,
    const RuntimeActionTemplate& action_template,
    std::uint32_t current_tick) {
    if (world.registry().all_of<WeaponState>(entity)) {
        WeaponState& weapon = world.registry().get<WeaponState>(entity);
        if (weapon.active_effect_net_id != 0u) {
            world.destroy(weapon.active_effect_net_id);
            weapon.active_effect_net_id = 0u;
        }
    }
    action.recovery_end_tick = current_tick + action_template.recovery_ticks;
    if (action_template.recovery_ticks == 0u) {
        const std::uint32_t next_generated_instance_id =
            action.next_generated_instance_id;
        const std::uint32_t last_advanced_tick = action.last_advanced_tick;
        action = ActionRuntimeState{};
        action.next_generated_instance_id = next_generated_instance_id;
        action.last_advanced_tick = last_advanced_tick;
    } else {
        action.phase = KernelActionPhase_Recovery;
    }
    action.cancel_after_first_commit = false;
}

void settle_recovery(ActionRuntimeState& action, std::uint32_t current_tick) {
    if (action.phase != KernelActionPhase_Recovery ||
        current_tick < action.recovery_end_tick) {
        return;
    }
    const std::uint32_t next_generated_instance_id =
        action.next_generated_instance_id;
    const std::uint32_t last_advanced_tick = action.last_advanced_tick;
    action = ActionRuntimeState{};
    action.next_generated_instance_id = next_generated_instance_id;
    action.last_advanced_tick = last_advanced_tick;
}

void begin_action(
    ActionRuntimeState& action,
    const ActionInputState& input,
    const RuntimeActionTemplate& action_template,
    std::uint8_t weapon_id,
    std::uint32_t current_tick) {
    action.action_template_id = action_template.action_template_id;
    if (input.client_action_id != 0u) {
        action.action_instance_id = input.client_action_id;
    } else {
        action.action_instance_id = action.next_generated_instance_id++;
        if (action.next_generated_instance_id == 0u) {
            action.next_generated_instance_id = 1u;
        }
    }
    action.source_weapon_id = weapon_id;
    action.start_tick = current_tick;
    action.next_commit_tick = current_tick + action_template.commit_offset_ticks;
    action.last_commit_tick = 0u;
    action.recovery_end_tick = 0u;
    action.commit_count = 0u;
    action.cancel_after_first_commit = false;
    action.phase = action_template.commit_offset_ticks == 0u
                       ? KernelActionPhase_Active
                       : KernelActionPhase_Windup;
}

void advance_action(
    World& world,
    entt::entity entity,
    std::uint32_t current_tick,
    std::vector<ActionCommit>* commits) {
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
        action.phase = KernelActionPhase_None;
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
    const bool released = (input.buttons & InputButton_Fire) == 0u;
    const bool weapon_changed = input.selected_weapon != action.source_weapon_id;
    const bool timed_out =
        action_template->trigger_mode == KernelActionTriggerMode_Hold &&
        current_tick - input.last_input_tick >=
            action_template->hold_input_timeout_ticks;
    const bool flagged_cancel =
        (released &&
         (action_template->flags & KernelActionTemplateFlag_CancelOnRelease) != 0u) ||
        (dead &&
         (action_template->flags & KernelActionTemplateFlag_CancelOnDeath) != 0u) ||
        (weapon_changed &&
         (action_template->flags &
          KernelActionTemplateFlag_CancelOnWeaponChange) != 0u);
    if (timed_out ||
        (flagged_cancel &&
         (action.commit_count > 0u ||
          (action_template->flags &
           KernelActionTemplateFlag_CancelBeforeFirstCommit) != 0u))) {
        enter_recovery(world, entity, action, *action_template, current_tick);
        update_visual_flags(world, entity);
        return;
    }
    if (flagged_cancel) {
        action.cancel_after_first_commit = true;
    }
    if (current_tick < action.next_commit_tick) {
        action.phase = KernelActionPhase_Windup;
        update_visual_flags(world, entity);
        return;
    }
    const std::size_t weapon_index =
        static_cast<std::size_t>(action.source_weapon_id);
    if (weapon_index >= weapon.ammo.size() ||
        weapon.is_reloading ||
        weapon.ammo[weapon_index] < action_template->ammo_cost_per_commit) {
        enter_recovery(world, entity, action, *action_template, current_tick);
        update_visual_flags(world, entity);
        return;
    }

    weapon.ammo[weapon_index] = static_cast<std::uint16_t>(
        weapon.ammo[weapon_index] - action_template->ammo_cost_per_commit);
    action.phase = KernelActionPhase_Active;
    action.last_commit_tick = current_tick;
    ++action.commit_count;
    action.next_commit_tick = current_tick + action_template->commit_interval_ticks;
    const NetworkIdentity& identity =
        world.registry().get<NetworkIdentity>(entity);
    commits->push_back(ActionCommit{
        identity.net_id,
        identity.owner_peer,
        action.source_weapon_id,
        action.action_instance_id,
        input.aim_direction,
    });
    if ((action_template->max_commit_count != 0u &&
         action.commit_count >= action_template->max_commit_count) ||
        action.cancel_after_first_commit) {
        enter_recovery(world, entity, action, *action_template, current_tick);
    }
    update_visual_flags(world, entity);
}

}  // namespace

std::vector<ActionCommit> simulate_actions(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick) {
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
        input.previous_buttons = input.buttons;
        input.buttons = queued_input.input.buttons;
        input.last_input_tick = current_tick;
        input.client_action_id = queued_input.input.client_action_id;
        input.selected_weapon = queued_input.input.selected_weapon;
        input.aim_direction = normalized_aim(queued_input.input);

        ActionRuntimeState& action =
            world.registry().get_or_emplace<ActionRuntimeState>(entity);
        settle_recovery(action, current_tick);
        const WeaponMechanicsDefinition* definition =
            weapon_definition(world, entity, input.selected_weapon);
        if (definition != nullptr && definition->fire_action_template_id != 0u &&
            action.phase == KernelActionPhase_None) {
            const RuntimeActionTemplate* action_template =
                world.find_action_template(definition->fire_action_template_id);
            const bool fire_held = (input.buttons & InputButton_Fire) != 0u;
            const bool fire_pressed = fire_held &&
                (input.previous_buttons & InputButton_Fire) == 0u;
            const bool should_start = action_template != nullptr &&
                ((action_template->trigger_mode == KernelActionTriggerMode_Press &&
                  fire_pressed) ||
                 (action_template->trigger_mode == KernelActionTriggerMode_Hold &&
                  fire_held));
            WeaponState& weapon = world.registry().get<WeaponState>(entity);
            if (should_start && !weapon.is_reloading) {
                begin_action(
                    action,
                    input,
                    *action_template,
                    definition->id,
                    current_tick);
            } else if ((input.buttons & InputButton_Reload) != 0u &&
                       weapon.ammo[definition->id] < definition->magazine_size &&
                       weapon.reserve_magazines[definition->id] > 0u) {
                weapon.is_reloading = true;
                weapon.reload_end_tick = current_tick + definition->reload_ticks;
            }
        }
        touched.insert(entity);
    }

    if (inputs.empty()) {
        const auto view = world.registry().view<ActionRuntimeState>();
        for (const entt::entity entity : view) {
            advance_action(world, entity, current_tick, &commits);
        }
    } else {
        for (const entt::entity entity : touched) {
            advance_action(world, entity, current_tick, &commits);
        }
    }
    return commits;
}

}  // namespace network_example
