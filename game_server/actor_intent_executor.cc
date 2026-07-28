#include "game_server/actor_intent_executor.h"

#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace network_example::game_server {
namespace {

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float length_squared(const KernelVec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

KernelVec3 scale(const KernelVec3& value, float scalar) {
    return KernelVec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

KernelVec3 normalized_direction(const KernelVec3& from, const KernelVec3& to) {
    const KernelVec3 delta = subtract(to, from);
    const float distance_squared = length_squared(delta);
    if (distance_squared <= 0.0001f * 0.0001f) {
        return KernelVec3{1.0f, 0.0f, 0.0f};
    }
    return scale(delta, 1.0f / std::sqrt(distance_squared));
}

void add_missing(
    ai::CapabilityReport* report,
    std::string_view type,
    std::string value) {
    if (report == nullptr) {
        return;
    }
    if (type == "data") {
        report->missing_data.push_back(std::move(value));
    } else if (type == "action") {
        report->missing_actions.push_back(std::move(value));
    } else if (type == "executor") {
        report->missing_executors.push_back(std::move(value));
    }
}

std::uint16_t attack_binding(
    const ActorIntentExecutorConfig& config,
    const KernelServerEntityState& entity_state) {
    if (config.weapon_id > UINT8_MAX || entity_state.is_reloading != 0u) {
        return UINT16_MAX;
    }
    const std::size_t slot = find_weapon_slot(
        entity_state, static_cast<std::uint8_t>(config.weapon_id));
    if (slot >= entity_state.weapon_slot_count) {
        return UINT16_MAX;
    }
    if (entity_state.ammo[slot] > 0) {
        return KernelActionBinding_PrimaryFire;
    }
    if (entity_state.reserve_magazines[slot] > 0) {
        return KernelActionBinding_Reload;
    }
    return UINT16_MAX;
}

}  // namespace

ActorIntentExecutor::ActorIntentExecutor(ActorIntentExecutorConfig config)
    : config_(config) {}

ActorIntentExecutionResult ActorIntentExecutor::execute(
    KernelHandle* kernel,
    AgentRuntimeState* actor,
    const ai::ScopedIntent& intent,
    const SentryPerceptionSnapshot& perception) const {
    ActorIntentExecutionResult result;
    if (kernel == nullptr || actor == nullptr ||
        intent.scope != ai::IntentScope::kActor || intent.subject != actor->net_id) {
        add_missing(&result.report, "executor", "Executor.ActorIntent");
        return result;
    }

    if (intent.type != "AttackTarget" && intent.type != "Reload") {
        add_missing(&result.report, "action", intent.type);
        add_missing(&result.report, "executor", "Executor.ActorIntent." + intent.type);
        return result;
    }

    if (!perception.has_self_state) {
        add_missing(&result.report, "data", "Data.ActorState");
        return result;
    }

    std::uint16_t binding_id = UINT16_MAX;
    KernelVec3 target_position = perception.target_position;
    if (intent.type == "AttackTarget") {
        if (!perception.has_visible_target || !perception.has_target_position) {
            add_missing(&result.report, "data", "Data.TargetPosition");
            return result;
        }
        binding_id = attack_binding(config_, perception.self_state);
        if (binding_id == UINT16_MAX) {
            if (perception.self_state.is_reloading != 0u) {
                result.status = ai::IntentStatus::kRunning;
                return result;
            }
            add_missing(&result.report, "action", "Action.AttackTarget");
            return result;
        }
    } else {
        const std::size_t slot =
            config_.weapon_id > UINT8_MAX
                ? KERNEL_MAX_WEAPON_SLOTS
                : find_weapon_slot(
                      perception.self_state,
                      static_cast<std::uint8_t>(config_.weapon_id));
        if (slot >= perception.self_state.weapon_slot_count ||
            perception.self_state.reserve_magazines[slot] == 0) {
            add_missing(&result.report, "action", "Action.Reload");
            return result;
        }
        if (perception.self_state.is_reloading != 0u) {
            result.status = ai::IntentStatus::kRunning;
            return result;
        }
        binding_id = KernelActionBinding_Reload;
    }

    KernelPlayerInput input{};
    input.input_seq = actor->next_input_seq++;
    input.selected_weapon = static_cast<std::uint8_t>(config_.weapon_id);
    if (perception.self_state.action.phase != KernelActionPhase_None) {
        if (intent.type != "AttackTarget" ||
            binding_id != KernelActionBinding_PrimaryFire) {
            result.status = ai::IntentStatus::kRunning;
            return result;
        }
        input.action_input = KernelActionInput{
            perception.self_state.action.action_instance_id, 1u, 0u, 0u};
    } else {
        const std::uint32_t action_instance_id = actor->next_action_instance_id++;
        if (actor->next_action_instance_id == 0u) {
            actor->next_action_instance_id = 1u;
        }
        input.action_intent = KernelActionIntent{
            action_instance_id, binding_id, 0u, 0u};
        input.action_input = KernelActionInput{action_instance_id, 1u, 0u, 0u};
    }
    input.aim_dir = normalized_direction(perception.self_state.position, target_position);
    result.submitted_input =
        Kernel_ServerEnqueueEntityInput(
            kernel,
            KernelCommandSource_AI,
            actor->net_id,
            &input);
    result.status =
        result.submitted_input ? ai::IntentStatus::kRunning : ai::IntentStatus::kFailed;
    if (!result.submitted_input) {
        add_missing(&result.report, "executor", "Executor.ActorIntent");
    }
    return result;
}

}  // namespace network_example::game_server
