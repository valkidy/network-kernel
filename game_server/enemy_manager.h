#ifndef GAME_SERVER_ENEMY_MANAGER_H_
#define GAME_SERVER_ENEMY_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/enemy.h"
#include "game_server/enemy_ai_controller.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class EnemyManager {
public:
    explicit EnemyManager(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    void despawn_all(std::uint32_t reason);

    std::size_t enemy_count() const;
    const std::vector<Enemy>& enemies() const;

private:
    void spawn_initial_enemy();
    void prune_missing_enemies();
    bool apply_weapon_mechanics(std::uint32_t net_id) const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    EnemyAIController ai_;
    std::vector<Enemy> enemies_;
    bool has_seen_player_ = false;
    bool has_spawned_initial_enemy_ = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_MANAGER_H_
