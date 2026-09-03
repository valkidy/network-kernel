#ifndef GAME_SERVER_AI_PERCEPTION_ADAPTER_H_
#define GAME_SERVER_AI_PERCEPTION_ADAPTER_H_

#include <cstddef>
#include <cstdint>

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

class AiPerceptionAdapter {
public:
    static SentryPerceptionSnapshot build_sentry_snapshot(
        KernelHandle* kernel,
        std::uint32_t agent_net_id);

    static ai::AIContext build_sentry_context(
        const SentryPerceptionSnapshot& snapshot,
        std::uint8_t weapon_id);
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AI_PERCEPTION_ADAPTER_H_
