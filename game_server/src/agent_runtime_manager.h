#ifndef GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
#define GAME_SERVER_AGENT_RUNTIME_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/src/agent_chaser_controller.h"
#include "game_server/src/agent_sentry_controller.h"
#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/agent_runtime.h"
#include "game_server/src/game_rule_director.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_director.h"
#include "game_server/src/patrol_navigation.h"
#include "game_server/src/spawner_director.h"
#include "game_server/src/world_rule_director.h"
#include "game_server/src/patrol_group_runtime.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class AgentRuntimeManager {
public:
    explicit AgentRuntimeManager(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    // Validates the authored directors. Neither kind is an entity any more --
    // both are game_server config, built in the constructor -- so this creates
    // nothing; it is kept because a catalog naming a template that is not a
    // director is still a configuration error worth refusing to start on.
    bool preload_directors();
    void despawn_all(std::uint32_t reason);

    std::size_t agent_count() const;
    const std::vector<AgentRuntimeState>& agents() const;
    // The squads this server is running. Empty until something creates one;
    // ticking them is already wired, so creating one is all that is left.
    PatrolGroupRuntime& patrol_groups();
    const PatrolGroupRuntime& patrol_groups() const;
    const PatrolDirector& patrol_director() const;
    const PatrolNavigation& patrol_navigation() const;
    const WorldRuleDirector& world_rule_director() const;
    const GameRuleDirector& game_rule_director() const;
    const SpawnerDirector& spawner_director() const;

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

    void sync_agents_from_kernel(const ActorStateView& actors);
    bool apply_weapon_mechanics(
        std::uint32_t net_id,
        std::uint32_t actor_template_id) const;
    bool has_live_agent() const;
    // Every agent entity in `actors`, counted the way the kernel's own
    // world-rule director counted: off the entity list, with no validity
    // filter. An agent created this tick is not valid until physics finalises,
    // so a count that skipped those would have the rule spawn a replacement for
    // the agent it just made.
    static std::uint32_t live_agent_count(const ActorStateView& actors);
    // Re-takes this tick's actor snapshot into actor_query_buffer_ and returns a
    // view of it. Called at the top of the tick and again after the directors
    // have created whatever they are going to; every phase in between reads the
    // view instead of asking the kernel again.
    ActorStateView refresh_actor_states() const;
    // Fills `buffer` with every actor state the kernel holds, growing it as
    // needed, and returns how many it wrote.
    std::uint32_t query_actor_states(
        std::vector<KernelServerEntityState>* buffer) const;
    void build_controllers();
    // Takes the tick's perception frame off `actors` first: the controllers used
    // to ask the kernel per agent for the vision state and for the entity state
    // this snapshot already holds.
    void dispatch_controllers(const ActorStateView& actors, float delta_seconds);
    AgentControllerBinding* binding_for(std::uint32_t actor_template_id);

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    std::vector<AgentControllerBinding> controllers_;
    std::vector<AgentRuntimeState> agents_;
    PatrolGroupRuntime patrol_groups_;
    PatrolDirector patrol_director_;
    PatrolNavigation patrol_navigation_;
    WorldRuleDirector world_rule_director_;
    GameRuleDirector game_rule_director_;
    SpawnerDirector spawner_director_;
    // Kept across ticks so that a population which has already been sized for
    // does not reallocate every tick.
    mutable std::vector<KernelServerEntityState> actor_query_buffer_;
    // Kept across ticks for the same reason the buffer above is: its vision
    // buffer and net_id indices are re-filled every tick, not re-allocated.
    PerceptionFrame perception_frame_;
    bool director_preload_attempted_ = false;
    bool director_preload_succeeded_ = false;
    bool despawn_pending_ = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
