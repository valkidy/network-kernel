#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include "game_server/enemy_manager.h"
#include "game_server/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

class GameServer {
public:
    explicit GameServer(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);

    EnemyManager& enemy_manager();
    const EnemyManager& enemy_manager() const;

private:
    void configure_player(std::uint32_t net_id) const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    EnemyManager enemy_manager_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
