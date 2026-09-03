#include "game_server/src/ai_perception_adapter.h"

#include <algorithm>
#include <cmath>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

// Where each query starts, not where it stops. Both grow to fit the population;
// the ceilings only exist so that a query which somehow never comes back short
// cannot grow without bound.
constexpr std::size_t kInitialQueriedActors = 128;
constexpr std::size_t kInitialVisionStates = 128;
constexpr std::size_t kMaxQueried = 65536;

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float length(const KernelVec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool get_entity_aim_point(
    KernelHandle* kernel,
    std::uint32_t net_id,
    KernelVec3* out_position) {
    if (kernel == nullptr || out_position == nullptr || net_id == 0) {
        return false;
    }
    return Kernel_ServerGetEntityAimPoint(kernel, net_id, out_position);
}

}  // namespace

void PerceptionFrame::index_actors(const ActorStateView& actors) {
    actors_ = actors;
    actor_by_net_id_.clear();
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        actor_by_net_id_[actors.states[index].net_id] = index;
    }
}

void PerceptionFrame::refresh_vision_states(KernelHandle* kernel) {
    vision_count_ = 0;
    vision_by_net_id_.clear();
    if (kernel == nullptr) {
        return;
    }
    if (vision_buffer_.size() < kInitialVisionStates) {
        vision_buffer_.resize(kInitialVisionStates);
    }
    while (true) {
        for (KernelVisionStateView& state : vision_buffer_) {
            state.struct_size = sizeof(KernelVisionStateView);
        }
        // A null query is "every agent", which is the whole point: the per-agent
        // form walks the same map and throws away everything but one entry.
        const std::uint32_t count = Kernel_QueryVisionState(
            kernel,
            nullptr,
            vision_buffer_.data(),
            static_cast<std::uint32_t>(vision_buffer_.size()));
        // The same truncation trap the actor query documents: the call reports
        // what it wrote, never what it had, so a full buffer is
        // indistinguishable from a truncated one. An agent dropped here is an
        // agent whose controller sees nothing and which stands still.
        if (count >= vision_buffer_.size() && vision_buffer_.size() < kMaxQueried) {
            vision_buffer_.resize(
                std::min(vision_buffer_.size() * 2, kMaxQueried));
            continue;
        }
        vision_count_ = count;
        break;
    }
    for (std::uint32_t index = 0; index < vision_count_; ++index) {
        vision_by_net_id_[vision_buffer_[index].agent_net_id] = index;
    }
}

void PerceptionFrame::refresh(
    KernelHandle* kernel,
    const ActorStateView& actors) {
    owned_actors_.clear();
    index_actors(actors);
    refresh_vision_states(kernel);
}

void PerceptionFrame::refresh(KernelHandle* kernel) {
    if (kernel == nullptr) {
        refresh(kernel, ActorStateView{});
        return;
    }
    if (owned_actors_.size() < kInitialQueriedActors) {
        owned_actors_.resize(kInitialQueriedActors);
    }
    std::uint32_t count = 0;
    while (true) {
        for (KernelServerEntityState& state : owned_actors_) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        count = Kernel_ServerQueryEntities(
            kernel,
            kEntityTypeActor,
            owned_actors_.data(),
            static_cast<std::uint32_t>(owned_actors_.size()));
        if (count < owned_actors_.size() || owned_actors_.size() >= kMaxQueried) {
            break;
        }
        owned_actors_.resize(std::min(owned_actors_.size() * 2, kMaxQueried));
    }
    index_actors(ActorStateView{owned_actors_.data(), count});
    refresh_vision_states(kernel);
}

const KernelServerEntityState* PerceptionFrame::actor_state(
    std::uint32_t net_id) const {
    const auto found = actor_by_net_id_.find(net_id);
    return found == actor_by_net_id_.end() ? nullptr
                                           : &actors_.states[found->second];
}

const KernelVisionStateView* PerceptionFrame::vision_state(
    std::uint32_t net_id) const {
    const auto found = vision_by_net_id_.find(net_id);
    return found == vision_by_net_id_.end() ? nullptr
                                            : &vision_buffer_[found->second];
}

SentryPerceptionSnapshot AiPerceptionAdapter::build_sentry_snapshot(
    KernelHandle* kernel,
    const PerceptionFrame& frame,
    std::uint32_t agent_net_id) {
    SentryPerceptionSnapshot snapshot;
    if (kernel == nullptr || agent_net_id == 0) {
        return snapshot;
    }

    const KernelServerEntityState* self_state = frame.actor_state(agent_net_id);
    if (self_state == nullptr || self_state->valid == 0u) {
        return snapshot;
    }
    snapshot.self_state = *self_state;
    snapshot.has_self_state = true;

    // An agent with no cone has no vision state, and leaves the snapshot with
    // its default forward and no target -- which is what the per-agent query
    // returning zero rows used to produce.
    const KernelVisionStateView* vision_state = frame.vision_state(agent_net_id);
    if (vision_state == nullptr || vision_state->valid == 0u) {
        return snapshot;
    }

    snapshot.vision_forward = vision_state->vision_forward;
    snapshot.has_visible_target = vision_state->current_target_candidate != 0;
    snapshot.target_id = vision_state->current_target_candidate;
    if (snapshot.target_id != 0) {
        // Still a kernel call, and still per agent -- but only for an agent that
        // has a target, and it reads a hitbox offset the actor snapshot does not
        // carry. Deriving it here would duplicate the kernel's aim maths.
        snapshot.target_position = vision_state->last_known_target_position;
        get_entity_aim_point(
            kernel,
            snapshot.target_id,
            &snapshot.target_position);
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
