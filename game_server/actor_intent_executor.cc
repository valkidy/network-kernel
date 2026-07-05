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

std::uint32_t attack_buttons(
    const ActorIntentExecutorConfig& config,
    const KernelServerEntityState& entity_state) {
    if (config.weapon_id >= KERNEL_MAX_WEAPONS || entity_state.is_reloading != 0u) {
        return 0u;
    }
    if (entity_state.ammo[config.weapon_id] > 0) {
        return InputButton_Fire;
    }
    if (entity_state.reserve_ammo[config.weapon_id] > 0) {
        return InputButton_Reload;
    }
    return 0u;
}

}  // namespace

ActorIntentExecutor::ActorIntentExecutor(ActorIntentExecutorConfig config)
    : config_(config) {}

ActorIntentExecutionResult ActorIntentExecutor::execute(
    KernelHandle* kernel,
    Enemy* actor,
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

    std::uint32_t buttons = 0;
    KernelVec3 target_position = perception.target_position;
    if (intent.type == "AttackTarget") {
        if (!perception.has_visible_target || !perception.has_target_position) {
            add_missing(&result.report, "data", "Data.TargetPosition");
            return result;
        }
        buttons = attack_buttons(config_, perception.self_state);
        if (buttons == 0u) {
            if (perception.self_state.is_reloading != 0u) {
                result.status = ai::IntentStatus::kRunning;
                return result;
            }
            add_missing(&result.report, "action", "Action.AttackTarget");
            return result;
        }
    } else {
        if (config_.weapon_id >= KERNEL_MAX_WEAPONS ||
            perception.self_state.reserve_ammo[config_.weapon_id] == 0) {
            add_missing(&result.report, "action", "Action.Reload");
            return result;
        }
        if (perception.self_state.is_reloading != 0u) {
            result.status = ai::IntentStatus::kRunning;
            return result;
        }
        buttons = InputButton_Reload;
    }

    PlayerInput input{};
    input.input_seq = actor->next_input_seq++;
    input.buttons = buttons;
    input.selected_weapon = config_.weapon_id;
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
