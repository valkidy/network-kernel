#include "game_server/src/agent_runtime_manager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// Where the actor query starts, not where it stops. It grows to fit the
// population; the ceiling only exists so that a query which somehow never comes
// back short cannot grow without bound.
constexpr std::size_t kInitialQueriedActors = 128;
constexpr std::size_t kMaxQueriedActors = 65536;
// The same tolerance the catalog validates every exit against, so "far enough
// out" and "close enough to stop" cannot drift apart.
constexpr float kEntryArrivalMeters = kSpawnerEntryArrivalMeters;

// Resolved from the agent's OWN actor template, not from the catalog's single
// `enemy:` entry. Holding one controller-wide config meant an agent spawned by a
// game-rule wave silently ran whichever template `enemy:` named -- a
// passive_patrol walker spawned alongside a stationary artillery sentry never
// patrolled, because the shared config came from the sentry.
AgentSentryConfig agent_sentry_config(
    const GameServerGameplayConfig& config,
    std::uint32_t actor_template_id) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, actor_template_id);
    if (actor_template == nullptr) {
        return AgentSentryConfig{};
    }

    AgentSentryConfig sentry = actor_template->sentry;
    if (sentry.weapon_id > UINT8_MAX) {
        return sentry;
    }
    const auto weapon_id = static_cast<std::uint8_t>(sentry.weapon_id);
    if (!config.weapons.configured[weapon_id]) {
        return sentry;
    }
    const std::uint32_t projectile_template_id =
        config.weapons.definitions[weapon_id].projectile_template_id;
    const auto projectile = std::find_if(
        config.projectile_templates.begin(),
        config.projectile_templates.end(),
        [projectile_template_id](const ProjectileTemplateConfig& candidate) {
            return candidate.definition.projectile_template_id ==
                   projectile_template_id;
        });
    if (projectile == config.projectile_templates.end() ||
        projectile->definition.mechanics.motion_model !=
            KernelProjectileMotionModel_Parabolic) {
        return sentry;
    }

    sentry.ballistic_aim.enabled = true;
    sentry.ballistic_aim.speed = projectile->definition.mechanics.speed;
    sentry.ballistic_aim.gravity = projectile->definition.mechanics.gravity;
    sentry.ballistic_aim.lifetime_ticks =
        projectile->definition.mechanics.lifetime_ticks;
    return sentry;
}

}  // namespace

AgentRuntimeManager::AgentRuntimeManager(
    KernelHandle* kernel,
    GameServerGameplayConfig config)
    : kernel_(kernel), config_(std::move(config)) {
    build_controllers();
    patrol_director_ = PatrolDirector(config_.patrols, config_.patrol_budget);
    // The preload list is what makes a director active; the config carries
    // every authored one so that the list stays the live decision.
    const auto preloaded = [this](std::uint32_t template_id) {
        return std::find(
                   config_.preload_director_template_ids.begin(),
                   config_.preload_director_template_ids.end(),
                   template_id) !=
            config_.preload_director_template_ids.end();
    };
    std::vector<WorldRuleSpawnConfig> world_rules;
    for (const WorldRuleSpawnConfig& rule : config_.world_rule_spawns) {
        if (preloaded(rule.director_template_id)) {
            world_rules.push_back(rule);
        }
    }
    std::vector<GameRuleConfig> game_rules;
    for (const GameRuleConfig& rule : config_.game_rules) {
        if (preloaded(rule.director_template_id)) {
            game_rules.push_back(rule);
        }
    }
    world_rule_director_ = WorldRuleDirector(std::move(world_rules));
    game_rule_director_ = GameRuleDirector(std::move(game_rules));
    // Not filtered by the preload list: a spawner is active because its
    // carrier is in the world, which is the whole point of putting the rule
    // on the carrier.
    spawner_director_ = SpawnerDirector(config_.spawner_carriers);
    if (!config_.navigation_mesh.artifact.empty()) {
        std::string error;
        if (patrol_navigation_.load(config_.navigation_mesh.artifact, &error)) {
            spdlog::info(
                "patrol navigation loaded entry={} bytes={}",
                config_.navigation_mesh.entry_path,
                config_.navigation_mesh.artifact.size());
        } else {
            // Not fatal: patrol routes fall back to a straight chord, which is
            // what they were before a navmesh was loaded at all. Fatal would
            // take the whole server down over a feature that degrades.
            spdlog::error(
                "patrol navigation failed entry={} reason={}",
                config_.navigation_mesh.entry_path,
                error);
        }
    }
}

void AgentRuntimeManager::build_controllers() {
    controllers_.clear();
    for (const ActorTemplateConfig& actor_template : config_.actor_templates) {
        if (actor_template.actor_type != kActorTypeAgent) {
            continue;
        }
        AgentControllerBinding binding;
        binding.actor_template_id = actor_template.actor_template_id;
        binding.ai_controller_type = actor_template.ai_controller_type;
        // Only the chaser carries tuning here; the sentry controller is
        // stateless and reads each agent's own AgentRuntimeState::sentry_config,
        // so a binding exists purely to route agents to the right controller.
        if (actor_template.ai_controller_type ==
            KernelAiControllerType_Chaser) {
            AgentChaserConfig chaser;
            chaser.sentry =
                agent_sentry_config(config_, actor_template.actor_template_id);
            chaser.chase = actor_template.chaser;
            chaser.patrol = actor_template.patrol;
            binding.chaser = AgentChaserController(chaser);
        }
        controllers_.push_back(std::move(binding));
    }

}

AgentRuntimeManager::AgentControllerBinding* AgentRuntimeManager::binding_for(
    std::uint32_t actor_template_id) {
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.actor_template_id == actor_template_id) {
            return &binding;
        }
    }
    // No fallback. build_controllers makes a binding for every agent actor
    // template in the catalog, so an agent with no binding was spawned from a
    // template the catalog does not have -- which the kernel would have
    // refused. The fallback that used to sit here was chosen by the `enemy:`
    // block and could never be reached.
    return nullptr;
}

void AgentRuntimeManager::dispatch_controllers(
    const ActorStateView& actors,
    float delta_seconds) {
    perception_frame_.refresh(kernel_, actors);
    for (AgentControllerBinding& binding : controllers_) {
        binding.batch.clear();
    }
    for (AgentRuntimeState& agent : agents_) {
        if (agent.entry.active) {
            // Still walking out. The entry pass has already submitted this
            // agent's input, and only the highest input_seq of a tick survives
            // -- a controller adding its own would win that tie and steer the
            // unit back into the building it is halfway out of.
            continue;
        }
        AgentControllerBinding* binding = binding_for(agent.actor_template_id);
        if (binding == nullptr) {
            continue;
        }
        binding->batch.push_back(&agent);
    }
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.batch.empty()) {
            continue;
        }
        if (binding.ai_controller_type == KernelAiControllerType_Chaser) {
            binding.chaser.tick(
                kernel_, perception_frame_, binding.batch, delta_seconds);
        } else {
            binding.sentry.tick(
                kernel_, perception_frame_, binding.batch, delta_seconds);
        }
    }
    // No write-back pass. Controllers never add or drop agents, and a batch now
    // points at the entries rather than holding copies of them, so what a
    // controller wrote is already in agents_.
}

void AgentRuntimeManager::handle_event(const KernelEvent& event) {
    if (event.type != KernelEventType_EntityDestroyed) {
        return;
    }
    agents_.erase(
        std::remove_if(
            agents_.begin(),
            agents_.end(),
            [&event](const AgentRuntimeState& agent) {
                return agent.net_id == event.net_id;
            }),
        agents_.end());
    // Every entry after the removed one has shifted, so the index describes the
    // wrong agents until it is rebuilt. This runs between ticks, not inside
    // one, so the next resync would otherwise read it stale.
    agent_index_.rebuild(agents_);
}

void AgentRuntimeManager::attach_pending_entries() {
    for (const SpawnerEntryRequest& request :
         spawner_director_.take_pending_entries()) {
        const std::size_t index = agent_index_.find(request.net_id);
        if (index == AgentIndex::kNotFound) {
            // Gone within the tick that created it, or not an agent at all.
            // Nothing here can drive it and nothing will put its mask back, so
            // it is worth a line rather than a silent ghost walking through
            // walls for the rest of its life.
            spdlog::warn("entry request has no agent net_id={}", request.net_id);
            continue;
        }
        AgentRuntimeState& agent = agents_[index];
        const ActorTemplateConfig* actor_template =
            find_actor_template(config_, agent.actor_template_id);
        agent.entry.active = true;
        agent.entry.exit = request.exit;
        agent.entry.hold_ticks = request.hold_ticks;
        agent.entry.remaining_ticks = request.max_ticks;
        // Read once, here: the walk has to put back exactly what this unit
        // would have been blocked by had it never been inside anything, and
        // zero -- the engine default -- is one of the legal answers.
        agent.entry.restore_movement_collision_mask = actor_template == nullptr
            ? 0u
            : actor_template->movement_collision_mask;
    }
}

void AgentRuntimeManager::tick_entries() {
    for (AgentRuntimeState& agent : agents_) {
        if (!agent.entry.active) {
            continue;
        }
        // Horizontal only. The door is a place on the floor, and a unit still
        // settling onto the ground must not read as short of it.
        const float to_exit_x = agent.entry.exit.x - agent.position.x;
        const float to_exit_z = agent.entry.exit.z - agent.position.z;
        const float distance =
            std::sqrt(to_exit_x * to_exit_x + to_exit_z * to_exit_z);
        const bool holding = agent.entry.hold_ticks > 0u;
        const bool arrived = distance <= kEntryArrivalMeters;
        const bool walking = !holding && !arrived;

        KernelPlayerInput input{};
        input.input_seq = agent.next_input_seq;
        if (walking) {
            input.move = KernelVec2{to_exit_x / distance, to_exit_z / distance};
        }
        // A zero aim_dir replicates as world +X, so even a unit waiting its turn
        // faces the door it is about to come out of rather than east.
        input.aim_dir = distance > 0.0001f
            ? KernelVec3{to_exit_x / distance, 0.0f, to_exit_z / distance}
            : KernelVec3{1.0f, 0.0f, 0.0f};
        // Submitted every tick, including the ones spent waiting: an agent that
        // sends nothing keeps the velocity it had, so a queue would coast out of
        // the carrier without ever having been told to walk.
        if (Kernel_ServerSubmitEntityInput(kernel_, agent.net_id, &input)) {
            ++agent.next_input_seq;
        }
        agent.animation_state = walking
            ? agent.sentry_config.animation_attack
            : agent.sentry_config.animation_idle;
        Kernel_ServerEnqueueEntityState(
            kernel_,
            KernelCommandSource_AI,
            agent.net_id,
            agent.animation_state,
            0);

        if (holding) {
            --agent.entry.hold_ticks;
            continue;
        }
        if (agent.entry.remaining_ticks > 0u) {
            --agent.entry.remaining_ticks;
        }
        if (!arrived && agent.entry.remaining_ticks > 0u) {
            continue;
        }
        // Through the door, or out of time. Either way the walk is over: put the
        // template's own mask back before the controllers get their hands on it,
        // or this unit spends the rest of the match walking through cover.
        if (!Kernel_ServerSetEntityMovementCollisionMask(
                kernel_,
                agent.net_id,
                agent.entry.restore_movement_collision_mask)) {
            spdlog::warn(
                "entry restore failed net_id={} mask={}",
                agent.net_id,
                agent.entry.restore_movement_collision_mask);
        }
        agent.entry = AgentEntryRuntimeState{};
    }
}

void AgentRuntimeManager::tick(float delta_seconds) {
    if (kernel_ == nullptr) {
        return;
    }
    if (despawn_pending_) {
        if (!has_live_agent()) {
            despawn_pending_ = false;
        } else {
            return;
        }
    }

    // One snapshot for the whole retirement pass, taken before any director has
    // run -- which is exactly what the per-squad queries used to see, because
    // retirement happens before this tick creates anything.
    ActorStateView actors = refresh_actor_states();

    // Ahead of the resync on purpose: the director creates entities, and the
    // group runtime drops members it cannot find in the agent list. Ticking it
    // after the resync would drop every member of a squad on the tick it was
    // spawned.
    const std::uint32_t spawned_groups_before =
        patrol_director_.spawned_group_count();
    patrol_director_.tick(kernel_, &patrol_groups_, &patrol_navigation_, actors);
    // Only on a tick a squad actually appeared, which is once per definition
    // per interval_ticks. The world rule counts every agent alive including the
    // ones just spawned -- counting the pre-spawn list would have it top up
    // against a population that already exists.
    if (patrol_director_.spawned_group_count() != spawned_groups_before) {
        actors = refresh_actor_states();
    }
    world_rule_director_.tick(kernel_, live_agent_count(actors));

    game_rule_director_.tick(kernel_);
    spawner_director_.tick(kernel_);
    // Re-taken because the three directors above all create, and the resync
    // exists to discover what they made -- that is the whole reason it runs
    // after them.
    actors = refresh_actor_states();
    sync_agents_from_kernel(actors);
    // After the resync, because the agent a walk names was created by one of
    // the directors above and is not in the list until now.
    attach_pending_entries();
    // Before the controllers, which skip anyone still walking: this is what
    // submits their input for the tick, and only one input per agent survives.
    tick_entries();
    // Ahead of the controllers, so a member reads the slot its squad wants it
    // in this tick rather than the one from last tick.
    patrol_groups_.tick(&agents_, agent_index_, delta_seconds);
    // Still on the post-director snapshot: nothing between it and here creates,
    // destroys or moves an entity, so it is what a per-agent query would return.
    dispatch_controllers(actors, delta_seconds);
}

void AgentRuntimeManager::despawn_all(std::uint32_t reason) {
    if (kernel_ == nullptr) {
        agents_.clear();
        agent_index_.rebuild(agents_);
        return;
    }

    for (const AgentRuntimeState& agent : agents_) {
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = agent.net_id;
        command.reason = reason;
        Kernel_ServerEnqueueEntityLifecycle(
            kernel_,
            KernelCommandSource_Internal,
            &command);
    }
    agents_.clear();
    agent_index_.rebuild(agents_);
    despawn_pending_ = true;
}

std::size_t AgentRuntimeManager::agent_count() const {
    return agents_.size();
}

const std::vector<AgentRuntimeState>& AgentRuntimeManager::agents() const {
    return agents_;
}

PatrolGroupRuntime& AgentRuntimeManager::patrol_groups() {
    return patrol_groups_;
}

const PatrolGroupRuntime& AgentRuntimeManager::patrol_groups() const {
    return patrol_groups_;
}

const PatrolDirector& AgentRuntimeManager::patrol_director() const {
    return patrol_director_;
}

const PatrolNavigation& AgentRuntimeManager::patrol_navigation() const {
    return patrol_navigation_;
}

const WorldRuleDirector& AgentRuntimeManager::world_rule_director() const {
    return world_rule_director_;
}

const GameRuleDirector& AgentRuntimeManager::game_rule_director() const {
    return game_rule_director_;
}

const SpawnerDirector& AgentRuntimeManager::spawner_director() const {
    return spawner_director_;
}

bool AgentRuntimeManager::preload_directors() {
    if (director_preload_attempted_) {
        return director_preload_succeeded_;
    }
    director_preload_attempted_ = true;

    for (const std::uint32_t template_id :
         config_.preload_director_template_ids) {
        const auto entity_template = std::find_if(
            config_.entity_templates.begin(),
            config_.entity_templates.end(),
            [template_id](const EntityTemplateConfig& candidate) {
                return candidate.actor_template_id == template_id;
            });
        if (entity_template == config_.entity_templates.end() ||
            entity_template->entity_type != KernelEntityType_Director) {
            spdlog::error(
                "director preload failed template_id={} reason=invalid_template",
                template_id);
            return false;
        }
    }

    director_preload_succeeded_ = true;
    spdlog::info(
        "directors ready world_rules={} game_rules={}",
        world_rule_director_.rules().size(),
        game_rule_director_.rules().size());
    return true;
}

void AgentRuntimeManager::sync_agents_from_kernel(const ActorStateView& actors) {
    std::vector<AgentRuntimeState> next_agents;
    next_agents.reserve(actors.count);
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        const KernelServerEntityState& state = actors.states[index];
        if (state.valid == 0u || state.actor_type != kActorTypeAgent) {
            continue;
        }
        // The index still describes the pre-resync list, which is exactly what
        // this lookup wants: "did I already know about this net_id".
        const std::size_t existing = agent_index_.find(state.net_id);
        const bool discovered_agent = existing == AgentIndex::kNotFound;
        AgentRuntimeState agent =
            discovered_agent ? AgentRuntimeState{} : agents_[existing];
        agent.net_id = state.net_id;
        agent.actor_template_id = state.actor_template_id;
        agent.position = state.position;
        agent.hp = state.hp;
        agent.max_hp = state.max_hp;
        agent.animation_state = state.animation_state;
        agent.sentry.self_id = state.net_id;
        if (discovered_agent) {
            agent.patrol_anchor = state.position;
            // An actor template is immutable, so this is resolved once rather
            // than every tick. state.actor_template_id is what the entity was
            // actually spawned from, which is the whole point: a wave can spawn
            // a template the catalog never names.
            agent.sentry_config =
                agent_sentry_config(config_, state.actor_template_id);
            apply_weapon_mechanics(state.net_id, state.actor_template_id);
        }
        next_agents.push_back(agent);
    }
    agents_ = std::move(next_agents);
    agent_index_.rebuild(agents_);
}

bool AgentRuntimeManager::apply_weapon_mechanics(
    std::uint32_t net_id,
    std::uint32_t actor_template_id) const {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, actor_template_id);
    if (actor_template == nullptr) {
        return false;
    }
    for (std::uint8_t slot = 0; slot < actor_template->weapon_slot_count; ++slot) {
        const KernelWeaponMechanicsDefinition& weapon =
            config_.weapons.definitions[actor_template->weapon_ids[slot]];
        if (!Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon)) {
            return false;
        }
    }
    return true;
}

std::uint32_t AgentRuntimeManager::live_agent_count(const ActorStateView& actors) {
    std::uint32_t agents = 0;
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        // No `valid` check on purpose; see the declaration.
        if (actors.states[index].actor_type == kActorTypeAgent) {
            ++agents;
        }
    }
    return agents;
}

ActorStateView AgentRuntimeManager::refresh_actor_states() const {
    const std::uint32_t count = query_actor_states(&actor_query_buffer_);
    return ActorStateView{actor_query_buffer_.data(), count};
}

bool AgentRuntimeManager::has_live_agent() const {
    std::vector<KernelServerEntityState> states;
    const std::uint32_t state_count = query_actor_states(&states);
    for (std::uint32_t index = 0; index < state_count; ++index) {
        if (states[index].valid != 0u &&
            states[index].actor_type == kActorTypeAgent) {
            return true;
        }
    }
    return false;
}

// Kernel_ServerQueryEntities reports how many states it wrote, never how many
// it had, and it stops writing when the buffer is full. A count that exactly
// fills the buffer is therefore indistinguishable from a truncated one, so the
// buffer is grown and the query repeated until it comes back short. Truncation
// here does not degrade anything gracefully: an agent the query drops is an
// agent the controllers never see, which stands still for as long as it stays
// dropped -- and which 128 of the actors get through was decided by world
// iteration order, so the same agent could be driven on one tick and frozen on
// the next.
std::uint32_t AgentRuntimeManager::query_actor_states(
    std::vector<KernelServerEntityState>* buffer) const {
    if (kernel_ == nullptr || buffer == nullptr) {
        return 0;
    }
    if (buffer->size() < kInitialQueriedActors) {
        buffer->resize(kInitialQueriedActors);
    }
    while (true) {
        for (KernelServerEntityState& state : *buffer) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel_,
            kEntityTypeActor,
            buffer->data(),
            static_cast<std::uint32_t>(buffer->size()));
        if (count < buffer->size() || buffer->size() >= kMaxQueriedActors) {
            return count;
        }
        buffer->resize(
            std::min(buffer->size() * 2, kMaxQueriedActors));
    }
}

}  // namespace network_example::game_server
