#ifndef GAME_SERVER_AI_PERCEPTION_ADAPTER_H_
#define GAME_SERVER_AI_PERCEPTION_ADAPTER_H_

#include <cstdint>

#include "ai_context.h"
#include "game_server/agent_runtime.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

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
