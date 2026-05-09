#include "game_server/game_server.h"

namespace network_example::game_server {

GameServer::GameServer(KernelHandle* kernel) : enemy_manager_(kernel) {}

void GameServer::handle_event(const KernelEvent& event) {
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

}  // namespace network_example::game_server
