#include "game_server/src/agent_runtime_manager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "game_server/src/agent_steering.h"
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

void take_controllers(
    std::vector<SpawnedControllerSpec> taken,
    std::vector<SpawnedControllerSpec>* pending) {
    pending->insert(
        pending->end(),
        std::make_move_iterator(taken.begin()),
        std::make_move_iterator(taken.end()));
}

// Guard, patrol, squad and hunter are all the chaser; everything else, including an agent
// whose template has no ai block, is the sentry, as it always was.
bool runs_chaser(AgentControllerRole role) {
    return role == AgentControllerRole::kGuard ||
           role == AgentControllerRole::kPatrol ||
           role == AgentControllerRole::kSquad ||
           role == AgentControllerRole::kHunter ||
           role == AgentControllerRole::kLockTarget;
}

// How to fire one weapon: its own ballistic aim when its projectile arcs, and
// its own charge time when its fire action charges.
ActorIntentExecutorConfig weapon_executor_config(
    const GameServerGameplayConfig& config,
    std::uint16_t weapon_id) {
    ActorIntentExecutorConfig executor;
    executor.weapon_id = weapon_id;
    executor.charge_ticks = weapon_charge_ticks(config, weapon_id);
    if (weapon_id > UINT8_MAX) {
        return executor;
    }
    const auto weapon = static_cast<std::uint8_t>(weapon_id);
    if (!config.weapons.configured[weapon]) {
        return executor;
    }
    const std::uint32_t projectile_template_id =
        config.weapons.definitions[weapon].projectile_template_id;
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
        return executor;
    }
    executor.ballistic_aim.enabled = true;
    executor.ballistic_aim.speed = projectile->definition.mechanics.speed;
    executor.ballistic_aim.gravity = projectile->definition.mechanics.gravity;
    executor.ballistic_aim.lifetime_ticks =
        projectile->definition.mechanics.lifetime_ticks;
    return executor;
}

// Resolved from the agent's OWN actor template, not from the catalog's single
// `enemy:` entry. Holding one controller-wide config meant an agent spawned by a
// game-rule wave silently ran whichever template `enemy:` named -- a
// walker spawned alongside a stationary artillery sentry never walked, because
// the shared config came from the sentry.
AgentSentryConfig agent_sentry_config(
    const GameServerGameplayConfig& config,
    std::uint32_t actor_template_id) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, actor_template_id);
    if (actor_template == nullptr) {
        return AgentSentryConfig{};
    }
    AgentSentryConfig sentry = actor_template->sentry;
    const ActorIntentExecutorConfig executor =
        weapon_executor_config(config, sentry.weapon_id);
    sentry.charge_ticks = executor.charge_ticks;
    sentry.ballistic_aim = executor.ballistic_aim;
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
    spawner_director_ = SpawnerDirector(
        config_.spawner_carriers, config_.reinforce_budget.max_live_agents);
    alert_carrier_template_ids_.clear();
    for (const SpawnerCarrierConfig& carrier : config_.spawner_carriers) {
        if (carrier.spawner.trigger == SpawnerTrigger::kOnAlert) {
            alert_carrier_template_ids_.push_back(carrier.entity_template_id);
        }
    }
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
        controllers_.push_back(
            make_binding(actor_template, actor_template.default_controller_role));
    }
}

AgentRuntimeManager::AgentControllerBinding AgentRuntimeManager::make_binding(
    const ActorTemplateConfig& actor_template,
    AgentControllerRole role) const {
    {
        AgentControllerBinding binding;
        binding.actor_template_id = actor_template.actor_template_id;
        binding.role = role;
        // Only the chaser carries tuning here; the sentry controller is
        // stateless and reads each agent's own AgentRuntimeState::sentry_config,
        // so a binding exists purely to route agents to the right controller.
        if (runs_chaser(binding.role)) {
            AgentChaserConfig chaser;
            chaser.sentry =
                agent_sentry_config(config_, actor_template.actor_template_id);
            chaser.chase = actor_template.chaser;
            chaser.squad = actor_template.squad;
            chaser.guard = actor_template.guard;
            chaser.patrol = actor_template.patrol;
            for (const AgentSkillConfig& skill : actor_template.skills) {
                if (skill.type != AgentSkillType::kWeapon) {
                    continue;
                }
                AgentWeaponSkill weapon;
                weapon.executor = weapon_executor_config(
                    config_, static_cast<std::uint16_t>(skill.weapon_id));
                weapon.range_min_meters = skill.range_min_meters;
                weapon.range_max_meters = skill.range_max_meters;
                weapon.cooldown_ticks = skill.cooldown_ticks;
                weapon.weight = skill.weight;
                chaser.weapon_skills.push_back(weapon);
            }
            chaser.gravity_y = actor_template.movement_gravity.y;
            chaser.hovers = actor_template.movement_controller_type ==
                KernelMovementControllerType_Hover;
            binding.chaser = AgentChaserController(chaser);
        }
        return binding;
    }
}

AgentRuntimeManager::AgentControllerBinding* AgentRuntimeManager::binding_for(
    std::uint32_t actor_template_id,
    AgentControllerRole role) {
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.actor_template_id == actor_template_id &&
            binding.role == role) {
            return &binding;
        }
    }
    // build_controllers made one per template for its default role; a spawn
    // source that chose another role (a squad's patrol) gets its binding the
    // first time one of its agents is ticked. A template the catalog does not
    // have still gets none -- the kernel would have refused to spawn it.
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, actor_template_id);
    if (actor_template == nullptr) {
        return nullptr;
    }
    controllers_.push_back(make_binding(*actor_template, role));
    return &controllers_.back();
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
        AgentControllerBinding* binding =
            binding_for(agent.actor_template_id, agent.controller_role);
        if (binding == nullptr) {
            continue;
        }
        binding->batch.push_back(&agent);
    }
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.batch.empty()) {
            continue;
        }
        if (runs_chaser(binding.role)) {
            binding.chaser.tick(
                kernel_, perception_frame_, binding.batch, delta_seconds,
                &patrol_navigation_);
        } else {
            binding.sentry.tick(
                kernel_, perception_frame_, binding.batch, delta_seconds);
        }
    }
    // No write-back pass. Controllers never add or drop agents, and a batch now
    // points at the entries rather than holding copies of them, so what a
    // controller wrote is already in agents_.
}

void AgentRuntimeManager::collect_alert_signals() {
    alert_signals_.clear();
    if (alert_carrier_template_ids_.empty()) {
        return;
    }
    for (AgentRuntimeState& agent : agents_) {
        if (std::find(
                alert_carrier_template_ids_.begin(),
                alert_carrier_template_ids_.end(),
                agent.actor_template_id) == alert_carrier_template_ids_.end()) {
            continue;
        }
        // Still walking out: no controller ran, so its state says nothing
        // about this tick. It cannot be engaged yet anyway.
        if (agent.entry.active) {
            continue;
        }
        const bool engaged = is_engaged(agent.sentry.state);
        const bool started = engaged && !agent.engaged_last_tick;
        agent.engaged_last_tick = engaged;
        if (!engaged) {
            continue;
        }
        AlertSignal signal;
        signal.net_id = agent.net_id;
        signal.entity_template_id = agent.actor_template_id;
        signal.engagement_started = started;
        // Off the frame the controllers decided on, not off their counters:
        // lost_target_ticks is zeroed by every state transition too, so attack
        // giving up into alert reads as "in sight" for a tick it is not.
        const KernelVisionStateView* vision =
            perception_frame_.vision_state(agent.net_id);
        signal.sees_target =
            vision != nullptr && vision->current_target_candidate != 0u;
        alert_signals_.push_back(signal);
    }
}

const std::vector<AlertSignal>& AgentRuntimeManager::alert_signals() const {
    return alert_signals_;
}

void AgentRuntimeManager::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_Disconnected) {
        // Its command window goes with it; a peer id may be reused.
        command_windows_.erase(event.peer_id);
        return;
    }
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

// Item skills (AI plan P5, D15, D18). The agent uses the item on itself the
// way a player would -- a consume request, here sent by the server, which owns
// every agent -- so its on_item_used graph runs, statuses land and the client
// sees the same thing. It never runs out: an item used up is replaced before
// the next use.
void AgentRuntimeManager::use_item_skills() {
    std::vector<KernelStatusEffectView> statuses;
    for (AgentRuntimeState& agent : agents_) {
        if (!agent.has_item_skills || agent.entry.active) {
            continue;
        }
        const ActorTemplateConfig* actor_template =
            find_actor_template(config_, agent.actor_template_id);
        if (actor_template == nullptr) {
            continue;
        }
        for (std::uint32_t& cooldown : agent.item_cooldown_ticks) {
            if (cooldown > 0u) --cooldown;
        }
        const AgentSentryState state = agent.sentry.state;
        const bool spawned = !agent.items_seen_spawn;
        const bool entered = state != agent.items_last_state;
        agent.items_seen_spawn = true;
        agent.items_last_state = state;
        const bool damaged = perception_frame_.damage_to(agent.net_id) != nullptr;

        if (agent.item_container == 0u) {
            std::uint32_t capacity = 0;
            for (const AgentSkillConfig& skill : actor_template->skills) {
                if (skill.type == AgentSkillType::kItem) ++capacity;
            }
            if (!Kernel_ServerCreateInventoryContainer(
                    kernel_, agent.net_id, capacity, &agent.item_container)) {
                agent.has_item_skills = false;
                spdlog::warn("item skills: no inventory for net_id={}", agent.net_id);
                continue;
            }
        }

        for (std::size_t index = 0; index < actor_template->skills.size(); ++index) {
            const AgentSkillConfig& skill = actor_template->skills[index];
            if (skill.type != AgentSkillType::kItem ||
                agent.item_cooldown_ticks[index] > 0u ||
                (skill.once && (agent.items_used_once & (1u << index)) != 0u)) {
                continue;
            }
            if ((skill.has_when_state && state != skill.when_state) ||
                (skill.has_on_state_enter && !(entered && state == skill.on_state_enter)) ||
                (skill.on_spawn && !spawned) || (skill.on_damaged && !damaged) ||
                (skill.hp_below > 0.0f &&
                 !(agent.max_hp > 0u &&
                   static_cast<float>(agent.hp) <
                       skill.hp_below * static_cast<float>(agent.max_hp)))) {
                continue;
            }
            if (skill.missing_status_id != 0u) {
                const std::uint32_t count =
                    Kernel_QueryStatusEffects(kernel_, agent.net_id, nullptr, 0u);
                statuses.assign(count, KernelStatusEffectView{});
                for (KernelStatusEffectView& view : statuses) {
                    view.struct_size = sizeof(KernelStatusEffectView);
                }
                (void)Kernel_QueryStatusEffects(kernel_, agent.net_id, statuses.data(), count);
                if (std::any_of(statuses.begin(), statuses.end(),
                                [&](const KernelStatusEffectView& view) {
                                    return view.status_effect_id == skill.missing_status_id;
                                })) {
                    continue;
                }
            }

            // Refilled, not kept: a fungible stack runs down and an empty one
            // is destroyed, so whatever is not there with some left is made.
            KernelItemInstanceView held{};
            held.struct_size = sizeof(held);
            if (agent.item_instances[index] == 0u ||
                !Kernel_GetItemInstance(kernel_, agent.item_instances[index], &held) ||
                held.quantity == 0u) {
                agent.item_instances[index] = 0u;
                if (!Kernel_ServerCreateInventoryItem(
                        kernel_, skill.item_template_id, 1u, agent.item_container,
                        &agent.item_instances[index])) {
                    spdlog::warn("item skills: could not stock {} for net_id={}",
                                 skill.item_ref, agent.net_id);
                    continue;
                }
            }
            KernelGameplayRequest request{};
            request.struct_size = sizeof(request);
            request.requester_peer = 0u;
            // Unique per agent: the kernel refuses a request id it has seen.
            request.request_id = (static_cast<std::uint64_t>(agent.net_id) << 32u) |
                agent.next_item_request++;
            request.instigator_net_id = agent.net_id;
            request.domain_action = KernelDomainAction_Consume;
            request.selected_item_instance_id = agent.item_instances[index];
            request.requested_quantity = 1u;
            if (Kernel_ServerSubmitGameplayRequest(kernel_, &request)) {
                agent.item_cooldown_ticks[index] = skill.cooldown_ticks;
                if (skill.once) agent.items_used_once |= 1u << index;
            }
        }
    }
}

// The broad phase (AI plan D10): where every hostile is, read once per tick off
// the snapshot the manager already has and shared by every hunter, which
// cheats its way to its target through it. Seeing that target -- the narrow
// phase -- is still vision's. Only the living: a dead player is not hunted.
// Each hunter reads the table of what is hostile to its own side, by the same
// rule vision uses (kernel classify_agent_relation).
void AgentRuntimeManager::update_hunters(const ActorStateView& actors) {
    hostiles_of_enemy_side_.clear();
    hostiles_of_player_side_.clear();
    hidden_hostiles_.clear();
    bool player_side_hunters = false;
    for (const AgentRuntimeState& agent : agents_) {
        player_side_hunters = player_side_hunters ||
            (agent.controller_role == AgentControllerRole::kHunter &&
             camp_of(agent.actor_template_id) == KernelAgentCamp_PlayerSide);
    }
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        const KernelServerEntityState& state = actors.states[index];
        if (state.valid == 0u || state.hp == 0u ||
            (state.visual_flags & KERNEL_VISUAL_FLAG_DEAD) != 0u) {
            continue;
        }
        if (state.actor_type == kActorTypePlayer) {
            // A tent that hides its occupants hides them from the cheat too,
            // as it does from every cone (Q3).
            if (hidden_in_shelter(state.net_id)) {
                hidden_hostiles_.push_back(state.net_id);
            } else {
                hostiles_of_enemy_side_.push_back(Hostile{state.net_id, state.position});
            }
        } else if (state.actor_type == kActorTypeAgent) {
            const std::uint8_t camp = camp_of(state.actor_template_id);
            if (camp == KernelAgentCamp_PlayerSide) {
                hostiles_of_enemy_side_.push_back(Hostile{state.net_id, state.position});
            } else if (camp == KernelAgentCamp_EnemySide && player_side_hunters) {
                hostiles_of_player_side_.push_back(Hostile{state.net_id, state.position});
            }
        }
    }
    const auto distance = [](const KernelVec3& a, const KernelVec3& b) {
        return std::hypot(a.x - b.x, a.z - b.z);
    };
    // Set per hunter, to the table of its own side.
    const std::vector<Hostile>* hostiles = &hostiles_of_enemy_side_;
    const auto nearest = [&](const KernelVec3& from) -> const Hostile* {
        const Hostile* best = nullptr;
        for (const Hostile& hostile : *hostiles) {
            if (best == nullptr ||
                distance(from, hostile.position) < distance(from, best->position)) {
                best = &hostile;
            }
        }
        return best;
    };
    // D11: a hunter's cone is its narrow phase and costs a query a tick. With
    // the nearest hostile past the cone's reach and a margin -- the ground a
    // player covers before the next look -- there is nothing for it to see, so
    // it is switched off until there is.
    constexpr float kVisionMarginMeters = 10.0f;
    const auto set_vision = [&](AgentRuntimeState& agent, bool on) {
        if (agent.vision_off != on) {
            return;
        }
        const ActorTemplateConfig* actor_template =
            find_actor_template(config_, agent.actor_template_id);
        if (actor_template == nullptr) {
            return;
        }
        const bool done = on
            ? Kernel_ServerSetEntityVisionConfig(kernel_, agent.net_id, &actor_template->vision)
            : Kernel_ServerClearEntityVisionConfig(kernel_, agent.net_id);
        if (done) {
            agent.vision_off = !on;
        }
    };
    for (AgentRuntimeState& agent : agents_) {
        if (agent.controller_role != AgentControllerRole::kHunter || agent.entry.active) {
            if (agent.vision_off) set_vision(agent, true);
            continue;
        }
        hostiles = camp_of(agent.actor_template_id) == KernelAgentCamp_PlayerSide
            ? &hostiles_of_player_side_
            : &hostiles_of_enemy_side_;
        if (agent.vision_reach_meters > 0.0f) {
            const Hostile* closest = nearest(agent.position);
            set_vision(agent, closest != nullptr &&
                                  distance(agent.position, closest->position) <=
                                      agent.vision_reach_meters + kVisionMarginMeters);
        }
        const Hostile* target = nullptr;
        for (const Hostile& hostile : *hostiles) {
            if (hostile.net_id == agent.hunt_target) target = &hostile;
        }
        // Hidden is not gone: a player who ducks into a tent is still its
        // target. It goes after another it can find, or waits where it is
        // until this one comes out -- never on_target_lost, which would turn
        // a guard for good over a player stepping inside for a moment.
        if (target == nullptr &&
            std::find(hidden_hostiles_.begin(), hidden_hostiles_.end(), agent.hunt_target) !=
                hidden_hostiles_.end()) {
            const Hostile* other = nearest(agent.position);
            if (agent.hunter.target_source != HunterTargetSource::kNearestHostile ||
                other == nullptr) {
                agent.seek_path.clear();
                agent.seek_next = 0;
                continue;
            }
            agent.hunt_target = other->net_id;
            agent.seek_repath_in = 0;
            target = other;
        }
        if (target == nullptr) {
            // Gone for good, or never had: an inherited hunter whose caller
            // had nothing to give it counts as having lost it.
            const bool lost = agent.hunt_target != 0u ||
                agent.hunter.target_source == HunterTargetSource::kInherited;
            agent.hunt_target = 0u;
            if (lost && agent.hunter.on_target_lost == HunterOnTargetLost::kGuard) {
                // Unchecked and without dropping the fight: whatever it can
                // see now, a guard fights as well.
                AgentControllerSpec guard;
                guard.role = AgentControllerRole::kGuard;
                apply_behavior(&agent, guard, 0u);
                continue;
            }
            agent.hunter.target_source = HunterTargetSource::kNearestHostile;
            target = nearest(agent.position);
            if (target == nullptr) {
                agent.seek_path.clear();
                agent.seek_next = 0;
                continue;
            }
            agent.hunt_target = target->net_id;
            agent.seek_repath_in = 0;
        } else if (agent.hunter.target_source == HunterTargetSource::kNearestHostile &&
                   agent.seek_repath_in == 0u) {
            // Switching only for one clearly nearer, or two at much the same
            // range would trade places on every look.
            const Hostile* closer = nearest(agent.position);
            if (closer != nullptr && closer != target &&
                distance(agent.position, closer->position) <
                    0.8f * distance(agent.position, target->position)) {
                target = closer;
                agent.hunt_target = closer->net_id;
            }
        }
        if (agent.seek_repath_in > 0u) {
            --agent.seek_repath_in;
            continue;
        }
        // Staggered by net id after the first, so a wave spawned on one tick
        // does not path on one tick forever after.
        agent.seek_repath_in = agent.hunter.repath_ticks +
            agent.net_id % std::max<std::uint32_t>(1u, agent.hunter.repath_ticks / 4u);
        agent.seek_path.clear();
        agent.seek_next = 0;
        PatrolRouteRequest request;
        request.start = agent.position;
        request.end = target->position;
        request.snap_half_extent_meters = 2.0f;
        if (!patrol_navigation_.valid() ||
            !patrol_navigation_.find_route(request, &agent.seek_path) ||
            agent.seek_path.empty()) {
            agent.seek_path.assign(1, target->position);
        }
    }
}

void AgentRuntimeManager::update_guard_homes() {
    for (AgentRuntimeState& agent : agents_) {
        if (agent.controller_role != AgentControllerRole::kGuard || agent.patrol.has_slot ||
            agent.entry.active) {
            continue;
        }
        // A path is for one walk home from where a fight left it; anything
        // else -- the fight, a Return on the leash -- makes it stale.
        if (agent.sentry.state != AgentSentryState::kIdle || !agent.has_guard_anchor ||
            agent_steering::horizontal_distance(agent.position, agent.guard_anchor) <=
                kGuardAnchorArrivalMeters) {
            agent.seek_path.clear();
            agent.seek_next = 0;
            continue;
        }
        if (!agent.seek_path.empty()) {
            continue;
        }
        PatrolRouteRequest request;
        request.start = agent.position;
        request.end = agent.guard_anchor;
        request.snap_half_extent_meters = 2.0f;
        agent.seek_next = 0;
        if (!patrol_navigation_.valid() ||
            !patrol_navigation_.find_route(request, &agent.seek_path) ||
            agent.seek_path.empty()) {
            agent.seek_path.assign(1, agent.guard_anchor);
        }
    }
}

void AgentRuntimeManager::update_lock_targets(const ActorStateView& actors) {
    for (AgentRuntimeState& agent : agents_) {
        if (agent.controller_role != AgentControllerRole::kLockTarget || agent.entry.active) {
            continue;
        }
        const KernelServerEntityState* target = nullptr;
        for (std::uint32_t index = 0; index < actors.count; ++index) {
            if (actors.states[index].net_id == agent.lock_target) {
                target = &actors.states[index];
                break;
            }
        }
        const bool gone = target == nullptr || target->valid == 0u || target->hp == 0u ||
            (target->visual_flags & KERNEL_VISUAL_FLAG_DEAD) != 0u;
        const bool given_up = agent.lock_tuning.give_up_ticks > 0u &&
            agent.lock_unseen_ticks >= agent.lock_tuning.give_up_ticks;
        if (gone || given_up) {
            // Unchecked and without dropping the fight, as a hunter turning
            // guard is: whatever it can see now, what it becomes fights too.
            AgentControllerSpec next;
            next.role = agent.lock_tuning.on_target_lost == LockTargetOnLost::kHunter
                ? AgentControllerRole::kHunter
                : AgentControllerRole::kGuard;
            apply_behavior(&agent, next, 0u);
            continue;
        }
        if (agent.seek_repath_in > 0u) {
            --agent.seek_repath_in;
            continue;
        }
        const std::uint32_t repath = agent.lock_tuning.repath_ticks;
        agent.seek_repath_in =
            repath + agent.net_id % std::max<std::uint32_t>(1u, repath / 4u);
        agent.seek_path.clear();
        agent.seek_next = 0;
        PatrolRouteRequest request;
        request.start = agent.position;
        request.end = target->position;
        request.snap_half_extent_meters = 2.0f;
        if (!patrol_navigation_.valid() ||
            !patrol_navigation_.find_route(request, &agent.seek_path) ||
            agent.seek_path.empty()) {
            agent.seek_path.assign(1, target->position);
        }
    }
}

std::uint8_t AgentRuntimeManager::camp_of(std::uint32_t actor_template_id) const {
    const auto cached = camp_cache_.find(actor_template_id);
    if (cached != camp_cache_.end()) {
        return cached->second;
    }
    const ActorTemplateConfig* actor_template = find_actor_template(config_, actor_template_id);
    const std::uint8_t camp = actor_template != nullptr
        ? actor_template->vision.camp
        : static_cast<std::uint8_t>(KernelAgentCamp_Unknown);
    camp_cache_.emplace(actor_template_id, camp);
    return camp;
}

bool AgentRuntimeManager::hidden_in_shelter(std::uint32_t net_id) const {
    if (shelter_ == nullptr) {
        return false;
    }
    const std::uint32_t building = shelter_->shelter_of(net_id);
    if (building == 0u) {
        return false;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel_, building, &state)) {
        return false;
    }
    // A building is a prop, so it is among the entity templates, not the
    // actor templates find_actor_template reads.
    for (const EntityTemplateConfig& building_template : config_.entity_templates) {
        if (building_template.actor_template_id == state.entity_template_id) {
            return building_template.shelter_hides_occupants != 0u;
        }
    }
    return false;
}

void AgentRuntimeManager::attach_pending_controllers() {
    for (const SpawnedControllerSpec& spawned :
         std::exchange(pending_controllers_, {})) {
        const std::size_t index = agent_index_.find(spawned.net_id);
        if (index == AgentIndex::kNotFound) {
            spdlog::warn("controller spec has no agent net_id={}", spawned.net_id);
            continue;
        }
        // The catalog has already checked the role suits the template. A
        // route makes a patrol (B8) however the spec was built; the parser
        // says so too, but a spec built in code may leave the role out.
        AgentControllerSpec spec = spawned.spec;
        if (spec.role == AgentControllerRole::kNone && !spec.route.empty()) {
            spec.role = AgentControllerRole::kPatrol;
        }
        if (spec.role != AgentControllerRole::kNone) {
            apply_behavior(&agents_[index], spec, spawned.source_net_id);
        }
    }
}

BehaviorSwitchResult AgentRuntimeManager::set_behavior(
    std::uint32_t net_id,
    const AgentControllerSpec& spec) {
    const std::size_t index = agent_index_.find(net_id);
    if (index == AgentIndex::kNotFound) {
        return BehaviorSwitchResult::kAgentMissing;
    }
    AgentRuntimeState& agent = agents_[index];
    const BehaviorSwitchResult result = check_behavior(agent, spec);
    if (result != BehaviorSwitchResult::kAccepted) {
        return result;
    }
    // Lets go at once (B10). An action already under way finishes on its own;
    // a wind-up the chaser drops itself once the agent is not pursuing.
    agent.sentry.target = 0;
    agent.sentry.has_attention_point = false;
    agent.sentry.alerted_by_damage = false;
    agent.chase_holding = false;
    agent_steering::transition_to(&agent, AgentSentryState::kIdle);
    apply_behavior(&agent, spec, 0u);
    return BehaviorSwitchResult::kAccepted;
}

namespace {

// A peer's agent commands per second before the rest are refused: a guard
// against a client flooding the server, counting every command, apart from
// the cooldown after an obeyed one -- which is what a player feels.
constexpr std::uint32_t kAgentCommandsPerPeerPerWindow = 16;
constexpr std::uint64_t kAgentCommandWindowTicks = 30;
// How far from the agent a command may post it (§12.11): one order moves it
// across a fight, not across the map -- a patrol route does that.
constexpr float kAgentCommandMaxAnchorMeters = 30.0f;

}  // namespace

KernelAgentCommandOutcome AgentRuntimeManager::handle_agent_command(
    const KernelAgentCommand& command) {
    KernelAgentCommandOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.requester_peer = command.requester_peer;
    outcome.request_id = command.request_id;
    outcome.agent_net_id = command.agent_net_id;
    const auto reject = [&](std::uint8_t reason) {
        outcome.status = KernelAgentCommandStatus_Rejected;
        outcome.rejection_reason = reason;
        return outcome;
    };

    CommandWindow& window = command_windows_[command.requester_peer];
    // A new flood window; the cooldown carries across it.
    if (ticks_ >= window.start_tick + kAgentCommandWindowTicks) {
        window.start_tick = ticks_;
        window.count = 0u;
    }
    if (++window.count > kAgentCommandsPerPeerPerWindow) {
        return reject(KernelAgentCommandRejection_RateLimited);
    }
    // Still cooling down from the last order obeyed: whatever this one asks,
    // it waits. Only an obeyed order starts the cooldown, so a mistaken one
    // costs nothing.
    if (window.obeyed &&
        ticks_ < window.last_obeyed_tick + config_.agent_command_peer_cooldown_ticks) {
        return reject(KernelAgentCommandRejection_RateLimited);
    }

    KernelServerEntityState requester{};
    requester.struct_size = sizeof(requester);
    if (command.requester_net_id == 0u ||
        !Kernel_ServerGetEntityState(kernel_, command.requester_net_id, &requester) ||
        requester.valid == 0u || requester.hp == 0u ||
        (requester.visual_flags & KERNEL_VISUAL_FLAG_DEAD) != 0u) {
        return reject(KernelAgentCommandRejection_RequesterDead);
    }

    const std::size_t index = agent_index_.find(command.agent_net_id);
    if (index == AgentIndex::kNotFound) {
        return reject(KernelAgentCommandRejection_AgentMissing);
    }
    AgentRuntimeState& agent = agents_[index];
    // Any living player commands any agent of its side (B13).
    if (camp_of(agent.actor_template_id) != KernelAgentCamp_PlayerSide) {
        return reject(KernelAgentCommandRejection_NotPlayerSide);
    }

    // What the command asks for, and nothing a behavior would ignore (B3).
    const bool has_position = (command.flags & KERNEL_AGENT_COMMAND_FLAG_POSITION) != 0u;
    if ((command.flags & ~KERNEL_AGENT_COMMAND_FLAG_POSITION) != 0u) {
        return reject(KernelAgentCommandRejection_InvalidData);
    }
    AgentControllerSpec spec;
    switch (command.behavior) {
        case KernelAgentCommandBehavior_Guard:
            if (command.route_id != 0u || command.target_net_id != 0u) {
                return reject(KernelAgentCommandRejection_InvalidData);
            }
            spec.role = AgentControllerRole::kGuard;
            if (has_position) {
                if (!std::isfinite(command.position.x) || !std::isfinite(command.position.y) ||
                    !std::isfinite(command.position.z) ||
                    agent_steering::horizontal_distance(agent.position, command.position) >
                        kAgentCommandMaxAnchorMeters) {
                    return reject(KernelAgentCommandRejection_InvalidData);
                }
                spec.has_anchor = true;
                spec.anchor = command.position;
            }
            break;
        case KernelAgentCommandBehavior_Patrol: {
            const AgentRouteConfig* route = find_agent_route(config_, command.route_id);
            if (route == nullptr || command.target_net_id != 0u || has_position) {
                return reject(KernelAgentCommandRejection_InvalidData);
            }
            spec.role = AgentControllerRole::kPatrol;
            spec.route = route->waypoints;
            break;
        }
        case KernelAgentCommandBehavior_LockTarget:
            if (command.route_id != 0u || has_position) {
                return reject(KernelAgentCommandRejection_InvalidData);
            }
            spec.role = AgentControllerRole::kLockTarget;
            spec.target_net_id = command.target_net_id;
            break;
        default:
            return reject(KernelAgentCommandRejection_BehaviorNotAllowed);
    }

    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, agent.actor_template_id);
    const std::uint64_t interval = actor_template != nullptr
        ? actor_template->remote_command_min_switch_interval_ticks
        : 0u;
    if (agent.remote_switched && ticks_ < agent.last_remote_switch_tick + interval) {
        return reject(KernelAgentCommandRejection_TooSoon);
    }

    switch (set_behavior(command.agent_net_id, spec)) {
        case BehaviorSwitchResult::kAccepted:
            break;
        case BehaviorSwitchResult::kAgentMissing:
            return reject(KernelAgentCommandRejection_AgentMissing);
        case BehaviorSwitchResult::kNotAllowed:
            return reject(KernelAgentCommandRejection_BehaviorNotAllowed);
        case BehaviorSwitchResult::kInvalidData:
            return reject(KernelAgentCommandRejection_InvalidData);
    }
    agent.remote_switched = true;
    agent.last_remote_switch_tick = ticks_;
    window.obeyed = true;
    window.last_obeyed_tick = ticks_;
    outcome.status = KernelAgentCommandStatus_Accepted;
    outcome.rejection_reason = KernelAgentCommandRejection_None;
    return outcome;
}

BehaviorSwitchResult AgentRuntimeManager::check_behavior(
    const AgentRuntimeState& agent,
    const AgentControllerSpec& spec) const {
    if (spec.role == AgentControllerRole::kNone) {
        return BehaviorSwitchResult::kInvalidData;
    }
    // A sentry cannot walk and nothing else can stand still, so neither side
    // switches across; and a squad keeps its members.
    if (spec.role == AgentControllerRole::kSentry ||
        agent.controller_role == AgentControllerRole::kSentry ||
        agent.controller_role == AgentControllerRole::kNone ||
        agent.patrol.has_slot) {
        return BehaviorSwitchResult::kNotAllowed;
    }
    const AgentBehavior* behavior = agent_behavior_for_role(spec.role);
    if (behavior == nullptr) {
        return BehaviorSwitchResult::kNotAllowed;
    }
    const BehaviorSwitchResult result = behavior->validate(spec);
    if (result == BehaviorSwitchResult::kAccepted &&
        spec.role == AgentControllerRole::kLockTarget &&
        !lockable(agent, spec.target_net_id)) {
        return BehaviorSwitchResult::kInvalidData;
    }
    return result;
}

bool AgentRuntimeManager::lockable(
    const AgentRuntimeState& agent,
    std::uint32_t target) const {
    if (target == 0u || target == agent.net_id) {
        return false;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel_, target, &state) || state.valid == 0u ||
        state.hp == 0u || (state.visual_flags & KERNEL_VISUAL_FLAG_DEAD) != 0u) {
        return false;
    }
    const ActorTemplateConfig* self_template =
        find_actor_template(config_, agent.actor_template_id);
    const ActorTemplateConfig* target_template =
        find_actor_template(config_, state.actor_template_id);
    if (self_template == nullptr || target_template == nullptr) {
        return false;
    }
    // The kernel's vision rule (classify_agent_relation): the two sides are
    // hostile to each other, and nothing else is.
    const std::uint8_t self_camp = self_template->vision.camp;
    const std::uint8_t target_camp = target_template->vision.camp;
    const bool hostile =
        (self_camp == KernelAgentCamp_PlayerSide && target_camp == KernelAgentCamp_EnemySide) ||
        (self_camp == KernelAgentCamp_EnemySide && target_camp == KernelAgentCamp_PlayerSide);
    return hostile &&
        (self_template->lock_target.target_relations & kLockTargetRelationHostile) != 0u;
}

void AgentRuntimeManager::apply_behavior(
    AgentRuntimeState* agent_state,
    const AgentControllerSpec& spec,
    std::uint32_t source_net_id) {
    AgentRuntimeState& agent = *agent_state;
    if (const AgentBehavior* old = agent_behavior_for_role(agent.controller_role)) {
        old->exit(agent);
    }
    agent.controller_role = spec.role;
    if (spec.has_hunter) {
        agent.hunter = spec.hunter;
    } else if (spec.role == AgentControllerRole::kHunter) {
        // Its template's own hunter tuning, not whatever an earlier spec left.
        const ActorTemplateConfig* actor_template =
            find_actor_template(config_, agent.actor_template_id);
        if (actor_template != nullptr) {
            agent.hunter = actor_template->hunter;
        }
    }
    if (spec.role == AgentControllerRole::kLockTarget) {
        const ActorTemplateConfig* actor_template =
            find_actor_template(config_, agent.actor_template_id);
        agent.lock_tuning = actor_template != nullptr ? actor_template->lock_target
                                                      : AgentLockTargetTuning{};
        agent.lock_target = spec.target_net_id;
    }
    if (const AgentBehavior* now = agent_behavior_for_role(agent.controller_role)) {
        now->enter(agent);
    }
    if (spec.has_anchor) {
        agent.has_guard_anchor = true;
        agent.guard_anchor = spec.anchor;
    }
    // Reinforcements go after what called them: the spawning agent's target
    // as it stands now. Nothing to inherit leaves it 0, which the hunter pass
    // reads as already lost.
    if (agent.controller_role == AgentControllerRole::kHunter &&
        agent.hunter.target_source == HunterTargetSource::kInherited &&
        source_net_id != 0u) {
        const std::size_t source = agent_index_.find(source_net_id);
        if (source != AgentIndex::kNotFound) {
            agent.hunt_target = agents_[source].sentry.target;
        }
    }
    if (!spec.route.empty()) {
        attach_route(&agent, spec.route);
    }
}

void AgentRuntimeManager::attach_route(
    AgentRuntimeState* agent_state,
    const std::vector<KernelVec3>& waypoints) {
    AgentRuntimeState& agent = *agent_state;
    {
        if (agent.controller_role != AgentControllerRole::kPatrol) {
            // Only a patrol walks a route; anything else decides its own
            // movement and would silently ignore one. The catalog and
            // set_behavior refuse this, so it is a bug if seen.
            spdlog::warn(
                "route on an agent that is not a patrol net_id={} template_id={}",
                agent.net_id,
                agent.actor_template_id);
            return;
        }
        // Each leg pathed on its own, so the authored waypoints are kept as
        // corners: they are what keeps a wide body off the walls, which the
        // navmesh, baked for a much narrower agent, does not know about. A leg
        // the navmesh cannot connect is walked straight rather than dropped --
        // the waypoints are authored, and a straight leg is visibly wrong
        // where a missing one would be invisibly wrong.
        std::vector<KernelVec3> path{waypoints.front()};
        const bool navigable = patrol_navigation_.valid();
        for (std::size_t leg = 1; leg < waypoints.size(); ++leg) {
            std::vector<KernelVec3> corners;
            PatrolRouteRequest request;
            request.start = path.back();
            request.end = waypoints[leg];
            request.snap_half_extent_meters = 2.0f;
            if (navigable && patrol_navigation_.find_route(request, &corners) &&
                !corners.empty()) {
                path.insert(path.end(), corners.begin(), corners.end());
            } else {
                if (navigable) {
                    spdlog::warn(
                        "patrol route leg not on the navmesh net_id={} leg={}; walking it straight",
                        agent.net_id,
                        leg);
                }
                path.push_back(waypoints[leg]);
            }
        }
        // Joined at the corner nearest where it stands, not at the first one:
        // a spawn point off the route's start would otherwise walk a straight
        // line to it, through whatever is in between.
        std::size_t nearest = 0u;
        float nearest_distance = 0.0f;
        for (std::size_t corner = 0; corner < path.size(); ++corner) {
            const float dx = path[corner].x - agent.position.x;
            const float dz = path[corner].z - agent.position.z;
            const float distance = dx * dx + dz * dz;
            if (corner == 0u || distance < nearest_distance) {
                nearest = corner;
                nearest_distance = distance;
            }
        }
        agent.route_patrol = AgentRoutePatrolState{std::move(path), nearest, 1};
        spdlog::info(
            "patrol route attached net_id={} waypoints={} corners={}",
            agent.net_id,
            waypoints.size(),
            agent.route_patrol.path.size());
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
    ++ticks_;
    if (despawn_pending_) {
        if (!has_live_agent()) {
            despawn_pending_ = false;
        } else {
            // No controller runs, so nobody is engaged this tick; a signal
            // left from before the despawn would name agents being destroyed.
            alert_signals_.clear();
            return;
        }
    }

    // One snapshot for the whole retirement pass, taken before any director has
    // run -- which is exactly what the per-squad queries used to see, because
    // retirement happens before this tick creates anything.
    ActorStateView actors = refresh_actor_states();
    AgentBudget agent_budget;
    agent_budget.ceiling = config_.agent_budget.max_live_agents;
    agent_budget.live = live_agent_count(actors);

    // Ahead of the resync on purpose: the director creates entities, and the
    // group runtime drops members it cannot find in the agent list. Ticking it
    // after the resync would drop every member of a squad on the tick it was
    // spawned.
    const std::uint32_t spawned_groups_before =
        patrol_director_.spawned_group_count();
    patrol_director_.tick(
        kernel_, &patrol_groups_, &patrol_navigation_, actors, &agent_budget);
    take_controllers(patrol_director_.take_spawned_controllers(), &pending_controllers_);
    // Only on a tick a squad actually appeared, which is once per definition
    // per interval_ticks. The world rule counts every agent alive including the
    // ones just spawned -- counting the pre-spawn list would have it top up
    // against a population that already exists.
    if (patrol_director_.spawned_group_count() != spawned_groups_before) {
        actors = refresh_actor_states();
        // Exact again: the patrol spent its largest draw, not what it drew.
        agent_budget.live = live_agent_count(actors);
    }
    // Counted, never refused: a world rule keeps a population the world is
    // meant to have.
    const std::uint32_t world_rule_spawned_before =
        world_rule_director_.spawned_agent_count();
    world_rule_director_.tick(kernel_, agent_budget.live);
    take_controllers(world_rule_director_.take_spawned_controllers(), &pending_controllers_);
    agent_budget.spend(
        world_rule_director_.spawned_agent_count() - world_rule_spawned_before);

    game_rule_director_.tick(kernel_);
    take_controllers(game_rule_director_.take_spawned_controllers(), &pending_controllers_);
    // Last tick's engagement: the controllers that produced it have all run,
    // and nothing has been created since.
    spawner_director_.tick(kernel_, alert_signals_, &agent_budget);
    take_controllers(spawner_director_.take_spawned_controllers(), &pending_controllers_);
    // Re-taken because the three directors above all create, and the resync
    // exists to discover what they made -- that is the whole reason it runs
    // after them.
    actors = refresh_actor_states();
    sync_agents_from_kernel(actors);
    // After the resync, because the agent a walk names was created by one of
    // the directors above and is not in the list until now.
    attach_pending_entries();
    attach_pending_controllers();
    // Before the controllers, which skip anyone still walking: this is what
    // submits their input for the tick, and only one input per agent survives.
    tick_entries();
    // Ahead of the controllers, so a member reads the slot its squad wants it
    // in this tick rather than the one from last tick.
    patrol_groups_.tick(&agents_, agent_index_, delta_seconds);
    // Before the controllers, so a hunter seeks along this tick's path.
    update_hunters(actors);
    update_lock_targets(actors);
    update_guard_homes();
    // Still on the post-director snapshot: nothing between it and here creates,
    // destroys or moves an entity, so it is what a per-agent query would return.
    dispatch_controllers(actors, delta_seconds);
    // After the controllers, so a state entered this tick is entered here too.
    use_item_skills();
    collect_alert_signals();
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
            // An actor template is immutable, so this is resolved once rather
            // than every tick. state.actor_template_id is what the entity was
            // actually spawned from, which is the whole point: a wave can spawn
            // a template the catalog never names.
            agent.sentry_config =
                agent_sentry_config(config_, state.actor_template_id);
            const ActorTemplateConfig* actor_template =
                find_actor_template(config_, state.actor_template_id);
            agent.controller_role = actor_template != nullptr
                ? actor_template->default_controller_role
                : AgentControllerRole::kNone;
            if (actor_template != nullptr) {
                agent.hunter = actor_template->hunter;
                for (const ColliderTemplateConfig& collider : config_.colliders.templates) {
                    if (collider.definition.template_id ==
                        actor_template->vision.vision_collider_template_id) {
                        agent.vision_reach_meters = collider.definition.shape_params.x;
                    }
                }
            }
            agent.has_item_skills = actor_template != nullptr &&
                std::any_of(actor_template->skills.begin(), actor_template->skills.end(),
                            [](const AgentSkillConfig& skill) {
                                return skill.type == AgentSkillType::kItem;
                            });
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
