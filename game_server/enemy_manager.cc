#include "game_server/enemy_manager.h"

#include <algorithm>

namespace network_example::game_server {
namespace {

constexpr KernelVec3 kInitialEnemyPosition{6.0f, 0.0f, 0.0f};
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};

bool is_enemy_destroyed_event(const KernelEvent& event, const Enemy& enemy) {
    return event.type == KernelEventType_EntityDestroyed &&
           event.net_id == enemy.net_id;
}

}  // namespace

EnemyManager::EnemyManager(KernelHandle* kernel) : kernel_(kernel) {}

void EnemyManager::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined) {
        has_seen_player_ = true;
        return;
    }

    if (event.type != KernelEventType_EntityDestroyed) {
        return;
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
        spawn_initial_enemy();
    }
    ai_.tick(kernel_, &enemies_, delta_seconds);
}

void EnemyManager::despawn_all(std::uint32_t reason) {
    if (kernel_ == nullptr) {
        enemies_.clear();
        return;
    }

    for (const Enemy& enemy : enemies_) {
        Kernel_ServerDestroyEntity(kernel_, enemy.net_id, reason);
    }
    enemies_.clear();
}

std::size_t EnemyManager::enemy_count() const {
    return enemies_.size();
}

const std::vector<Enemy>& EnemyManager::enemies() const {
    return enemies_;
}

void EnemyManager::spawn_initial_enemy() {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(KernelServerEntityCreateInfo);
    create_info.entity_type = kEntityTypeEnemy;
    create_info.owner_peer = 0;
    create_info.position = kInitialEnemyPosition;
    create_info.rotation = kIdentityRotation;
    create_info.animation_state = kEnemyAnimationIdle;
    create_info.visual_flags = 0;

    std::uint32_t net_id = 0;
    if (!Kernel_ServerCreateEntity(kernel_, &create_info, &net_id) || net_id == 0) {
        return;
    }

    Enemy enemy;
    enemy.net_id = net_id;
    enemy.position = create_info.position;
    enemy.patrol_anchor = create_info.position;
    enemy.velocity = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.hp = kEnemyInitialHp;
    enemy.max_hp = kEnemyInitialHp;
    enemy.animation_state = create_info.animation_state;
    enemies_.push_back(enemy);
    has_spawned_initial_enemy_ = true;
}

void EnemyManager::prune_missing_enemies() {
    enemies_.erase(
        std::remove_if(
            enemies_.begin(),
            enemies_.end(),
            [this](const Enemy& enemy) {
                KernelServerEntityState state{};
                state.struct_size = sizeof(KernelServerEntityState);
                return !Kernel_ServerGetEntityState(kernel_, enemy.net_id, &state) ||
                       !state.valid;
            }),
        enemies_.end());
}

}  // namespace network_example::game_server
