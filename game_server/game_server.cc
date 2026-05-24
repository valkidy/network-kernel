#include "game_server/game_server.h"

#include <utility>

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      enemy_manager_(kernel, config_) {}

void GameServer::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined && event.net_id != 0) {
        configure_player(event.net_id);
    }
    enemy_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    enemy_manager_.tick(delta_seconds);
}

EnemyManager& GameServer::enemy_manager() {
    return enemy_manager_;
}

const EnemyManager& GameServer::enemy_manager() const {
    return enemy_manager_;
}

void GameServer::configure_player(std::uint32_t net_id) const {
    if (kernel_ == nullptr) {
        return;
    }
    KernelCombatStateDefinition combat_state = make_player_combat_state(config_);
    if (!Kernel_ServerSetEntityCombatState(kernel_, net_id, &combat_state)) {
        return;
    }
    for (const KernelWeaponMechanicsDefinition& weapon : config_.weapons.definitions) {
        Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon);
    }
}

}  // namespace network_example::game_server
