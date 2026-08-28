#include "game_server/agent_runtime_manager.h"

#include <algorithm>
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
            binding.chaser = AgentChaserController(chaser);
        }
        controllers_.push_back(std::move(binding));
    }

    fallback_controller_index_ = controllers_.size();
    for (std::size_t index = 0; index < controllers_.size(); ++index) {
        if (controllers_[index].actor_template_id ==
            config_.agent.actor_template_id) {
            fallback_controller_index_ = index;
            break;
        }
    }
}

AgentRuntimeManager::AgentControllerBinding* AgentRuntimeManager::binding_for(
    std::uint32_t actor_template_id) {
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.actor_template_id == actor_template_id) {
            return &binding;
        }
    }
    return fallback_controller_index_ < controllers_.size()
        ? &controllers_[fallback_controller_index_]
        : nullptr;
}

void AgentRuntimeManager::dispatch_controllers(float delta_seconds) {
    for (AgentControllerBinding& binding : controllers_) {
        binding.batch.clear();
    }
    for (const AgentRuntimeState& agent : agents_) {
        AgentControllerBinding* binding = binding_for(agent.actor_template_id);
        if (binding == nullptr) {
            continue;
        }
        binding->batch.push_back(agent);
    }
    for (AgentControllerBinding& binding : controllers_) {
        if (binding.batch.empty()) {
            continue;
        }
        if (binding.ai_controller_type == KernelAiControllerType_Chaser) {
            binding.chaser.tick(kernel_, &binding.batch, delta_seconds);
        } else {
            binding.sentry.tick(kernel_, &binding.batch, delta_seconds);
        }
    }
    // Controllers never add or drop agents, so the batches only carry updates
    // back to the entries they were copied from.
    for (const AgentControllerBinding& binding : controllers_) {
        for (const AgentRuntimeState& updated : binding.batch) {
            const auto existing = std::find_if(
                agents_.begin(),
                agents_.end(),
                [&updated](const AgentRuntimeState& agent) {
                    return agent.net_id == updated.net_id;
                });
            if (existing != agents_.end()) {
                *existing = updated;
            }
        }
    }
}

void AgentRuntimeManager::handle_event(const KernelEvent& event) {
    if (event.type != KernelEventType_EntityDestroyed) {
        return;
    }
    director_net_ids_.erase(
        std::remove(
            director_net_ids_.begin(), director_net_ids_.end(), event.net_id),
        director_net_ids_.end());
    agents_.erase(
        std::remove_if(
            agents_.begin(),
            agents_.end(),
            [&event](const AgentRuntimeState& agent) {
                return agent.net_id == event.net_id;
            }),
        agents_.end());
}

void AgentRuntimeManager::tick(float delta_seconds) {
    if (kernel_ == nullptr) {
        return;
    }
    if (despawn_pending_) {
        if (!has_live_agent_or_director()) {
            despawn_pending_ = false;
        } else {
            return;
        }
    }

    sync_agents_from_kernel();
    dispatch_controllers(delta_seconds);
}

void AgentRuntimeManager::despawn_all(std::uint32_t reason) {
    if (kernel_ == nullptr) {
        agents_.clear();
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
    for (const std::uint32_t director_net_id : director_net_ids_) {
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = director_net_id;
        command.reason = reason;
        Kernel_ServerEnqueueEntityLifecycle(
            kernel_,
            KernelCommandSource_Internal,
            &command);
    }
    director_net_ids_.clear();
    agents_.clear();
    despawn_pending_ = true;
}

std::size_t AgentRuntimeManager::agent_count() const {
    return agents_.size();
}

const std::vector<AgentRuntimeState>& AgentRuntimeManager::agents() const {
    return agents_;
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
        if (!spawn_director(*entity_template)) {
            return false;
        }
    }

    director_preload_succeeded_ = true;
    spdlog::info(
        "director preload complete count={}",
        director_net_ids_.size());
    return true;
}

bool AgentRuntimeManager::spawn_director(
    const EntityTemplateConfig& director_template) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(KernelServerEntityCreateInfo);
    create_info.entity_type = KernelEntityType_Director;
    create_info.owner_peer = 0;
    create_info.position = director_template.transform_position;
    create_info.rotation = kIdentityRotation;
    create_info.entity_template_id = director_template.actor_template_id;

    std::uint32_t net_id = 0;
    if (!Kernel_ServerCreateEntity(kernel_, &create_info, &net_id) || net_id == 0) {
        spdlog::error(
            "director preload failed name={} template_id={} reason=create_entity",
            director_template.name,
            director_template.actor_template_id);
        return false;
    }
    director_net_ids_.push_back(net_id);
    spdlog::info(
        "director preloaded name={} template_id={} net_id={} position=({}, {}, {})",
        director_template.name,
        director_template.actor_template_id,
        net_id,
        director_template.transform_position.x,
        director_template.transform_position.y,
        director_template.transform_position.z);
    return true;
}

void AgentRuntimeManager::sync_agents_from_kernel() {
    director_net_ids_.erase(
        std::remove_if(
            director_net_ids_.begin(),
            director_net_ids_.end(),
            [this](std::uint32_t net_id) {
                KernelServerEntityState state{};
                state.struct_size = sizeof(KernelServerEntityState);
                return !Kernel_ServerGetEntityState(kernel_, net_id, &state) ||
                    state.valid == 0u;
            }),
        director_net_ids_.end());

    const std::uint32_t state_count = query_actor_states(&actor_query_buffer_);
    const std::vector<KernelServerEntityState>& states = actor_query_buffer_;

    std::vector<AgentRuntimeState> next_agents;
    next_agents.reserve(state_count);
    for (std::uint32_t index = 0; index < state_count; ++index) {
        const KernelServerEntityState& state = states[index];
        if (state.valid == 0u || state.actor_type != kActorTypeAgent) {
            continue;
        }
        const auto existing = std::find_if(
            agents_.begin(),
            agents_.end(),
            [&](const AgentRuntimeState& agent) {
                return agent.net_id == state.net_id;
            });
        AgentRuntimeState agent =
            existing == agents_.end() ? AgentRuntimeState{} : *existing;
        const bool discovered_agent = existing == agents_.end();
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

bool AgentRuntimeManager::has_live_agent_or_director() const {
    for (const std::uint32_t director_net_id : director_net_ids_) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(KernelServerEntityState);
        if (Kernel_ServerGetEntityState(kernel_, director_net_id, &state) &&
            state.valid != 0u) {
            return true;
        }
    }
    std::vector<KernelServerEntityState> states;
    const std::uint32_t state_count = query_actor_states(&states);
    for (std::uint32_t index = 0; index < state_count; ++index) {
        if (states[index].valid != 0u && states[index].actor_type == kActorTypeAgent) {
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
