#include "game_server/enemy_manager.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kPi = 3.14159265358979323846f;

bool is_enemy_destroyed_event(const KernelEvent& event, const Enemy& enemy) {
    return event.type == KernelEventType_EntityDestroyed &&
           event.net_id == enemy.net_id;
}

AgentSentryConfig agent_sentry_config(const GameServerGameplayConfig& config) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config, config.enemy.actor_template_id);
    return actor_template == nullptr ? AgentSentryConfig{} : actor_template->sentry;
}

}  // namespace

EnemyManager::EnemyManager(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      sentry_(agent_sentry_config(config_)) {}

void EnemyManager::handle_event(const KernelEvent& event) {
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
    enemies_.erase(
        std::remove_if(
            enemies_.begin(),
            enemies_.end(),
            [&event](const Enemy& enemy) {
                return is_enemy_destroyed_event(event, enemy);
            }),
        enemies_.end());
}

void EnemyManager::tick(float delta_seconds) {
    if (kernel_ == nullptr) {
        return;
    }

    prune_missing_enemies();
    if (has_seen_player_ && !has_spawned_initial_enemy_) {
        spawn_initial_enemies();
    }
    sentry_.tick(kernel_, &enemies_, delta_seconds);
}

void EnemyManager::despawn_all(std::uint32_t reason) {
    if (kernel_ == nullptr) {
        enemies_.clear();
        return;
    }

    for (const Enemy& enemy : enemies_) {
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = enemy.net_id;
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
    enemies_.clear();
}

std::size_t EnemyManager::enemy_count() const {
    return enemies_.size();
}

const std::vector<Enemy>& EnemyManager::enemies() const {
    return enemies_;
}

void EnemyManager::spawn_initial_enemies() {
    spawn_director();
    std::mt19937 rng(config_.enemy.spawn_seed);
    std::uniform_real_distribution<float> unit_distribution(0.0f, 1.0f);
    const KernelVec3 center = config_.enemy.spawn_position;
    for (std::uint32_t index = 0; index < config_.enemy.spawn_count; ++index) {
        KernelVec3 position = center;
        if (config_.enemy.spawn_radius > 0.0f) {
            const float radius =
                config_.enemy.spawn_radius * std::sqrt(unit_distribution(rng));
            const float angle = 2.0f * kPi * unit_distribution(rng);
            position.x = center.x + radius * std::cos(angle);
            position.y = 0.0f;
            position.z = center.z + radius * std::sin(angle);
        }
        spawn_enemy_at(position);
    }
    has_spawned_initial_enemy_ = true;
}

bool EnemyManager::spawn_director() {
    if (director_net_id_ != 0) {
        return true;
    }

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(KernelServerEntityCreateInfo);
    create_info.entity_type = KernelEntityType_Director;
    create_info.owner_peer = 0;
    create_info.position = config_.enemy.spawn_position;
    create_info.rotation = kIdentityRotation;
    create_info.entity_template_id = kDefaultDirectorEntityTemplateId;

    std::uint32_t net_id = 0;
    if (!Kernel_ServerCreateEntity(kernel_, &create_info, &net_id) || net_id == 0) {
        return false;
    }
    director_net_id_ = net_id;
    return true;
}

bool EnemyManager::spawn_enemy_at(const KernelVec3& position) {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, config_.enemy.actor_template_id);
    if (actor_template == nullptr) {
        return false;
    }
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(KernelServerEntityCreateInfo);
    create_info.entity_type = actor_template->entity_type;
    create_info.actor_type = actor_template->actor_type;
    create_info.owner_peer = 0;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    create_info.animation_state = actor_template->animation_idle;
    create_info.visual_flags = 0;
    create_info.actor_template_id = actor_template->actor_template_id;
    create_info.entity_template_id = actor_template->actor_template_id;

    std::uint32_t net_id = 0;
    if (!Kernel_ServerCreateEntity(kernel_, &create_info, &net_id) || net_id == 0) {
        return false;
    }
    KernelCombatStateDefinition combat_state = make_enemy_combat_state(config_);
    if (!Kernel_ServerSetEntityCombatState(kernel_, net_id, &combat_state) ||
        !Kernel_ServerSetEntityVisionConfig(kernel_, net_id, &actor_template->vision) ||
        !apply_weapon_mechanics(net_id)) {
        Kernel_ServerDestroyEntity(kernel_, net_id, KernelDespawnReason_Destroyed);
        return false;
    }

    Enemy enemy;
    enemy.net_id = net_id;
    enemy.position = create_info.position;
    enemy.patrol_anchor = create_info.position;
    enemy.velocity = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.hp = combat_state.hp;
    enemy.max_hp = combat_state.max_hp;
    enemy.animation_state = create_info.animation_state;
    enemy.sentry.self_id = net_id;
    enemies_.push_back(enemy);
    return true;
}

bool EnemyManager::apply_weapon_mechanics(std::uint32_t net_id) const {
    const ActorTemplateConfig* actor_template =
        find_actor_template(config_, config_.enemy.actor_template_id);
    if (actor_template == nullptr) {
        return false;
    }
    for (std::uint8_t slot = 0; slot < actor_template->weapon_slot_count; ++slot) {
        const KernelWeaponMechanicsDefinition& weapon =
            config_.weapons.definitions[actor_template->weapon_slots[slot]];
        if (!Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon)) {
            return false;
        }
    }
    return true;
}

void EnemyManager::prune_missing_enemies() {
    if (director_net_id_ != 0) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(KernelServerEntityState);
        if (!Kernel_ServerGetEntityState(kernel_, director_net_id_, &state) ||
            state.valid == 0u) {
            director_net_id_ = 0;
        }
    }
    enemies_.erase(
        std::remove_if(
            enemies_.begin(),
            enemies_.end(),
            [this](const Enemy& enemy) {
                KernelServerEntityState state{};
                state.struct_size = sizeof(KernelServerEntityState);
                return !Kernel_ServerGetEntityState(kernel_, enemy.net_id, &state) ||
                       state.valid == 0u;
            }),
        enemies_.end());
}

}  // namespace network_example::game_server
