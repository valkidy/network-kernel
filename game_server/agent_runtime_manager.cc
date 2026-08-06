#include "game_server/agent_runtime_manager.h"

#include <algorithm>
#include <array>
#include <utility>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint32_t kMaxQueriedAgents = 128;

AgentSentryConfig agent_sentry_config(const GameServerGameplayConfig& config) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, config.agent.actor_template_id);
    return actor_template == nullptr ? AgentSentryConfig{} : actor_template->sentry;
}

}  // namespace

AgentRuntimeManager::AgentRuntimeManager(
    KernelHandle* kernel,
    GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      sentry_(agent_sentry_config(config_)) {}

void AgentRuntimeManager::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined) {
        has_seen_player_ = true;
        return;
    }

    if (event.type != KernelEventType_EntityDestroyed) {
        return;
    }
    if (event.net_id == director_net_id_) {
        director_net_id_ = 0;
    }
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
    if (has_seen_player_ && !has_bootstrapped_director_) {
        bootstrap_directors();
    }
    sync_agents_from_kernel();
    sentry_.tick(kernel_, &agents_, delta_seconds);
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
    if (director_net_id_ != 0) {
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = director_net_id_;
        command.reason = reason;
        Kernel_ServerEnqueueEntityLifecycle(
            kernel_,
            KernelCommandSource_Internal,
            &command);
        director_net_id_ = 0;
    }
    agents_.clear();
    despawn_pending_ = true;
}

std::size_t AgentRuntimeManager::agent_count() const {
    return agents_.size();
}

const std::vector<AgentRuntimeState>& AgentRuntimeManager::agents() const {
    return agents_;
}

void AgentRuntimeManager::bootstrap_directors() {
    bool spawned_director = false;
    for (const EntityTemplateConfig& entity_template : config_.entity_templates) {
        if (entity_template.entity_type != KernelEntityType_Director) {
            continue;
        }
        spawned_director = spawn_director(entity_template) || spawned_director;
    }
    if (!spawned_director) {
        EntityTemplateConfig default_director{};
        default_director.actor_template_id = kDefaultDirectorEntityTemplateId;
        default_director.entity_type = KernelEntityType_Director;
        default_director.server_only = true;
        default_director.transform_position = config_.agent.spawn_position;
        spawn_director(default_director);
    }
    has_bootstrapped_director_ = true;
}

bool AgentRuntimeManager::spawn_director(
    const EntityTemplateConfig& director_template) {
    if (director_net_id_ != 0) {
        return true;
    }

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(KernelServerEntityCreateInfo);
    create_info.entity_type = KernelEntityType_Director;
    create_info.owner_peer = 0;
    create_info.position = director_template.transform_position;
    create_info.rotation = kIdentityRotation;
    create_info.entity_template_id = director_template.actor_template_id;

    std::uint32_t net_id = 0;
    if (!Kernel_ServerCreateEntity(kernel_, &create_info, &net_id) || net_id == 0) {
        return false;
    }
    director_net_id_ = net_id;
    return true;
}

void AgentRuntimeManager::sync_agents_from_kernel() {
    if (director_net_id_ != 0) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(KernelServerEntityState);
        if (!Kernel_ServerGetEntityState(kernel_, director_net_id_, &state) ||
            state.valid == 0u) {
            director_net_id_ = 0;
        }
    }

    std::array<KernelServerEntityState, kMaxQueriedAgents> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t state_count = Kernel_ServerQueryEntities(
        kernel_,
        kEntityTypeActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));

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
        agent.position = state.position;
        agent.hp = state.hp;
        agent.max_hp = state.max_hp;
        agent.animation_state = state.animation_state;
        agent.sentry.self_id = state.net_id;
        if (discovered_agent) {
            agent.patrol_anchor = state.position;
            apply_weapon_mechanics(state.net_id);
        }
        next_agents.push_back(agent);
    }
    agents_ = std::move(next_agents);
}

bool AgentRuntimeManager::apply_weapon_mechanics(std::uint32_t net_id) const {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, config_.agent.actor_template_id);
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
    if (director_net_id_ != 0) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(KernelServerEntityState);
        if (Kernel_ServerGetEntityState(kernel_, director_net_id_, &state) &&
            state.valid != 0u) {
            return true;
        }
    }
    std::array<KernelServerEntityState, kMaxQueriedAgents> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t state_count = Kernel_ServerQueryEntities(
        kernel_,
        kEntityTypeActor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < state_count; ++index) {
        if (states[index].valid != 0u && states[index].actor_type == kActorTypeAgent) {
            return true;
        }
    }
    return false;
}

}  // namespace network_example::game_server
