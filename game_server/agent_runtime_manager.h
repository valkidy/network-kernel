#ifndef GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
#define GAME_SERVER_AGENT_RUNTIME_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/agent_sentry_controller.h"
#include "game_server/agent_runtime.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class AgentRuntimeManager {
public:
    explicit AgentRuntimeManager(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    bool preload_directors();
    void despawn_all(std::uint32_t reason);

    std::size_t agent_count() const;
    const std::vector<AgentRuntimeState>& agents() const;

private:
    bool spawn_director(const EntityTemplateConfig& director_template);
    void sync_agents_from_kernel();
    bool apply_weapon_mechanics(std::uint32_t net_id) const;
    bool has_live_agent_or_director() const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    AgentSentryController sentry_;
    std::vector<AgentRuntimeState> agents_;
    std::vector<std::uint32_t> director_net_ids_;
    bool director_preload_attempted_ = false;
    bool director_preload_succeeded_ = false;
    bool despawn_pending_ = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
