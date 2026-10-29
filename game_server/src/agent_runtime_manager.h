#ifndef GAME_SERVER_AGENT_RUNTIME_MANAGER_H_
#define GAME_SERVER_AGENT_RUNTIME_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/src/agent_behavior.h"
#include "game_server/src/agent_chaser_controller.h"
#include "game_server/src/agent_sentry_controller.h"
#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/agent_runtime.h"
#include "game_server/src/game_rule_director.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/patrol_director.h"
#include "game_server/src/shelter_director.h"
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
    // Switches an agent to another behavior at runtime (docs/AI_BEHAVIOR_PLAN.md
    // B3, B10): checked against what the behavior needs first, then applied at
    // once -- whatever it was fighting is dropped. The chaser runs it from the
    // next tick. A sentry, and a squad member, keep what they are.
    BehaviorSwitchResult set_behavior(
        std::uint32_t net_id,
        const AgentControllerSpec& spec);
    // Who is inside which building, so hunters can leave alone a player a
    // tent hides (AI plan Q3). Not owned; null leaves every player huntable.
    void set_shelter_director(const ShelterDirector* shelter) { shelter_ = shelter; }
    // The squads this server is running. Empty until something creates one;
    // ticking them is already wired, so creating one is all that is left.
    PatrolGroupRuntime& patrol_groups();
    const PatrolGroupRuntime& patrol_groups() const;
    const PatrolDirector& patrol_director() const;
    const PatrolNavigation& patrol_navigation() const;
    const WorldRuleDirector& world_rule_director() const;
    const GameRuleDirector& game_rule_director() const;
    const SpawnerDirector& spawner_director() const;
    // Who carried an on_alert spawner and was engaged at the end of the last
    // tick. The spawner reads this on the next one.
    const std::vector<AlertSignal>& alert_signals() const;

private:
    // One controller per agent actor template and role. Agents from different
    // templates share the runtime list but never each other's tuning, so the
    // list is split into per-binding batches before the controllers run.
    struct AgentControllerBinding {
        std::uint32_t actor_template_id = 0;
        AgentControllerRole role = AgentControllerRole::kNone;
        AgentSentryController sentry;
        AgentChaserController chaser;
        // Pointers into agents_, so a controller writes straight through to the
        // entry it was given. Rebuilt every tick; never outlives one.
        AgentBatch batch;
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
    // Takes the walks a director handed over this tick and binds each to the
    // agent it names. Runs after the resync, because the agent was created by
    // that same director moments ago and is not in the list until then.
    void attach_pending_entries();
    // Binds the routes game_rule spawns handed over to their agents, expanded
    // over the navmesh. Same timing as attach_pending_entries.
    void attach_pending_controllers();
    void use_item_skills();
    // Whether a building that hides its occupants holds `net_id` now.
    bool hidden_in_shelter(std::uint32_t net_id) const;
    void update_hunters(const ActorStateView& actors);
    BehaviorSwitchResult check_behavior(
        const AgentRuntimeState& agent,
        const AgentControllerSpec& spec) const;
    // Leaves the old behavior, enters the new one and applies the spec's data.
    // Unchecked: a spawn source's spec was checked by the catalog. A spawner's
    // agent passes its carrier as `source_net_id`, whose target an inherited
    // hunter takes.
    void apply_behavior(
        AgentRuntimeState* agent,
        const AgentControllerSpec& spec,
        std::uint32_t source_net_id);
    void attach_route(
        AgentRuntimeState* agent,
        const std::vector<KernelVec3>& waypoints);
    // Drives everyone still walking out of whatever spawned them: one input per
    // tick towards the door, and the template's own movement mask back the
    // moment they are through it or out of time.
    void tick_entries();
    void build_controllers();
    // Takes the tick's perception frame off `actors` first: the controllers used
    // to ask the kernel per agent for the vision state and for the entity state
    // this snapshot already holds.
    void dispatch_controllers(const ActorStateView& actors, float delta_seconds);
    // After the controllers: who is engaged now, among agents that carry an
    // on_alert spawner. Rebuilt every tick, and empty -- at no cost -- in a
    // catalog that has none.
    void collect_alert_signals();
    AgentControllerBinding* binding_for(
        std::uint32_t actor_template_id,
        AgentControllerRole role);
    AgentControllerBinding make_binding(
        const ActorTemplateConfig& actor_template,
        AgentControllerRole role) const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    std::vector<AgentControllerBinding> controllers_;
    std::vector<AgentRuntimeState> agents_;
    // Always describes agents_. Rebuilt wherever that list is replaced,
    // shortened or cleared -- the resync, a destroy event, and despawn_all --
    // rather than by whoever is about to read it.
    AgentIndex agent_index_;
    PatrolGroupRuntime patrol_groups_;
    PatrolDirector patrol_director_;
    PatrolNavigation patrol_navigation_;
    WorldRuleDirector world_rule_director_;
    GameRuleDirector game_rule_director_;
    std::vector<SpawnedControllerSpec> pending_controllers_;
    // The hunters' broad phase, rebuilt every tick (update_hunters).
    struct Hostile {
        std::uint32_t net_id = 0;
        KernelVec3 position{0.0f, 0.0f, 0.0f};
    };
    std::vector<Hostile> hostiles_;
    // Players a tent hides this tick: out of the table, but not gone.
    std::vector<std::uint32_t> hidden_hostiles_;
    const ShelterDirector* shelter_ = nullptr;
    SpawnerDirector spawner_director_;
    // Templates whose spawner calls on alert. A handful at most, so a scan
    // beats a set; empty means the signal pass never walks the agents at all.
    std::vector<std::uint32_t> alert_carrier_template_ids_;
    std::vector<AlertSignal> alert_signals_;
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
