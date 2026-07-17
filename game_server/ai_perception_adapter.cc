#include "game_server/ai_perception_adapter.h"

#include <cmath>

namespace network_example::game_server {
namespace {

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float length(const KernelVec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool query_vision_state(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    KernelVisionStateView* out_state) {
    if (kernel == nullptr || out_state == nullptr) {
        return false;
    }
    KernelVisionStateQuery query{};
    query.struct_size = sizeof(query);
    query.agent_net_id = agent_net_id;
    out_state->struct_size = sizeof(*out_state);
    return Kernel_QueryVisionState(kernel, &query, out_state, 1) == 1 &&
           out_state->valid != 0u;
}

bool get_entity_position(
    KernelHandle* kernel,
    std::uint32_t net_id,
    KernelVec3* out_position) {
    if (kernel == nullptr || out_position == nullptr || net_id == 0) {
        return false;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state) || state.valid == 0u) {
        return false;
    }
    *out_position = state.position;
    return true;
}

}  // namespace

SentryPerceptionSnapshot AiPerceptionAdapter::build_sentry_snapshot(
    KernelHandle* kernel,
    std::uint32_t agent_net_id) {
    SentryPerceptionSnapshot snapshot;
    if (kernel == nullptr || agent_net_id == 0) {
        return snapshot;
    }

    snapshot.self_state.struct_size = sizeof(snapshot.self_state);
    snapshot.has_self_state =
        Kernel_ServerGetEntityState(kernel, agent_net_id, &snapshot.self_state) &&
        snapshot.self_state.valid != 0u;
    if (!snapshot.has_self_state) {
        return snapshot;
    }

    KernelVisionStateView vision_state{};
    if (!query_vision_state(kernel, agent_net_id, &vision_state)) {
        return snapshot;
    }

    snapshot.vision_forward = vision_state.vision_forward;
    snapshot.has_visible_target = vision_state.current_target_candidate != 0;
    snapshot.target_id = vision_state.current_target_candidate;
    if (snapshot.target_id != 0) {
        snapshot.target_position = vision_state.last_known_target_position;
        get_entity_position(kernel, snapshot.target_id, &snapshot.target_position);
        snapshot.has_target_position = true;
    }
    return snapshot;
}

ai::AIContext AiPerceptionAdapter::build_sentry_context(
    const SentryPerceptionSnapshot& snapshot,
    std::uint8_t weapon_id) {
    ai::AIContext context;
    context.set_feature("visible_hostile", snapshot.has_visible_target);
    context.set_feature("nearest_hostile_id", snapshot.target_id);
    if (snapshot.has_self_state) {
        const float hp_ratio =
            snapshot.self_state.max_hp == 0
                ? 0.0f
                : static_cast<float>(snapshot.self_state.hp) /
                      static_cast<float>(snapshot.self_state.max_hp);
        context.set_feature("hp_ratio", hp_ratio);
        context.set_feature(
            "is_reloading",
            static_cast<bool>(snapshot.self_state.is_reloading != 0u));
        const std::size_t slot =
            find_weapon_slot(snapshot.self_state, weapon_id);
        if (slot < snapshot.self_state.weapon_slot_count) {
            context.set_feature(
                "has_ammo",
                static_cast<bool>(snapshot.self_state.ammo[slot] > 0));
        }
    }
    if (snapshot.has_self_state && snapshot.has_target_position) {
        context.set_feature(
            "target_distance",
            length(subtract(snapshot.target_position, snapshot.self_state.position)));
    }
    return context;
}

}  // namespace network_example::game_server
