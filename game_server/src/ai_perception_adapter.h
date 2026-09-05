#ifndef GAME_SERVER_AI_PERCEPTION_ADAPTER_H_
#define GAME_SERVER_AI_PERCEPTION_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ai_context.h"
#include "game_server/src/agent_runtime.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

inline std::size_t find_weapon_slot(
    const KernelServerEntityState& state,
    std::uint8_t weapon_id) {
    for (std::size_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == weapon_id) {
            return slot;
        }
    }
    return KERNEL_MAX_WEAPON_SLOTS;
}

struct SentryPerceptionSnapshot {
    bool has_self_state = false;
    KernelServerEntityState self_state{};
    bool has_visible_target = false;
    std::uint32_t target_id = 0;
    bool has_target_position = false;
    KernelVec3 target_position{0.0f, 0.0f, 0.0f};
    KernelVec3 vision_forward{1.0f, 0.0f, 0.0f};
};

// One tick's perception inputs for the whole population, taken once instead of
// once per agent.
//
// Both halves used to be a kernel call inside the controller loop.
// `Kernel_QueryVisionState` answers an agent-id query by walking its entire
// vision-state map and discarding the misses, so every agent asking for its own
// state made the loop quadratic; and the agent's own KernelServerEntityState had
// already been fetched, for every actor, by the resync a few lines earlier in
// the same tick.
//
// Reading a frame is equivalent to querying per agent, not merely close to it:
// between the snapshot and the controllers nothing mutates what it holds. The
// controllers only enqueue commands -- the one call named `Submit` writes an
// input buffer, not entity state -- and vision is recomputed inside
// Kernel_Update.
class PerceptionFrame {
public:
    // `actors` is the tick's actor snapshot. The frame indexes it rather than
    // copying it, so it has to outlive every read of this frame.
    void refresh(KernelHandle* kernel, const ActorStateView& actors);
    // For callers that have no snapshot of their own and take one here. The
    // manager passes the one it already has; this exists for the tests and
    // benches that drive a controller outside a manager tick.
    void refresh(KernelHandle* kernel);

    const KernelServerEntityState* actor_state(std::uint32_t net_id) const;
    const KernelVisionStateView* vision_state(std::uint32_t net_id) const;

private:
    void index_actors(const ActorStateView& actors);
    void refresh_vision_states(KernelHandle* kernel);

    ActorStateView actors_{};
    // Owned only by the snapshot-less overload above; empty otherwise, and
    // never what `actors_` points at when the caller supplied its own.
    std::vector<KernelServerEntityState> owned_actors_;
    // Kept across ticks, like the manager's actor buffer, so a population that
    // has already been sized for does not reallocate every tick.
    std::vector<KernelVisionStateView> vision_buffer_;
    std::uint32_t vision_count_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> actor_by_net_id_;
    std::unordered_map<std::uint32_t, std::uint32_t> vision_by_net_id_;
};

class AiPerceptionAdapter {
public:
    static SentryPerceptionSnapshot build_sentry_snapshot(
        KernelHandle* kernel,
        const PerceptionFrame& frame,
        std::uint32_t agent_net_id);

    static ai::AIContext build_sentry_context(
        const SentryPerceptionSnapshot& snapshot,
        std::uint8_t weapon_id);
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AI_PERCEPTION_ADAPTER_H_
