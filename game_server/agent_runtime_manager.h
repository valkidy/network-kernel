#ifndef GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
#define GAME_SERVER_AGENT_RUNTIME_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/agent_chaser_controller.h"
#include "game_server/agent_sentry_controller.h"
#include "game_server/agent_runtime.h"
#include "game_server/gameplay_config.h"
#include "game_server/patrol_director.h"
#include "game_server/patrol_group_runtime.h"
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
    // The squads this server is running. Empty until something creates one;
    // ticking them is already wired, so creating one is all that is left.
    PatrolGroupRuntime& patrol_groups();
    const PatrolGroupRuntime& patrol_groups() const;
    const PatrolDirector& patrol_director() const;

private:
    // One controller per agent actor template. Agents from different templates
    // share the runtime list but never each other's tuning, so the list is
    // split into per-template batches before the controllers run.
    struct AgentControllerBinding {
        std::uint32_t actor_template_id = 0;
        std::uint32_t ai_controller_type = KernelAiControllerType_None;
        AgentSentryController sentry;
        AgentChaserController chaser;
        std::vector<AgentRuntimeState> batch;
    };

    bool spawn_director(const EntityTemplateConfig& director_template);
    void sync_agents_from_kernel();
    bool apply_weapon_mechanics(
        std::uint32_t net_id,
        std::uint32_t actor_template_id) const;
    bool has_live_agent_or_director() const;
    // Fills `buffer` with every actor state the kernel holds, growing it as
    // needed, and returns how many it wrote.
    std::uint32_t query_actor_states(
        std::vector<KernelServerEntityState>* buffer) const;
    void build_controllers();
    void dispatch_controllers(float delta_seconds);
    AgentControllerBinding* binding_for(std::uint32_t actor_template_id);

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    std::vector<AgentControllerBinding> controllers_;
    // Serves agents whose template carries no controller of its own; keeps the
    // previous single-controller behavior for anything unrecognized.
    std::size_t fallback_controller_index_ = 0;
    std::vector<AgentRuntimeState> agents_;
    PatrolGroupRuntime patrol_groups_;
    PatrolDirector patrol_director_;
    // Kept across ticks so that a population which has already been sized for
    // does not reallocate every tick.
    mutable std::vector<KernelServerEntityState> actor_query_buffer_;
    std::vector<std::uint32_t> director_net_ids_;
    bool director_preload_attempted_ = false;
    bool director_preload_succeeded_ = false;
    bool despawn_pending_ = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
